#include "hdr_output.h"
#include "native_pixel_fixture.h"
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/logging.h>
#include <d3d12sdklayers.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>
#include <immintrin.h>

using namespace em2::pixel_test;
namespace {
constexpr unsigned width=17,height=11;
constexpr std::array<em2::HdrOutputSettings,8> settings={{{1,200,1000},{.25f,80,600},{4,300,2000},
    {256,1000,10000},{1.f/256,80,80},{1,80,80},{1,203,10000},{1,200,400}}};
float Half(uint16_t bits) {
    const auto e=(bits>>10)&31,m=bits&1023;
    const double value=e==31?(m?std::numeric_limits<double>::quiet_NaN():std::numeric_limits<double>::infinity()):
        e?std::ldexp(double(1024+m),int(e)-25):std::ldexp(double(m),-24);
    return float(bits&0x8000?-value:value);
}
std::array<double,4> Reference(Float4 input,em2::HdrOutputSettings s) {
    std::array<double,4> result{};double magnitude=0;
    for(unsigned c=0;c<3;++c) {
        result[c]=(std::isfinite(input[c])?std::clamp(double(input[c]),-65504.,65504.):0)*s.exposure*s.paper_white_nits;
        magnitude=std::max(magnitude,std::abs(result[c]));
    }
    if(magnitude>s.paper_white_nits) {
        const double room=double(s.peak_nits)-s.paper_white_nits,delta=magnitude-s.paper_white_nits;
        const double mapped=s.paper_white_nits+room*delta/(room+delta);
        for(unsigned c=0;c<3;++c)result[c]*=mapped/magnitude;
    }
    for(unsigned c=0;c<3;++c)result[c]/=80;
    result[3]=1;return result;
}
bool Texture(ID3D12Device* device,DXGI_FORMAT format,unsigned w,unsigned h,D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES state,ID3D12Resource** out) {
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=w;desc.Height=h;
    desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Format=format;desc.Flags=flags;
    return SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(out)));
}
struct Probe:Fixture {
    em2::HdrOutputPass pass;
    std::array<std::unique_ptr<em2::HdrOutputBindings>,2> bindings;
    ComPtr<ID3D12DescriptorHeap> clear_views;
    ComPtr<IDXGISwapChain3> hdr_swapchain;
    ComPtr<ID3D12Resource> backbuffer;
};
}
bool em2::TestHdrOutputPass(rex::ui::d3d12::D3D12Provider& provider,const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);std::ofstream progress(directory/"hdr-output-progress.txt");
    auto stage=[&](const char* text){progress<<text<<std::endl;};stage("start");
    const auto csr=_mm_getcsr();struct FP {unsigned value;~FP(){_mm_setcsr(value);}}fp{csr};
    auto check=[](bool ok,const char* message){if(!ok)REXLOG_ERROR("HDR output probe: {}",message);return ok;};
    auto f=std::make_unique<Probe>();auto* device=provider.GetDevice();auto* queue=provider.GetDirectQueue();
    ComPtr<ID3D12InfoQueue> validation;device->QueryInterface(IID_PPV_ARGS(&validation));
    const auto first_message=validation?validation->GetNumStoredMessagesAllowedByRetrievalFilter():0;
    if(!check(!f->pass.Initialize(nullptr) && f->pass.Initialize(device) && f->pass.Initialize(device),"initialization/idempotence"))return false;
    stage("shader and pipeline initialized");
    if(!f->event || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f->allocator))) ||
       FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f->allocator.Get(),nullptr,IID_PPV_ARGS(&f->list))) ||
       FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f->fence))))return false;
    constexpr DXGI_FORMAT formats[]={DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32G32B32A32_FLOAT};
    if(!Texture(device,kHdrOutputFormat,width,height,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,&f->target))return false;
    DXGI_SWAP_CHAIN_DESC1 swap_desc{};swap_desc.Width=width;swap_desc.Height=height;swap_desc.Format=kHdrOutputFormat;
    swap_desc.SampleDesc.Count=1;swap_desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;swap_desc.BufferCount=2;
    swap_desc.Scaling=DXGI_SCALING_STRETCH;swap_desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;swap_desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
    ComPtr<IDXGISwapChain1> created;HdrDisplayInfo display;bool swapchain_configured=false;
    if(SUCCEEDED(provider.GetDXGIFactory()->CreateSwapChainForComposition(queue,&swap_desc,nullptr,&created)) &&
       SUCCEEDED(created.As(&f->hdr_swapchain))) {
        swapchain_configured=ConfigureHdrSwapchain(f->hdr_swapchain.Get(),&display)==HdrColorSpaceResult::Configured;
        if(swapchain_configured && FAILED(f->hdr_swapchain->GetBuffer(0,IID_PPV_ARGS(&f->backbuffer))))return false;
    }
    if(!check(ConfigureHdrSwapchain(nullptr)==HdrColorSpaceResult::InvalidFormat,"null swapchain rejected"))return false;
    stage(swapchain_configured?"FP16 scRGB composition swapchain negotiated":"offscreen FP16 swapchain negotiation unavailable");
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,2> uploads{};std::array<uint64_t,2> upload_sizes{};
    uint64_t upload_bytes=0;
    for(unsigned format=0;format<2;++format) {
        if(!Texture(device,formats[format],width,height,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST,&f->textures[format]))return false;
        const auto desc=f->textures[format]->GetDesc();
        device->GetCopyableFootprints(&desc,0,1,upload_bytes,&uploads[format],nullptr,nullptr,&upload_sizes[format]);
        upload_bytes=(uploads[format].Offset+upload_sizes[format]+511)&~uint64_t(511);
        f->bindings[format]=f->pass.CreateBindings(f->textures[format].Get(),f->target.Get());
        if(!f->bindings[format])return false;
    }
    stage("source and output resources and bindings created");
    unsigned rejected_bindings=0;
    for(const auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R32_FLOAT}) {
        ComPtr<ID3D12Resource> wrong;
        if(!Texture(device,format,width,height,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COMMON,&wrong) ||
            f->pass.CreateBindings(wrong.Get(),f->target.Get()))return false;++rejected_bindings;
    }
    for(unsigned mode=0;mode<3;++mode) {
        ComPtr<ID3D12Resource> wrong;
        if(!Texture(device,mode==0?DXGI_FORMAT_R8G8B8A8_UNORM:kHdrOutputFormat,width+(mode==1?1:0),height,
            mode==2?D3D12_RESOURCE_FLAG_NONE:D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON,&wrong) ||
            f->pass.CreateBindings(f->textures[0].Get(),wrong.Get()))return false;++rejected_bindings;
    }
    if(!check(!f->pass.CreateBindings(f->target.Get(),f->target.Get()) && !f->pass.CreateBindings(nullptr,f->target.Get()),"alias/null rejection"))return false;
    stage("invalid binding guards passed");
    const auto target_desc=f->target->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT output{};uint64_t output_bytes=0;
    device->GetCopyableFootprints(&target_desc,0,1,0,&output,nullptr,nullptr,&output_bytes);
    const uint64_t stride=(output_bytes+511)&~uint64_t(511),images=settings.size()*2+2;
    if(!Buffer(device,upload_bytes,D3D12_HEAP_TYPE_UPLOAD,&f->upload) ||
       !Buffer(device,stride*images,D3D12_HEAP_TYPE_READBACK,&f->readback))return false;
    std::array<std::vector<Float4>,2> inputs;for(auto& image:inputs)image.resize(width*height);
    constexpr uint16_t samples[]={0,0x3000,0x3800,0x3C00,0x4000,0x4400,0x4800,0x4C00,0x5400,0x6400,0xB400,0x7BFF,0x7C00,0x7E00,1,0xFBFF};
    uint8_t* mapped=nullptr;D3D12_RANGE none{};
    if(FAILED(f->upload->Map(0,&none,reinterpret_cast<void**>(&mapped))))return false;
    progress<<"upload mapped bytes="<<upload_bytes<<" offsets="<<uploads[0].Offset<<","<<uploads[1].Offset<<std::endl;
    for(unsigned format=0;format<2;++format)for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
        const unsigned i=y*width+x;auto* pixel=mapped+uploads[format].Offset+y*uploads[format].Footprint.RowPitch+x*(format?16:8);
        for(unsigned c=0;c<4;++c) {
            uint16_t bits=samples[(i+c*3)%std::size(samples)];
            if(i<3)bits=i==0?0:i==1?0x3C00:0x4C00;
            float value=Half(bits);
            if(format && i%29==0 && i>3)value=c&1?-std::numeric_limits<float>::max():std::numeric_limits<float>::max();
            inputs[format][i][c]=value;
            if(format)std::memcpy(pixel+c*4,&value,4);else std::memcpy(pixel+c*2,&bits,2);
        }
    }
    f->upload->Unmap(0,nullptr);
    stage("source pixels uploaded");
    for(unsigned format=0;format<2;++format) {
        D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=f->upload.Get();source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;source.PlacedFootprint=uploads[format];
        D3D12_TEXTURE_COPY_LOCATION target{};target.pResource=f->textures[format].Get();target.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        f->list->CopyTextureRegion(&target,0,0,0,&source,nullptr);
        Barrier(f->list.Get(),f->textures[format].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    stage("source upload commands recorded");
    auto copy=[&](uint64_t image) {
        Barrier(f->list.Get(),f->target.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=f->target.Get();source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION target{};target.pResource=f->readback.Get();target.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;target.PlacedFootprint=output;target.PlacedFootprint.Offset=image*stride;
        f->list->CopyTextureRegion(&target,0,0,0,&source,nullptr);
        Barrier(f->list.Get(),f->target.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    };
    auto& bindings=*f->bindings[0];ID3D12DescriptorHeap* heaps[]={bindings.views_.Get()};f->list->SetDescriptorHeaps(1,heaps);
    // ClearUnorderedAccessViewFloat explicitly requires a second, CPU-only
    // descriptor. The CPU handle of the shader-visible heap is not valid here.
    const D3D12_DESCRIPTOR_HEAP_DESC clear_heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    if(FAILED(device->CreateDescriptorHeap(&clear_heap,IID_PPV_ARGS(&f->clear_views))))return false;
    const auto cpu=f->clear_views->GetCPUDescriptorHandleForHeapStart();auto gpu=bindings.views_->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_UNORDERED_ACCESS_VIEW_DESC clear_view{};clear_view.Format=kHdrOutputFormat;clear_view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(f->target.Get(),nullptr,&clear_view,cpu);
    const float sentinel[]={-17,-17,-17,-17};f->list->ClearUnorderedAccessViewFloat(gpu,cpu,f->target.Get(),sentinel,0,nullptr);
    stage("UAV sentinel clear recorded");
    const HdrOutputSettings invalid[]={{0,200,1000},{257,200,1000},{1,0,1000},{1,200,199},{1,200,10001},
        {std::numeric_limits<float>::infinity(),200,1000},{1,std::numeric_limits<float>::quiet_NaN(),1000}};
    for(auto s:invalid) {
        const auto before=_mm_getcsr();
        if(!check(!f->pass.Record(f->list.Get(),bindings,s) && _mm_getcsr()==before,"invalid settings reject without CPU FP changes"))return false;
    }
    copy(images-2);
    for(unsigned format=0;format<2;++format)for(unsigned setting=0;setting<settings.size();++setting) {
        if(!f->pass.Record(f->list.Get(),*f->bindings[format],settings[setting]))return false;
        copy(format*settings.size()+setting);
    }
    if(swapchain_configured) {
        Barrier(f->list.Get(),f->target.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(f->list.Get(),f->backbuffer.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST);
        f->list->CopyResource(f->backbuffer.Get(),f->target.Get());
        Barrier(f->list.Get(),f->backbuffer.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=f->backbuffer.Get();source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION target{};target.pResource=f->readback.Get();target.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;target.PlacedFootprint=output;target.PlacedFootprint.Offset=(images-1)*stride;
        f->list->CopyTextureRegion(&target,0,0,0,&source,nullptr);
        Barrier(f->list.Get(),f->backbuffer.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PRESENT);
        Barrier(f->list.Get(),f->target.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    stage("dispatch and copy commands recorded");
    if(FAILED(f->list->Close()))return false;ID3D12CommandList* lists[]={f->list.Get()};queue->ExecuteCommandLists(1,lists);
    if(FAILED(queue->Signal(f->fence.Get(),1)) || FAILED(f->fence->SetEventOnCompletion(1,f->event)) ||
       WaitForSingleObject(f->event,15000)!=WAIT_OBJECT_0){f.release();return false;}
    stage("GPU fence completed");
    const uint8_t* pixels=nullptr;D3D12_RANGE range{0,size_t(stride*images)};
    if(FAILED(f->readback->Map(0,&range,reinterpret_cast<void**>(const_cast<uint8_t**>(&pixels)))))return false;
    stage("readback mapped");
    double max_error=0;unsigned invalid_pixels=0,highlight_components=0,negative_components=0,white_checks=0;
    for(unsigned format=0;format<2;++format)for(unsigned setting=0;setting<settings.size();++setting)for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
        const auto reference=Reference(inputs[format][y*width+x],settings[setting]);
        const auto* pixel=pixels+(format*settings.size()+setting)*stride+y*output.Footprint.RowPitch+x*8;
        for(unsigned c=0;c<4;++c) {
            uint16_t bits;std::memcpy(&bits,pixel+c*2,2);const double actual=Half(bits),error=std::abs(actual-reference[c]);
            max_error=std::max(max_error,error);
            const bool bad=!std::isfinite(actual) || error>0.0006*std::max(1.,std::abs(reference[c]))+1e-5;
            if(bad && invalid_pixels<8)REXLOG_ERROR("HDR reference mismatch: format={} setting={} pixel={},{} channel={} actual={} expected={} half={:04X}",format,setting,x,y,c,actual,reference[c],bits);
            invalid_pixels+=bad;
            highlight_components+=c<3 && actual>1;negative_components+=c<3 && actual<0;
            if(y==0 && x==0 && c<3)invalid_pixels+=actual!=0;
            if(y==0 && x==1 && c<3 && settings[setting].exposure==1) {
                invalid_pixels+=std::abs(actual-double(settings[setting].paper_white_nits)/80)>0.002;++white_checks;
            }
        }
    }
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)for(unsigned c=0;c<4;++c) {
        uint16_t bits;std::memcpy(&bits,pixels+(images-2)*stride+y*output.Footprint.RowPitch+x*8+c*2,2);invalid_pixels+=Half(bits)!=-17;
    }
    unsigned swapchain_differences=0;
    if(swapchain_configured)for(unsigned y=0;y<height;++y)
        swapchain_differences+=std::memcmp(pixels+(images-1)*stride+y*output.Footprint.RowPitch,
            pixels+(settings.size()*2-1)*stride+y*output.Footprint.RowPitch,width*8)!=0;
    f->readback->Unmap(0,&none);
    stage("readback reference and sentinel checked");
    unsigned device_errors=0;
    if(validation)for(uint64_t i=first_message;i<validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
        SIZE_T size=0;validation->GetMessage(i,nullptr,&size);std::vector<uint8_t> data(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());
        if(SUCCEEDED(validation->GetMessage(i,message,&size)) && message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) {
            ++device_errors;REXLOG_ERROR("HDR output GPU validation: {}",message->pDescription);
        }
    }
    std::filesystem::create_directories(directory);std::ofstream report(directory/"hdr-output-results.json");
    report<<"{\n  \"dispatches\":16,\n  \"pixels\":"<<16*width*height<<",\n  \"max_absolute_error\":"<<max_error<<",\n  \"invalid_components\":"<<invalid_pixels
        <<",\n  \"highlight_components_above_one\":"<<highlight_components<<",\n  \"negative_components\":"<<negative_components
        <<",\n  \"diffuse_white_checks\":"<<white_checks<<",\n  \"rejected_bindings\":"<<rejected_bindings+2<<",\n  \"device_errors\":"<<device_errors<<",\n  \"scope\":\"offscreen float HDR output pass; no title, monitor or swapchain HDR acceptance\"\n}\n";
    REXLOG_INFO("HDR output pass:16 dispatches,{} pixels, max reference error {}, invalid {}, highlights>1 {}, signed-negative {}, white checks {}, binding guards {}, device errors {}; float sources only, nonmultiple dimensions, live exposure/white/peak, black, shoulder, nonfinite/extreme input and rejected-setting sentinel; no window/title/HDR monitor",16*width*height,max_error,invalid_pixels,highlight_components,negative_components,white_checks,rejected_bindings+2,device_errors);
    std::ofstream transport(directory/"hdr-swapchain-results.json");
    transport<<"{\n  \"scRGB_configured\":"<<(swapchain_configured?"true":"false")<<",\n  \"different_rows\":"<<swapchain_differences
        <<",\n  \"display_info_available\":"<<(display.available?"true":"false")<<",\n  \"desktop_hdr_enabled\":"<<(display.desktop_hdr_enabled?"true":"false")
        <<",\n  \"bits_per_color\":"<<display.bits_per_color<<",\n  \"peak_nits\":"<<display.peak_nits
        <<",\n  \"scope\":\"windowless composition swapchain configuration and FP16 copy/readback; no Present or physical HDR acceptance\"\n}\n";
    REXLOG_INFO("HDR scRGB swapchain transport: configured {}, differing rows {}, display info {}, desktop HDR {}; no window or Present call",swapchain_configured,swapchain_differences,display.available,display.desktop_hdr_enabled);
    return !invalid_pixels && !device_errors && !swapchain_differences && highlight_components && negative_components && report.good() && transport.good();
}
