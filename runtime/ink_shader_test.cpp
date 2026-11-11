#include "ink_shader_test.h"
#include "native_pixel_fixture.h"
#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/logging.h>
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

namespace {
uint32_t Be(const uint8_t* p) {
    return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3];
}
struct Validation {
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> queue;
    uint64_t first=0;
    unsigned Count() const {
        unsigned count=0;
        if(queue)for(uint64_t i=first;i<queue->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
            SIZE_T size=0;queue->GetMessage(i,nullptr,&size);
            std::vector<uint8_t> bytes(size);
            auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
            if(SUCCEEDED(queue->GetMessage(i,message,&size)) && message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) {
                ++count;REXLOG_ERROR("Ink shader validation: {}",message->pDescription);
            }
        }
        return count;
    }
    bool reported=false;
    ~Validation(){if(!reported)Count();}
};
}

bool em2::TestOriginalInkPixels(rex::ui::d3d12::D3D12Provider& provider,
    const std::filesystem::path& effect_path,const std::filesystem::path& output) {
    using namespace em2::pixel_test;using namespace rex::graphics;
    std::filesystem::create_directories(output);
    std::ifstream input(effect_path,std::ios::binary);
    std::vector<uint8_t> effect{std::istreambuf_iterator<char>(input),{}};
    if(effect.size()!=22908 || Be(effect.data())!=0xA3D70141 ||
       Be(effect.data()+0x42B0)!=1 || Be(effect.data()+0x42CC)!=0 ||
       Be(effect.data()+0x3824)!=0) {
        REXLOG_ERROR("Ink shader test needs the generated original-source ink FXLO: {}",effect_path.string());return false;
    }
    constexpr unsigned side=4,cases=6,program_count=4,blend_count=2;
    constexpr unsigned draws=cases*program_count*blend_count;
    constexpr uint64_t image=1024,stride=4096,constants_offset=8192;
    const uint64_t upload_size=constants_offset+cases*program_count*stride;
    auto f=std::make_unique<Fixture>();auto* device=provider.GetDevice();
    Validation validation;device->QueryInterface(IID_PPV_ARGS(&validation.queue));
    if(validation.queue)validation.first=validation.queue->GetNumStoredMessagesAllowedByRetrievalFilter();
    if(!f->event || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f->allocator))) ||
       FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f->allocator.Get(),nullptr,IID_PPV_ARGS(&f->list))) ||
       FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f->fence))))return false;
    auto library=LoadLibraryW(L"d3dcompiler_47.dll");if(!library)return false;
    struct ReleaseLibrary {HMODULE value;~ReleaseLibrary(){FreeLibrary(value);}} release{library};
    auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(library,"D3DCompile"));if(!compile)return false;
    // The actual native PS consumes parameter0 UV.xy/UV.zw and parameter1
    // view-space normal.xyz + authored vertex alpha.w. The normal varies while
    // environment RGB is uniform, isolating source color/alpha from coordinates.
    constexpr char vs_source[]=R"(
cbuffer TestInput:register(b4){float4 normal_alpha;}
struct V{float4 t[16]:TEXCOORD0;float4 p:SV_Position;};
V main(uint id:SV_VertexID){V o;float2 uv=float2((id<<1)&2,id&2);
o.p=float4(uv*float2(2,-2)+float2(-1,1),0,1);
[unroll]for(uint i=0;i<16;++i)o.t[i]=0;
o.t[0]=float4(uv*2.25-float2(1.125,.625),uv.yx*float2(-1.75,3.25)+float2(.375,-.875));
o.t[1]=normal_alpha;o.t[2]=o.p;o.t[3]=o.p;return o;})";
    ComPtr<ID3DBlob> vertex,errors,serialized;
    if(FAILED(compile(vs_source,sizeof(vs_source)-1,"original-ink-interface",nullptr,nullptr,
                      "main","vs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vertex,&errors)))return false;
    std::array<std::unique_ptr<DxbcShader>,program_count> shaders;
    constexpr unsigned bindings=8;
    D3D12_ROOT_PARAMETER params[7]{};
    for(unsigned c=0;c<4;++c){params[c].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[c].Descriptor.ShaderRegister=c;}
    const D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,bindings,1,0,0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,4,0,0,0}};
    for(unsigned c=0;c<2;++c){params[4+c].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[4+c].DescriptorTable={1,&ranges[c]};}
    params[6].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[6].Constants={4,0,4};
    params[6].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;
    const D3D12_ROOT_SIGNATURE_DESC root{7,params};
    if(FAILED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors)) ||
       FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&f->root))))return false;
    uint64_t program_fingerprint=14695981039346656037ull;
    for(unsigned variant=0;variant<program_count;++variant) {
        const unsigned offsets[]={10400,11280,12436,13808};
        std::vector<uint32_t> words(variant>=2?108:54);const unsigned at=offsets[variant];
        for(unsigned i=0;i<words.size();++i) {
            words[i]=Be(effect.data()+at+4*i);
            for(unsigned b=0;b<4;++b)program_fingerprint=(program_fingerprint^effect[at+4*i+b])*1099511628211ull;
        }
        const bool depth=variant>=2;
        if(words[(depth?26:9)*3+1]!=0x1F1FF688 || words[(depth?34:16)*3]!=0xC90F8000) {
            REXLOG_ERROR("Ink candidate does not contain the audited RGBA footprint and final export");return false;
        }
        shaders[variant]=std::make_unique<DxbcShader>(xenos::ShaderType::kPixel,0x494E4B00+variant,
            words.data(),words.size(),std::endian::native);
        rex::string::StringBuffer text;shaders[variant]->AnalyzeUcode(text);
        DxbcShaderTranslator translator(provider.GetAdapterVendorID(),false,false,false,true,1,1);
        auto* translated=shaders[variant]->GetOrCreateTranslation(translator.GetDefaultPixelShaderModification(0));
        if(!translator.TranslateAnalyzedShader(*translated) || !translated->is_valid())return false;
        std::ofstream disassembly(output/("ink-"+std::to_string(variant)+".ucode.txt"));
        disassembly<<shaders[variant]->ucode_disassembly();if(!disassembly.good())return false;
        for(unsigned blend=0;blend<blend_count;++blend) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=f->root.Get();
            p.VS={vertex->GetBufferPointer(),vertex->GetBufferSize()};
            p.PS={translated->translated_binary().data(),translated->translated_binary().size()};
            auto& rt=p.BlendState.RenderTarget[0];rt.RenderTargetWriteMask=15;rt.BlendEnable=blend!=0;
            // The authored GX blend factors apply to all four channels. Test
            // source-alpha/inverse-source-alpha, including destination alpha.
            rt.SrcBlend=rt.SrcBlendAlpha=D3D12_BLEND_SRC_ALPHA;
            rt.DestBlend=rt.DestBlendAlpha=D3D12_BLEND_INV_SRC_ALPHA;
            rt.BlendOp=rt.BlendOpAlpha=D3D12_BLEND_OP_ADD;
            p.SampleMask=UINT_MAX;p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
            p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.RasterizerState.DepthClipEnable=TRUE;
            p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            p.NumRenderTargets=1;p.RTVFormats[0]=DXGI_FORMAT_R32G32B32A32_FLOAT;p.SampleDesc.Count=1;
            if(FAILED(device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&f->pipelines[variant*blend_count+blend]))))return false;
        }
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,bindings*program_count,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    if(FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&f->views))))return false;
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;hd.NumDescriptors=4*program_count;
    if(FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&f->samplers))))return false;
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if(FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&f->rtvs))))return false;
    D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width=texture.Height=side;texture.DepthOrArraySize=texture.MipLevels=texture.SampleDesc.Count=1;
    texture.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    for(unsigned i=0;i<4;++i)if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,
        D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&f->textures[i]))))return false;
    texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,
        D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&f->target))))return false;
    device->CreateRenderTargetView(f->target.Get(),nullptr,f->rtvs->GetCPUDescriptorHandleForHeapStart());
    if(!Buffer(device,upload_size,D3D12_HEAP_TYPE_UPLOAD,&f->upload) ||
       !Buffer(device,draws*image,D3D12_HEAP_TYPE_READBACK,&f->readback))return false;
    uint8_t* mapped=nullptr;D3D12_RANGE none{};
    if(FAILED(f->upload->Map(0,&none,reinterpret_cast<void**>(&mapped))))return false;
    std::memset(mapped,0,upload_size);
    auto& system=*reinterpret_cast<DxbcShaderTranslator::SystemConstants*>(mapped);
    system.flags=(1u<<DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift)|
        (1u<<DxbcShaderTranslator::kSysFlag_AlphaPassIfEqual_Shift)|
        (1u<<DxbcShaderTranslator::kSysFlag_AlphaPassIfGreater_Shift);
    for(auto& bias:system.color_exp_bias)bias=1;
    static_assert(sizeof(DxbcShaderTranslator::SystemConstants)<=1024);
    auto* fetch=reinterpret_cast<uint32_t*>(mapped+1024);
    for(unsigned i=0;i<4;++i)fetch[i*6+2]=(side-1)|((side-1)<<13);
    auto texel=[](unsigned t,unsigned x,unsigned y)->Float4 {
        if(t==0)return {.87f,.43f,.19f,(x+y)/6.f}; // Env alpha must not affect opacity.
        const float alpha[]={0,127.f/255.f,128.f/255.f,1};
        if(t==1)return {.11f+.1f*x,.77f-.1f*y,.63f,alpha[(x+2*y)%4]};
        if(t==2)return {.97f,.04f,.82f,alpha[(3*x+y)%4]}; // Unused mask2 poison.
        return {.5f,.5f,.5f,.5f}; // Native depth prefix, unused by final Wii output.
    };
    const Float4 tints[cases]={{32.f/255,48.f/255,60.f/255,0},{32.f/255,48.f/255,60.f/255,0},
        {.8f,.2f,.9f,0},{1.4f,.4f,6.f,1},{0,0,0,1},{32.f/255,48.f/255,60.f/255,0}};
    const Float4 normals[cases]={{0,0,1,0},{.3f,.2f,.8f,.125f},{-.6f,.1f,.7f,.5f},
        {.5f,-.6f,.2f,.8f},{0,0,2,1},{0,0,1,1}};
    for(unsigned t=0;t<4;++t)for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x) {
        const auto value=texel(t,x,y);std::memcpy(mapped+3072+t*image+y*256+x*16,value.data(),16);
    }
    for(unsigned test=0;test<cases;++test)for(unsigned variant=0;variant<program_count;++variant) {
        auto* constants=mapped+constants_offset+(test*program_count+variant)*stride;
        const auto tint_index=shaders[variant]->constant_register_map().GetPackedFloatConstantIndex(variant>=2?6:0);
        const auto literal_index=shaders[variant]->constant_register_map().GetPackedFloatConstantIndex(255);
        if(tint_index>=256 || literal_index>=256){f->upload->Unmap(0,nullptr);return false;}
        const unsigned code_offsets[]={10400,11280,12436,13808};
        Float4 literals{};
        for(unsigned lane=0;lane<4;++lane)
            literals[lane]=std::bit_cast<float>(Be(effect.data()+code_offsets[variant]-16+4*lane));
        std::memcpy(constants+tint_index*16,tints[test].data(),16);
        std::memcpy(constants+literal_index*16,literals.data(),16);
        if(variant>=2) {
            const Float4 dimensions{float(side),float(side),1.f/side,1.f/side};
            for(unsigned reg=0;reg<6;++reg) {
                const auto index=shaders[variant]->constant_register_map().GetPackedFloatConstantIndex(reg);
                if(index>=256)continue;
                Float4 value{};
                if(reg<2)value=dimensions;else value[reg-2]=1;
                std::memcpy(constants+index*16,value.data(),16);
            }
        }
    }
    f->upload->Unmap(0,nullptr);
    for(unsigned variant=0;variant<program_count;++variant) {
        const auto increment=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto srv=f->views->GetCPUDescriptorHandleForHeapStart();srv.ptr+=variant*bindings*increment;
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=texture.Format;
        d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2DArray.MipLevels=d.Texture2DArray.ArraySize=1;
        for(unsigned i=0;i<bindings;++i){auto handle=srv;handle.ptr+=i*increment;device->CreateShaderResourceView(nullptr,&d,handle);}
        if(shaders[variant]->GetTextureBindingsAfterTranslation().size()>bindings)return false;
        for(const auto& binding:shaders[variant]->GetTextureBindingsAfterTranslation()) {
            if(binding.fetch_constant>(variant>=2?3u:2u))return false;
            const unsigned depth_mapping[]={3,0,1,2};
            const auto texture_index=variant>=2?depth_mapping[binding.fetch_constant]:binding.fetch_constant;
            device->CreateShaderResourceView(f->textures[texture_index].Get(),&d,srv);srv.ptr+=increment;
        }
        auto sampler=f->samplers->GetCPUDescriptorHandleForHeapStart();
        sampler.ptr+=variant*4*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        for(unsigned i=0;i<4;++i) {
            D3D12_SAMPLER_DESC d{};d.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;
            d.AddressU=d.AddressV=d.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            d.MaxLOD=FLT_MAX;d.MaxAnisotropy=1;d.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
            device->CreateSampler(&d,sampler);sampler.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        }
    }
    for(unsigned i=0;i<4;++i) {
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=f->upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint.Offset=3072+i*image;from.PlacedFootprint.Footprint={texture.Format,side,side,1,256};
        to.pResource=f->textures[i].Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        Barrier(f->list.Get(),f->textures[i].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    f->list->SetGraphicsRootSignature(f->root.Get());ID3D12DescriptorHeap* heaps[]={f->views.Get(),f->samplers.Get()};
    f->list->SetDescriptorHeaps(2,heaps);
    f->list->SetGraphicsRootConstantBufferView(0,f->upload->GetGPUVirtualAddress());
    f->list->SetGraphicsRootConstantBufferView(2,f->upload->GetGPUVirtualAddress()+2048);
    f->list->SetGraphicsRootConstantBufferView(3,f->upload->GetGPUVirtualAddress()+1024);
    const D3D12_VIEWPORT viewport{0,0,side,side,0,1};const D3D12_RECT scissor{0,0,side,side};
    f->list->RSSetViewports(1,&viewport);f->list->RSSetScissorRects(1,&scissor);
    f->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const auto rtv=f->rtvs->GetCPUDescriptorHandleForHeapStart();f->list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
    const Float4 background{.27f,.36f,.54f,.6f};
    for(unsigned test=0;test<cases;++test)for(unsigned variant=0;variant<program_count;++variant)for(unsigned blend=0;blend<blend_count;++blend) {
        const unsigned draw=(test*program_count+variant)*blend_count+blend;
        auto srv=f->views->GetGPUDescriptorHandleForHeapStart();srv.ptr+=variant*bindings*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto sampler=f->samplers->GetGPUDescriptorHandleForHeapStart();sampler.ptr+=variant*4*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        f->list->SetGraphicsRootDescriptorTable(4,srv);f->list->SetGraphicsRootDescriptorTable(5,sampler);
        f->list->SetPipelineState(f->pipelines[variant*blend_count+blend].Get());
        f->list->ClearRenderTargetView(rtv,background.data(),0,nullptr);
        f->list->SetGraphicsRootConstantBufferView(1,f->upload->GetGPUVirtualAddress()+constants_offset+(test*program_count+variant)*stride);
        f->list->SetGraphicsRoot32BitConstants(6,4,normals[test].data(),0);f->list->DrawInstanced(3,1,0,0);
        Barrier(f->list.Get(),f->target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=f->target.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Offset=draw*image;to.PlacedFootprint.Footprint={texture.Format,side,side,1,256};
        f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        Barrier(f->list.Get(),f->target.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    if(FAILED(f->list->Close()))return false;
    ID3D12CommandList* lists[]={f->list.Get()};provider.GetDirectQueue()->ExecuteCommandLists(1,lists);
    if(FAILED(provider.GetDirectQueue()->Signal(f->fence.Get(),1)) ||
       FAILED(f->fence->SetEventOnCompletion(1,f->event)) || WaitForSingleObject(f->event,15000)!=WAIT_OBJECT_0) {
        f.release();return false;
    }
    const uint8_t* pixels=nullptr;D3D12_RANGE range{0,draws*image};
    if(FAILED(f->readback->Map(0,&range,reinterpret_cast<void**>(const_cast<uint8_t**>(&pixels)))))return false;
    unsigned invalid=0,changed=0;double max_error=0;
    for(unsigned test=0;test<cases;++test)for(unsigned variant=0;variant<program_count;++variant)for(unsigned blend=0;blend<blend_count;++blend)
    for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x) {
        const unsigned draw=(test*program_count+variant)*blend_count+blend;
        const double u=(x+.5)/side*2.25-1.125,v=(y+.5)/side*2.25-.625;
        const unsigned tx=unsigned(std::floor((u-std::floor(u))*side)),ty=unsigned(std::floor((v-std::floor(v))*side));
        const auto env=texel(0,0,0),footprint=texel(1,tx,ty);
        const double alpha=std::clamp(double(footprint[3])*normals[test][3],0.,1.);
        for(unsigned c=0;c<4;++c) {
            float actual;std::memcpy(&actual,pixels+draw*image+y*256+x*16+c*4,4);
            const double source=c==3?alpha:std::clamp(double(env[c])*tints[test][c],0.,1.);
            const double expected=blend?source*alpha+background[c]*(1-alpha):source;
            const auto delta=std::abs(double(actual)-expected);max_error=std::max(max_error,delta);
            if(std::isfinite(actual) && std::abs(actual-background[c])>1e-5)++changed;
            if(!std::isfinite(actual)||delta>2e-6) {
                if(invalid<6)REXLOG_ERROR("Ink shader mismatch test{} variant{} blend{} pixel{},{} channel{} actual{} expected{}",test,variant,blend,x,y,c,actual,expected);
                ++invalid;
            }
        }
    }
    f->readback->Unmap(0,&none);
    const unsigned device_errors=validation.Count();validation.reported=true;
    std::ofstream report(output/"ink-pixel-results.json");
    report<<"{\"draws\":"<<draws<<",\"values\":"<<draws*side*side*4<<",\"invalid\":"<<invalid
        <<",\"changed_values\":"<<changed<<",\"max_error\":"<<max_error<<",\"device_errors\":"<<device_errors
        <<",\"program_fnv64\":"<<program_fingerprint
        <<",\"scope\":\"All four actual staged ink PS variants native translation/execution, including depthBlend/SA with native c6 tint and shifted samplers. Original source tint RGB and footprint-times-vertex alpha; varying normals, source-alpha blending, negative repeat UVs, nonuniform footprint and poison unused mask. Environment RGB uniform: sphere-map equivalence, native FXLO registration, vertex matrices and live ribbon binding remain separate gates.\"}\n";
    REXLOG_INFO("Original ink GPU shader test: draws{} invalid{} max_error{} changed{} device_errors{}",draws,invalid,max_error,changed,device_errors);
    return !invalid&&!device_errors&&changed&&report.good();
}
