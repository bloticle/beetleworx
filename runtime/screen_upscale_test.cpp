#include "screen_upscale_test.h"
#include "hdr_presenter.h"
#include "hdr_output.h"
#include "hdr_upscale.h"
#include <bit>
#include <cmath>
#include "native_pixel_fixture.h"
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/logging.h>
#include <d3d12sdklayers.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace {
using namespace em2::pixel_test;
using P=rex::ui::d3d12::D3D12Presenter;
using B=rex::ui::Presenter;
// The production presenter owns these precompiled PSOs. Inspect them directly,
// rather than compiling a second implementation of FSR just for this check.
template<class Tag,auto Member>struct Access{friend auto MemberOf(Tag){return Member;}};
struct Roots{friend auto MemberOf(Roots);};template struct Access<Roots,&P::guest_output_paint_root_signatures_>;
struct Finals{friend auto MemberOf(Finals);};template struct Access<Finals,&P::guest_output_paint_final_pipelines_>;
struct Intermediates{friend auto MemberOf(Intermediates);};template struct Access<Intermediates,&P::guest_output_paint_intermediate_pipelines_>;
struct RootIndex{friend auto MemberOf(RootIndex);};template struct Access<RootIndex,&P::GetGuestOutputPaintRootSignatureIndex>;
struct Flow{friend auto MemberOf(Flow);};template struct Access<Flow,&B::GetGuestOutputPaintFlow>;
struct Width{friend auto MemberOf(Width);};template struct Access<Width,&B::surface_width_in_paint_connection_>;
struct Height{friend auto MemberOf(Height);};template struct Access<Height,&B::surface_height_in_paint_connection_>;
struct Types:B {
 using B::GuestOutputProperties;using B::GuestOutputPaintEffect;
 using B::BilinearConstants;using B::CasSharpenConstants;using B::CasResampleConstants;
 using B::FsrEasuConstants;using B::FsrRcasConstants;
};
constexpr unsigned source_w=320,source_h=180,row_pitch=3840,max_h=source_h*3;
constexpr uint64_t image_stride=(row_pitch*max_h+511)&~511u,source_stride=(row_pitch*source_h+511)&~511u;
uint16_t PackHalf(float value){
 const auto bits=std::bit_cast<uint32_t>(value);const unsigned sign=(bits>>16)&32768;const int exponent=int((bits>>23)&255)-127+15;unsigned mantissa=bits&0x7fffff;
 if(exponent<=0){if(exponent<-10)return uint16_t(sign);mantissa|=0x800000;const unsigned shift=14-exponent;return uint16_t(sign+((mantissa+((1u<<(shift-1))-1)+((mantissa>>shift)&1))>>shift));}
 if(exponent>=31)return uint16_t(sign|0x7c00);
 return uint16_t(sign+(unsigned(exponent)<<10)+((mantissa+0xfff+((mantissa>>13)&1))>>13));
}
double ReadHalf(const uint8_t* p){uint16_t b;std::memcpy(&b,p,2);const auto e=(b>>10)&31,m=b&1023;return (b&32768?-1:1)*(e==31?INFINITY:e?std::ldexp(double(1024+m),int(e)-25):std::ldexp(double(m),-24));}
struct Pixels:Fixture{std::vector<ComPtr<ID3D12Resource>> retained;};
struct Case{unsigned pattern;float scale;unsigned mode,sharpness,width,height;};
bool Texture(ID3D12Device* device,unsigned width,unsigned height,DXGI_FORMAT format,
             D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES state,ComPtr<ID3D12Resource>& resource){
 D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
 D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=width;desc.Height=height;
 desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Format=format;desc.Flags=flags;
 return SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&resource)));
}
}

void em2::EnableScreenUpscaleValidation(){
 ComPtr<ID3D12Debug> debug;
 if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
}

bool em2::TestScreenUpscalePixels(P& presenter,const rex::ui::d3d12::D3D12Provider& provider,
                                 const ScreenUpscaleCases& configs,const std::filesystem::path& directory,bool hdr){
 const unsigned source_w=hdr?160:320,source_h=hdr?90:180,pixel_bytes=hdr?8:4;
 const auto source_format=hdr?em2::kHdrOutputFormat:P::kGuestOutputFormat,final_format=hdr?em2::kHdrOutputFormat:P::kSwapChainFormat,intermediate_format=hdr?em2::kHdrOutputFormat:P::kGuestOutputIntermediateFormat;
 auto f=std::make_unique<Pixels>();auto* device=provider.GetDevice();auto* queue=provider.GetDirectQueue();
 // The production flow uses cached physical surface dimensions as well as
 // target dimensions. Supply an offscreen surface extent without creating a
 // window or changing the presenter's connection/ownership state.
 struct ExtentScope{P& p;uint32_t w,h;~ExtentScope(){p.*MemberOf(Width{})=w;p.*MemberOf(Height{})=h;}}
 extent{presenter,presenter.*MemberOf(Width{}),presenter.*MemberOf(Height{})};
 std::filesystem::create_directories(directory);
 ComPtr<ID3D12InfoQueue> validation;device->QueryInterface(IID_PPV_ARGS(&validation));
 const auto first_message=validation?validation->GetNumStoredMessagesAllowedByRetrievalFilter():0;
 REXLOG_INFO("Screen GPU fixture start, validation {}",bool(validation));
 if(!f->event || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f->allocator))) ||
    FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f->allocator.Get(),nullptr,IID_PPV_ARGS(&f->list))) ||
    FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f->fence))))return false;
 if(!Buffer(device,3*source_stride,D3D12_HEAP_TYPE_UPLOAD,&f->upload) ||
    !Buffer(device,162*image_stride,D3D12_HEAP_TYPE_READBACK,&f->readback))return false;
 D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.NumDescriptors=512;heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
 if(FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&f->views))))return false;
 heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
 if(FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&f->rtvs))))return false;
 const auto srv_step=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
 const auto rtv_step=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
 unsigned srv_count=0,rtv_count=0;
 auto srv=[&](ID3D12Resource* texture){
  const auto index=srv_count++;D3D12_SHADER_RESOURCE_VIEW_DESC desc{};desc.Format=texture->GetDesc().Format;
  desc.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;desc.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;desc.Texture2D.MipLevels=1;
  auto cpu=f->views->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=index*srv_step;device->CreateShaderResourceView(texture,&desc,cpu);
  auto gpu=f->views->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=index*srv_step;return gpu;
 };
 uint8_t* upload=nullptr;D3D12_RANGE none{};
 if(FAILED(f->upload->Map(0,&none,reinterpret_cast<void**>(&upload))))return false;
 for(unsigned pattern=0;pattern<3;++pattern)for(unsigned y=0;y<source_h;++y)for(unsigned x=0;x<source_w;++x){
  unsigned rgb[]={67,129,203};
  if(pattern==1){const bool edge=x*source_h>y*source_w;rgb[0]=edge?225:15;rgb[1]=edge?180:60;rgb[2]=edge?90:160;}
  if(pattern==2){rgb[0]=((x+y)%3)*80+20;rgb[1]=(x*13+y*7)%220+15;rgb[2]=(x*5+y*19)%220+15;}
  const uint32_t packed=((rgb[0]*1023+127)/255)|(((rgb[1]*1023+127)/255)<<10)|(((rgb[2]*1023+127)/255)<<20)|(3u<<30);
  if(hdr){
   float color[]={-1.175312f,2.265790f,-.201194f,1};
   if(pattern==1){const float level=x<source_w/4?0:x<source_w/2?.0001f:x<3*source_w/4?2.5f:12.5f;for(unsigned c=0;c<3;++c)color[c]=level;}
   if(pattern==2)for(unsigned c=0;c<3;++c)color[c]=float(rgb[c])/255*12.5f;
   for(unsigned c=0;c<4;++c){const auto half=PackHalf(color[c]);std::memcpy(upload+pattern*source_stride+y*row_pitch+x*8+c*2,&half,2);}
  }else std::memcpy(upload+pattern*source_stride+y*row_pitch+x*4,&packed,4);
 }
 f->upload->Unmap(0,nullptr);
 if(!Texture(device,source_w,source_h,source_format,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST,f->textures[0]))return false;
 const auto source_view=srv(f->textures[0].Get());std::vector<Case> cases;
 unsigned easu=0,rcas=0,cas=0;
 for(unsigned pattern=0;pattern<3;++pattern){
  if(pattern)Barrier(f->list.Get(),f->textures[0].Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=f->upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint.Offset=pattern*source_stride;src.PlacedFootprint.Footprint={source_format,source_w,source_h,1,row_pitch};
  D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=f->textures[0].Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  f->list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
  Barrier(f->list.Get(),f->textures[0].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  for(float scale:{.5f,1.f,1.5f,2.f,2.5f,3.f})for(unsigned mode=0;mode<3;++mode)for(unsigned sharpness=0;sharpness<3;++sharpness){
   Case c{pattern,scale,mode,sharpness,unsigned(source_w*scale),unsigned(source_h*scale)};
   presenter.*MemberOf(Width{})=c.width;presenter.*MemberOf(Height{})=c.height;
   Types::GuestOutputProperties properties;properties.frontbuffer_width=source_w;properties.frontbuffer_height=source_h;
   properties.display_aspect_ratio_x=source_w;properties.display_aspect_ratio_y=source_h;properties.is_8bpc=!hdr;
   const auto& config=configs[mode][sharpness];const auto flow=(presenter.*MemberOf(Flow{}))(properties,c.width,c.height,16384,16384,config);
   if(!flow.effect_count || flow.output_x || flow.output_y){REXLOG_ERROR("Screen flow invalid: pattern {} scale {} mode {} sharp {} passes {} offset {},{}",pattern,scale,mode,sharpness,flow.effect_count,flow.output_x,flow.output_y);return false;}
   auto input=source_view;ID3D12DescriptorHeap* heaps[]={f->views.Get()};f->list->SetDescriptorHeaps(1,heaps);
   for(size_t pass=0;pass<flow.effect_count;++pass){
    const bool final=pass+1==flow.effect_count;const auto effect=flow.effects[pass];const auto size=flow.effect_output_sizes[pass];
    ComPtr<ID3D12Resource> target;
    if(!Texture(device,size.first,size.second,final?final_format:intermediate_format,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET,target))return false;
    auto rtv=f->rtvs->GetCPUDescriptorHandleForHeapStart();rtv.ptr+=rtv_count++*rtv_step;device->CreateRenderTargetView(target.Get(),nullptr,rtv);
    const auto index=(*MemberOf(RootIndex{}))(effect);
    auto* pipeline=final?(presenter.*MemberOf(Finals{}))[size_t(effect)].Get():(presenter.*MemberOf(Intermediates{}))[size_t(effect)].Get();
    if(!pipeline){REXLOG_ERROR("Screen pipeline missing: scale {} mode {} pass {} effect {} final {}",scale,mode,pass,int(effect),final);return false;}
    f->list->SetPipelineState(pipeline);f->list->SetGraphicsRootSignature((presenter.*MemberOf(Roots{}))[size_t(index)].Get());
    f->list->SetGraphicsRootDescriptorTable(0,input);const float rectangle[]={-1,1,2,-2};f->list->SetGraphicsRoot32BitConstants(1,4,rectangle,0);
    auto constants=[&](const auto& value){f->list->SetGraphicsRoot32BitConstants(2,sizeof(value)/4,&value,0);};
    switch(size_t(index)){
     case 0:{Types::BilinearConstants v;v.Initialize(flow,pass);constants(v);break;}
     case 1:{Types::CasSharpenConstants v;v.Initialize(flow,pass,config);constants(v);++cas;break;}
     case 2:{Types::CasResampleConstants v;v.Initialize(flow,pass,config);constants(v);++cas;break;}
     case 3:{Types::FsrEasuConstants v;v.Initialize(flow,pass);constants(v);++easu;break;}
     case 4:{Types::FsrRcasConstants v;v.Initialize(flow,pass,config);constants(v);++rcas;break;}
     default:return false;
    }
    if(hdr){em2::HdrUpscaleConstants control;const auto mode=em2::DescribeHdrUpscaleFlow(flow,[](auto effect){return unsigned((*MemberOf(RootIndex{}))(effect));});control.flags=(pass?control.encoded_input:0)|(final?control.final_output:0)|(!mode.filtered?control.passthrough:0)|(mode.perceptual?control.perceptual:0);f->list->SetGraphicsRoot32BitConstants(2,sizeof(control)/4,&control,control.offset);}
    D3D12_VIEWPORT viewport{0,0,float(size.first),float(size.second),0,1};D3D12_RECT scissor{0,0,LONG(size.first),LONG(size.second)};
    f->list->RSSetViewports(1,&viewport);f->list->RSSetScissorRects(1,&scissor);f->list->OMSetRenderTargets(1,&rtv,TRUE,nullptr);
    f->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);f->list->DrawInstanced(4,1,0,0);
    if(final){
     Barrier(f->list.Get(),target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
     D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=target.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
     D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
     to.PlacedFootprint.Offset=cases.size()*image_stride;to.PlacedFootprint.Footprint={final_format,c.width,c.height,1,row_pitch};
     f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    }else{Barrier(f->list.Get(),target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);input=srv(target.Get());}
    f->retained.push_back(std::move(target));
   }
   cases.push_back(c);
  }
 }
 REXLOG_INFO("Screen GPU recorded {} cases, {} SRVs, {} RTVs",cases.size(),srv_count,rtv_count);
 if(cases.size()!=162 || FAILED(f->list->Close()))return false;
 ID3D12CommandList* lists[]={f->list.Get()};queue->ExecuteCommandLists(1,lists);
 if(FAILED(queue->Signal(f->fence.Get(),1)) || FAILED(f->fence->SetEventOnCompletion(1,f->event)) || WaitForSingleObject(f->event,15000)!=WAIT_OBJECT_0){f.release();return false;}
 const uint8_t* pixels=nullptr;D3D12_RANGE range{0,cases.size()*image_stride};
 if(FAILED(f->readback->Map(0,&range,reinterpret_cast<void**>(const_cast<uint8_t**>(&pixels)))))return false;
 unsigned constant_bad=0,alpha_bad=0,fsr_differences=0,sharpness_differences=0;unsigned nonfinite=0,negative=0,highlights=0,radiance_bad=0;
 for(size_t i=0;i<cases.size();++i){const auto& c=cases[i];
  for(unsigned y=0;y<c.height;++y)for(unsigned x=0;x<c.width;++x){const auto* pixel=pixels+i*image_stride+y*row_pitch+x*pixel_bytes;
   if(hdr){alpha_bad+=ReadHalf(pixel+6)!=1;const double expected[]={-1.175312,2.265790,-.201194};
    for(unsigned channel=0;channel<3;++channel){const auto value=ReadHalf(pixel+channel*2);nonfinite+=!std::isfinite(value);negative+=value<0;highlights+=value>1;if(!c.pattern)constant_bad+=std::abs(value-expected[channel])>.002+std::abs(expected[channel])*.003;}
    // Flat stripe interiors cover black, near black, 200-nit diffuse white
    // and a 1000-nit highlight. Exclude the actual sharpening boundary taps.
    if(c.pattern==1){const unsigned quarter=c.width/4,position=x%quarter;
     if(position>=8*c.scale && position+8*c.scale<quarter){const double levels[]={0,.0001,2.5,12.5};const auto reference=levels[x/quarter];
      for(unsigned channel=0;channel<3;++channel)radiance_bad+=std::abs(ReadHalf(pixel+channel*2)-reference)>.000001+std::abs(reference)*.003;
     }
    }
    if(c.pattern && c.mode==2 && c.scale>1){const size_t bilinear=i-c.mode*3;fsr_differences+=std::memcmp(pixel,pixels+bilinear*image_stride+y*row_pitch+x*8,6)!=0;}
    if(c.pattern && c.mode && c.sharpness==2)sharpness_differences+=std::memcmp(pixel,pixels+(i-2)*image_stride+y*row_pitch+x*8,6)!=0;
    continue;
   }
   alpha_bad+=pixel[3]!=255;
   if(!c.pattern){const int expected[]={203,129,67};for(unsigned channel=0;channel<3;++channel)constant_bad+=std::abs(int(pixel[channel])-expected[channel])>1;}
   if(c.pattern && c.mode==2 && c.scale>1){const size_t bilinear=i-c.mode*3;fsr_differences+=std::memcmp(pixel,pixels+bilinear*image_stride+y*row_pitch+x*4,3)!=0;}
   if(c.pattern && c.mode && c.sharpness==2){sharpness_differences+=std::memcmp(pixel,pixels+(i-2)*image_stride+y*row_pitch+x*4,3)!=0;}
  }
 }
 std::ofstream raw(directory/(hdr?"screen-upscale-rgba16f.bin":"screen-upscale-bgra8.bin"),std::ios::binary);raw.write(reinterpret_cast<const char*>(pixels),cases.size()*image_stride);f->readback->Unmap(0,&none);
 unsigned errors=0;
 if(validation)for(uint64_t i=first_message;i<validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T bytes=0;validation->GetMessage(i,nullptr,&bytes);std::vector<uint8_t> storage(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());if(SUCCEEDED(validation->GetMessage(i,message,&bytes)) && message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;REXLOG_ERROR("Screen upscaler GPU: {}",message->pDescription);}}
 std::ofstream report(directory/(hdr?"hdr-upscale-results.json":"screen-upscale-results.json"));report<<"{\n\"validation_enabled\":"<<(validation?"true":"false")<<",\n\"cases\":"<<cases.size()<<",\n\"source\":\""<<(hdr?"160x90 linear scRGB FP16":"320x180 native R10G10B10A2")<<"\",\n\"output\":\""<<(hdr?"linear scRGB FP16, 0.5x/1x/1.5x/2x/2.5x/3x":"BGRA8, 0.5x/1x/1.5x/2x/2.5x/3x")<<"\",\n\"easu_draws\":"<<easu<<",\n\"rcas_draws\":"<<rcas<<",\n\"cas_draws\":"<<cas<<",\n\"constant_invalid_components\":"<<constant_bad<<",\n\"alpha_invalid_pixels\":"<<alpha_bad<<",\n\"fsr_vs_bilinear_changed_pixels\":"<<fsr_differences<<",\n\"sharpening_changed_pixels\":"<<sharpness_differences<<",\n\"flat_radiance_invalid_components\":"<<radiance_bad<<",\n\"nonfinite_components\":"<<nonfinite<<",\n\"negative_components\":"<<negative<<",\n\"highlight_components\":"<<highlights<<",\n\"device_errors\":"<<errors<<",\n\"scope\":\"production presenter flow and precompiled shaders offscreen; no window, gameplay, native menu interaction or Present\"\n}\n";
 REXLOG_INFO("Screen upscale GPU (hdr={}): {} cases, EASU {} RCAS {} CAS {}, constant invalid {}, alpha invalid {}, FSR changes {}, sharpness changes {}, device errors {}",hdr,cases.size(),easu,rcas,cas,constant_bad,alpha_bad,fsr_differences,sharpness_differences,errors);
 if(hdr)REXLOG_INFO("HDR upscaler: nonfinite {}, negative components {}, highlights {}, flat radiance invalid {}",nonfinite,negative,highlights,radiance_bad);
 return (hdr?easu && rcas && cas && fsr_differences && negative && highlights && !nonfinite && !radiance_bad:easu && rcas && cas && fsr_differences) && !constant_bad && !alpha_bad && sharpness_differences && !errors && raw.good() && report.good();
}
