#include "bloom_pixel_test.h"
#include "bloom_kernel.h"
#include "../private/bloom_shader_test_data.h"
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/logging.h>
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
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
constexpr unsigned side=32,cases=4,modes=4,variants=5,source_side=8;
constexpr uint64_t image_bytes=side*side*sizeof(Float4),draws=cases*modes*8;

}

bool em2::TestNativeBloomPixels(rex::ui::d3d12::D3D12Provider& provider,const std::filesystem::path& output) {
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
    if(FAILED(compile(vertex_source,sizeof(vertex_source)-1,"bloom-fixture",nullptr,nullptr,"main","vs_5_1",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&errors)))return false;
    std::array<D3D12_ROOT_PARAMETER,6> parameters{};
    for(unsigned i=0;i<4;++i){parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;parameters[i].Descriptor.ShaderRegister=i;parameters[i].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;}
    const D3D12_DESCRIPTOR_RANGE ranges[2]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,12,1,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,6,0,0,0}};
    for(unsigned i=0;i<2;++i){parameters[4+i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[4+i].DescriptorTable={1,&ranges[i]};parameters[4+i].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;}
    D3D12_ROOT_SIGNATURE_DESC root{unsigned(parameters.size()),parameters.data(),0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> serialized;
    if(FAILED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors)) ||
       FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&f->root))))return false;
    std::array<std::span<const uint32_t>,variants> programs={bloom_test_data::horizontal_original,bloom_test_data::horizontal_linear,bloom_test_data::vertical_original,bloom_test_data::vertical_linear,DisabledBloomShader()};
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
        if(FAILED(hr)){
            REXLOG_ERROR("Bloom pixel pipeline {} failed {:08X}",variant,uint32_t(hr));
            ComPtr<ID3D12InfoQueue> messages;
            if(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&messages)))) {
                for(uint64_t i=0;i<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
                    SIZE_T size=0;messages->GetMessage(i,nullptr,&size);std::vector<uint8_t> storage(size);
                    auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                    if(SUCCEEDED(messages->GetMessage(i,message,&size)))REXLOG_ERROR("Bloom pixel device: {}",message->pDescription);
                }
            }
            return false;
        }
    }
    const auto& register_map=shaders[0]->constant_register_map();
    const auto packed=register_map.GetPackedFloatConstantIndex(255);
    if(packed==UINT32_MAX)return false;
    for(unsigned i=0;i<4;++i)if(shaders[i]->GetTextureBindingsAfterTranslation().size()!=2 ||
        shaders[i]->constant_register_map().GetPackedFloatConstantIndex(255)!=packed)return false;
    if(!shaders[4]->GetTextureBindingsAfterTranslation().empty() || shaders[4]->writes_color_targets()!=1)return false;
    D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors=12*modes;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if(FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&f->views))))return false;
    heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;heap.NumDescriptors=12;
    if(FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&f->samplers))))return false;
    heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap.NumDescriptors=1;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if(FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&f->rtvs))))return false;
    D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width=texture.Height=source_side;texture.DepthOrArraySize=texture.MipLevels=texture.SampleDesc.Count=1;
    texture.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
    D3D12_HEAP_PROPERTIES gpu{};gpu.Type=D3D12_HEAP_TYPE_DEFAULT;gpu.CreationNodeMask=gpu.VisibleNodeMask=1;
    for(unsigned mode=0;mode<modes;++mode)if(FAILED(device->CreateCommittedResource(&gpu,D3D12_HEAP_FLAG_NONE,&texture,
        D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&f->textures[mode]))))return false;
    texture.Width=texture.Height=side;texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if(FAILED(device->CreateCommittedResource(&gpu,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_RENDER_TARGET,
        nullptr,IID_PPV_ARGS(&f->target))))return false;
    device->CreateRenderTargetView(f->target.Get(),nullptr,f->rtvs->GetCPUDescriptorHandleForHeapStart());
    constexpr uint64_t source_row=256,source_bytes=source_row*source_side;
    constexpr uint64_t system_offset=modes*source_bytes,fetch_offset=system_offset+1024;
    constexpr uint64_t cases_offset=fetch_offset+1024,case_stride=4096,upload_bytes=cases_offset+modes*cases*2*case_stride;
    if(!Buffer(device,upload_bytes,D3D12_HEAP_TYPE_UPLOAD,&f->upload) ||
        !Buffer(device,draws*image_bytes,D3D12_HEAP_TYPE_READBACK,&f->readback))return false;
    uint8_t* mapped=nullptr;D3D12_RANGE none{0,0};
    if(FAILED(f->upload->Map(0,&none,reinterpret_cast<void**>(&mapped))))return false;
    std::memset(mapped,0,upload_bytes);
    std::array<std::array<Float4,source_side*source_side>,modes> source{};
    for(unsigned mode=0;mode<modes;++mode)for(unsigned y=0;y<source_side;++y)for(unsigned x=0;x<source_side;++x) {
        Float4 value;
        if(mode==1)value={.2f,.4f,.6f,.8f};
        else {
            const float scale=mode==3?8.f:1.f;
            const float edge=mode==2?float(x+y>=source_side):float(x>=source_side/2);
            value={scale*float(x==source_side/2 && y==source_side/2),scale*edge,
                scale*float(x+2*y)/float(3*(source_side-1)),float((x+y)&1)};
        }
        source[mode][y*source_side+x]=value;
        std::memcpy(mapped+mode*source_bytes+y*source_row+x*sizeof(Float4),value.data(),sizeof(value));
    }
    auto& system=*reinterpret_cast<rex::graphics::DxbcShaderTranslator::SystemConstants*>(mapped+system_offset);
    system.flags=(1u<<rex::graphics::DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift)|
        (1u<<rex::graphics::DxbcShaderTranslator::kSysFlag_AlphaPassIfEqual_Shift)|
        (1u<<rex::graphics::DxbcShaderTranslator::kSysFlag_AlphaPassIfGreater_Shift);
    for(auto& bias:system.color_exp_bias)bias=1.f;
    auto* fetch=reinterpret_cast<uint32_t*>(mapped+fetch_offset);
    fetch[2]=(source_side-1)|((source_side-1)<<13);
    auto srv=f->views->GetCPUDescriptorHandleForHeapStart();
    const auto view_step=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for(unsigned mode=0;mode<modes;++mode)for(unsigned binding=0;binding<12;++binding) {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;desc.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Texture2DArray.MipLevels=desc.Texture2DArray.ArraySize=1;
        device->CreateShaderResourceView(f->textures[mode].Get(),&desc,srv);srv.ptr+=view_step;
    }
    auto sampler=f->samplers->GetCPUDescriptorHandleForHeapStart();
    const auto sampler_step=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    for(unsigned index=0;index<12;++index) {
        D3D12_SAMPLER_DESC desc{};desc.Filter=index<6?D3D12_FILTER_MIN_MAG_MIP_POINT:D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        desc.AddressU=desc.AddressV=desc.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        desc.MaxAnisotropy=1;desc.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;desc.MaxLOD=D3D12_FLOAT32_MAX;
        device->CreateSampler(&desc,sampler);sampler.ptr+=sampler_step;
    }
    for(unsigned mode=0;mode<modes;++mode) {
        D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=f->upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint.Offset=mode*source_bytes;
        from.PlacedFootprint.Footprint={DXGI_FORMAT_R32G32B32A32_FLOAT,source_side,source_side,1,source_row};
        D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=f->textures[mode].Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        Barrier(f->list.Get(),f->textures[mode].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    ID3D12DescriptorHeap* heaps[]={f->views.Get(),f->samplers.Get()};f->list->SetDescriptorHeaps(2,heaps);
    f->list->SetGraphicsRootSignature(f->root.Get());
    f->list->SetGraphicsRootConstantBufferView(0,f->upload->GetGPUVirtualAddress()+system_offset);
    f->list->SetGraphicsRootConstantBufferView(3,f->upload->GetGPUVirtualAddress()+fetch_offset);
    const auto rtv=f->rtvs->GetCPUDescriptorHandleForHeapStart();f->list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
    D3D12_VIEWPORT viewport{0,0,float(side),float(side),0,1};D3D12_RECT scissor{0,0,side,side};
    f->list->RSSetViewports(1,&viewport);f->list->RSSetScissorRects(1,&scissor);
    f->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for(unsigned mode=0;mode<modes;++mode)for(unsigned phase=0;phase<cases;++phase)for(unsigned axis=0;axis<2;++axis) {
        const auto case_index=(mode*cases+phase)*2+axis;
        auto* block=mapped+cases_offset+case_index*case_stride;
        std::memcpy(block+packed*16,bloom_test_data::literal_bits.data()+12,16);
        std::array<Float4,9> seeds{};
        for(unsigned tap=0;tap<7;++tap)seeds[tap][axis]=(float(tap)-3.f+float(phase)*.25f)/float(source_side);
        std::memcpy(block+2048,seeds.data(),sizeof(seeds));
        f->list->SetGraphicsRootConstantBufferView(1,f->upload->GetGPUVirtualAddress()+cases_offset+case_index*case_stride);
        f->list->SetGraphicsRootConstantBufferView(2,f->upload->GetGPUVirtualAddress()+cases_offset+case_index*case_stride+2048);
        auto table=f->views->GetGPUDescriptorHandleForHeapStart();table.ptr+=uint64_t(mode)*12*view_step;
        f->list->SetGraphicsRootDescriptorTable(4,table);
        for(unsigned variant=0;variant<4;++variant) {
            auto samplers=f->samplers->GetGPUDescriptorHandleForHeapStart();if(variant)samplers.ptr+=6*sampler_step;
            f->list->SetGraphicsRootDescriptorTable(5,samplers);
            const float clear[4]={-17,-17,-17,-17};f->list->ClearRenderTargetView(rtv,clear,0,nullptr);
            f->list->SetPipelineState(f->pipelines[variant==3?4:axis*2+(variant==2)].Get());f->list->DrawInstanced(3,1,0,0);
            Barrier(f->list.Get(),f->target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=f->target.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.PlacedFootprint.Offset=(case_index*4+variant)*image_bytes;
            to.PlacedFootprint.Footprint={DXGI_FORMAT_R32G32B32A32_FLOAT,side,side,1,side*sizeof(Float4)};
            f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            Barrier(f->list.Get(),f->target.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
    }
    f->upload->Unmap(0,nullptr);
    if(FAILED(f->list->Close()))return false;ID3D12CommandList* lists[]={f->list.Get()};queue->ExecuteCommandLists(1,lists);
    if(FAILED(queue->Signal(f->fence.Get(),1)) || FAILED(f->fence->SetEventOnCompletion(1,f->event)) || WaitForSingleObject(f->event,15000)!=WAIT_OBJECT_0) {
        f.release();REXLOG_ERROR("Bloom pixel fence failed; pending GPU resources retained until process exit");return false;
    }
    const Float4* pixels=nullptr;D3D12_RANGE range{0,draws*image_bytes};
    if(FAILED(f->readback->Map(0,&range,reinterpret_cast<void**>(const_cast<Float4**>(&pixels)))))return false;
    double max_reference=0,max_control=0;unsigned control_bits=0,changed=0,invalid=0,flat_bits=0,off_bits=0;
    const float weights[7]={.05f,.1f,.2f,.3f,.2f,.1f,.05f};
    auto sample=[&](unsigned mode,float u,float v,unsigned channel,bool linear) {
        auto at=[&](int x,int y){return source[mode][std::clamp(y,0,int(source_side)-1)*source_side+std::clamp(x,0,int(source_side)-1)][channel];};
        if(!linear)return at(int(std::floor(u*source_side)),int(std::floor(v*source_side)));
        const float sx=u*source_side-.5f,sy=v*source_side-.5f;
        const int x=int(std::floor(sx)),y=int(std::floor(sy));const float fx=sx-x,fy=sy-y;
        return std::lerp(std::lerp(at(x,y),at(x+1,y),fx),std::lerp(at(x,y+1),at(x+1,y+1),fx),fy);
    };
    for(unsigned mode=0;mode<modes;++mode)for(unsigned phase=0;phase<cases;++phase)for(unsigned axis=0;axis<2;++axis)
    for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)for(unsigned channel=0;channel<4;++channel) {
        const auto index=(((mode*cases+phase)*2+axis)*4)*side*side+y*side+x;
        const float original=pixels[index][channel],control=pixels[index+side*side][channel],patched=pixels[index+2*side*side][channel];
        max_control=std::max(max_control,double(std::abs(control-patched)));
        control_bits+=std::bit_cast<uint32_t>(control)!=std::bit_cast<uint32_t>(patched);
        if(mode==1)flat_bits+=std::bit_cast<uint32_t>(original)!=std::bit_cast<uint32_t>(patched);
        off_bits+=std::bit_cast<uint32_t>(pixels[index+3*side*side][channel])!=0;
        changed+=std::abs(original-patched)>1e-6f;
        for(unsigned variant=0;variant<3;++variant) {
            double expected=0;
            for(unsigned tap=0;tap<7;++tap){const float offset=(float(tap)-3.f+phase*.25f)/source_side;
                expected+=weights[tap]*sample(mode,(x+.5f)/side+(axis==0?offset:0),(y+.5f)/side+(axis==1?offset:0),channel,variant!=0);}
            const float actual=pixels[index+variant*side*side][channel];
            invalid+=!std::isfinite(actual) || actual==-17;
            max_reference=std::max(max_reference,std::abs(double(actual)-expected));
        }
    }
    std::filesystem::create_directories(output);std::ofstream raw(output/"bloom-pixels.f32",std::ios::binary);
    raw.write(reinterpret_cast<const char*>(pixels),draws*image_bytes);raw.close();f->readback->Unmap(0,&none);
    unsigned device_errors=0;
    if(validation)for(uint64_t i=first_message;i<validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
        SIZE_T size=0;validation->GetMessage(i,nullptr,&size);std::vector<uint8_t> storage(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if(SUCCEEDED(validation->GetMessage(i,message,&size)) && message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++device_errors;REXLOG_ERROR("Bloom pixel validation: {}",message->pDescription);}
    }
    std::ofstream report(output/"bloom-pixels.json");report<<"{\n\"draws\":"<<draws<<",\n\"source_side\":"<<source_side<<",\n\"output_side\":"<<side
        <<",\n\"control_bit_differences\":"<<control_bits<<",\n\"max_control_error\":"<<max_control<<",\n\"max_reference_error\":"<<max_reference
        <<",\n\"off_nonzero_components\":"<<off_bits<<",\n\"flat_bit_differences\":"<<flat_bits<<",\n\"changed_components\":"<<changed<<",\n\"invalid_components\":"<<invalid<<",\n\"device_errors\":"<<device_errors
        <<",\n\"debug_available\":"<<(validation?"true":"false")<<"\n}\n";report.close();
    REXLOG_INFO("Native bloom pixels: {} draws, original-linear/patched control bit differences {}, CPU reference max error {}, changed components {}, invalid {}, device errors {}",
        draws,control_bits,max_reference,changed,invalid,device_errors);
    return raw && report && !off_bits && !control_bits && !flat_bits && max_reference<1e-5 && changed && !invalid && !device_errors;
}
