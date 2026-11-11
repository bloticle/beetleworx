#include "hdr_scene_cache.h"
#include "hdr_scene_target.h"
#include "native_pixel_fixture.h"
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/deferred_command_list.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/logging.h>
#include <d3d12sdklayers.h>
#include <cmath>
#include <cstring>
#include <fstream>
namespace {
using CP=rex::graphics::d3d12::D3D12CommandProcessor;using Cache=rex::graphics::d3d12::D3D12RenderTargetCache;using List=rex::graphics::d3d12::DeferredCommandList;
template<class Tag,auto Member>struct Access{friend auto MemberOf(Tag){return Member;}};
struct Stream{friend auto MemberOf(Stream);};template struct Access<Stream,&List::command_stream_>;
using namespace em2::pixel_test;
struct EncodingFixture:Fixture {std::array<std::shared_ptr<em2::HdrSceneTarget>,5> captures;};
double Half(uint16_t bits){const unsigned e=(bits>>10)&31,m=bits&1023;const double value=e?std::ldexp(1.+m/1024.,int(e)-15):std::ldexp(double(m),-24);return bits&32768?-value:value;}
}
namespace em2 {
bool TestHdrSceneEncoding(CP& cp,Cache& cache,const std::filesystem::path& directory){
    if(GetHdrSceneEncoding(&cache)!=HdrSceneEncoding::kNativeLinear)return false;
    auto f=std::make_unique<EncodingFixture>();auto* device=cp.GetD3D12Provider().GetDevice();ComPtr<ID3D12InfoQueue> validation;device->QueryInterface(IID_PPV_ARGS(&validation));const auto first=validation?validation->GetNumStoredMessagesAllowedByRetrievalFilter():0;
    if(!f->event || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f->allocator))) || FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f->allocator.Get(),nullptr,IID_PPV_ARGS(&f->list))) || FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f->fence))) || !Buffer(device,6*8192,D3D12_HEAP_TYPE_READBACK,&f->readback))return false;
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=d.Height=32;d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    for(unsigned i=0;i<5;++i)if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&f->textures[i]))))return false;
    auto& stream=cp.GetDeferredCommandList().*MemberOf(Stream{});auto saved=std::move(stream);stream.clear();struct Restore{decltype(stream) stream;decltype(saved)& saved;~Restore(){stream=std::move(saved);}}restore{stream,saved};
    const float raw[]={.125f,.25f,.5f,.75f},corner_colour[]={.25f,.5f,.75f,1};const D3D12_RECT full{0,0,32,32},corner{0,0,8,8};
    auto execute=[&]{cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();};
    auto copy=[&](unsigned source,unsigned index){auto* texture=f->captures[source]->texture();Barrier(f->list.Get(),texture,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=texture;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=index*8192;to.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,32,32,1,256};f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),texture,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);};
    for(unsigned policy=0;policy<4;++policy){auto* source=f->textures[policy].Get();if(policy==3)MirrorHdrSceneClear(&cache,source,raw,full,cp.GetDeferredCommandList());auto binding=AcquireHdrSceneBinding(&cache,source,&cp.GetDeferredCommandList(),policy==2,policy!=0);if(!binding.target || binding.encoding!=HdrSceneEncoding::kNativeLinear)return false;f->captures[policy]=binding.target;if(policy!=3)MirrorHdrSceneClear(&cache,source,raw,full,cp.GetDeferredCommandList());execute();copy(policy,policy);}
    auto transfer=PrepareHdrSceneTransfer(&cache,f->textures[1].Get(),f->textures[4].Get(),cp.GetDeferredCommandList());if(!transfer)return false;f->captures[4]=transfer.destination;execute();
    Barrier(f->list.Get(),transfer.source->texture(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);Barrier(f->list.Get(),transfer.destination->texture(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST);f->list->CopyResource(transfer.destination->texture(),transfer.source->texture());Barrier(f->list.Get(),transfer.source->texture(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);Barrier(f->list.Get(),transfer.destination->texture(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET);
    MirrorHdrSceneClear(&cache,f->textures[4].Get(),corner_colour,corner,cp.GetDeferredCommandList());execute();copy(4,4);
    bool srgb=true;auto found=FindHdrSceneSource(&cache,f->textures[4].Get(),srgb);if(!found.target || srgb || found.encoding!=HdrSceneEncoding::kNativeLinear)return false;
    auto mismatch=AcquireHdrSceneBinding(&cache,f->textures[2].Get(),&cp.GetDeferredCommandList(),false,true);if(!mismatch.active || mismatch.target || FindHdrSceneSource(&cache,f->textures[2].Get(),srgb).target)return false;
    MirrorHdrSceneClear(&cache,f->textures[2].Get(),raw,corner,cp.GetDeferredCommandList());if(FindHdrSceneSource(&cache,f->textures[2].Get(),srgb).target)return false;
    MirrorHdrSceneClear(&cache,f->textures[2].Get(),raw,full,cp.GetDeferredCommandList());if(FindHdrSceneSource(&cache,f->textures[2].Get(),srgb).target)return false;
    auto recovered=AcquireHdrSceneBinding(&cache,f->textures[2].Get(),&cp.GetDeferredCommandList(),false,true);if(!recovered.target)return false;execute();copy(2,5);
    UpdateHdrSceneBindingPolicy(&cache,false,false);if(!UpdateHdrSceneBindingPolicy(&cache,false,true) || UpdateHdrSceneBindingPolicy(&cache,false,true) || !UpdateHdrSceneBindingPolicy(&cache,false,false))return false;
    if(FAILED(f->list->Close()))return false;ID3D12CommandList* lists[]={f->list.Get()};cp.GetD3D12Provider().GetDirectQueue()->ExecuteCommandLists(1,lists);if(FAILED(cp.GetD3D12Provider().GetDirectQueue()->Signal(f->fence.Get(),1)) || FAILED(f->fence->SetEventOnCompletion(1,f->event)) || WaitForSingleObject(f->event,15000)!=WAIT_OBJECT_0){f.release();return false;}
    const uint8_t* pixels=nullptr;D3D12_RANGE range{0,6*8192};if(FAILED(f->readback->Map(0,&range,reinterpret_cast<void**>(const_cast<uint8_t**>(&pixels)))))return false;unsigned bad=0;double maximum=0;
    for(unsigned image=0;image<6;++image)for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x)for(unsigned c=0;c<4;++c){const float encoded=image==4 && x<8 && y<8?corner_colour[c]:raw[c];double expected=encoded;if(c<3){if(image==2)expected=encoded<=.04045f?encoded/12.92f:std::pow((encoded+.055f)/1.055f,2.4f);else if(image!=0)expected=rex::graphics::xenos::PWLGammaToLinear(encoded);}uint16_t bits;std::memcpy(&bits,pixels+image*8192+y*256+x*8+c*2,2);const auto value=Half(bits),error=std::abs(value-expected);maximum=std::max(maximum,error);bad+=!std::isfinite(value) || error>std::max(6e-8,std::abs(expected)/1024);}
    D3D12_RANGE none{};f->readback->Unmap(0,&none);unsigned errors=0;if(validation)for(uint64_t i=first;i<validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;validation->GetMessage(i,nullptr,&size);std::vector<uint8_t> storage(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());if(SUCCEEDED(validation->GetMessage(i,message,&size)) && message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;REXLOG_ERROR("HDR encoding validation: {}",message->pDescription);}}
    std::filesystem::create_directories(directory);std::ofstream report(directory/"hdr-encoding-results.json");report<<"{\"components\":24576,\"invalid\":"<<bad<<",\"max_error\":"<<maximum<<",\"device_errors\":"<<errors<<",\"scope\":\"Linear cache raw/sRGB/Xenos PWL clears, pending clear before first view, transfer policy propagated to later partial clear, mismatch rejection and full-clear reinitialization, native gamma binding refresh without sRGB change. Actual deferred FP16 clears and direct GPU copy isolate policy; reference uses pinned SDK PWL CPU decoder. Native ownership drawing tested separately. No live scene/presentation.\"}\n";REXLOG_INFO("HDR encoding:24576 components, invalid {}, error {}, device errors {}",bad,maximum,errors);return !bad && !errors && report.good();
}
}
