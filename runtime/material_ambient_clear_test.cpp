#include "scene_shader_audit.h"
#include "material_ambient_export.h"
#include "hdr_shader_export.h"
#include "native_pixel_fixture.h"
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/logging.h>
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

namespace {
using namespace em2::pixel_test;
struct ClearFixture:Fixture {std::array<ComPtr<ID3D12PipelineState>,60> states;};
float Half(uint16_t bits){const unsigned e=(bits>>10)&31,m=bits&1023;return (bits&32768?-1.f:1.f)*(e?std::ldexp(float(1024+m),int(e)-25):std::ldexp(float(m),-24));}
}
bool em2::TestMaterialAmbientClear(rex::ui::d3d12::D3D12Provider& provider,const std::filesystem::path& directory){
    constexpr unsigned side=8,cases=20;constexpr uint64_t image=2048;
    auto f=std::make_unique<ClearFixture>();auto* device=provider.GetDevice();
    ComPtr<ID3D12InfoQueue> validation;device->QueryInterface(IID_PPV_ARGS(&validation));const auto first=validation?validation->GetNumStoredMessagesAllowedByRetrievalFilter():0;
    if(!f->event || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f->allocator))) || FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f->allocator.Get(),nullptr,IID_PPV_ARGS(&f->list))) || FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f->fence))))return false;
    const auto library=LoadLibraryW(L"d3dcompiler_47.dll");if(!library)return false;
    struct Library{HMODULE value;~Library(){FreeLibrary(value);}}guard{library};const auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(library,"D3DCompile"));if(!compile)return false;
    const char vs[]=R"(float4 main(uint id:SV_VertexID):SV_Position{float2 uv=float2((id<<1)&2,id&2);return float4(uv*float2(2,-2)+float2(-1,1),0,1);})";
    const char ps[]=R"(cbuffer Params:register(b0){float4 colour;};float4 main(float4 p:SV_Position):SV_Target0{clip(p.x-3);return colour;})";
    const char factor_ps[]=R"(cbuffer Params:register(b0){float4 colour;float4 hdr_colour;};struct Outputs{float4 main:SV_Target0;float4 hdr:SV_Target4;};Outputs main(float4 p:SV_Position){clip(p.x-3);Outputs o;o.main=colour;o.hdr=hdr_colour;return o;})";
    ComPtr<ID3DBlob> vertex,pixel,factor_pixel,error,serialized;
    if(FAILED(compile(vs,sizeof(vs)-1,"ambient-clear-vs",nullptr,nullptr,"main","vs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vertex,&error)) || FAILED(compile(ps,sizeof(ps)-1,"ambient-clear-ps",nullptr,nullptr,"main","ps_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&pixel,&error)))return false;
    const auto hdr=AddHdrSceneExport({static_cast<const uint8_t*>(pixel->GetBufferPointer()),pixel->GetBufferSize()});if(hdr.empty() || !AddLinearHdrSceneExport({static_cast<const uint8_t*>(pixel->GetBufferPointer()),pixel->GetBufferSize()}).empty() || !AddLinearHdrSceneExport(hdr).empty() || !AddLinearHdrSceneExport(std::span(hdr).first(32)).empty())return false;
    if(FAILED(compile(factor_ps,sizeof(factor_ps)-1,"ambient-factor-ps",nullptr,nullptr,"main","ps_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&factor_pixel,&error)) || !ExportHdrAttenuationMaterialAmbient({static_cast<const uint8_t*>(pixel->GetBufferPointer()),pixel->GetBufferSize()}).empty() || !ExportHdrAttenuationMaterialAmbient(std::span(hdr).first(32)).empty())return false;
    D3D12_ROOT_PARAMETER parameter{};parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;parameter.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    const D3D12_ROOT_SIGNATURE_DESC root{1,&parameter};if(FAILED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error)) || FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&f->root))))return false;
    for(unsigned test=0;test<cases;++test){
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=f->root.Get();p.VS={vertex->GetBufferPointer(),vertex->GetBufferSize()};p.PS={hdr.data(),hdr.size()};p.NumRenderTargets=5;p.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;p.RTVFormats[4]=DXGI_FORMAT_R16G16B16A16_FLOAT;p.SampleDesc.Count=1;p.SampleMask=UINT_MAX;p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.RasterizerState.DepthClipEnable=TRUE;p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        p.BlendState.IndependentBlendEnable=TRUE;auto& b=p.BlendState.RenderTarget[0];b.RenderTargetWriteMask=test==5?1:15;b.BlendEnable=test!=0 && test!=5;b.SrcBlend=D3D12_BLEND_SRC_ALPHA;b.DestBlend=D3D12_BLEND_INV_SRC_ALPHA;b.BlendOp=D3D12_BLEND_OP_ADD;b.SrcBlendAlpha=D3D12_BLEND_ONE;b.DestBlendAlpha=D3D12_BLEND_ZERO;b.BlendOpAlpha=D3D12_BLEND_OP_ADD;
        if(test==2){b.SrcBlend=D3D12_BLEND_ONE;b.DestBlend=D3D12_BLEND_ONE;}if(test==3)b.DestBlend=D3D12_BLEND_SRC_ALPHA;if(test==4)b.BlendOp=D3D12_BLEND_OP_SUBTRACT;if(test==6)b.BlendOp=D3D12_BLEND_OP_REV_SUBTRACT;if(test==7)b.DestBlend=D3D12_BLEND_ZERO;
        if(test>=8){p.PS={factor_pixel->GetBufferPointer(),factor_pixel->GetBufferSize()};b.DestBlend=test==9 || test==13?D3D12_BLEND_INV_SRC_COLOR:D3D12_BLEND_SRC_COLOR;if(test==10)b.BlendOp=D3D12_BLEND_OP_SUBTRACT;if(test==11)b.BlendOp=D3D12_BLEND_OP_REV_SUBTRACT;if(test==12)b.RenderTargetWriteMask=1;}
        if(test>=14){b.SrcBlend=D3D12_BLEND_DEST_COLOR;b.DestBlend=test==14?D3D12_BLEND_ZERO:test==15?D3D12_BLEND_ONE:test==16?D3D12_BLEND_SRC_COLOR:test==17?D3D12_BLEND_INV_SRC_COLOR:D3D12_BLEND_SRC_ALPHA;if(test==18)b.BlendOp=D3D12_BLEND_OP_SUBTRACT;if(test==19)b.BlendOp=D3D12_BLEND_OP_REV_SUBTRACT;}
        p.BlendState.RenderTarget[4]=b;
        if(FAILED(device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&f->states[test*3]))))return false;
        auto modified=MaterialAmbientClearPipeline::Create(p);if(!modified || modified->desc().CachedPSO.pCachedBlob || !ExportZeroMaterialAmbient({static_cast<const uint8_t*>(modified->desc().PS.pShaderBytecode),modified->desc().PS.BytecodeLength}).empty()){REXLOG_ERROR("Ambient auxiliary PSO case{} failed, shader rewrite{}",test,bool(modified));return false;}
        auto surface=MaterialSurfaceClearPipeline::Create(modified->desc());
        if(!surface || surface->desc().NumRenderTargets!=7 || surface->desc().RTVFormats[6]!=DXGI_FORMAT_R16G16B16A16_FLOAT || surface->desc().BlendState.RenderTarget[6].BlendEnable || surface->desc().BlendState.RenderTarget[6].RenderTargetWriteMask!=15 || !ExportUnavailableMaterialSurface({static_cast<const uint8_t*>(surface->desc().PS.pShaderBytecode),surface->desc().PS.BytecodeLength}).empty() || !ExportUnavailableMaterialSurface(std::span(hdr).first(32)).empty() || MaterialSurfaceClearPipeline::Create(surface->desc()) || FAILED(device->CreateGraphicsPipelineState(&surface->desc(),IID_PPV_ARGS(&f->states[test*3+1]))))return false;
        auto keep=MaterialSurfaceClearPipeline::Create(modified->desc(),true);
        if(!keep || keep->desc().BlendState.RenderTarget[6].RenderTargetWriteMask || keep->desc().PS.BytecodeLength!=modified->desc().PS.BytecodeLength || std::memcmp(keep->desc().PS.pShaderBytecode,modified->desc().PS.pShaderBytecode,modified->desc().PS.BytecodeLength) || FAILED(device->CreateGraphicsPipelineState(&keep->desc(),IID_PPV_ARGS(&f->states[test*3+2]))))return false;
        auto invalid=p;invalid.BlendState.RenderTarget[0].BlendEnable=TRUE;invalid.BlendState.RenderTarget[0].DestBlend=D3D12_BLEND_DEST_COLOR;if(MaterialAmbientClearPipeline::Supports(invalid))return false;
        invalid=p;invalid.BlendState.RenderTarget[0].BlendEnable=TRUE;invalid.BlendState.RenderTarget[0].BlendOp=D3D12_BLEND_OP_MIN;if(MaterialAmbientClearPipeline::Supports(invalid))return false;
        invalid=p;invalid.BlendState.RenderTarget[0].BlendEnable=TRUE;invalid.BlendState.RenderTarget[0].SrcBlend=D3D12_BLEND_INV_DEST_COLOR;if(MaterialAmbientClearPipeline::Supports(invalid))return false;invalid=p;invalid.BlendState.RenderTarget[0].LogicOpEnable=TRUE;if(MaterialAmbientClearPipeline::Supports(invalid))return false;
        if(test>=8){invalid=p;invalid.NumRenderTargets=1;if(MaterialAmbientClearPipeline::Supports(invalid))return false;invalid=p;invalid.BlendState.RenderTarget[4].DestBlend=p.BlendState.RenderTarget[4].DestBlend==D3D12_BLEND_ONE?D3D12_BLEND_ZERO:D3D12_BLEND_ONE;if(MaterialAmbientClearPipeline::Supports(invalid))return false;invalid=p;invalid.PS={pixel->GetBufferPointer(),pixel->GetBufferSize()};if(MaterialAmbientClearPipeline::Create(invalid))return false;}
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,7,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};if(FAILED(device->CreateDescriptorHeap(&heap_desc,IID_PPV_ARGS(&f->rtvs))))return false;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;texture.Width=texture.Height=side;texture.DepthOrArraySize=texture.MipLevels=texture.SampleDesc.Count=1;texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    auto handle=f->rtvs->GetCPUDescriptorHandleForHeapStart();const auto step=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for(unsigned i=0;i<6;++i){texture.Format=i<2?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_R16G16B16A16_FLOAT;if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&f->textures[i]))))return false;device->CreateRenderTargetView(f->textures[i].Get(),nullptr,handle);handle.ptr+=step;}
    D3D12_RENDER_TARGET_VIEW_DESC null{};null.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;null.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;device->CreateRenderTargetView(nullptr,&null,handle);
    if(!Buffer(device,512,D3D12_HEAP_TYPE_UPLOAD,&f->upload) || !Buffer(device,cases*10*image,D3D12_HEAP_TYPE_READBACK,&f->readback))return false;
    void* mapped=nullptr;D3D12_RANGE none{};if(FAILED(f->upload->Map(0,&none,&mapped)))return false;const float colour[]={.4f,.3f,.2f,.25f},factor_colour[]={2.5f,-.25f,.5f,.25f,1.25f,-.5f,.25f,.25f};std::memcpy(mapped,colour,sizeof(colour));std::memcpy(static_cast<uint8_t*>(mapped)+256,factor_colour,sizeof(factor_colour));f->upload->Unmap(0,nullptr);
    f->list->SetGraphicsRootSignature(f->root.Get());f->list->SetGraphicsRootConstantBufferView(0,f->upload->GetGPUVirtualAddress());const D3D12_VIEWPORT viewport{0,0,side,side,0,1};const D3D12_RECT scissor{1,1,7,7};f->list->RSSetViewports(1,&viewport);f->list->RSSetScissorRects(1,&scissor);f->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const float seed[]={.25f,.5f,2,1},surface_seed[]={.125f,.5f,4,32},legacy[]={.25f,.5f,.75f,1};const auto first_rtv=f->rtvs->GetCPUDescriptorHandleForHeapStart();
    for(unsigned test=0;test<cases;++test){
        f->list->SetGraphicsRootConstantBufferView(0,f->upload->GetGPUVirtualAddress()+(test>=8?256:0));
        for(unsigned i=0;i<6;++i){auto rtv=first_rtv;rtv.ptr+=i*step;f->list->ClearRenderTargetView(rtv,i<2?legacy:i==5?surface_seed:seed,0,nullptr);}
        for(unsigned mode=0;mode<3;++mode){D3D12_CPU_DESCRIPTOR_HANDLE targets[7];for(auto& t:targets){t=first_rtv;t.ptr+=6*step;}targets[0]=first_rtv;targets[0].ptr+=(mode?1:0)*step;targets[4]=first_rtv;targets[4].ptr+=(mode?3:2)*step;targets[5]=first_rtv;targets[5].ptr+=4*step;targets[6]=first_rtv;targets[6].ptr+=5*step;f->list->OMSetRenderTargets(mode?7:5,targets,FALSE,nullptr);f->list->SetPipelineState(f->states[test*3+(mode==1?2:mode==2?1:0)].Get());f->list->DrawInstanced(3,1,0,0);
            if(mode==1){
                for(unsigned k=0;k<4;++k){const unsigned i=k==0?5:k==1?1:k==2?3:4;auto* resource=f->textures[i].Get();Barrier(f->list.Get(),resource,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=resource;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=(test*10+6+k)*image;to.PlacedFootprint.Footprint={i==1?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_R16G16B16A16_FLOAT,side,side,1,256};f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),resource,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);}
                for(unsigned i:{1u,3u,4u}){auto rtv=first_rtv;rtv.ptr+=i*step;f->list->ClearRenderTargetView(rtv,i==1?legacy:seed,0,nullptr);}
            }
        }
        for(unsigned i=0;i<6;++i){auto* resource=f->textures[i].Get();Barrier(f->list.Get(),resource,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=resource;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=(test*10+i)*image;to.PlacedFootprint.Footprint={i<2?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_R16G16B16A16_FLOAT,side,side,1,256};f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),resource,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);}
    }
    if(FAILED(f->list->Close()))return false;ID3D12CommandList* lists[]={f->list.Get()};provider.GetDirectQueue()->ExecuteCommandLists(1,lists);if(FAILED(provider.GetDirectQueue()->Signal(f->fence.Get(),1)) || FAILED(f->fence->SetEventOnCompletion(1,f->event)) || WaitForSingleObject(f->event,15000)!=WAIT_OBJECT_0){f.release();return false;}
    D3D12_RANGE read{0,cases*10*image};if(FAILED(f->readback->Map(0,&read,&mapped)))return false;const auto* bytes=static_cast<const uint8_t*>(mapped);unsigned bad=0,changed=0;double maximum=0;
    for(unsigned test=0;test<cases;++test)for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x){
        const bool covered=x>=3 && x<7 && y>=1 && y<7;for(unsigned c=0;c<4;++c){uint16_t bits;std::memcpy(&bits,bytes+(test*10+5)*image+y*256+x*8+c*2,2);const double expected=covered?(c==3?-1:0):surface_seed[c];bad+=!std::isfinite(Half(bits)) || Half(bits)!=expected;}
const auto offset=y*256+x*4;bad+=std::memcmp(bytes+(test*10)*image+offset,bytes+(test*10+1)*image+offset,4)!=0;changed+=covered && std::memcmp(bytes+(test*10)*image+offset,bytes+offset,4)!=0;
        bad+=std::memcmp(bytes+(test*10+2)*image+y*256+x*8,bytes+(test*10+3)*image+y*256+x*8,8)!=0;
        bad+=std::memcmp(bytes+(test*10)*image+y*256+x*4,bytes+(test*10+7)*image+y*256+x*4,4)!=0;
        bad+=std::memcmp(bytes+(test*10+2)*image+y*256+x*8,bytes+(test*10+8)*image+y*256+x*8,8)!=0;
        bad+=std::memcmp(bytes+(test*10+4)*image+y*256+x*8,bytes+(test*10+9)*image+y*256+x*8,8)!=0;
        for(unsigned c=0;c<4;++c){uint16_t bits;std::memcpy(&bits,bytes+(test*10+6)*image+y*256+x*8+c*2,2);bad+=Half(bits)!=surface_seed[c];}

        for(unsigned c=0;c<4;++c){uint16_t bits;std::memcpy(&bits,bytes+(test*10+4)*image+y*256+x*8+c*2,2);double expected=seed[c];if(covered && c<3 && ((test!=5 && test!=12) || c==0)){double factor=test==0 || test==5 || test==7?0:test==2?1:test==3?.25:test==4?-.75:.75;if(test>=8){factor=factor_colour[c]*(c==1?2:.5);if(test==9 || test==13)factor=1-factor;if(test==10)factor=-factor;if(test>=14){const double source=factor_colour[c+4];const double dest=test==14?0:test==15?1:test==16?source:test==17?1-source:.25;factor=test==18?source-dest:test==19?dest-source:source+dest;}}expected*=factor;}const auto delta=std::abs(Half(bits)-expected);maximum=std::max(maximum,delta);bad+=!std::isfinite(Half(bits)) || delta!=0;}
    }
    f->readback->Unmap(0,&none);unsigned errors=0;if(validation)for(uint64_t i=first;i<validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;validation->GetMessage(i,nullptr,&size);std::vector<uint8_t> storage(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());if(SUCCEEDED(validation->GetMessage(i,message,&size)) && message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;REXLOG_ERROR("Ambient clear validation: {}",message->pDescription);}}
    std::filesystem::create_directories(directory);std::ofstream report(directory/"material-clear-results.json");report<<"{\"draws\":60,\"ambient_components\":5120,\"surface_components\":5120,\"preserved_surface_components\":5120,\"invalid\":"<<bad<<",\"max_error\":"<<maximum<<",\"primary_changes\":"<<changed<<",\"device_errors\":"<<errors<<",\"scope\":\"Generic DXBC colour0/HDR4 preserved; ambient follows opaque/masked coverage, alpha/add/subtract and source-destination-colour or destination-source-colour/inverse-source-colour blending. Twelve colour-factor cases use HDR RGB distinct from colour0, negative and above-one values, operations and masks. Pixel discard/scissor preserve surrounding values and alpha. Private surface6 becomes RGB0/alpha-1 only on covered pixels, independent of original blend factors and masks; high-range surrounding surface values preserved. Preserve policy uses unchanged prepared pixel code and zero surface write mask; every surface component stays unchanged while original colour/HDR and ambient match the control/clear variants. Missing/mismatched HDR factor and unsupported blend/logic states reject. Controlled shaders and buffers, no live material coverage.\"}\n";REXLOG_INFO("Ambient material clear:60 draws, invalid {}, error {}, device errors {}",bad,maximum,errors);return !bad && !errors && changed && report.good();
}
