#include "bloom_composite_test.h"
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
constexpr unsigned side=32,cases=3,modes=4,variants=2;
constexpr uint64_t image_bytes=side*side*sizeof(Float4),draws=modes*2*2*cases*cases*2*variants;

}

namespace em2 {
static bool TestCompositePixels(rex::ui::d3d12::D3D12Provider& provider,const std::filesystem::path& output,unsigned source_side) {
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
    std::array<std::span<const uint32_t>,variants> programs={bloom_test_data::composite,bloom_test_data::composite_linear};
    std::array<std::unique_ptr<rex::graphics::DxbcShader>,variants> shaders;
    for(unsigned variant=0;variant<variants;++variant) {
        auto& shader=shaders[variant];shader=std::make_unique<rex::graphics::DxbcShader>(rex::graphics::xenos::ShaderType::kPixel,
            variant+1,programs[variant].data(),programs[variant].size(),std::endian::native);
        rex::string::StringBuffer disassembly;shader->AnalyzeUcode(disassembly);
        rex::graphics::DxbcShaderTranslator translator(provider.GetAdapterVendorID(),false,false,false,true,1,1);
        auto* translation=shader->GetOrCreateTranslation(translator.GetDefaultPixelShaderModification(0));
        if(!translator.TranslateAnalyzedShader(*translation) || !translation->is_valid())return false;
        if(variant==1 && !std::all_of(shader->GetSamplerBindingsAfterTranslation().begin(),shader->GetSamplerBindingsAfterTranslation().end(),[](const auto& binding){
            return binding.min_filter==rex::graphics::xenos::TextureFilter::kLinear && binding.mag_filter==rex::graphics::xenos::TextureFilter::kLinear;
        }))return false;
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
    const auto& shader=*shaders[0];const auto& bindings=shader.GetTextureBindingsAfterTranslation();
    const auto c0=shader.constant_register_map().GetPackedFloatConstantIndex(0),c1=shader.constant_register_map().GetPackedFloatConstantIndex(1);
    if(c0==UINT32_MAX || c1==UINT32_MAX || bindings.size()!=4 || shader.GetSamplerBindingsAfterTranslation().size()!=2)return false;
    for(const auto& binding:bindings)if(binding.fetch_constant>1)return false;
    D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors=12*modes*4;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if(FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&f->views))))return false;
    heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;heap.NumDescriptors=12;
    if(FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&f->samplers))))return false;
    heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap.NumDescriptors=1;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if(FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&f->rtvs))))return false;
    D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width=texture.Height=source_side;texture.DepthOrArraySize=texture.MipLevels=texture.SampleDesc.Count=1;
    texture.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
    D3D12_HEAP_PROPERTIES gpu{};gpu.Type=D3D12_HEAP_TYPE_DEFAULT;gpu.CreationNodeMask=gpu.VisibleNodeMask=1;
    for(unsigned i=0;i<6;++i)if(FAILED(device->CreateCommittedResource(&gpu,D3D12_HEAP_FLAG_NONE,&texture,
        D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&f->textures[i]))))return false;
    texture.Width=texture.Height=side;texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if(FAILED(device->CreateCommittedResource(&gpu,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_RENDER_TARGET,
        nullptr,IID_PPV_ARGS(&f->target))))return false;
    device->CreateRenderTargetView(f->target.Get(),nullptr,f->rtvs->GetCPUDescriptorHandleForHeapStart());
    const uint64_t row=(source_side*sizeof(Float4)+255)&~uint64_t(255),source_bytes=row*source_side,system_offset=6*source_bytes,fetch_offset=system_offset+1024;
    const uint64_t cases_offset=fetch_offset+1024,stride=4096,upload_bytes=cases_offset+draws*stride;
    if(!Buffer(device,upload_bytes,D3D12_HEAP_TYPE_UPLOAD,&f->upload) ||
       !Buffer(device,draws*image_bytes,D3D12_HEAP_TYPE_READBACK,&f->readback))return false;
    uint8_t* mapped=nullptr;D3D12_RANGE none{0,0};if(FAILED(f->upload->Map(0,&none,reinterpret_cast<void**>(&mapped))))return false;
    std::memset(mapped,0,upload_bytes);
    std::array<std::vector<Float4>,6> inputs;for(auto& input:inputs)input.resize(source_side*source_side);
    const Float4 bloom_color={.125f,.25f,2.f,.75f};
    for(unsigned t=0;t<6;++t)for(unsigned y=0;y<source_side;++y)for(unsigned x=0;x<source_side;++x) {
        Float4 value{};
        if(t==4)value=bloom_color;
        else if(t<4) {
            if(t==1)value={.2f,.4f,.6f,.8f};
            else {const float factor=t==2?8.f:1.f;
                value={factor*x/float(source_side-1),factor*y/float(source_side-1),factor*(x+y)/float(2*(source_side-1)),float((x+y)&1)};
                if(t==3)for(auto& v:value)v=v*3.f-2.f;}
        }
        inputs[t][y*source_side+x]=value;
        std::memcpy(mapped+t*source_bytes+y*row+x*sizeof(Float4),value.data(),sizeof(value));
    }
    auto& system=*reinterpret_cast<rex::graphics::DxbcShaderTranslator::SystemConstants*>(mapped+system_offset);
    system.flags=(1u<<rex::graphics::DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift)|
        (1u<<rex::graphics::DxbcShaderTranslator::kSysFlag_AlphaPassIfEqual_Shift)|
        (1u<<rex::graphics::DxbcShaderTranslator::kSysFlag_AlphaPassIfGreater_Shift);
    for(auto& bias:system.color_exp_bias)bias=1.f;
    auto* fetch=reinterpret_cast<uint32_t*>(mapped+fetch_offset);
    for(unsigned i=0;i<2;++i)fetch[i*6+2]=(source_side-1)|((source_side-1)<<13);
    auto srv=f->views->GetCPUDescriptorHandleForHeapStart();const auto view_step=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for(unsigned mode=0;mode<modes;++mode)for(unsigned slot=0;slot<2;++slot)for(unsigned lit=0;lit<2;++lit)
    for(unsigned i=0;i<12;++i) {
        const auto source_slot=i<bindings.size()?bindings[i].fetch_constant:0;
        const auto source=source_slot==slot?mode:lit?4:5;
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;desc.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        desc.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;desc.Texture2DArray.MipLevels=desc.Texture2DArray.ArraySize=1;
        device->CreateShaderResourceView(f->textures[source].Get(),&desc,srv);srv.ptr+=view_step;
    }
    auto sampler=f->samplers->GetCPUDescriptorHandleForHeapStart();const auto sampler_step=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    for(unsigned i=0;i<12;++i){D3D12_SAMPLER_DESC desc{};desc.Filter=i<6?D3D12_FILTER_MIN_MAG_MIP_POINT:D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        desc.AddressU=desc.AddressV=desc.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;desc.MaxAnisotropy=1;
        desc.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;desc.MaxLOD=D3D12_FLOAT32_MAX;device->CreateSampler(&desc,sampler);sampler.ptr+=sampler_step;}
    for(unsigned t=0;t<6;++t) {
        D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=f->upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint.Offset=t*source_bytes;from.PlacedFootprint.Footprint={DXGI_FORMAT_R32G32B32A32_FLOAT,source_side,source_side,1,UINT(row)};
        D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=f->textures[t].Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),f->textures[t].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    ID3D12DescriptorHeap* heaps[]={f->views.Get(),f->samplers.Get()};f->list->SetDescriptorHeaps(2,heaps);f->list->SetGraphicsRootSignature(f->root.Get());
    f->list->SetGraphicsRootConstantBufferView(0,f->upload->GetGPUVirtualAddress()+system_offset);
    f->list->SetGraphicsRootConstantBufferView(3,f->upload->GetGPUVirtualAddress()+fetch_offset);
    const auto rtv=f->rtvs->GetCPUDescriptorHandleForHeapStart();f->list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
    D3D12_VIEWPORT viewport{0,0,float(side),float(side),0,1};D3D12_RECT scissor{0,0,side,side};
    f->list->RSSetViewports(1,&viewport);f->list->RSSetScissorRects(1,&scissor);f->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    constexpr float scene_weights[cases]={0.f,.5f,1.f},bloom_weights[cases]={0.f,.25f,2.f};
    unsigned draw_index=0;
    for(unsigned variant=0;variant<variants;++variant)for(unsigned mode=0;mode<modes;++mode)for(unsigned slot=0;slot<2;++slot)for(unsigned linear=0;linear<2;++linear)
    for(unsigned sw=0;sw<cases;++sw)for(unsigned bw=0;bw<cases;++bw)for(unsigned lit=0;lit<2;++lit,++draw_index) {
        auto* block=mapped+cases_offset+draw_index*stride;
        f->list->SetPipelineState(f->pipelines[variant].Get());
        const Float4 zero_weight={slot==1?scene_weights[sw]:bloom_weights[bw],17,19,23};
        const Float4 one_weight={slot==0?scene_weights[sw]:bloom_weights[bw],29,31,37};
        std::memcpy(block+c0*16,zero_weight.data(),16);std::memcpy(block+c1*16,one_weight.data(),16);
        f->list->SetGraphicsRootConstantBufferView(1,f->upload->GetGPUVirtualAddress()+cases_offset+draw_index*stride);
        f->list->SetGraphicsRootConstantBufferView(2,f->upload->GetGPUVirtualAddress()+cases_offset+draw_index*stride+2048);
        auto table=f->views->GetGPUDescriptorHandleForHeapStart();table.ptr+=uint64_t((mode*2+slot)*2+lit)*12*view_step;f->list->SetGraphicsRootDescriptorTable(4,table);
        auto samplers=f->samplers->GetGPUDescriptorHandleForHeapStart();samplers.ptr+=uint64_t(linear || variant)*6*sampler_step;f->list->SetGraphicsRootDescriptorTable(5,samplers);
        const float clear[4]={-17,-17,-17,-17};f->list->ClearRenderTargetView(rtv,clear,0,nullptr);f->list->DrawInstanced(3,1,0,0);
        Barrier(f->list.Get(),f->target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=f->target.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Offset=draw_index*image_bytes;to.PlacedFootprint.Footprint={DXGI_FORMAT_R32G32B32A32_FLOAT,side,side,1,side*sizeof(Float4)};
        f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),f->target.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    f->upload->Unmap(0,nullptr);
    if(draw_index!=draws || FAILED(f->list->Close()))return false;ID3D12CommandList* lists[]={f->list.Get()};queue->ExecuteCommandLists(1,lists);
    if(FAILED(queue->Signal(f->fence.Get(),1)) || FAILED(f->fence->SetEventOnCompletion(1,f->event)) || WaitForSingleObject(f->event,15000)!=WAIT_OBJECT_0){f.release();return false;}
    const Float4* pixels=nullptr;D3D12_RANGE range{0,draws*image_bytes};
    if(FAILED(f->readback->Map(0,&range,reinterpret_cast<void**>(const_cast<Float4**>(&pixels)))))return false;
    auto sample=[&](unsigned mode,unsigned x,unsigned y,unsigned channel,bool linear) {
        auto at=[&](int ix,int iy){return inputs[mode][std::clamp(iy,0,int(source_side)-1)*source_side+std::clamp(ix,0,int(source_side)-1)][channel];};
        const float u=(x+.5f)/side*source_side,v=(y+.5f)/side*source_side;
        if(!linear)return at(int(u),int(v));
        const int ix=int(std::floor(u-.5f)),iy=int(std::floor(v-.5f));const float fx=u-.5f-ix,fy=v-.5f-iy;
        return std::lerp(std::lerp(at(ix,iy),at(ix+1,iy),fx),std::lerp(at(ix,iy+1),at(ix+1,iy+1),fx),fy);
    };
    double max_reference=0;unsigned invalid=0,changed_images=0,off_control_bits=0,patch_control_bits=0,point_changed=0;draw_index=0;
    for(unsigned variant=0;variant<variants;++variant)for(unsigned mode=0;mode<modes;++mode)for(unsigned slot=0;slot<2;++slot)for(unsigned linear=0;linear<2;++linear)
    for(unsigned sw=0;sw<cases;++sw)for(unsigned bw=0;bw<cases;++bw)for(unsigned lit=0;lit<2;++lit,++draw_index) {
        const auto* image=pixels+draw_index*side*side;const auto* baseline=pixels+((draw_index/6)*6+1)*side*side;
        bool changed=false;
        for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)for(unsigned channel=0;channel<4;++channel) {
            const auto index=y*side+x;const float actual=image[index][channel];
            const double expected=double(sample(mode,x,y,channel,linear || variant))*scene_weights[sw]+(lit?double(bloom_color[channel])*bloom_weights[bw]:0.);
            invalid+=!std::isfinite(actual) || actual==-17;max_reference=std::max(max_reference,std::abs(double(actual)-expected));
            if(!lit)off_control_bits+=std::bit_cast<uint32_t>(actual)!=std::bit_cast<uint32_t>(baseline[index][channel]);
            changed|=std::abs(actual-baseline[index][channel])>1e-6f;
            if(variant) {
                const auto local=draw_index-unsigned(draws/variants);
                const auto* control=pixels+(local+(linear?0:18))*side*side;
                const auto* point=pixels+(local-(linear?18:0))*side*side;
                patch_control_bits+=std::bit_cast<uint32_t>(actual)!=std::bit_cast<uint32_t>(control[index][channel]);
                point_changed+=std::bit_cast<uint32_t>(actual)!=std::bit_cast<uint32_t>(point[index][channel]);
            }
        }
        if(lit && bw)changed_images+=changed;
    }
    std::filesystem::create_directories(output);std::ofstream raw(output/"composite-pixels.f32",std::ios::binary);
    raw.write(reinterpret_cast<const char*>(pixels),draws*image_bytes);raw.close();f->readback->Unmap(0,&none);
    unsigned errors_count=0;
    if(validation)for(uint64_t i=first_message;i<validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
        SIZE_T size=0;validation->GetMessage(i,nullptr,&size);std::vector<uint8_t> storage(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if(SUCCEEDED(validation->GetMessage(i,message,&size)) && message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors_count;REXLOG_ERROR("Bloom composite validation: {}",message->pDescription);}
    }
    std::ofstream report(output/"composite-pixels.json");report<<"{\n\"draws\":"<<draws<<",\n\"zero_bloom_control_bit_differences\":"<<off_control_bits
        <<",\n\"source_side\":"<<source_side<<",\n\"patch_linear_control_bit_differences\":"<<patch_control_bits<<",\n\"patch_point_changed_components\":"<<point_changed
        <<",\n\"max_reference_error\":"<<max_reference<<",\n\"changed_lit_images\":"<<changed_images<<",\n\"invalid_components\":"<<invalid
        <<",\n\"device_errors\":"<<errors_count<<",\n\"debug_available\":"<<(validation?"true":"false")<<"\n}\n";report.close();
    REXLOG_INFO("Native bloom composite: {} draws, source {}x{}, zero-bloom control bit differences {}, patch/linear differences {}, patch/point changed {}, reference max error {}, responsive lit images {}, invalid {}, device errors {}",
        draws,source_side,source_side,off_control_bits,patch_control_bits,point_changed,max_reference,changed_images,invalid,errors_count);
    return raw && report && !off_control_bits && !patch_control_bits && (source_side==side?!point_changed:point_changed>0) && max_reference<1e-5 && changed_images==96*variants && !invalid && !errors_count;
}
bool TestNativeBloomCompositePixels(rex::ui::d3d12::D3D12Provider& provider,const std::filesystem::path& output) {
    return TestCompositePixels(provider,output,8) && TestCompositePixels(provider,output/"native-size",32);
}
}
