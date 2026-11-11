#include "scene_shader_audit.h"
#include "native_pixel_fixture.h"
#include "em1_blend_microcode.h"
#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/logging.h>
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <bit>
#include <cmath>
#include <fstream>
#include <memory>
#include <vector>
#include <cstring>
#include <span>
bool em2::TestWiiBlendPixels(rex::ui::d3d12::D3D12Provider& provider,const std::filesystem::path& output){
    using namespace em2::pixel_test;using namespace rex::graphics;
    std::filesystem::create_directories(output);
    constexpr unsigned side=4,cases=6,variants=13;constexpr uint64_t image=1024,stride=4096,constants_offset=8192;
    auto f=std::make_unique<Fixture>();auto* device=provider.GetDevice();ComPtr<ID3D12InfoQueue> validation;device->QueryInterface(IID_PPV_ARGS(&validation));const auto first=validation?validation->GetNumStoredMessagesAllowedByRetrievalFilter():0;
    if(!f->event || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f->allocator))) || FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f->allocator.Get(),nullptr,IID_PPV_ARGS(&f->list))) || FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f->fence))))return false;
    auto library=LoadLibraryW(L"d3dcompiler_47.dll");if(!library)return false;struct Library{HMODULE value;~Library(){FreeLibrary(value);}} release{library};auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(library,"D3DCompile"));if(!compile)return false;
    constexpr char source[]=R"(cbuffer TestInput:register(b4){float vertex_alpha;} struct V{float4 t[16]:TEXCOORD0;float4 p:SV_Position;};V main(uint id:SV_VertexID){V o;float2 uv=float2((id<<1)&2,id&2);o.p=float4(uv*float2(2,-2)+float2(-1,1),0,1);[unroll]for(uint i=0;i<16;++i)o.t[i]=0;o.t[0]=float4(uv*2.25-float2(1.125,.625),uv.yx*float2(-1.75,3.25)+float2(.375,-.875));o.t[2]=float4(.7,1.4,.3,vertex_alpha);o.t[1]=o.t[2];o.t[3]=o.t[2];return o;})";
    ComPtr<ID3DBlob> vertex,errors,serialized;if(FAILED(compile(source,sizeof(source)-1,"wii-blend-input",nullptr,nullptr,"main","vs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vertex,&errors)))return false;
    const std::array programs={em2::em1::BuildWiiBlendPixelProgram(false),em2::em1::BuildWiiBlendPixelProgram(true),em2::em1::BuildWiiBlendPixelProgram(false,em2::em1::WiiBlendVertexInterface::NativeSimple),em2::em1::BuildWiiBlendPixelProgram(true,em2::em1::WiiBlendVertexInterface::NativeSimple),em2::em1::BuildWiiBlendPixelProgram(false,em2::em1::WiiBlendVertexInterface::PackedDualUv),em2::em1::BuildWiiBlendPixelProgram(true,em2::em1::WiiBlendVertexInterface::PackedDualUv),em2::em1::BuildWiiBlendPixelProgram(false,em2::em1::WiiBlendVertexInterface::PackedDualSimple),em2::em1::BuildWiiBlendPixelProgram(true,em2::em1::WiiBlendVertexInterface::PackedDualSimple),em2::em1::BuildWiiCutoutPixelProgram(),em2::em1::BuildSimpleAlphaPixelProgram(),em2::em1::BuildSimplePropertyCutoutPixelProgram(true),em2::em1::BuildSimplePropertyCutoutPixelProgram(false),em2::em1::BuildWiiBlendPixelProgram(true,em2::em1::WiiBlendVertexInterface::NativeSimple,true)};
    std::array<std::unique_ptr<DxbcShader>,variants> shaders;
    unsigned bindings=0;
    for(unsigned i=0;i<variants;++i){shaders[i]=std::make_unique<DxbcShader>(xenos::ShaderType::kPixel,i+1,programs[i].data(),programs[i].size(),std::endian::native);rex::string::StringBuffer text;shaders[i]->AnalyzeUcode(text);DxbcShaderTranslator translator(provider.GetAdapterVendorID(),false,false,false,true,1,1);auto* translated=shaders[i]->GetOrCreateTranslation(translator.GetDefaultPixelShaderModification(0));if(!translator.TranslateAnalyzedShader(*translated) || !translated->is_valid())return false;
        if(i==0){bindings=6;
            D3D12_ROOT_PARAMETER params[7]{};for(unsigned c=0;c<4;++c){params[c].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[c].Descriptor.ShaderRegister=c;}
            const D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,bindings,1,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,3,0,0,0}};for(unsigned c=0;c<2;++c){params[4+c].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[4+c].DescriptorTable={1,&ranges[c]};}
            params[6].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[6].Constants={4,0,1};params[6].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;const D3D12_ROOT_SIGNATURE_DESC root{7,params};if(FAILED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors)) || FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&f->root))))return false;}
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=f->root.Get();p.VS={vertex->GetBufferPointer(),vertex->GetBufferSize()};p.PS={translated->translated_binary().data(),translated->translated_binary().size()};for(unsigned target=0;target<3;++target)p.BlendState.RenderTarget[target].RenderTargetWriteMask=15;p.SampleMask=UINT_MAX;p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.RasterizerState.DepthClipEnable=TRUE;p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.NumRenderTargets=1;for(unsigned target=0;target<p.NumRenderTargets;++target)p.RTVFormats[target]=DXGI_FORMAT_R32G32B32A32_FLOAT;p.SampleDesc.Count=1;if(FAILED(device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&f->pipelines[i]))))return false;
        const char* names[]={"blend.ucode.txt","masked.ucode.txt","simple-blend.ucode.txt","simple-masked.ucode.txt","dual-blend.ucode.txt","dual-masked.ucode.txt","dual-simple-blend.ucode.txt","dual-simple-masked.ucode.txt","cutout.ucode.txt","simple-alpha.ucode.txt","greater127.ucode.txt","gequal127.ucode.txt","masked-greater127.ucode.txt"};std::ofstream disasm(output/names[i]);disasm<<shaders[i]->ucode_disassembly();if(!disasm.good())return false;std::ofstream code(output/(std::string(names[i])+".bin"),std::ios::binary);for(auto word:programs[i]){const char bytes[]={char(word>>24),char(word>>16),char(word>>8),char(word)};code.write(bytes,4);}if(!code.good())return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,bindings*variants,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};if(FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&f->views))))return false;hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;hd.NumDescriptors=3*variants;if(FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&f->samplers))))return false;hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=3*variants;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;if(FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&f->rtvs))))return false;
    D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;texture.Width=texture.Height=side;texture.DepthOrArraySize=texture.MipLevels=texture.SampleDesc.Count=1;texture.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    for(unsigned i=1;i<=3;++i)if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&f->textures[i]))))return false;
    texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&f->target))))return false;
    device->CreateRenderTargetView(f->target.Get(),nullptr,f->rtvs->GetCPUDescriptorHandleForHeapStart());
    if(!Buffer(device,constants_offset+cases*variants*stride,D3D12_HEAP_TYPE_UPLOAD,&f->upload) || !Buffer(device,cases*(variants+2)*image,D3D12_HEAP_TYPE_READBACK,&f->readback))return false;
    uint8_t* mapped=nullptr;D3D12_RANGE none{};if(FAILED(f->upload->Map(0,&none,reinterpret_cast<void**>(&mapped))))return false;std::memset(mapped,0,constants_offset+cases*variants*stride);
    auto& system=*reinterpret_cast<DxbcShaderTranslator::SystemConstants*>(mapped);system.flags=(1u<<DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift)|(1u<<DxbcShaderTranslator::kSysFlag_AlphaPassIfEqual_Shift)|(1u<<DxbcShaderTranslator::kSysFlag_AlphaPassIfGreater_Shift);for(auto& bias:system.color_exp_bias)bias=1;
    static_assert(sizeof(DxbcShaderTranslator::SystemConstants)<=2048);
    auto cutout_system=system;cutout_system.flags&=~(1u<<DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift);
    cutout_system.alpha_test_reference=128.f/255.f;std::memcpy(mapped+6144,&cutout_system,sizeof(cutout_system));
    auto* fetch=reinterpret_cast<uint32_t*>(mapped+1024);for(unsigned i=0;i<=3;++i)fetch[i*6+2]=(side-1)|((side-1)<<13);
    auto texel=[](unsigned t,unsigned x,unsigned y)->Float4 {
        if(t==1){const float alpha[]={0,127.f/255.f,128.f/255.f,1};return {x/4.f,y/4.f,.2f,alpha[x]};}
        if(t==2)return {.1f,.2f,.3f,(x+y)/6.f};
        return {.8f-.1f*y,.1f+.1f*x,.7f,.6f};
    };
    for(unsigned t=1;t<=3;++t)for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x){const auto c=texel(t,x,y);std::memcpy(mapped+3072+(t-1)*image+y*256+x*16,c.data(),16);}
    for(unsigned test=0;test<cases;++test){
        const Float4 minimum{.8f,.2f,.4f,9.f};std::memcpy(mapped+constants_offset+(test*variants+9)*stride+shaders[9]->constant_register_map().GetPackedFloatConstantIndex(0)*16,minimum.data(),sizeof(minimum));
        for(unsigned v:{10u,11u}){
            const Float4 property_minimum{.8f,.2f,.4f,9.f};
            const float property_cutoff=127.f/255.f;
            std::memcpy(mapped+constants_offset+(test*variants+v)*stride+shaders[v]->constant_register_map().GetPackedFloatConstantIndex(2)*16,property_minimum.data(),sizeof(property_minimum));
            std::memcpy(mapped+constants_offset+(test*variants+v)*stride+shaders[v]->constant_register_map().GetPackedFloatConstantIndex(252)*16,&property_cutoff,sizeof(property_cutoff));
        }
        const float masked_cutoff=127.f/255.f;std::memcpy(mapped+constants_offset+(test*variants+12)*stride+shaders[12]->constant_register_map().GetPackedFloatConstantIndex(252)*16,&masked_cutoff,sizeof(masked_cutoff));
        const float cutoff=128.f/255.f;std::memcpy(mapped+constants_offset+(test*variants+8)*stride+shaders[8]->constant_register_map().GetPackedFloatConstantIndex(252)*16,&cutoff,sizeof(cutoff));
    }
    f->upload->Unmap(0,nullptr);
    for(unsigned variant=0;variant<variants;++variant){
        auto srv=f->views->GetCPUDescriptorHandleForHeapStart();srv.ptr+=variant*bindings*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for(const auto& binding:shaders[variant]->GetTextureBindingsAfterTranslation()){
            if(binding.fetch_constant>3 || (!binding.fetch_constant && variant!=9))return false;
            D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=texture.Format;d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Texture2DArray.MipLevels=d.Texture2DArray.ArraySize=1;device->CreateShaderResourceView(f->textures[binding.fetch_constant?binding.fetch_constant:1].Get(),&d,srv);srv.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        auto sampler=f->samplers->GetCPUDescriptorHandleForHeapStart();sampler.ptr+=variant*3*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        for(unsigned i=0;i<3;++i){D3D12_SAMPLER_DESC d{};d.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;d.AddressU=d.AddressV=d.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;d.MaxLOD=FLT_MAX;d.MaxAnisotropy=1;d.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;device->CreateSampler(&d,sampler);sampler.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);}
    }
    for(unsigned i=0;i<3;++i){D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=f->upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint.Offset=3072+i*image;from.PlacedFootprint.Footprint={texture.Format,side,side,1,256};to.pResource=f->textures[i+1].Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),f->textures[i+1].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);}
    f->list->SetGraphicsRootSignature(f->root.Get());ID3D12DescriptorHeap* heaps[]={f->views.Get(),f->samplers.Get()};f->list->SetDescriptorHeaps(2,heaps);f->list->SetGraphicsRootConstantBufferView(0,f->upload->GetGPUVirtualAddress());f->list->SetGraphicsRootConstantBufferView(2,f->upload->GetGPUVirtualAddress()+2048);f->list->SetGraphicsRootConstantBufferView(3,f->upload->GetGPUVirtualAddress()+1024);f->list->SetGraphicsRootDescriptorTable(4,f->views->GetGPUDescriptorHandleForHeapStart());f->list->SetGraphicsRootDescriptorTable(5,f->samplers->GetGPUDescriptorHandleForHeapStart());
    const D3D12_VIEWPORT viewport{0,0,side,side,0,1};const D3D12_RECT scissor{0,0,side,side};f->list->RSSetViewports(1,&viewport);f->list->RSSetScissorRects(1,&scissor);f->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);const auto rtv=f->rtvs->GetCPUDescriptorHandleForHeapStart();f->list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
    for(unsigned test=0;test<cases;++test)for(unsigned variant=0;variant<variants;++variant){
        auto srv=f->views->GetGPUDescriptorHandleForHeapStart();srv.ptr+=variant*bindings*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto sampler=f->samplers->GetGPUDescriptorHandleForHeapStart();sampler.ptr+=variant*3*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        f->list->SetGraphicsRootDescriptorTable(4,srv);f->list->SetGraphicsRootDescriptorTable(5,sampler);
        const float clear[]={-999,-999,-999,-999};f->list->ClearRenderTargetView(rtv,clear,0,nullptr);f->list->SetPipelineState(f->pipelines[variant].Get());f->list->SetGraphicsRootConstantBufferView(1,f->upload->GetGPUVirtualAddress()+constants_offset+(test*variants+variant)*stride);f->list->SetGraphicsRootConstantBufferView(0,f->upload->GetGPUVirtualAddress()+(((variant==8 || variant==9) && test%2)?6144:0));f->list->SetGraphicsRoot32BitConstant(6,std::bit_cast<uint32_t>(test/5.f),0);f->list->DrawInstanced(3,1,0,0);
        Barrier(f->list.Get(),f->target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=f->target.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=(test*variants+variant)*image;to.PlacedFootprint.Footprint={texture.Format,side,side,1,256};f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),f->target.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    if(FAILED(f->list->Close()))return false;ID3D12CommandList* lists[]={f->list.Get()};provider.GetDirectQueue()->ExecuteCommandLists(1,lists);if(FAILED(provider.GetDirectQueue()->Signal(f->fence.Get(),1)) || FAILED(f->fence->SetEventOnCompletion(1,f->event)) || WaitForSingleObject(f->event,15000)!=WAIT_OBJECT_0){f.release();return false;}
    const uint8_t* pixels=nullptr;D3D12_RANGE range{0,cases*(variants+2)*image};if(FAILED(f->readback->Map(0,&range,reinterpret_cast<void**>(const_cast<uint8_t**>(&pixels)))))return false;
    unsigned bad=0;double error=0;
    // Independent source-equation oracle, with explicit normalized repeat and
    // point sampling. Distinct textures expose swapped bindings and clamping.
    for(unsigned test=0;test<cases;++test)for(unsigned variant=0;variant<variants;++variant)for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x){
        const double u=(x+.5)/side*2.25-1.125,v=(y+.5)/side*2.25-.625;
        const unsigned tx=unsigned(std::floor((u-std::floor(u))*side)),ty=unsigned(std::floor((v-std::floor(v))*side));
        const double u1=(y+.5)/side*-1.75+.375,v1=(x+.5)/side*3.25-.875;
        const unsigned tx1=unsigned(std::floor((u1-std::floor(u1))*side)),ty1=unsigned(std::floor((v1-std::floor(v1))*side));
        const auto first=texel(1,tx,ty),second=texel(3,(variant>=4 && variant<=7)?tx1:tx,(variant>=4 && variant<=7)?ty1:ty),mask=texel(2,tx,ty);
        const double alpha=test/5.0,weight=(variant%2 || variant==12)?std::clamp(2*(alpha*mask[3]+alpha-.5),0.0,1.0):alpha;
        const double modulation[]={.7,1.4,.3};
        for(unsigned c=0;c<4;++c){float actual;std::memcpy(&actual,pixels+(test*variants+variant)*image+y*256+x*16+c*4,4);
            const double simple_alpha=first[3]*alpha;
            const bool property_rejected=variant==10?float(first[3]*float(test/5.f))<=127.f/255.f:float(first[3]*float(test/5.f))<127.f/255.f;
            const double expected=variant==12?(first[3]<=127.f/255.f?-999:c==3?double(first[3]):std::clamp((first[c]*weight+second[c]*(1-weight))*modulation[c],0.0,1.0)):variant>=10?(property_rejected?-999:c==3?simple_alpha:first[c]*std::max(modulation[c],c==0?.8:c==1?.2:.4)):variant==9?((test%2 && simple_alpha<128.f/255.f)?-999:c==3?simple_alpha:first[c]*std::max(modulation[c],c==0?.8:c==1?.2:.4)):(variant==8 && first[3]<128.f/255.f)?-999:variant==8?(c==3?double(first[3]):std::clamp(first[c]*modulation[c],0.0,1.0)):c==3?((variant%2)?double(first[3]):1.0):std::clamp((first[c]*weight+second[c]*(1-weight))*modulation[c],0.0,1.0);
            const double delta=std::abs(actual-expected);error=std::max(error,delta);if(!std::isfinite(actual)||delta>2e-6){if(bad<4)REXLOG_ERROR("Wii blend mismatch case{} variant{} pixel{},{} channel{} actual{} expected{}",test,variant,x,y,c,actual,expected);++bad;}
        }
    }
    f->readback->Unmap(0,&none);
    unsigned device_errors=0;if(validation)for(uint64_t i=first;i<validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;validation->GetMessage(i,nullptr,&size);std::vector<uint8_t> bytes(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());if(SUCCEEDED(validation->GetMessage(i,m,&size)) && m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++device_errors;REXLOG_ERROR("Material validation: {}",m->pDescription);}}
    std::ofstream report(output/"wii-blend-results.json");report<<"{\"draws\":78,\"invalid\":"<<bad<<",\"max_error\":"<<error<<",\"device_errors\":"<<device_errors<<",\"scope\":\"GPU translation and pixel output for single-UV and independently varying packed dual-UV Wii blend and masked blend, nonuniform textures, negative/repeating UVs, six vertex-alpha weights, saturation and output alpha. Includes shader discard with fixed-function alpha testing both off and on, cutout RGB/texture alpha and GEQUAL 128/255 rejection at alpha 0, 127, 128, 255. Also tests exact one-map Simple RGB/minimum-color arithmetic and texture times vertex alpha with fixed-function GEQUAL enabled/disabled. Includes one-map source GREATER127 and GEQUAL127 shader discard with fixed-function alpha disabled, preserving authored vertex alpha. Includes masked blend GREATER127 shader rejection of texture0 alpha independent of the vertex-alpha blend weight, with fixed-function alpha disabled. Does not verify native FXLO registration or live material binding.\"}\n";
    REXLOG_INFO("Wii blend pixel tests: invalid{} max error{} device errors{}",bad,error,device_errors);
    return !bad&&!device_errors&&report.good();
}
