#include "scene_indirect_benchmark.h"
#include "built_shaders.h"
#include "native_pixel_fixture.h"
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/logging.h>
#include <d3d12sdklayers.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <immintrin.h>
#include <vector>
namespace {
using namespace em2;using namespace em2::pixel_test;
constexpr unsigned variants=3,rounds=12,warmups=3,samples=rounds-warmups;
constexpr BuiltShader shaders[]={BuiltShader::SceneIndirect,BuiltShader::SceneIndirectLoopEmitters,BuiltShader::SceneIndirectLoopAll};
constexpr auto read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
struct Bench:Fixture {
    ComPtr<ID3D12QueryHeap> queries;ComPtr<ID3D12Resource> query_readback;
    std::array<ComPtr<ID3D12Resource>,variants> outputs;
    uint64_t fence_value=0;
};
uint16_t Half(float value){
    const auto bits=std::bit_cast<uint32_t>(value);const int e=int((bits>>23)&255)-127+15;
    if(e<=0)return 0;if(e>=31)return uint16_t((bits>>16)&32768)|0x7c00;
    return uint16_t(((bits>>16)&32768)|(e<<10)|((bits&0x7fffff)>>13));
}
float Float(uint16_t bits){
    const unsigned e=(bits>>10)&31,m=bits&1023;
    if(e==31)return m?NAN:(bits&32768?-INFINITY:INFINITY);
    const float v=e?std::ldexp(float(1024+m),int(e)-25):std::ldexp(float(m),-24);
    return bits&32768?-v:v;
}
bool Texture(ID3D12Device* device,unsigned width,unsigned height,DXGI_FORMAT format,bool output,ID3D12Resource** resource){
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=width;d.Height=height;d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=format;d.Flags=output?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;
    return SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,output?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(resource)));
}
struct Result {
    unsigned width,height,pattern,differences[2]{},invalid=0,positive=0;
    double median[variants]{},gpu_ms[variants][samples]{},max_error[2]{};
};
bool Run(const rex::ui::d3d12::D3D12Provider& provider,unsigned width,unsigned height,unsigned pattern,Result& result){
    auto* device=provider.GetDevice();auto* queue=provider.GetDirectQueue();uint64_t frequency=0;
    if(!device || !queue || FAILED(queue->GetTimestampFrequency(&frequency)) || !frequency)return false;
    auto f=std::make_unique<Bench>();
    if(!f->event || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f->allocator))) || FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f->allocator.Get(),nullptr,IID_PPV_ARGS(&f->list))) || FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f->fence))))return false;
    const D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP,samples*variants*2};
    if(FAILED(device->CreateQueryHeap(&query_desc,IID_PPV_ARGS(&f->queries))) || !Buffer(device,samples*variants*16,D3D12_HEAP_TYPE_READBACK,&f->query_readback))return false;
    D3D12_ROOT_PARAMETER parameters[6]{};D3D12_DESCRIPTOR_RANGE ranges[5]{};
    for(unsigned i=0;i<5;++i){ranges[i]={i<4?D3D12_DESCRIPTOR_RANGE_TYPE_SRV:D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,i<4?i:0,0,0};parameters[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[i+1].DescriptorTable={1,&ranges[i]};}
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants={0,0,8};
    const D3D12_ROOT_SIGNATURE_DESC root{6,parameters};ComPtr<ID3DBlob> serialized,error;
    if(FAILED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error)) || FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&f->root))))return false;
    for(unsigned i=0;i<variants;++i){const auto* shader=GetBuiltShader(shaders[i]);if(!shader)return false;D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=f->root.Get();desc.CS={shader->GetBufferPointer(),shader->GetBufferSize()};if(FAILED(device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&f->pipelines[i]))))return false;}
    const D3D12_DESCRIPTOR_HEAP_DESC views{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,4+variants,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    if(FAILED(device->CreateDescriptorHeap(&views,IID_PPV_ARGS(&f->views))))return false;
    const auto increment=device->GetDescriptorHandleIncrementSize(views.Type);auto cpu=f->views->GetCPUDescriptorHandleForHeapStart();
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,4> layouts{};uint64_t upload_bytes=0;
    for(unsigned i=0;i<4;++i){const auto format=i==2?DXGI_FORMAT_R32G32B32A32_FLOAT:DXGI_FORMAT_R16G16B16A16_FLOAT;if(!Texture(device,width,height,format,false,&f->textures[i]))return false;
        const auto d=f->textures[i]->GetDesc();device->GetCopyableFootprints(&d,0,1,upload_bytes,&layouts[i],nullptr,nullptr,nullptr);upload_bytes=layouts[i].Offset+uint64_t(layouts[i].Footprint.RowPitch)*height;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;device->CreateShaderResourceView(f->textures[i].Get(),&srv,cpu);cpu.ptr+=increment;
    }
    const unsigned out_width=(width+1)/2,out_height=(height+1)/2;
    for(auto& output:f->outputs){if(!Texture(device,out_width,out_height,DXGI_FORMAT_R16G16B16A16_FLOAT,true,&output))return false;D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device->CreateUnorderedAccessView(output.Get(),nullptr,&uav,cpu);cpu.ptr+=increment;}
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT output_layout{};uint64_t output_bytes=0;const auto output_desc=f->outputs[0]->GetDesc();device->GetCopyableFootprints(&output_desc,0,1,0,&output_layout,nullptr,nullptr,&output_bytes);
    // Each output's placed footprint must start at the native 512-byte alignment.
    const uint64_t output_stride=(output_bytes+511)&~uint64_t(511);
    if(!Buffer(device,upload_bytes,D3D12_HEAP_TYPE_UPLOAD,&f->upload) || !Buffer(device,output_stride*variants,D3D12_HEAP_TYPE_READBACK,&f->readback))return false;
    void* mapped=nullptr;D3D12_RANGE none{};if(FAILED(f->upload->Map(0,&none,&mapped)))return false;
    std::memset(mapped,0,upload_bytes);
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
        const unsigned local=x%64;const bool wall=pattern==0 && local>=32;const float tile=float(x/64)*1.2f;
        const std::array<Float4,4> values{{{wall?8.f:.02f,wall?2.f:.03f,wall?.5f:.01f,.625f},{.5f,.25f,.75f,pattern==2?-1.f:16.f},{tile+(wall?.32f:local*.01f),y*.01f,wall?(local-32)*.01f:0.f,1},{wall?-1.f:0.f,0,wall?0.f:1.f,1}}};
        for(unsigned i=0;i<4;++i){auto* dst=static_cast<uint8_t*>(mapped)+layouts[i].Offset+y*layouts[i].Footprint.RowPitch+x*(i==2?16:8);for(unsigned c=0;c<4;++c){if(i==2)std::memcpy(dst+c*4,&values[i][c],4);else{const auto bits=Half(values[i][c]);std::memcpy(dst+c*2,&bits,2);}}}
    }
    f->upload->Unmap(0,&none);
    for(unsigned i=0;i<4;++i){D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=f->upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=layouts[i];to.pResource=f->textures[i].Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),to.pResource,D3D12_RESOURCE_STATE_COPY_DEST,read);}
    auto submit=[&]{
        if(FAILED(f->list->Close()))return false;ID3D12CommandList* lists[]={f->list.Get()};queue->ExecuteCommandLists(1,lists);++f->fence_value;
        if(FAILED(queue->Signal(f->fence.Get(),f->fence_value)) || FAILED(f->fence->SetEventOnCompletion(f->fence_value,f->event)) || WaitForSingleObject(f->event,15000)!=WAIT_OBJECT_0){f.release();return false;}
        return SUCCEEDED(f->allocator->Reset()) && SUCCEEDED(f->list->Reset(f->allocator.Get(),nullptr));
    };
    if(!submit())return false;
    for(unsigned round=0;round<rounds;++round){
        ID3D12DescriptorHeap* heaps[]={f->views.Get()};f->list->SetDescriptorHeaps(1,heaps);f->list->SetComputeRootSignature(f->root.Get());
        struct Constants {uint32_t width,height,out_width,out_height;float radius,pixel_radius,thickness,strength;}constants{width,height,out_width,out_height,2,16,.03f,.25f};
        f->list->SetComputeRoot32BitConstants(0,8,&constants,0);
        auto gpu=f->views->GetGPUDescriptorHandleForHeapStart();for(unsigned i=0;i<4;++i){f->list->SetComputeRootDescriptorTable(i+1,gpu);gpu.ptr+=increment;}
        for(unsigned order=0;order<variants;++order){const unsigned variant=(round+order)%variants;auto output=gpu;output.ptr+=variant*increment;
            f->list->SetComputeRootDescriptorTable(5,output);f->list->SetPipelineState(f->pipelines[variant].Get());
            const unsigned query=round>=warmups?((round-warmups)*variants+variant)*2:0;
            if(round>=warmups)f->list->EndQuery(f->queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,query);
            f->list->Dispatch((out_width+7)/8,(out_height+7)/8,1);
            if(round>=warmups)f->list->EndQuery(f->queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,query+1);
            D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;barrier.UAV.pResource=f->outputs[variant].Get();f->list->ResourceBarrier(1,&barrier);
        }
        // A fence per short round bounds submitted work and prevents a long
        // benchmark list from provoking a driver timeout on slower hardware.
        if(!submit())return false;
    }
    for(unsigned i=0;i<variants;++i){Barrier(f->list.Get(),f->outputs[i].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=f->outputs[i].Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=output_layout;to.PlacedFootprint.Offset=i*output_stride;f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);}
    f->list->ResolveQueryData(f->queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,samples*variants*2,f->query_readback.Get(),0);if(!submit())return false;
    D3D12_RANGE timing_range{0,samples*variants*16};if(FAILED(f->query_readback->Map(0,&timing_range,&mapped)))return false;const auto* ticks=static_cast<const uint64_t*>(mapped);
    for(unsigned variant=0;variant<variants;++variant){std::vector<double> times;for(unsigned sample=0;sample<samples;++sample){const unsigned q=(sample*variants+variant)*2;if(ticks[q+1]<=ticks[q])return false;result.gpu_ms[variant][sample]=double(ticks[q+1]-ticks[q])*1000/frequency;times.push_back(result.gpu_ms[variant][sample]);}std::sort(times.begin(),times.end());result.median[variant]=times[samples/2];}f->query_readback->Unmap(0,&none);
    D3D12_RANGE pixels_range{0,output_stride*variants};if(FAILED(f->readback->Map(0,&pixels_range,&mapped)))return false;const auto* pixels=static_cast<const uint8_t*>(mapped);
    for(unsigned y=0;y<out_height;++y)for(unsigned x=0;x<out_width;++x)for(unsigned c=0;c<4;++c){uint16_t reference;const auto offset=y*output_layout.Footprint.RowPitch+x*8+c*2;std::memcpy(&reference,pixels+offset,2);const float expected=Float(reference);result.invalid+=!std::isfinite(expected);if(c<3)result.positive+=expected>0;
        for(unsigned v=1;v<variants;++v){uint16_t bits;std::memcpy(&bits,pixels+v*output_stride+offset,2);const float actual=Float(bits);result.invalid+=!std::isfinite(actual);result.differences[v-1]+=bits!=reference;result.max_error[v-1]=std::max(result.max_error[v-1],double(std::abs(actual-expected)));}}
    f->readback->Unmap(0,&none);result.width=width;result.height=height;result.pattern=pattern;
    return !result.invalid && (pattern!=0 || result.positive);
}
}
bool em2::TestSceneIndirectPerformance(const rex::ui::d3d12::D3D12Provider& provider,const std::filesystem::path& directory){
    struct Fp {unsigned value=_mm_getcsr();~Fp(){_mm_setcsr(value);}}fp;
    auto* device=provider.GetDevice();if(!device)return false;const auto luid=device->GetAdapterLuid();uint64_t frequency=0;if(FAILED(provider.GetDirectQueue()->GetTimestampFrequency(&frequency)) || !frequency)return false;
    ComPtr<ID3D12InfoQueue> validation;device->QueryInterface(IID_PPV_ARGS(&validation));const auto first=validation?validation->GetNumStoredMessagesAllowedByRetrievalFilter():0;
    std::vector<Result> results;
    for(auto dimensions:{std::array<unsigned,2>{1280,720},{1920,1080},{2560,1440}})for(unsigned pattern=0;pattern<3;++pattern){Result result{};if(!Run(provider,dimensions[0],dimensions[1],pattern,result))return false;results.push_back(result);REXLOG_INFO("Indirect loop benchmark {}x{} pattern{}: unrolled{} ms / emitter loops{} ms / all loops{} ms; changed components {} / {}",result.width,result.height,pattern,result.median[0],result.median[1],result.median[2],result.differences[0],result.differences[1]);}
    unsigned errors=0;if(validation)for(uint64_t i=first;i<validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;validation->GetMessage(i,nullptr,&size);std::vector<uint8_t> data(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());if(SUCCEEDED(validation->GetMessage(i,message,&size)) && message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;REXLOG_ERROR("Indirect benchmark validation: {}",message->pDescription);}}
    std::filesystem::create_directories(directory);std::ofstream report(directory/"indirect-loop-benchmark.json");report<<"{\"adapter_vendor\":"<<static_cast<unsigned>(provider.GetAdapterVendorID())<<",\"adapter_luid_low\":"<<luid.LowPart<<",\"adapter_luid_high\":"<<luid.HighPart<<",\"timestamp_frequency\":"<<frequency<<",\"samples_per_variant\":"<<samples<<",\"warmups_per_variant\":"<<warmups<<",\"variants\":[\"unrolled\",\"loop_emitters\",\"loop_all\"],\"dxbc_bytes\":[";for(unsigned i=0;i<variants;++i)report<<(i?",":"")<<GetBuiltShader(shaders[i])->size;report<<"],\"device_errors\":"<<errors<<",\"cases\":[";
    for(unsigned i=0;i<results.size();++i){const auto& r=results[i];report<<(i?",":"")<<"{\"width\":"<<r.width<<",\"height\":"<<r.height<<",\"pattern\":"<<r.pattern<<",\"median_gpu_ms\":["<<r.median[0]<<","<<r.median[1]<<","<<r.median[2]<<"],\"changed_components\":["<<r.differences[0]<<","<<r.differences[1]<<"],\"max_error\":["<<r.max_error[0]<<","<<r.max_error[1]<<"],\"invalid\":"<<r.invalid<<",\"positive_components\":"<<r.positive<<",\"gpu_samples_ms\":[";for(unsigned v=0;v<variants;++v){report<<(v?",":"")<<"[";for(unsigned sample=0;sample<samples;++sample)report<<(sample?",":"")<<r.gpu_ms[v][sample];report<<"]";}report<<"]}";}
    report<<"],\"scope\":\"GPU timestamp medians for identical 16-emitter/four-probe equations at half resolution; rotating pipeline order, three warmups and nine samples, repeated corners/flat/unavailable materials, full FP16 output comparison. Driver PSO creation and uploads excluded from timing. Own short fenced lists and retained resources. Synthetic pass measurements, not gameplay FPS.\"}\n";
    return !errors && report.good();
}
