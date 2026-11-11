#include "scene_shader_audit.h"
#include "scene_raster_geometry.h"
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
#include <limits>
#include <vector>
using namespace em2::pixel_test;
bool em2::TestNativeFogVertex(rex::ui::d3d12::D3D12Provider& provider,const std::filesystem::path& output){
    constexpr unsigned side=16,cases=6;constexpr uint64_t stride=4096,image=side*side*16;
    struct GeometryFixture:Fixture{std::array<SceneRasterGeometryKernel,4> geometry;};
    auto f=std::make_unique<GeometryFixture>();auto* device=provider.GetDevice();ComPtr<ID3D12InfoQueue> validation;device->QueryInterface(IID_PPV_ARGS(&validation));const auto first=validation?validation->GetNumStoredMessagesAllowedByRetrievalFilter():0;
    const char* stage="initialization";bool complete=false;
    struct Failure{const char*& stage;bool& complete;ID3D12InfoQueue* queue;uint64_t first;~Failure(){if(complete)return;REXLOG_ERROR("Native fog vertex test failed at {}",stage);if(queue)for(uint64_t i=first;i<queue->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;queue->GetMessage(i,nullptr,&size);std::vector<uint8_t> bytes(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());if(SUCCEEDED(queue->GetMessage(i,m,&size)) && m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)REXLOG_ERROR("Fog vertex device: {}",m->pDescription);}}} failure{stage,complete,validation.Get(),first};
    if(!f->event || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f->allocator))) || FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f->allocator.Get(),nullptr,IID_PPV_ARGS(&f->list))) || FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f->fence))))return false;
    auto library=LoadLibraryW(L"d3dcompiler_47.dll");if(!library)return false;struct Library{HMODULE value;~Library(){FreeLibrary(value);}} release{library};
    auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(library,"D3DCompile"));if(!compile)return false;
    constexpr char ps[]=R"(struct V{float4 t[16]:TEXCOORD0;float4 p:SV_Position;};float4 main(V i):SV_Target0{return float4(i.t[0].xy,i.t[1].xy);})";
    ComPtr<ID3DBlob> pixel,errors;if(FAILED(compile(ps,sizeof(ps)-1,"fog-vertex-observer",nullptr,nullptr,"main","ps_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_IEEE_STRICTNESS,0,&pixel,&errors)))return false;
    D3D12_ROOT_PARAMETER parameters[5]{};for(unsigned i=0;i<3;++i){parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;parameters[i].Descriptor.ShaderRegister=i==2?3:i;parameters[i].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;}
    parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[3].Descriptor.ShaderRegister=0;parameters[3].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[4].Descriptor.ShaderRegister=0;parameters[4].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;
    const D3D12_ROOT_SIGNATURE_DESC root{5,parameters};ComPtr<ID3DBlob> serialized;
    if(FAILED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors)) || FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&f->root))))return false;
    for(unsigned variant=0;variant<4;++variant){stage="translation/pipeline";auto code=scene_shader_data::depth_fog_vertex_linked_example;
        if(variant%2){code[3*3+2]=(code[3*3+2]&~255u)|3;code[4*3+2]=(code[4*3+2]&~255u)|3;code[4*3+1]=(code[4*3+1]&~(63u<<16))|(31u<<16);}
        rex::graphics::DxbcShader vertex(rex::graphics::xenos::ShaderType::kVertex,variant+1,code.data(),code.size(),std::endian::native);rex::string::StringBuffer disasm;vertex.AnalyzeUcode(disasm);
        rex::graphics::DxbcShaderTranslator translator(provider.GetAdapterVendorID(),variant>=2,false,false,true,1,1);auto* translated=vertex.GetOrCreateTranslation(translator.GetDefaultVertexShaderModification(0));
        if(!translator.TranslateAnalyzedShader(*translated) || !translated->is_valid() || vertex.constant_register_map().GetPackedFloatConstantIndex(0)!=0 || vertex.constant_register_map().GetPackedFloatConstantIndex(255)!=1)return false;
        if(!f->geometry[variant].Initialize(device,translated->translated_binary()))return false;
        if(variant>=2)continue; // Bindless VS compatibility checked by geometry draws below.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=f->root.Get();pipeline.VS={translated->translated_binary().data(),translated->translated_binary().size()};pipeline.PS={pixel->GetBufferPointer(),pixel->GetBufferSize()};
        pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask=15;pipeline.SampleMask=UINT_MAX;pipeline.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pipeline.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pipeline.RasterizerState.DepthClipEnable=TRUE;
        pipeline.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pipeline.NumRenderTargets=1;pipeline.RTVFormats[0]=DXGI_FORMAT_R32G32B32A32_FLOAT;pipeline.SampleDesc.Count=1;
        if(FAILED(device->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&f->pipelines[variant]))))return false;
    }
    stage="resources";D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;texture.Width=texture.Height=side;texture.DepthOrArraySize=texture.MipLevels=texture.SampleDesc.Count=1;texture.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC writable{};writable.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;writable.Width=cases*stride;writable.Height=writable.DepthOrArraySize=writable.MipLevels=writable.SampleDesc.Count=1;writable.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;writable.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&writable,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&f->textures[0]))))return false;
    if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&f->target))))return false;
    auto normal_desc=texture;normal_desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&normal_desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&f->textures[1]))))return false;
    auto depth_desc=texture;depth_desc.Flags=D3D12_RESOURCE_FLAG_NONE;for(unsigned i=2;i<=3;++i)if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&depth_desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&f->textures[i]))))return false;
    D3D12_DESCRIPTOR_HEAP_DESC rtv{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,2,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};if(FAILED(device->CreateDescriptorHeap(&rtv,IID_PPV_ARGS(&f->rtvs))))return false;auto normal_rtv=f->rtvs->GetCPUDescriptorHandleForHeapStart();device->CreateRenderTargetView(f->target.Get(),nullptr,normal_rtv);normal_rtv.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);device->CreateRenderTargetView(f->textures[1].Get(),nullptr,normal_rtv);
    const D3D12_DESCRIPTOR_HEAP_DESC views{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,4,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0},samplers{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,1,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};if(FAILED(device->CreateDescriptorHeap(&views,IID_PPV_ARGS(&f->views))) || FAILED(device->CreateDescriptorHeap(&samplers,IID_PPV_ARGS(&f->samplers))))return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=texture.Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2DArray.MipLevels=srv.Texture2DArray.ArraySize=1;for(unsigned i=0;i<2;++i){auto handle=f->views->GetCPUDescriptorHandleForHeapStart();handle.ptr+=i*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);device->CreateShaderResourceView(f->textures[2+i].Get(),&srv,handle);}
    D3D12_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.MaxLOD=FLT_MAX;sampler.MaxAnisotropy=1;sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_NEVER;device->CreateSampler(&sampler,f->samplers->GetCPUDescriptorHandleForHeapStart());
    if(!Buffer(device,cases*stride+2*image,D3D12_HEAP_TYPE_UPLOAD,&f->upload) || !Buffer(device,3*cases*image,D3D12_HEAP_TYPE_READBACK,&f->readback))return false;
    uint8_t* mapped=nullptr;D3D12_RANGE none{};if(FAILED(f->upload->Map(0,&none,reinterpret_cast<void**>(&mapped))))return false;std::memset(mapped,0,cases*stride+2*image);
    auto raw_handle=f->views->GetCPUDescriptorHandleForHeapStart();const auto view_step=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);raw_handle.ptr+=2*view_step;
    D3D12_SHADER_RESOURCE_VIEW_DESC raw{};raw.Format=DXGI_FORMAT_R32_TYPELESS;raw.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;raw.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;raw.Buffer.Flags=D3D12_BUFFER_SRV_FLAG_RAW;raw.Buffer.NumElements=uint32_t((cases*stride+2*image)/4);device->CreateShaderResourceView(f->upload.Get(),&raw,raw_handle);raw_handle.ptr+=view_step;
    D3D12_UNORDERED_ACCESS_VIEW_DESC raw_uav{};raw_uav.Format=DXGI_FORMAT_R32_TYPELESS;raw_uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;raw_uav.Buffer.NumElements=1024;raw_uav.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;device->CreateUnorderedAccessView(f->textures[0].Get(),nullptr,&raw_uav,raw_handle);
    std::array<RasterGeometryConstants,cases> geometry_constants{};
    for(unsigned pattern=0;pattern<2;++pattern)for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x){const float red=pattern&&x<3?.9f:pattern&&x==7&&y==7?std::numeric_limits<float>::quiet_NaN():.1f+.005f*x+.003f*y;const Float4 depth{red,0,0,1};std::memcpy(mapped+cases*stride+pattern*image+y*side*16+x*16,depth.data(),16);}
    const float scales[cases][2]={{1,1},{1,1},{1,1},{.75f,1.25f},{1,-1},{-1,.75f}};
    const float offsets[cases][2]={{0,0},{0,0},{1.f/side,-1.f/side},{.125f,-.1875f},{0,0},{.25f,.125f}};
    for(unsigned test=0;test<cases;++test){auto* block=mapped+test*stride;auto& system=*reinterpret_cast<rex::graphics::DxbcShaderTranslator::SystemConstants*>(block);static_assert(sizeof(system)<=1024);
        SceneDrawInputs scene{};for(unsigned i=0;i<4;++i)scene.inverse_view_projection[i][i]=1;
        if(test==3){scene.inverse_view_projection[2][0]=.25f;scene.inverse_view_projection[2][1]=.125f;}
        if(test==5){const float camera[]={10000000,-20000000,30000000};for(unsigned i=0;i<3;++i)scene.inverse_view[i][3]=scene.inverse_view_projection[i][3]=camera[i];}
        if(!PrepareRasterGeometryConstants(scene,geometry_constants[test]))return false;
        auto bad=scene;bad.inverse_view_projection[3]={0,0,0,0};auto reject=geometry_constants[test];if(PrepareRasterGeometryConstants(bad,reject) || reject.relative_inverse!=geometry_constants[test].relative_inverse)return false;
        const uint16_t indices[]={0,0,0,1,2};std::memcpy(block+3584,indices,sizeof(indices));
        system.vertex_index_max=UINT32_MAX;for(unsigned axis=0;axis<3;++axis){system.ndc_scale[axis]=axis<2?scales[test][axis]:1;system.ndc_offset[axis]=axis<2?offsets[test][axis]:0;}
        auto* constants=reinterpret_cast<Float4*>(block+1024);constants[0]={side,side,test%2?1.f/side:0,test%2?1.f/side:0};
        for(unsigned c=0;c<4;++c)constants[1][c]=std::bit_cast<float>(scene_shader_data::depth_fog_vertex_literals[c]);
        rex::graphics::xenos::xe_gpu_vertex_fetch_t fetch{};fetch.type=rex::graphics::xenos::FetchConstantType::kVertex;fetch.address=uint32_t((test*stride+3072)/4);fetch.size=test==5?9:12;
        std::memcpy(block+2048+95*8,&fetch,sizeof(fetch));
        const float uv[3][2]={{0,0},{2,0},{0,2}};
        for(unsigned v=0;v<3;++v){const float host[]={uv[v][0]*2-1,1-uv[v][1]*2};float values[4];values[0]=((host[0]-offsets[test][0])/scales[test][0]+1+constants[0][2])*.5f;values[1]=(1+constants[0][3]-(host[1]-offsets[test][1])/scales[test][1])*.5f;values[2]=uv[v][0];values[3]=uv[v][1];
            auto* destination=block+3072+v*(test==5?12:16);std::memcpy(destination,values,test==5?8:16);if(test==5){const uint32_t packed=(v==1?0x4000u:0u)|(v==2?0x40000000u:0u);std::memcpy(destination+8,&packed,4);}}
    }
    f->upload->Unmap(0,nullptr);f->list->SetGraphicsRootSignature(f->root.Get());f->list->SetGraphicsRootShaderResourceView(3,f->upload->GetGPUVirtualAddress());f->list->SetGraphicsRootUnorderedAccessView(4,f->textures[0]->GetGPUVirtualAddress());
    D3D12_TEXTURE_COPY_LOCATION depth_from{},depth_to{};depth_from.pResource=f->upload.Get();depth_from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;depth_from.PlacedFootprint.Footprint={texture.Format,side,side,1,side*16};depth_to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;for(unsigned i=0;i<2;++i){depth_from.PlacedFootprint.Offset=cases*stride+i*image;depth_to.pResource=f->textures[2+i].Get();f->list->CopyTextureRegion(&depth_to,0,0,0,&depth_from,nullptr);Barrier(f->list.Get(),f->textures[2+i].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);}
    ID3D12DescriptorHeap* heaps[]={f->views.Get(),f->samplers.Get()};f->list->SetDescriptorHeaps(2,heaps);
    D3D12_VIEWPORT viewport{0,0,float(side),float(side),0,1};D3D12_RECT scissor{0,0,side,side};f->list->RSSetViewports(1,&viewport);f->list->RSSetScissorRects(1,&scissor);f->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const auto target=f->rtvs->GetCPUDescriptorHandleForHeapStart();f->list->OMSetRenderTargets(1,&target,FALSE,nullptr);
    for(unsigned test=0;test<cases;++test){const auto base=f->upload->GetGPUVirtualAddress()+test*stride;const float sentinel[4]={-999,-999,-999,-999};f->list->ClearRenderTargetView(target,sentinel,0,nullptr);f->list->SetPipelineState(f->pipelines[test==5?1:0].Get());
        f->list->SetGraphicsRootSignature(f->root.Get());f->list->SetGraphicsRootShaderResourceView(3,f->upload->GetGPUVirtualAddress());f->list->SetGraphicsRootUnorderedAccessView(4,f->textures[0]->GetGPUVirtualAddress());f->list->OMSetRenderTargets(1,&target,FALSE,nullptr);
        for(unsigned i=0;i<3;++i)f->list->SetGraphicsRootConstantBufferView(i,base+i*1024);f->list->DrawInstanced(3,1,0,0);
        Barrier(f->list.Get(),f->target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=f->target.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=test*image;to.PlacedFootprint.Footprint={texture.Format,side,side,1,side*16};f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),f->target.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto& geometry=f->geometry[test==5?3:test==2?2:0];f->list->SetGraphicsRootSignature(geometry.root());f->list->SetPipelineState(geometry.pipeline());
        for(unsigned i=0;i<3;++i)f->list->SetGraphicsRootConstantBufferView(i,base+i*1024);for(unsigned i=5;i<=6;++i)f->list->SetGraphicsRootConstantBufferView(i,base);
        auto memory_handle=f->views->GetGPUDescriptorHandleForHeapStart();memory_handle.ptr+=2*view_step;f->list->SetGraphicsRootDescriptorTable(3,memory_handle);memory_handle.ptr+=view_step;f->list->SetGraphicsRootDescriptorTable(4,memory_handle);auto depth_handle=f->views->GetGPUDescriptorHandleForHeapStart();if(test==4)depth_handle.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);f->list->SetGraphicsRootDescriptorTable(7,depth_handle);f->list->SetGraphicsRootDescriptorTable(8,f->samplers->GetGPUDescriptorHandleForHeapStart());f->list->SetGraphicsRoot32BitConstants(9,20,&geometry_constants[test],0);
        const D3D12_CPU_DESCRIPTOR_HANDLE targets[]={target,normal_rtv};const float clear[4]={};f->list->OMSetRenderTargets(2,targets,FALSE,nullptr);f->list->ClearRenderTargetView(target,clear,0,nullptr);f->list->ClearRenderTargetView(normal_rtv,clear,0,nullptr);
        if(test%2){const D3D12_INDEX_BUFFER_VIEW indices{base+3584,10,DXGI_FORMAT_R16_UINT};f->list->IASetIndexBuffer(&indices);f->list->DrawIndexedInstanced(3,2,2,0,7);}else f->list->DrawInstanced(3,1,0,0);
        for(unsigned output=0;output<2;++output){auto* resource=output?f->textures[1].Get():f->target.Get();Barrier(f->list.Get(),resource,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);from.pResource=resource;to.PlacedFootprint.Offset=((output+1)*cases+test)*image;to.PlacedFootprint.Footprint={output?normal_desc.Format:texture.Format,side,side,1,256};f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),resource,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);}
    }
    stage="submission";if(FAILED(f->list->Close()))return false;ID3D12CommandList* lists[]={f->list.Get()};provider.GetDirectQueue()->ExecuteCommandLists(1,lists);
    if(FAILED(provider.GetDirectQueue()->Signal(f->fence.Get(),1)) || FAILED(f->fence->SetEventOnCompletion(1,f->event)) || WaitForSingleObject(f->event,15000)!=WAIT_OBJECT_0){f.release();return false;}
    const Float4* pixels=nullptr;D3D12_RANGE range{0,3*cases*image};if(FAILED(f->readback->Map(0,&range,reinterpret_cast<void**>(const_cast<Float4**>(&pixels)))))return false;
    unsigned invalid=0;double error=0;
    for(unsigned test=0;test<cases;++test)for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)for(unsigned c=0;c<4;++c){const double uv[]={ (double(x)+.5)/side,(double(y)+.5)/side};const double expected=c<2?((c==0?2*uv[0]-1:1-2*uv[1])-offsets[test][c])/scales[test][c]:uv[c-2];const double value=pixels[test*side*side+y*side+x][c];const double delta=std::abs(value-expected);error=std::max(error,delta);invalid+=!std::isfinite(value)||delta>2e-5;}
    unsigned geometry_bad=0;double position_error=0,normal_error=0;
    const auto* bytes=reinterpret_cast<const uint8_t*>(pixels);
    for(unsigned test=0;test<cases;++test)for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x){
        const double clip[]={((2*(double(x)+.5)/side-1)-offsets[test][0])/scales[test][0],((1-2*(double(y)+.5)/side)-offsets[test][1])/scales[test][1]};
        const double ax=test==3?.25:0,ay=test==3?.125:0;
        const bool foreground=test==4&&x<3,invalid_position=test==4&&x==7&&y==7;
        double world[]={clip[0],clip[1],foreground?.1:.9-.005*x-.003*y+ax*clip[0]+ay*clip[1],1};if(invalid_position)for(double& value:world)value=0;
        const double dx=2./(side*scales[test][0]),dy=-2./(side*scales[test][1]),dzx=ax*dx-(foreground?0:.005),dzy=ay*dy-(foreground?0:.003);
        double normal[]={-dzx*dy,-dx*dzy,dx*dy,1};double length=std::sqrt(normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2]);for(unsigned c=0;c<3;++c)normal[c]/=length;
        if(normal[0]*world[0]+normal[1]*world[1]+normal[2]*world[2]>0)for(unsigned c=0;c<3;++c)normal[c]=-normal[c];
        if(test==4&&(x==2||x==3||(x==6&&y==7)||(x==7&&y==6)||invalid_position))for(double& value:normal)value=0;
        for(unsigned c=0;c<4;++c){float position;uint16_t half;std::memcpy(&position,bytes+(cases+test)*image+y*256+x*16+c*4,4);std::memcpy(&half,bytes+(2*cases+test)*image+y*256+x*8+c*2,2);
            const unsigned e=(half>>10)&31,m=half&1023;double n=e?std::ldexp(1.+m/1024.,int(e)-15):std::ldexp(double(m),-24);if(half&32768)n=-n;
            const double dp=std::abs(position-world[c]),dn=std::abs(n-normal[c]);position_error=std::max(position_error,dp);normal_error=std::max(normal_error,dn);geometry_bad+=!std::isfinite(position)||!std::isfinite(n)||dp>2e-5||dn>5e-4;
        }
    }
    std::filesystem::create_directories(output);std::ofstream geometry_report(output/"raster-geometry-results.json");geometry_report<<"{\"draws\":6,\"components\":12288,\"invalid\":"<<geometry_bad<<",\"position_error\":"<<position_error<<",\"normal_error\":"<<normal_error<<",\"scope\":\"Original linked bindful/bindless native VS with geometry MRT pixel kernel, indexed/nonindexed instanced draws, point-sampled varying depth, independent clip/UV, half-pixel constants, NDC scale/offset/flips, tilted plane, large camera origin, depth discontinuity and invalid-depth derivative guards. Synthetic mesh, not captured game replay.\"}\n";
    REXLOG_INFO("Native raster geometry: invalid{}, position error{}, normal error{}",geometry_bad,position_error,normal_error);
    f->readback->Unmap(0,&none);unsigned device_errors=0;if(validation)for(uint64_t i=first;i<validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;validation->GetMessage(i,nullptr,&size);std::vector<uint8_t> bytes(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());if(SUCCEEDED(validation->GetMessage(i,message,&size))&&message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++device_errors;REXLOG_ERROR("Native fog vertex validation: {}",message->pDescription);}}
    std::filesystem::create_directories(output);std::ofstream report(output/"fog-vertex-results.json");report<<"{\"draws\":"<<cases<<",\"components\":"<<cases*side*side*4<<",\"invalid\":"<<invalid<<",\"max_error\":"<<error<<",\"device_errors\":"<<device_errors<<",\"scope\":\"Original fog VS arithmetic with runtime-style linked float32/float16 declarations, native shared-memory vertex loads and real rasterization; clip/UV versus CPU oracle under half-pixel constants and NDC scale/offset/flips. Synthetic vertices, not actual game draw.\"}\n";
    REXLOG_INFO("Native fog vertex/raster: {} components, invalid{}, error{}, device errors{}",cases*side*side*4,invalid,error,device_errors);complete=report.good()&&geometry_report.good()&&!geometry_bad&&!invalid&&!device_errors;return complete;
}
