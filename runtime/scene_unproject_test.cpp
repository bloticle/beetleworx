#include "scene_shader_audit.h"
#include "scene_unproject_shader.h"
#include "native_pixel_fixture.h"
#include "../private/scene_shader_data.h"
#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/logging.h>
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>
using namespace em2::pixel_test;
bool em2::TestSceneUnprojection(rex::ui::d3d12::D3D12Provider& provider,const std::filesystem::path& output){
    std::filesystem::create_directories(output);
    constexpr unsigned side=16,cases=12,variants=2;
    constexpr uint64_t image=side*side*sizeof(Float4),texture_offset=4096,constants_offset=texture_offset+image,stride=2048;
    auto f=std::make_unique<Fixture>();auto* device=provider.GetDevice();
    ComPtr<ID3D12InfoQueue> validation;device->QueryInterface(IID_PPV_ARGS(&validation));
    const auto first_message=validation?validation->GetNumStoredMessagesAllowedByRetrievalFilter():0;
    if(!f->event || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f->allocator))) ||
       FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f->allocator.Get(),nullptr,IID_PPV_ARGS(&f->list))) ||
       FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f->fence))))return false;
    auto library=LoadLibraryW(L"d3dcompiler_47.dll");if(!library)return false;
    struct Library{HMODULE h;~Library(){FreeLibrary(h);}} release{library};
    auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(library,"D3DCompile"));if(!compile)return false;
    auto build=[&](const std::string& text,const char* profile,ComPtr<ID3DBlob>& blob){
        ComPtr<ID3DBlob> errors;const auto hr=compile(text.data(),text.size(),"scene-position",nullptr,nullptr,"main",profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&errors);
        if(FAILED(hr) && errors)REXLOG_ERROR("Scene position compile: {}",static_cast<const char*>(errors->GetBufferPointer()));return SUCCEEDED(hr);
    };
    const std::string vs=R"(
struct V{float4 t[16]:TEXCOORD0;float4 p:SV_Position;};
V main(uint id:SV_VertexID){V o;float2 uv=float2((id<<1)&2,id&2);o.p=float4(uv*float2(2,-2)+float2(-1,1),0,1);
[unroll]for(uint i=0;i<16;++i)o.t[i]=0;o.t[0]=o.p;o.t[1]=float4(uv,0,1);return o;})";
    const std::string ps=std::string(kSceneUnprojectShader)+R"(
cbuffer C:register(b1){float4 c[18];}
Texture2DArray<float4> depth:register(t1);SamplerState pointSampler:register(s0);
struct V{float4 t[16]:TEXCOORD0;float4 p:SV_Position;};
float4 main(V i):SV_Target0{float4 rows[4]={c[13],c[14],c[15],c[16]};float3 world;
bool valid=SceneUnproject(i.t[0].xy,depth.SampleLevel(pointSampler,float3(i.t[1].xy,0),0).x,rows,world);
return float4(world,valid?1:0);})";
    ComPtr<ID3DBlob> vertex,reference;if(!build(vs,"vs_5_1",vertex) || !build(ps,"ps_5_1",reference))return false;
    const auto& code=scene_shader_data::depth_fog_position_probe;
    rex::graphics::DxbcShader native(rex::graphics::xenos::ShaderType::kPixel,1,code.data(),code.size(),std::endian::native);
    rex::string::StringBuffer disasm;native.AnalyzeUcode(disasm);
    {std::ofstream file(output/"native-position-probe.ucode.txt");file<<native.ucode_disassembly();}
    rex::graphics::DxbcShaderTranslator translator(provider.GetAdapterVendorID(),false,false,false,true,1,1);
    auto* translated=native.GetOrCreateTranslation(translator.GetDefaultPixelShaderModification(0));
    if(!translator.TranslateAnalyzedShader(*translated) || !translated->is_valid())return false;
    if(auto disassemble=reinterpret_cast<decltype(&D3DDisassemble)>(GetProcAddress(library,"D3DDisassemble"))){ComPtr<ID3DBlob> text;if(SUCCEEDED(disassemble(translated->translated_binary().data(),translated->translated_binary().size(),0,nullptr,&text))){std::ofstream file(output/"native-position-probe.dxbc.txt");file.write(static_cast<const char*>(text->GetBufferPointer()),text->GetBufferSize());}}
    // Cutting unrelated fog arithmetic changes tight packing. Bind original
    // guest constants through each program's own map, never guessed indices.
    for(unsigned i=13;i<=16;++i)if(native.constant_register_map().GetPackedFloatConstantIndex(i)==UINT32_MAX)return false;
    std::array<D3D12_ROOT_PARAMETER,6> params{};
    for(unsigned i=0;i<4;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[i].Descriptor.ShaderRegister=i;params[i].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;}
    const unsigned bindings=unsigned(native.GetTextureBindingsAfterTranslation().size());if(bindings!=4)return false;
    const D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,bindings,1,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,2,0,0,0}};
    for(unsigned i=0;i<2;++i){params[4+i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[4+i].DescriptorTable={1,&ranges[i]};params[4+i].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;}
    const D3D12_ROOT_SIGNATURE_DESC root{unsigned(params.size()),params.data(),0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> serialized,errors;if(FAILED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors)) ||
       FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&f->root))))return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=f->root.Get();pipeline.VS={vertex->GetBufferPointer(),vertex->GetBufferSize()};
    pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask=15;pipeline.SampleMask=UINT_MAX;pipeline.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
    pipeline.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pipeline.RasterizerState.DepthClipEnable=TRUE;
    pipeline.DepthStencilState.DepthEnable=FALSE;pipeline.DepthStencilState.StencilEnable=FALSE;pipeline.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets=1;pipeline.RTVFormats[0]=DXGI_FORMAT_R32G32B32A32_FLOAT;pipeline.SampleDesc.Count=1;
    for(unsigned i=0;i<variants;++i){pipeline.PS=i?D3D12_SHADER_BYTECODE{reference->GetBufferPointer(),reference->GetBufferSize()}:D3D12_SHADER_BYTECODE{translated->translated_binary().data(),translated->translated_binary().size()};
        if(FAILED(device->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&f->pipelines[i]))))return false;}
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,bindings,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    if(FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&f->views))))return false;
    heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;heap.NumDescriptors=2;if(FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&f->samplers))))return false;
    heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap.NumDescriptors=1;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;if(FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&f->rtvs))))return false;
    D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;texture.Width=texture.Height=side;
    texture.DepthOrArraySize=texture.MipLevels=texture.SampleDesc.Count=1;texture.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
    D3D12_HEAP_PROPERTIES gpu{};gpu.Type=D3D12_HEAP_TYPE_DEFAULT;gpu.CreationNodeMask=gpu.VisibleNodeMask=1;
    if(FAILED(device->CreateCommittedResource(&gpu,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&f->textures[0]))))return false;
    texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if(FAILED(device->CreateCommittedResource(&gpu,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&f->target))))return false;
    device->CreateRenderTargetView(f->target.Get(),nullptr,f->rtvs->GetCPUDescriptorHandleForHeapStart());
    if(!Buffer(device,constants_offset+cases*variants*stride,D3D12_HEAP_TYPE_UPLOAD,&f->upload) || !Buffer(device,cases*variants*image,D3D12_HEAP_TYPE_READBACK,&f->readback))return false;
    uint8_t* mapped=nullptr;D3D12_RANGE none{};if(FAILED(f->upload->Map(0,&none,reinterpret_cast<void**>(&mapped))))return false;
    std::memset(mapped,0,constants_offset+cases*variants*stride);
    auto& system=*reinterpret_cast<rex::graphics::DxbcShaderTranslator::SystemConstants*>(mapped);
    system.flags=(1u<<rex::graphics::DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift)|(1u<<rex::graphics::DxbcShaderTranslator::kSysFlag_AlphaPassIfEqual_Shift)|(1u<<rex::graphics::DxbcShaderTranslator::kSysFlag_AlphaPassIfGreater_Shift);
    for(auto& bias:system.color_exp_bias)bias=1;
    auto* fetch=reinterpret_cast<uint32_t*>(mapped+1024);for(unsigned i=0;i<2;++i)fetch[i*6+2]=(side-1)|((side-1)<<13);
    for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x){Float4 p{float((x+2*y)%17)/16,0,0,1};std::memcpy(mapped+texture_offset+(y*side+x)*16,p.data(),16);}
    std::array<std::array<Float4,4>,cases> matrices{};
    for(unsigned test=0;test<cases;++test){std::array<Float4,256> c{};
        c[13]={1+test*.1f,.05f,.03f,float(int(test)-5)};c[14]={-.1f,1.2f,.15f,-float(test)*.25f};c[15]={.02f,-.04f,.5f,.25f};c[16]={float(test%3)*.03f,-.02f,.2f,1};c[255]={0,1,3,2};
        for(unsigned row=0;row<4;++row)matrices[test][row]=c[13+row];
        auto* native_block=reinterpret_cast<Float4*>(mapped+constants_offset+test*variants*stride);
        auto* reference_block=reinterpret_cast<Float4*>(mapped+constants_offset+(test*variants+1)*stride);
        for(unsigned i=0;i<256;++i){const auto packed=native.constant_register_map().GetPackedFloatConstantIndex(i);if(packed!=UINT32_MAX)native_block[packed]=c[i];}
        for(unsigned i=0;i<17;++i)reference_block[i]=c[i];reference_block[17]=c[255];}
    f->upload->Unmap(0,nullptr);
    auto srv=f->views->GetCPUDescriptorHandleForHeapStart();
    for(unsigned i=0;i<bindings;++i){D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=texture.Format;d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Texture2DArray.MipLevels=d.Texture2DArray.ArraySize=1;device->CreateShaderResourceView(f->textures[0].Get(),&d,srv);srv.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);}
    auto sampler=f->samplers->GetCPUDescriptorHandleForHeapStart();for(unsigned i=0;i<2;++i){D3D12_SAMPLER_DESC d{};d.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;
        d.AddressU=d.AddressV=d.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;d.MaxLOD=D3D12_FLOAT32_MAX;d.MaxAnisotropy=1;d.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        device->CreateSampler(&d,sampler);sampler.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);}
    D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=f->upload.Get();source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Offset=texture_offset;source.PlacedFootprint.Footprint={texture.Format,side,side,1,side*16};
    D3D12_TEXTURE_COPY_LOCATION dest{};dest.pResource=f->textures[0].Get();dest.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    f->list->CopyTextureRegion(&dest,0,0,0,&source,nullptr);Barrier(f->list.Get(),f->textures[0].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    f->list->SetGraphicsRootSignature(f->root.Get());ID3D12DescriptorHeap* heaps[]={f->views.Get(),f->samplers.Get()};f->list->SetDescriptorHeaps(2,heaps);
    f->list->SetGraphicsRootConstantBufferView(0,f->upload->GetGPUVirtualAddress());
    f->list->SetGraphicsRootConstantBufferView(2,f->upload->GetGPUVirtualAddress()+2048);
    f->list->SetGraphicsRootConstantBufferView(3,f->upload->GetGPUVirtualAddress()+1024);
    f->list->SetGraphicsRootDescriptorTable(4,f->views->GetGPUDescriptorHandleForHeapStart());f->list->SetGraphicsRootDescriptorTable(5,f->samplers->GetGPUDescriptorHandleForHeapStart());
    D3D12_VIEWPORT viewport{0,0,float(side),float(side),0,1};D3D12_RECT scissor{0,0,side,side};
    f->list->RSSetViewports(1,&viewport);f->list->RSSetScissorRects(1,&scissor);f->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const auto rtv=f->rtvs->GetCPUDescriptorHandleForHeapStart();f->list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
    for(unsigned test=0;test<cases;++test)for(unsigned variant=0;variant<variants;++variant){
        const float sentinel[4]={-999,-999,-999,-999};f->list->ClearRenderTargetView(rtv,sentinel,0,nullptr);
        f->list->SetPipelineState(f->pipelines[variant].Get());f->list->SetGraphicsRootConstantBufferView(1,f->upload->GetGPUVirtualAddress()+constants_offset+(test*variants+variant)*stride);f->list->DrawInstanced(3,1,0,0);
        Barrier(f->list.Get(),f->target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
        source.pResource=f->target.Get();source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;source.SubresourceIndex=0;
        dest.pResource=f->readback.Get();dest.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dest.PlacedFootprint.Offset=(test*variants+variant)*image;dest.PlacedFootprint.Footprint={texture.Format,side,side,1,side*16};
        f->list->CopyTextureRegion(&dest,0,0,0,&source,nullptr);Barrier(f->list.Get(),f->target.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    if(FAILED(f->list->Close()))return false;ID3D12CommandList* lists[]={f->list.Get()};provider.GetDirectQueue()->ExecuteCommandLists(1,lists);
    if(FAILED(provider.GetDirectQueue()->Signal(f->fence.Get(),1)) || FAILED(f->fence->SetEventOnCompletion(1,f->event)) || WaitForSingleObject(f->event,15000)!=WAIT_OBJECT_0){f.release();return false;}
    const Float4* pixels=nullptr;D3D12_RANGE range{0,cases*variants*image};if(FAILED(f->readback->Map(0,&range,reinterpret_cast<void**>(const_cast<Float4**>(&pixels)))))return false;
    unsigned bad=0,cpu_bad=0;double error=0,cpu_error=0;
    for(unsigned test:{0u,1u}){const auto& a=pixels[test*variants*side*side];const auto& b=pixels[(test*variants+1)*side*side];REXLOG_INFO("Scene position case{} first pixel native({},{},{},{}) reference({},{},{},{})",test,a[0],a[1],a[2],a[3],b[0],b[1],b[2],b[3]);}
    for(unsigned test=0;test<cases;++test)for(unsigned p=0;p<side*side;++p)for(unsigned lane=0;lane<4;++lane){
        const float a=pixels[(test*variants)*side*side+p][lane],b=pixels[(test*variants+1)*side*side+p][lane];
        if(!std::isfinite(a) || !std::isfinite(b) || a==-999 || b==-999 || (lane==3 && (a!=1 || b!=1)))++bad;
        const double difference=std::abs(double(a)-b);error=std::max(error,difference);if(difference>3e-5*(1+std::abs(double(b))))++bad;
        const unsigned x=p%side,y=p/side;
        const double clip[]={2*(double(x)+.5)/side-1,1-2*(double(y)+.5)/side,1-double((x+2*y)%17)/16,1};
        double projected[4]{};for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col)projected[row]+=double(matrices[test][row][col])*clip[col];
        const double expected=lane==3?1:projected[lane]/projected[3];
        for(float actual:{a,b}){const double delta=std::abs(double(actual)-expected);cpu_error=std::max(cpu_error,delta);if(!std::isfinite(actual) || delta>3e-5*(1+std::abs(expected)))++cpu_bad;}
    }
    f->readback->Unmap(0,&none);unsigned device_errors=0;
    if(validation)for(uint64_t i=first_message;i<validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;validation->GetMessage(i,nullptr,&size);std::vector<uint8_t> bytes(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());if(SUCCEEDED(validation->GetMessage(i,m,&size)) && m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++device_errors;REXLOG_ERROR("Scene position validation: {}",m->pDescription);}}
    std::filesystem::create_directories(output);std::ofstream report(output/"scene-unproject-results.json");
    report<<"{\"draws\":"<<cases*variants<<",\"components\":"<<cases*side*side*4<<",\"invalid\":"<<bad<<",\"max_error\":"<<error<<",\"cpu_invalid\":"<<cpu_bad<<",\"cpu_max_error\":"<<cpu_error<<",\"device_errors\":"<<device_errors<<",\"scope\":\"Original native fog reconstruction prefix, only clauses rescheduled and final XYZ export added, versus reusable HLSL and independent CPU double-precision pixel/depth oracle on offscreen matrices and varying reverse-depth texture. No actual scene capture, AO or title.\"}\n";
    REXLOG_INFO("Scene unprojection native differential: {} components, invalid{}, max error{}, CPU invalid{} error{}, device errors{}",cases*side*side*4,bad,error,cpu_bad,cpu_error,device_errors);
    return report.good() && !bad && !cpu_bad && !device_errors;
}
