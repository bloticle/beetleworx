#include "hdr_upscale.h"
#include "hdr_presenter.h"
#include "hdr_output.h"
#include "native_pixel_fixture.h"
#include <rex/logging.h>
#include <d3d12sdklayers.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>
using namespace em2::pixel_test;
namespace {
using P=rex::ui::d3d12::D3D12Presenter;
template<class Tag,auto Member>struct Access{friend auto MemberOf(Tag){return Member;}};
struct RefreshTag{friend auto MemberOf(RefreshTag);};template struct Access<RefreshTag,&P::RefreshGuestOutputImpl>;
struct RootsTag{friend auto MemberOf(RootsTag);};template struct Access<RootsTag,&P::guest_output_paint_root_signatures_>;
struct PipelinesTag{friend auto MemberOf(PipelinesTag);};template struct Access<PipelinesTag,&P::guest_output_paint_final_pipelines_>;
constexpr unsigned w=17,h=11;constexpr uint64_t stride=256*h;
struct Probe:Fixture{std::unique_ptr<P> hdr,sdr;em2::HdrOutputPass output;std::unique_ptr<em2::HdrOutputBindings> bindings;ComPtr<ID3D12Resource> mailbox,fallback;};
bool Texture(ID3D12Device* device,DXGI_FORMAT format,D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES state,ID3D12Resource** out){
 D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=format;d.Flags=flags;return SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(out)));
}
double Half(uint16_t bits){const auto e=(bits>>10)&31,m=bits&1023;return (bits&32768?-1:1)*(e==31?INFINITY:e?std::ldexp(double(1024+m),int(e)-25):std::ldexp(double(m),-24));}
double Decode(double v){return v<=.04045?v/12.92:std::pow((v+.055)/1.055,2.4);}
}
bool em2::TestHdrPresenter(rex::ui::d3d12::D3D12Provider& provider,const std::filesystem::path& directory){
 std::filesystem::create_directories(directory);std::ofstream progress(directory/"hdr-presenter-progress.txt");const char* stage="create SDR/HDR presenters";bool complete=false;auto mark=[&](const char* s){stage=s;progress<<s<<std::endl;};struct Failure{const char*& stage;bool& complete;~Failure(){if(!complete)REXLOG_ERROR("HDR presenter probe failed at {}",stage);}}failure{stage,complete};mark(stage);
 auto f=std::make_unique<Probe>();auto* device=provider.GetDevice();auto* queue=provider.GetDirectQueue();bool lost=false;
 ComPtr<ID3D12InfoQueue> validation;device->QueryInterface(IID_PPV_ARGS(&validation));auto first=validation?validation->GetNumStoredMessagesAllowedByRetrievalFilter():0;
 auto loss=[&](bool,bool){lost=true;};f->sdr=P::Create(loss,provider);f->hdr=CreateHdrPresenter(loss,provider);REXLOG_INFO("HDR presenter creation sdr={} hdr={} sdr_mode={} hdr_mode={}",bool(f->sdr),bool(f->hdr),IsHdrPresenter(f->sdr.get()),IsHdrPresenter(f->hdr.get()));if(!f->sdr || !f->hdr){if(validation)for(uint64_t i=first;i<validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T n=0;validation->GetMessage(i,nullptr,&n);std::vector<uint8_t> v(n);auto* m=reinterpret_cast<D3D12_MESSAGE*>(v.data());if(SUCCEEDED(validation->GetMessage(i,m,&n)))REXLOG_ERROR("HDR presenter initialization: {}",m->pDescription);}}if(!f->sdr || !f->hdr || IsHdrPresenter(f->sdr.get()) || !IsHdrPresenter(f->hdr.get()) || SetHdrPresenterPaperWhite(f->sdr.get(),200) || SetHdrPresenterPaperWhite(f->hdr.get(),0))return false;
 mark("SDR native mailbox");
 bool eight=false,sdr_checked=false;
 if(!(f->sdr.get()->*MemberOf(RefreshTag{}))(0,w,h,[&](rex::ui::Presenter::GuestOutputRefreshContext& base){auto& ctx=static_cast<P::D3D12GuestOutputRefreshContext&>(base);sdr_checked=ctx.resource_uav_capable()->GetDesc().Format==P::kGuestOutputFormat;return sdr_checked;},eight) || !sdr_checked)return false;
 mark("fixture command list");
 if(!f->event || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f->allocator))) || FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f->allocator.Get(),nullptr,IID_PPV_ARGS(&f->list))) || FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f->fence))) || !f->output.Initialize(device))return false;
 mark("fixture resources");
 if(!Buffer(device,512*h+stride,D3D12_HEAP_TYPE_UPLOAD,&f->upload) || !Buffer(device,6*stride,D3D12_HEAP_TYPE_READBACK,&f->readback) || !Texture(device,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST,&f->textures[0]) || !Texture(device,kHdrOutputFormat,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET,&f->target))return false;
 mark("upload data");
 uint8_t* mapped=nullptr;D3D12_RANGE none{};if(FAILED(f->upload->Map(0,&none,reinterpret_cast<void**>(&mapped))))return false;std::memset(mapped,0,512*h+stride);
 for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){const Float4 color={float(x)-8,float(y)*.5f,float(x+y)*2,1};std::memcpy(mapped+y*512+x*16,color.data(),16);
  const double alpha=(x%3)*.5;const double rgb[]={double(x%3)/2,double(y%3)/2,.6};auto* ui=mapped+512*h+y*256+x*4;ui[0]=uint8_t(std::lround(rgb[2]*alpha*255));ui[1]=uint8_t(std::lround(rgb[1]*alpha*255));ui[2]=uint8_t(std::lround(rgb[0]*alpha*255));ui[3]=uint8_t(std::lround(alpha*255));if(!x){ui[0]=ui[1]=ui[2]=255;ui[3]=0;}
 }
 f->upload->Unmap(0,nullptr);D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=f->upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint.Footprint={DXGI_FORMAT_R32G32B32A32_FLOAT,w,h,1,512};D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=f->textures[0].Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;f->list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Barrier(f->list.Get(),f->textures[0].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 mark("HDR native refresh");
 bool submitted=false;
 if(!(f->hdr.get()->*MemberOf(RefreshTag{}))(0,w,h,[&](rex::ui::Presenter::GuestOutputRefreshContext& base){auto& ctx=static_cast<P::D3D12GuestOutputRefreshContext&>(base);ctx.SetIs8bpc(false);auto* target=ctx.resource_uav_capable();if(target->GetDesc().Format!=kHdrOutputFormat)return false;f->mailbox=target;f->bindings=f->output.CreateBindings(f->textures[0].Get(),target);if(!f->bindings)return false;Barrier(f->list.Get(),target,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);if(!f->output.Record(f->list.Get(),*f->bindings,{1,200,1000}))return false;Barrier(f->list.Get(),target,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);if(FAILED(f->list->Close()))return false;ID3D12CommandList* lists[]={f->list.Get()};queue->ExecuteCommandLists(1,lists);submitted=true;return true;},eight) || eight || !submitted)return false;
 mark("HDR mailbox fence");
 if(FAILED(queue->Signal(f->fence.Get(),1)) || FAILED(f->fence->SetEventOnCompletion(1,f->event)) || WaitForSingleObject(f->event,15000)!=WAIT_OBJECT_0){f.release();return false;}
 if(FAILED(f->allocator->Reset()) || FAILED(f->list->Reset(f->allocator.Get(),nullptr)))return false;
 auto copy=[&](ID3D12Resource* texture,D3D12_RESOURCE_STATES state,unsigned image){Barrier(f->list.Get(),texture,state,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=texture;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=image*stride;to.PlacedFootprint.Footprint={kHdrOutputFormat,w,h,1,256};f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),texture,D3D12_RESOURCE_STATE_COPY_SOURCE,state);};
 copy(f->mailbox.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,0);
 mark("native paint descriptors");
 const D3D12_DESCRIPTOR_HEAP_DESC rtvs{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0},srvs{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};if(FAILED(device->CreateDescriptorHeap(&rtvs,IID_PPV_ARGS(&f->rtvs))) || FAILED(device->CreateDescriptorHeap(&srvs,IID_PPV_ARGS(&f->views))))return false;
 device->CreateRenderTargetView(f->target.Get(),nullptr,f->rtvs->GetCPUDescriptorHandleForHeapStart());D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=kHdrOutputFormat;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;device->CreateShaderResourceView(f->mailbox.Get(),&view,f->views->GetCPUDescriptorHandleForHeapStart());
 mark("host UI resource");
 D3D12_CPU_DESCRIPTOR_HANDLE ui_rtv{};auto* ui=PrepareHdrPresenterUi(f->hdr.get(),device,w,h,ui_rtv);if(!ui)return false;
 Barrier(f->list.Get(),ui,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);src.PlacedFootprint.Offset=512*h;src.PlacedFootprint.Footprint={P::kSwapChainFormat,w,h,1,256};dst.pResource=ui;f->list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Barrier(f->list.Get(),ui,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
 mark("native paint and host UI recording");
 for(unsigned image=1;image<4;++image){const auto rtv=f->rtvs->GetCPUDescriptorHandleForHeapStart();ID3D12DescriptorHeap* heaps[]={f->views.Get()};f->list->SetDescriptorHeaps(1,heaps);f->list->SetGraphicsRootSignature((f->hdr.get()->*MemberOf(RootsTag{}))[0].Get());f->list->SetPipelineState((f->hdr.get()->*MemberOf(PipelinesTag{}))[0].Get());f->list->SetGraphicsRootDescriptorTable(0,f->views->GetGPUDescriptorHandleForHeapStart());const float rectangle[]={-1,1,2,-2};f->list->SetGraphicsRoot32BitConstants(1,4,rectangle,0);struct Filter{int x,y;float invx,invy;}filter{0,0,1.f/w,1.f/h};f->list->SetGraphicsRoot32BitConstants(2,4,&filter,0);em2::HdrUpscaleConstants control;control.flags=control.passthrough|control.final_output;f->list->SetGraphicsRoot32BitConstants(2,sizeof(control)/4,&control,control.offset);D3D12_VIEWPORT vp{0,0,w,h,0,1};D3D12_RECT scissor{0,0,w,h};f->list->RSSetViewports(1,&vp);f->list->RSSetScissorRects(1,&scissor);f->list->OMSetRenderTargets(1,&rtv,TRUE,nullptr);f->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);f->list->DrawInstanced(4,1,0,0);
  if(image>=2){if(!SetHdrPresenterPaperWhite(f->hdr.get(),image==2?200:400) || !DrawHdrPresenterUi(f->hdr.get(),f->list.Get()))return false;}copy(f->target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,image);
 }
 if(FAILED(f->list->Close()))return false;ID3D12CommandList* lists[]={f->list.Get()};queue->ExecuteCommandLists(1,lists);if(FAILED(queue->Signal(f->fence.Get(),2)) || FAILED(f->fence->SetEventOnCompletion(2,f->event)) || WaitForSingleObject(f->event,15000)!=WAIT_OBJECT_0){f.release();return false;}
 mark("native SDR fallback mailbox");
 if(IsHdrLegacyRefresh() || !HdrSdrPaintPipeline(f->hdr.get()) || HdrSdrPaintPipeline(f->sdr.get()))return false;
 {HdrLegacyRefreshScope legacy;if(!IsHdrLegacyRefresh())return false;{HdrLegacyRefreshScope nested;if(!IsHdrLegacyRefresh())return false;}if(!IsHdrLegacyRefresh())return false;
  if(!(f->hdr.get()->*MemberOf(RefreshTag{}))(0,w,h,[&](rex::ui::Presenter::GuestOutputRefreshContext& base){auto& context=static_cast<P::D3D12GuestOutputRefreshContext&>(base);context.SetIs8bpc(true);f->fallback=context.resource_uav_capable();return f->fallback->GetDesc().Format==P::kGuestOutputFormat;},eight) || !eight)return false;
 }if(IsHdrLegacyRefresh())return false;
 if(FAILED(f->allocator->Reset()) || FAILED(f->list->Reset(f->allocator.Get(),nullptr)) || FAILED(f->upload->Map(0,&none,reinterpret_cast<void**>(&mapped))))return false;
 for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){const unsigned r=x%3==0?0:x%3==1?512:1023,g=y%3==0?0:y%3==1?512:1023;const uint32_t value=r|(g<<10)|(256<<20)|(3u<<30);std::memcpy(mapped+y*256+x*4,&value,4);}
 f->upload->Unmap(0,nullptr);src.PlacedFootprint.Offset=0;src.PlacedFootprint.Footprint={P::kGuestOutputFormat,w,h,1,256};dst.pResource=f->fallback.Get();Barrier(f->list.Get(),f->fallback.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);f->list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Barrier(f->list.Get(),f->fallback.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
 view.Format=P::kGuestOutputFormat;device->CreateShaderResourceView(f->fallback.Get(),&view,f->views->GetCPUDescriptorHandleForHeapStart());
 for(unsigned image=4;image<6;++image){ID3D12DescriptorHeap* heaps[]={f->views.Get()};f->list->SetDescriptorHeaps(1,heaps);f->list->SetGraphicsRootSignature((f->hdr.get()->*MemberOf(RootsTag{}))[0].Get());f->list->SetPipelineState(HdrSdrPaintPipeline(f->hdr.get()));f->list->SetGraphicsRootDescriptorTable(0,f->views->GetGPUDescriptorHandleForHeapStart());const float rectangle[]={-1,1,2,-2};f->list->SetGraphicsRoot32BitConstants(1,4,rectangle,0);struct Filter{int x,y;float invx,invy,white;}filter{0,0,1.f/w,1.f/h,image==4?200.f:400.f};f->list->SetGraphicsRoot32BitConstants(2,5,&filter,0);D3D12_VIEWPORT vp{0,0,w,h,0,1};D3D12_RECT scissor{0,0,w,h};f->list->RSSetViewports(1,&vp);f->list->RSSetScissorRects(1,&scissor);const auto rtv=f->rtvs->GetCPUDescriptorHandleForHeapStart();f->list->OMSetRenderTargets(1,&rtv,TRUE,nullptr);f->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);f->list->DrawInstanced(4,1,0,0);copy(f->target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,image);}
 if(FAILED(f->list->Close()))return false;ID3D12CommandList* fallback_lists[]={f->list.Get()};queue->ExecuteCommandLists(1,fallback_lists);if(FAILED(queue->Signal(f->fence.Get(),3)) || FAILED(f->fence->SetEventOnCompletion(3,f->event)) || WaitForSingleObject(f->event,15000)!=WAIT_OBJECT_0){f.release();return false;}
 bool restored=false;if(!(f->hdr.get()->*MemberOf(RefreshTag{}))(0,w,h,[&](rex::ui::Presenter::GuestOutputRefreshContext& base){auto& context=static_cast<P::D3D12GuestOutputRefreshContext&>(base);restored=context.resource_uav_capable()->GetDesc().Format==kHdrOutputFormat;context.SetIs8bpc(false);return restored;},eight) || !restored || eight)return false;
 mark("paint fence and readback");
 const uint8_t* pixels=nullptr;D3D12_RANGE read{0,6*stride};if(FAILED(f->readback->Map(0,&read,reinterpret_cast<void**>(const_cast<uint8_t**>(&pixels)))))return false;
 unsigned differences=0,bad=0,highlights=0,negative=0,fallback_bad=0;double error_max=0,fallback_error=0;
 for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){const auto* p=pixels+y*256+x*8;differences+=std::memcmp(p,p+stride,8)!=0;double original[4];for(unsigned c=0;c<4;++c){uint16_t b;std::memcpy(&b,p+c*2,2);original[c]=Half(b);if(c<3){highlights+=original[c]>1;negative+=original[c]<0;}}
  const double a=std::lround((x%3)*.5*255)/255.;const double rgb[]={double(x%3)/2,double(y%3)/2,.6};
  for(unsigned image=2;image<4;++image)for(unsigned c=0;c<4;++c){double expected=original[c];if(a>0){if(c<3){const double stored=std::lround(rgb[c]*(x%3)*.5*255)/255.;expected=Decode(std::clamp(stored/a,0.,1.))*(image==2?200:400)/80*a+original[c]*(1-a);}else expected=a+original[c]*(1-a);}uint16_t bits;std::memcpy(&bits,p+image*stride+c*2,2);const double actual=Half(bits),error=std::abs(actual-expected);error_max=std::max(error_max,error);bad+=!std::isfinite(actual) || error>.0007*std::max(1.,std::abs(expected))+1e-5;}
 }
 for(unsigned image=4;image<6;++image)for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)for(unsigned c=0;c<4;++c){const double encoded[]={double(x%3==0?0:x%3==1?512:1023)/1023,double(y%3==0?0:y%3==1?512:1023)/1023,256./1023};const double expected=c==3?1:Decode(encoded[c])*(image==4?200:400)/80;uint16_t bits;std::memcpy(&bits,pixels+image*stride+y*256+x*8+c*2,2);const double actual=Half(bits),error=std::abs(actual-expected);fallback_error=std::max(fallback_error,error);fallback_bad+=!std::isfinite(actual) || error>.0007*std::max(1.,std::abs(expected))+1e-5;}
 std::filesystem::create_directories(directory);std::ofstream raw(directory/"hdr-presenter-readback.bin",std::ios::binary);raw.write(reinterpret_cast<const char*>(pixels),6*stride);f->readback->Unmap(0,&none);unsigned errors=0;
 if(validation)for(uint64_t i=first;i<validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T bytes=0;validation->GetMessage(i,nullptr,&bytes);std::vector<uint8_t> data(bytes);auto* m=reinterpret_cast<D3D12_MESSAGE*>(data.data());if(SUCCEEDED(validation->GetMessage(i,m,&bytes)) && m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;REXLOG_ERROR("HDR presenter: {}",m->pDescription);}}
 std::ofstream report(directory/"hdr-presenter-results.json");report<<"{\n\"fallback_components\":1496,\n\"invalid_fallback_components\":"<<fallback_bad<<",\n\"max_fallback_error\":"<<fallback_error<<",\n\"native_paint_pixel_differences\":"<<differences<<",\n\"invalid_overlay_components\":"<<bad<<",\n\"max_overlay_error\":"<<error_max<<",\n\"highlight_components\":"<<highlights<<",\n\"negative_components\":"<<negative<<",\n\"device_errors\":"<<errors<<",\n\"scope\":\"actual native SDR/HDR mailbox refresh, native HDR bilinear PSO and host-UI helper offscreen; no surface, swapchain paint loop or Present\"\n}\n";
 REXLOG_INFO("Native HDR presenter: mailboxFP16, raw paint differences {}, UI invalid {}, max error {}, highlights {}, negatives {}, device errors {}; offscreen/no Present",differences,bad,error_max,highlights,negative,errors);
 REXLOG_INFO("HDR SDR fallback: nested scopes restored, 10-bit/FP16 mailbox transitions, 200/400-nit decode, invalid {}, max error {}",fallback_bad,fallback_error);
 complete=!lost && !differences && !bad && !fallback_bad && !errors && highlights && negative && raw.good() && report.good();return complete;
}
