#include "shadow_pixel_test.h"
#include "hdr_shader_export.h"
#include "../private/shadow_shader_test_data.h"
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/logging.h>
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <span>
#include <vector>

#include "native_pixel_fixture.h"
using namespace em2::pixel_test;
namespace {
constexpr unsigned side=32,cases=32,modes=6,variants=6;
constexpr uint64_t image_bytes=side*side*sizeof(Float4),draws=cases*modes*variants;

}

bool em2::TestNativeShadowPixels(rex::ui::d3d12::D3D12Provider& provider,const std::filesystem::path& output) {
    auto f=std::make_unique<Fixture>();auto* device=provider.GetDevice();auto* queue=provider.GetDirectQueue();
    ComPtr<ID3D12InfoQueue> validation;device->QueryInterface(IID_PPV_ARGS(&validation));
    const uint64_t first_message=validation?validation->GetNumStoredMessagesAllowedByRetrievalFilter():0;
    if(!f->event || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f->allocator))) ||
       FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f->allocator.Get(),nullptr,IID_PPV_ARGS(&f->list))) ||
       FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f->fence))))return false;
    const auto compiler=LoadLibraryW(L"d3dcompiler_47.dll");if(!compiler)return false;
    struct Library {HMODULE handle;~Library(){FreeLibrary(handle);}} library{compiler};
    auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler,"D3DCompile"));if(!compile)return false;
    const char vertex_source[]=R"(
cbuffer Inputs:register(b2){float4 seeds[9];}
struct Output{float4 t[16]:TEXCOORD0;float4 position:SV_Position;};
Output main(uint id:SV_VertexID){Output o;float2 uv=float2((id<<1)&2,id&2);
o.position=float4(uv*float2(2,-2)+float2(-1,1),0.5,1);
[unroll]for(uint i=0;i<16;++i)o.t[i]=float4(uv,0,1)+seeds[i%9];return o;})";
    ComPtr<ID3DBlob> vs,errors;
    if(FAILED(compile(vertex_source,sizeof(vertex_source)-1,"shadow-fixture",nullptr,nullptr,"main","vs_5_1",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&errors)))return false;
    std::array<D3D12_ROOT_PARAMETER,6> parameters{};
    for(unsigned i=0;i<4;++i){parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;parameters[i].Descriptor.ShaderRegister=i;parameters[i].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;}
    const D3D12_DESCRIPTOR_RANGE ranges[2]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,12,1,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,6,0,0,0}};
    for(unsigned i=0;i<2;++i){parameters[4+i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[4+i].DescriptorTable={1,&ranges[i]};parameters[4+i].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;}
    D3D12_ROOT_SIGNATURE_DESC root{unsigned(parameters.size()),parameters.data(),0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> serialized;
    if(FAILED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors)) ||
       FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&f->root))))return false;
    std::array<std::span<const uint32_t>,variants> programs={shadow_test_data::original,shadow_test_data::control,shadow_test_data::samples8,shadow_test_data::samples16,shadow_test_data::adaptive16,shadow_test_data::radius_probe};
    std::array<std::unique_ptr<rex::graphics::DxbcShader>,variants> shaders;
    for(unsigned variant=0;variant<variants;++variant) {
        auto& shader=shaders[variant];shader=std::make_unique<rex::graphics::DxbcShader>(rex::graphics::xenos::ShaderType::kPixel,
            variant+1,programs[variant].data(),programs[variant].size(),std::endian::native);
        rex::string::StringBuffer disassembly;shader->AnalyzeUcode(disassembly);
        rex::graphics::DxbcShaderTranslator translator(provider.GetAdapterVendorID(),false,false,false,true,1,1);
        auto* translation=shader->GetOrCreateTranslation(translator.GetDefaultPixelShaderModification(0));
        if(!translator.TranslateAnalyzedShader(*translation) || !translation->is_valid())return false;
        const auto& ps=translation->translated_binary();
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=f->root.Get();
        pipeline.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pipeline.PS={ps.data(),ps.size()};
        pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        auto& blend=pipeline.BlendState.RenderTarget[0];blend.SrcBlend=blend.SrcBlendAlpha=D3D12_BLEND_ONE;
        blend.DestBlend=blend.DestBlendAlpha=D3D12_BLEND_ZERO;blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;
        pipeline.SampleMask=UINT_MAX;pipeline.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pipeline.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        pipeline.RasterizerState.DepthClipEnable=TRUE;pipeline.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        auto& stencil=pipeline.DepthStencilState.FrontFace;stencil.StencilFailOp=stencil.StencilDepthFailOp=stencil.StencilPassOp=D3D12_STENCIL_OP_KEEP;
        stencil.StencilFunc=D3D12_COMPARISON_FUNC_ALWAYS;pipeline.DepthStencilState.BackFace=stencil;
        pipeline.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pipeline.NumRenderTargets=1;
        pipeline.RTVFormats[0]=DXGI_FORMAT_R32G32B32A32_FLOAT;pipeline.SampleDesc.Count=1;
        const auto hr=device->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&f->pipelines[variant]));
        // Validate the additional high-range export on actual original/patched
        // EM2 receiver programs as well as the separately executed native ALU
        // fixture. This check proves device acceptance, not live HDR lighting.
        const auto hdr=AddHdrSceneExport(ps);if(hdr.empty())return false;
        auto hdr_pipeline=pipeline;hdr_pipeline.PS={hdr.data(),hdr.size()};hdr_pipeline.NumRenderTargets=5;
        hdr_pipeline.RTVFormats[4]=DXGI_FORMAT_R16G16B16A16_FLOAT;
        ComPtr<ID3D12PipelineState> hdr_state;
        if(FAILED(device->CreateGraphicsPipelineState(&hdr_pipeline,IID_PPV_ARGS(&hdr_state))))return false;
        if(FAILED(hr)){
            REXLOG_ERROR("Shadow pixel pipeline {} failed {:08X}",variant,uint32_t(hr));
            ComPtr<ID3D12InfoQueue> messages;
            if(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&messages)))) {
                for(uint64_t i=0;i<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
                    SIZE_T size=0;messages->GetMessage(i,nullptr,&size);std::vector<uint8_t> storage(size);
                    auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                    if(SUCCEEDED(messages->GetMessage(i,message,&size)))REXLOG_ERROR("Shadow pixel device: {}",message->pDescription);
                }
            }
            return false;
        }
    }
    REXLOG_INFO("HDR additional color export: all{} actual EM2 receiver shader variants accepted by D3D12 with FP16 target4; pipeline validation only, no live HDR claim",variants);
    D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.NumDescriptors=12*modes;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if(FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&f->views))))return false;
    heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;heap.NumDescriptors=6;
    if(FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&f->samplers))))return false;
    heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap.NumDescriptors=1;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if(FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&f->rtvs))))return false;
    D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;texture.Width=side;texture.Height=side;
    texture.DepthOrArraySize=texture.MipLevels=texture.SampleDesc.Count=1;texture.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
    D3D12_HEAP_PROPERTIES gpu{};gpu.Type=D3D12_HEAP_TYPE_DEFAULT;gpu.CreationNodeMask=gpu.VisibleNodeMask=1;
    for(auto& item:f->textures)if(FAILED(device->CreateCommittedResource(&gpu,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&item))))return false;
    texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if(FAILED(device->CreateCommittedResource(&gpu,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&f->target))))return false;
    device->CreateRenderTargetView(f->target.Get(),nullptr,f->rtvs->GetCPUDescriptorHandleForHeapStart());
    constexpr uint64_t texture_upload=9*image_bytes,system_offset=texture_upload,fetch_offset=system_offset+1024;
    constexpr uint64_t cases_offset=fetch_offset+1024,case_stride=4096;
    if(!Buffer(device,cases_offset+cases*modes*case_stride,D3D12_HEAP_TYPE_UPLOAD,&f->upload) ||
       !Buffer(device,draws*image_bytes,D3D12_HEAP_TYPE_READBACK,&f->readback))return false;
    uint8_t* mapped=nullptr;D3D12_RANGE none{0,0};if(FAILED(f->upload->Map(0,&none,reinterpret_cast<void**>(&mapped))))return false;
    std::memset(mapped,0,cases_offset+cases*modes*case_stride);
    for(unsigned t=0;t<9;++t)for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x) {
        const float edge=t==6?side/2+(float(y%16)-8.f)*.5f:float(side/2);
        float value=t==0?.5f:t==2?1.f:t==4?.2f:t==5?.8f:(float(x)<edge?.2f:.8f);
        if(t>=7)value=x<side/2?(t==7?.5001f:.8f):.2f;
        const float angle=float((x*13+y*7)%32)*(6.28318530718f/32.f);
        const Float4 pixel=t==1?Float4{std::cos(angle),std::sin(angle),0,1}:Float4{value,value,value,1};
        std::memcpy(mapped+t*image_bytes+(y*side+x)*sizeof(pixel),pixel.data(),sizeof(pixel));
    }
    auto& system=*reinterpret_cast<rex::graphics::DxbcShaderTranslator::SystemConstants*>(mapped+system_offset);
    system.flags=(1u<<rex::graphics::DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift) |
        (1u<<rex::graphics::DxbcShaderTranslator::kSysFlag_AlphaPassIfEqual_Shift) |
        (1u<<rex::graphics::DxbcShaderTranslator::kSysFlag_AlphaPassIfGreater_Shift);
    for(auto& bias:system.color_exp_bias)bias=1.f;
    auto* fetch=reinterpret_cast<uint32_t*>(mapped+fetch_offset);
    for(unsigned i=0;i<6;++i)fetch[i*6+2]=(side-1)|((side-1)<<13);
    auto srv=f->views->GetCPUDescriptorHandleForHeapStart();const auto view_step=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto& bindings=shaders[0]->GetTextureBindingsAfterTranslation();
    if(bindings.size()!=12)return false;
    for(unsigned variant=1;variant<variants;++variant) {
        const auto& actual=shaders[variant]->GetTextureBindingsAfterTranslation();if(actual.size()!=bindings.size())return false;
        for(unsigned i=0;i<bindings.size();++i)
            if(actual[i].fetch_constant!=bindings[i].fetch_constant || actual[i].dimension!=bindings[i].dimension || actual[i].is_signed!=bindings[i].is_signed)return false;
        for(unsigned i=0;i<256;++i)
            if(shaders[variant]->constant_register_map().GetPackedFloatConstantIndex(i)!=shaders[0]->constant_register_map().GetPackedFloatConstantIndex(i))return false;
    }
    for(unsigned mode=0;mode<modes;++mode)for(const auto& binding:bindings) {
        const unsigned source=binding.fetch_constant<3?binding.fetch_constant:mode==0?3:mode==1?6:mode==2?4:mode==3?5:mode+3;
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;desc.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        desc.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;desc.Texture2DArray.MipLevels=desc.Texture2DArray.ArraySize=1;
        device->CreateShaderResourceView(f->textures[source].Get(),&desc,srv);srv.ptr+=view_step;
    }
    auto sampler=f->samplers->GetCPUDescriptorHandleForHeapStart();const auto sampler_step=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    for(unsigned i=0;i<6;++i){D3D12_SAMPLER_DESC desc{};desc.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;
        desc.AddressU=desc.AddressV=desc.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;desc.MaxLOD=D3D12_FLOAT32_MAX;
        desc.MaxAnisotropy=1;
        desc.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;device->CreateSampler(&desc,sampler);sampler.ptr+=sampler_step;}
    for(unsigned t=0;t<9;++t) {
        D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=f->upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint.Offset=t*image_bytes;from.PlacedFootprint.Footprint={DXGI_FORMAT_R32G32B32A32_FLOAT,side,side,1,side*sizeof(Float4)};
        D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=f->textures[t].Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),f->textures[t].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    f->list->SetGraphicsRootSignature(f->root.Get());ID3D12DescriptorHeap* heaps[]={f->views.Get(),f->samplers.Get()};f->list->SetDescriptorHeaps(2,heaps);
    f->list->SetGraphicsRootConstantBufferView(0,f->upload->GetGPUVirtualAddress()+system_offset);
    f->list->SetGraphicsRootConstantBufferView(3,f->upload->GetGPUVirtualAddress()+fetch_offset);
    f->list->SetGraphicsRootDescriptorTable(5,f->samplers->GetGPUDescriptorHandleForHeapStart());
    const D3D12_VIEWPORT viewport{0,0,float(side),float(side),0,1};const D3D12_RECT scissor{0,0,side,side};
    f->list->RSSetViewports(1,&viewport);f->list->RSSetScissorRects(1,&scissor);f->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const auto rtv=f->rtvs->GetCPUDescriptorHandleForHeapStart();f->list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
    uint32_t random=0x71839521;auto next=[&]{random^=random<<13;random^=random>>17;random^=random<<5;return (float(random&65535)/65535.f-.5f)*.5f;};
    for(unsigned mode=0;mode<modes;++mode)for(unsigned test=0;test<cases;++test) {
        random=0x71839521u^(test*0x9E3779B9u);
        std::array<Float4,256> constants{};
        for(unsigned i=0;i<24;++i)for(auto& value:constants[i])value=next();
        constants[0]={1,0,1,0};constants[1]={0,0,1,1};
        for(unsigned light=0;light<3;++light) {
            // The native receiver stores Y, X, depth, W rows, not XYZW.
            constants[6+4*light]={0,0,0,(float(test/8)-1.5f)/32.f+light*.015f};
            constants[7+4*light]={0,0,0,.33f+(float(test%8)-3.5f)/64.f+light*.01f};
            constants[8+4*light]={0,0,0,.5f};
            constants[9+4*light]={0,0,0,1};constants[18+light]={0,0,0,0};
        }
        constants[21]={.2f,0,0,0};constants[22]={0,.3f,0,0};constants[23]={0,0,.4f,0};
        for(unsigned i=0;i<32;++i)constants[248+i/4][i%4]=std::bit_cast<float>(shadow_test_data::literal_bits[i]);
        if(mode>=4) {
            for(unsigned lane:{0u,1u})constants[249][lane]=.5f+(constants[249][lane]-.5f)*4;
            for(unsigned lane:{2u,3u})constants[250][lane]=.5f+(constants[250][lane]-.5f)*4;
            for(unsigned i=252;i<256;++i)for(auto& value:constants[i])value*=4;
        }
        auto* block=mapped+cases_offset+(mode*cases+test)*case_stride;
        const auto& register_map=shaders[0]->constant_register_map();
        for(unsigned i=0;i<256;++i){const auto packed=register_map.GetPackedFloatConstantIndex(i);if(packed!=UINT32_MAX)std::memcpy(block+packed*16,constants[i].data(),16);}
        std::array<Float4,9> seeds{};for(auto& seed:seeds)for(auto& value:seed)value=next();seeds[0][3]=0;
        std::memcpy(block+2048,seeds.data(),sizeof(seeds));
        f->list->SetGraphicsRootConstantBufferView(1,f->upload->GetGPUVirtualAddress()+cases_offset+(mode*cases+test)*case_stride);
        f->list->SetGraphicsRootConstantBufferView(2,f->upload->GetGPUVirtualAddress()+cases_offset+(mode*cases+test)*case_stride+2048);
        auto table=f->views->GetGPUDescriptorHandleForHeapStart();table.ptr+=uint64_t(mode)*12*view_step;f->list->SetGraphicsRootDescriptorTable(4,table);
        for(unsigned variant=0;variant<variants;++variant) {
            const float clear[4]={-17,-17,-17,-17};f->list->ClearRenderTargetView(rtv,clear,0,nullptr);
            f->list->SetPipelineState(f->pipelines[variant].Get());f->list->DrawInstanced(3,1,0,0);
            Barrier(f->list.Get(),f->target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=f->target.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.PlacedFootprint.Offset=((mode*cases+test)*variants+variant)*image_bytes;
            to.PlacedFootprint.Footprint={DXGI_FORMAT_R32G32B32A32_FLOAT,side,side,1,side*sizeof(Float4)};
            f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),f->target.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
    }
    if(FAILED(f->list->Close()))return false;ID3D12CommandList* lists[]={f->list.Get()};queue->ExecuteCommandLists(1,lists);
    if(FAILED(queue->Signal(f->fence.Get(),1)) || FAILED(f->fence->SetEventOnCompletion(1,f->event)) || WaitForSingleObject(f->event,15000)!=WAIT_OBJECT_0) {
        f.release();REXLOG_ERROR("Shadow pixel diagnostic fence failed; pending GPU resources retained until process exit");return false;
    }
    const Float4* pixels=nullptr;D3D12_RANGE range{0,draws*image_bytes};if(FAILED(f->readback->Map(0,&range,reinterpret_cast<void**>(const_cast<Float4**>(&pixels)))))return false;
    unsigned nonfinite=0,unwritten=0,changed8=0,changed16=0,control_bits=0,spatial_images=0;double max_control=0,max_flat=0,flat_reference=0;std::array<unsigned,3> changed_channels{},changed_channels8{};
    for(unsigned item=0;item<2*cases;++item) {
        const auto* original=pixels+(item*variants)*side*side;bool varying=false;
        for(unsigned pixel=1;pixel<side*side;++pixel)varying|=std::memcmp(original,original+pixel,sizeof(Float4))!=0;
        spatial_images+=varying;
    }
    for(unsigned item=0;item<cases*modes;++item)for(unsigned pixel=0;pixel<side*side;++pixel) {
        const auto& original=pixels[(item*variants)*side*side+pixel];
        for(unsigned variant=0;variant<variants;++variant)for(unsigned channel=0;channel<4;++channel) {
            const float value=pixels[(item*variants+variant)*side*side+pixel][channel];
            if(!std::isfinite(value))++nonfinite;if(value==-17)++unwritten;
            if(variant==1){max_control=std::max(max_control,double(std::abs(value-original[channel])));control_bits+=std::bit_cast<uint32_t>(value)!=std::bit_cast<uint32_t>(original[channel]);}
            if(variant<5 && (item/cases==2 || item/cases==3))max_flat=std::max(max_flat,double(std::abs(value-original[channel])));
            if(variant<5 && (item/cases==2 || item/cases==3)) {
                const float expected=channel==3?1.f:item/cases==2?Float4{.2f,.3f,.4f,1.f}[channel]:0.f;
                flat_reference=std::max(flat_reference,double(std::abs(value-expected)));
            }
            if(variant==2 && std::abs(value-original[channel])>1e-6f){++changed8;if(channel<3)++changed_channels8[channel];}
            if(variant==3 && std::abs(value-original[channel])>1e-6f){++changed16;if(channel<3)++changed_channels[channel];}
        }
    }
    unsigned depth_parity=0,radius_errors=0;std::array<unsigned,3> adaptive_changes{},partial_near{},partial_far{};
    const auto pixel_at=[&](unsigned mode,unsigned test,unsigned variant,unsigned pixel)->const Float4& {
        return pixels[(((mode*cases+test)*variants+variant)*side*side)+pixel];
    };
    for(unsigned test=0;test<cases;++test)for(unsigned pixel=0;pixel<side*side;++pixel)for(unsigned channel=0;channel<3;++channel) {
        for(unsigned variant=0;variant<4;++variant)
            depth_parity+=std::bit_cast<uint32_t>(pixel_at(4,test,variant,pixel)[channel])!=std::bit_cast<uint32_t>(pixel_at(5,test,variant,pixel)[channel]);
        const float close_value=pixel_at(4,test,4,pixel)[channel],distant_value=pixel_at(5,test,4,pixel)[channel];
        const float gain=Float4{.2f,.3f,.4f,1}[channel];
        adaptive_changes[channel]+=std::abs(close_value-distant_value)>1e-6f;
        partial_near[channel]+=close_value>gain*.01f && close_value<gain*.99f;
        partial_far[channel]+=distant_value>gain*.01f && distant_value<gain*.99f;
        const float blocker=1.f-.5001f,expected_near=.25f+4*(.5f-blocker)/blocker;
        for(unsigned mode=2;mode<6;++mode) {
            const float value=pixel_at(mode,test,5,pixel)[channel];
            const float target=mode==2?.25f:mode==3?4.f:mode==4?expected_near:4.f;
            radius_errors+=std::abs(value-target)>2e-5f && !(mode>=4 && std::abs(value-.25f)<=2e-5f);
        }
    }
    REXLOG_INFO("Adaptive shadow GPU depth checks: baseline parity bits {}, radius errors {}, changed RGB {}/{}/{}, partial near {}/{}/{}, far {}/{}/{}",depth_parity,radius_errors,adaptive_changes[0],adaptive_changes[1],adaptive_changes[2],partial_near[0],partial_near[1],partial_near[2],partial_far[0],partial_far[1],partial_far[2]);
    std::filesystem::create_directories(output);
    std::ofstream raw(output/"shadow-pixels.f32",std::ios::binary);raw.write(reinterpret_cast<const char*>(pixels),draws*image_bytes);raw.close();
    REXLOG_INFO("Native shadow pixel raw output: {}",(output/"shadow-pixels.f32").string());
    f->readback->Unmap(0,&none);
    unsigned device_errors=0;
    if(validation)for(uint64_t i=first_message;i<validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
        SIZE_T size=0;validation->GetMessage(i,nullptr,&size);std::vector<uint8_t> storage(size);
        auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if(SUCCEEDED(validation->GetMessage(i,message,&size)) && message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) {
            ++device_errors;REXLOG_ERROR("Shadow pixel validation: {}",message->pDescription);
        }
    }
    REXLOG_INFO("Native shadow pixel probe: {} draws, control max error {}, control bit differences {}, flat max error {}, flat reference error {}, nonfinite {}, unwritten {}, device errors {}, changed 8/16 {}/{}, changed 8 RGB {}/{}/{}, changed 16 RGB {}/{}/{}",draws,max_control,control_bits,max_flat,flat_reference,nonfinite,unwritten,device_errors,changed8,changed16,changed_channels8[0],changed_channels8[1],changed_channels8[2],changed_channels[0],changed_channels[1],changed_channels[2]);
    REXLOG_INFO("Native shadow pixel rotation fixture: 32 input angles, {} original edge images vary spatially",spatial_images);
    std::ofstream metrics(output/"shadow-pixels.json");
    metrics<<"{\n  \"draws\": "<<draws<<",\n  \"pixels_per_draw\": "<<side*side<<",\n  \"control_max_error\": "<<max_control
        <<",\n  \"control_bit_differences\": "<<control_bits<<",\n  \"flat_max_error\": "<<max_flat<<",\n  \"flat_reference_error\": "<<flat_reference
        <<",\n  \"nonfinite\": "<<nonfinite<<",\n  \"unwritten\": "<<unwritten<<",\n  \"device_errors\": "<<device_errors
        <<",\n  \"debug_validation_available\": "<<(validation?"true":"false")<<",\n  \"changed_8\": "<<changed8<<",\n  \"changed_16\": "<<changed16<<",\n  \"spatial_original_edge_images\": "<<spatial_images<<"\n}\n";
    metrics.close();
    return raw.good() && metrics.good() && spatial_images && !nonfinite && !unwritten && !device_errors && !control_bits && max_control<=1e-6 && max_flat<=1e-6 && flat_reference<=1e-6 &&
        !depth_parity && !radius_errors && adaptive_changes[0] && adaptive_changes[1] && adaptive_changes[2] &&
        partial_far[0]>partial_near[0] && partial_far[1]>partial_near[1] && partial_far[2]>partial_near[2] &&
        changed_channels8[0] && changed_channels8[1] && changed_channels8[2] && changed_channels[0] && changed_channels[1] && changed_channels[2];
}
