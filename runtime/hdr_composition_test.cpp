#include "hdr_output.h"
#include "native_pixel_fixture.h"
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/logging.h>
#include <d3d12sdklayers.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>
using namespace em2::pixel_test;
namespace {
constexpr unsigned w=17,h=11,bw=5,bh=3,modes=6,variants=3;
constexpr uint64_t upload_stride=512*h,read_stride=256*h;
constexpr em2::HdrOutputSettings settings[variants]={{1,200,1000,.5},{4,200,1000,2},{.25,300,600,0}};
struct Probe:Fixture{em2::HdrOutputPass pass;std::array<std::unique_ptr<em2::HdrOutputBindings>,modes> bindings;};
bool Image(ID3D12Device* device,DXGI_FORMAT format,unsigned width,unsigned height,D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES state,ID3D12Resource** out){
 D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
 D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=width;d.Height=height;d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=format;d.Flags=flags;
 return SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(out)));
}
double Half(uint16_t v){auto e=(v>>10)&31,m=v&1023;return (v&32768?-1:1)*(e==31?INFINITY:e?std::ldexp(double(1024+m),int(e)-25):std::ldexp(double(m),-24));}
double Decode(double v){return v<=.04045?v/12.92:std::pow((v+.055)/1.055,2.4);}
}
bool em2::TestHdrCompositionPass(rex::ui::d3d12::D3D12Provider& provider,const std::filesystem::path& output){
 auto f=std::make_unique<Probe>();auto* device=provider.GetDevice();auto* queue=provider.GetDirectQueue();
 Microsoft::WRL::ComPtr<ID3D12InfoQueue> validation;device->QueryInterface(IID_PPV_ARGS(&validation));auto first=validation?validation->GetNumStoredMessagesAllowedByRetrievalFilter():0;
 if(!f->pass.Initialize(device) || !f->event || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f->allocator))) || FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f->allocator.Get(),nullptr,IID_PPV_ARGS(&f->list))) || FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f->fence))))return false;
 std::array<std::vector<Float4>,6> inputs;
 if(!Buffer(device,6*upload_stride,D3D12_HEAP_TYPE_UPLOAD,&f->upload) || !Buffer(device,modes*variants*read_stride,D3D12_HEAP_TYPE_READBACK,&f->readback) || !Image(device,kHdrOutputFormat,w,h,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,&f->target))return false;
 uint8_t* mapped=nullptr;D3D12_RANGE none{};if(FAILED(f->upload->Map(0,&none,reinterpret_cast<void**>(&mapped))))return false;std::memset(mapped,0,6*upload_stride);
 for(unsigned i=0;i<6;++i){const unsigned width=i==1?bw:w,height=i==1?bh:h;const bool bytes=i>=4;auto format=bytes?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_R32G32B32A32_FLOAT;
  if(!Image(device,format,width,height,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST,&f->textures[i]))return false;inputs[i].resize(width*height);
  for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){Float4 value{};
   if(i==0)value={float(x)-2,float(y)*2,float(x+y)*4,1};
   else if(i==1)value={float(x)*2,float(y)*.5f,float(x+y)*4,1};
   else {const float alpha=float(x%5)*.25f;value={float(x)/(w-1),float(y)/(h-1),.6f,alpha};if(x==0 && y==0)value={1,1,1,1};
    if(i==3 || i==5)for(unsigned c=0;c<3;++c)value[c]*=value[3];
    if(x==1 && y==0)value={1,1,1,0};
   }
   if(bytes)for(unsigned c=0;c<4;++c){const auto v=uint8_t(std::lround(value[c]*255));mapped[i*upload_stride+y*512+x*4+c]=v;value[c]=v/255.f;}
   else std::memcpy(mapped+i*upload_stride+y*512+x*16,value.data(),16);
   inputs[i][y*width+x]=value;
  }
  D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=f->upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint.Offset=i*upload_stride;from.PlacedFootprint.Footprint={format,width,height,1,512};D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=f->textures[i].Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),f->textures[i].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 }
 f->upload->Unmap(0,nullptr);
 unsigned guards=0;
 if(f->pass.CreateBindings(f->textures[0].Get(),f->target.Get(),f->textures[4].Get(),f->textures[4].Get()))return false;++guards;
 if(f->pass.CreateBindings(f->textures[0].Get(),f->target.Get(),f->textures[4].Get()) || f->pass.CreateBindings(f->textures[0].Get(),f->target.Get(),nullptr,f->textures[1].Get()) || f->pass.CreateBindings(f->textures[0].Get(),f->target.Get(),nullptr,f->textures[2].Get(),HdrUiEncoding::None) || f->pass.CreateBindings(f->textures[0].Get(),f->target.Get(),f->target.Get()))return false;guards+=4;
 for(unsigned mode=0;mode<modes;++mode){const auto encoding=mode<2?HdrUiEncoding::None:HdrUiEncoding(mode-1);
  f->bindings[mode]=f->pass.CreateBindings(f->textures[0].Get(),f->target.Get(),mode?f->textures[1].Get():nullptr,mode>=2?f->textures[mode].Get():nullptr,encoding);if(!f->bindings[mode])return false;
  for(unsigned v=0;v<variants;++v){if(!f->pass.Record(f->list.Get(),*f->bindings[mode],settings[v]))return false;Barrier(f->list.Get(),f->target.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
   D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=f->target.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=(mode*variants+v)*read_stride;to.PlacedFootprint.Footprint={kHdrOutputFormat,w,h,1,256};f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),f->target.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
 }
 auto invalid=settings[0];for(float strength:{-1.f,17.f,INFINITY}){invalid.bloom_strength=strength;if(f->pass.Record(f->list.Get(),*f->bindings[1],invalid))return false;++guards;}
 if(FAILED(f->list->Close()))return false;ID3D12CommandList* lists[]={f->list.Get()};queue->ExecuteCommandLists(1,lists);if(FAILED(queue->Signal(f->fence.Get(),1)) || FAILED(f->fence->SetEventOnCompletion(1,f->event)) || WaitForSingleObject(f->event,15000)!=WAIT_OBJECT_0){f.release();return false;}
 const uint8_t* pixels=nullptr;D3D12_RANGE range{0,modes*variants*read_stride};if(FAILED(f->readback->Map(0,&range,reinterpret_cast<void**>(const_cast<uint8_t**>(&pixels)))))return false;
 auto bloom=[&](unsigned x,unsigned y,unsigned c){const double px=(double(x)+.5)*bw/w-.5,py=(double(y)+.5)*bh/h-.5;const int ix=int(std::floor(px)),iy=int(std::floor(py));const double fx=px-ix,fy=py-iy;auto at=[&](int a,int b){return double(inputs[1][std::clamp(b,0,int(bh)-1)*bw+std::clamp(a,0,int(bw)-1)][c]);};return std::lerp(std::lerp(at(ix,iy),at(ix+1,iy),fx),std::lerp(at(ix,iy+1),at(ix+1,iy+1),fx),fy);};
 unsigned bad=0,white_checks=0,transparent_differences=0;double max_error=0;
 for(unsigned mode=0;mode<modes;++mode)for(unsigned v=0;v<variants;++v)for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){const auto s=settings[v];double rgb[3],magnitude=0;for(unsigned c=0;c<3;++c){rgb[c]=(inputs[0][y*w+x][c]+(mode?bloom(x,y,c)*s.bloom_strength:0))*s.exposure*s.paper_white_nits;magnitude=std::max(magnitude,std::abs(rgb[c]));}
  if(magnitude>s.paper_white_nits){const double delta=magnitude-s.paper_white_nits,room=s.peak_nits-s.paper_white_nits,mapped=s.paper_white_nits+room*delta/(room+delta);for(auto& c:rgb)c*=mapped/magnitude;}for(auto& c:rgb)c/=80;
  if(mode>=2){const auto ui=inputs[mode][y*w+x];const double a=ui[3];if(a>0)for(unsigned c=0;c<3;++c){double color=std::clamp(double(ui[c])/((mode==3 || mode==5)?a:1),0.,1.);if(mode>=4)color=Decode(color);rgb[c]=std::lerp(rgb[c],color*s.paper_white_nits/80,a);}}
  const auto* p=pixels+(mode*variants+v)*read_stride+y*256+x*8;
  for(unsigned c=0;c<4;++c){uint16_t bits;std::memcpy(&bits,p+c*2,2);const double actual=Half(bits),expected=c<3?rgb[c]:1,error=std::abs(actual-expected);max_error=std::max(max_error,error);bad+=!std::isfinite(actual) || error>.0006*std::max(1.,std::abs(expected))+1e-5;if(mode>=2 && !x && !y && c<3){++white_checks;bad+=std::abs(actual-s.paper_white_nits/80.)>.002;}}
  if(mode>=2 && x==1 && !y)transparent_differences+=std::memcmp(p,pixels+(variants+v)*read_stride+y*256+x*8,8)!=0;
 }
 std::filesystem::create_directories(output);std::ofstream raw(output/"hdr-composition-readback.bin",std::ios::binary);raw.write(reinterpret_cast<const char*>(pixels),modes*variants*read_stride);f->readback->Unmap(0,&none);
 unsigned errors=0;if(validation)for(uint64_t i=first;i<validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T bytes=0;validation->GetMessage(i,nullptr,&bytes);std::vector<uint8_t> data(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());if(SUCCEEDED(validation->GetMessage(i,message,&bytes)) && message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;REXLOG_ERROR("HDR composition: {}",message->pDescription);}}
 std::ofstream report(output/"hdr-composition-results.json");report<<"{\n\"dispatches\":18,\n\"components\":13464,\n\"invalid\":"<<bad<<",\n\"max_error\":"<<max_error<<",\n\"ui_white_checks\":"<<white_checks<<",\n\"transparent_pixel_differences\":"<<transparent_differences<<",\n\"guards\":"<<guards<<",\n\"device_errors\":"<<errors<<"\n}\n";
 REXLOG_INFO("HDR bloom/UI composition:18 dispatches,13464 components, invalid {}, max error {}, white checks {}, transparent differences {}, guards {}, device errors {}",bad,max_error,white_checks,transparent_differences,guards,errors);
 return !bad && !transparent_differences && !errors && raw.good() && report.good();
}
