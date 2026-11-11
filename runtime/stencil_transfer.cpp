#include "stencil_transfer.h"
#include "fenced_scratch_cache.h"
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <map>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <immintrin.h>
REXCVAR_DEFINE_BOOL(em2_stencil_plane_copy,true,"GPU","Use native stencil value transfers on supported hardware; preserves original fallback");
REXCVAR_DEFINE_BOOL(em2_skip_stencil_scratch_clear,false,"GPU","Omit the scratch pre-clear when a native stencil value draw fully overwrites the copied samples");
REXCVAR_DEFINE_BOOL(em2_stencil_plane_barriers,false,"GPU","Transition only the stencil plane for native R8 copies when depth remains in DEPTH_WRITE");
REXCVAR_DEFINE_BOOL(em2_coalesce_stencil_copies,false,"GPU","Combine exact adjacent stencil copy rectangles without copying holes or unrelated pixels");
REXCVAR_DEFINE_BOOL(em2_fuse_depth_stencil_transfers,true,"GPU","Combine compatible native depth and stencil transfer draws; preserves original sample coverage, precision and unsupported-path fallback");
namespace {
using Key=std::tuple<uint64_t,uint32_t,uint32_t,uint32_t>;
struct Pool {bool checked=false,supported=false;em2::FencedScratchCache<Key,em2::StencilTransferScratch> cache{256ull*1024*1024};std::map<Key,uint64_t> retry_after;};
std::mutex mutex;std::unordered_map<const void*,Pool> pools;
thread_local uint64_t meshes=0;
thread_local uint64_t fused_meshes=0;
thread_local uint64_t clear_omissions=0;
thread_local uint64_t plane_barrier_transfers=0;
thread_local uint64_t copy_commands=0,copy_commands_saved=0;
}
std::shared_ptr<em2::StencilTransferScratch> em2::AcquireStencilTransferScratch(const void* owner,rex::graphics::d3d12::D3D12CommandProcessor& cp,ID3D12Resource* destination){
 if(!REXCVAR_GET(em2_stencil_plane_copy) || !owner || !destination)return {};
 struct Fp{unsigned csr=_mm_getcsr();~Fp(){_mm_setcsr(csr);}}fp;
 std::lock_guard lock(mutex);auto& pool=pools[owner];auto* device=cp.GetD3D12Provider().GetDevice();
 if(!pool.checked){pool.checked=true;D3D12_FEATURE_DATA_D3D12_OPTIONS2 options{};D3D12_FEATURE_DATA_FORMAT_SUPPORT format{DXGI_FORMAT_R8_UINT};
  pool.supported=SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2,&options,sizeof(options))) && options.ProgrammableSamplePositionsTier!=D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED && SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&format,sizeof(format))) && (format.Support1&D3D12_FORMAT_SUPPORT1_RENDER_TARGET);
 }
 if(!pool.supported)return {};
 auto desc=destination->GetDesc();if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize!=1 || desc.MipLevels!=1 || (desc.SampleDesc.Count!=1 && desc.SampleDesc.Count!=4))return {};
 const Key key{desc.Width,desc.Height,desc.SampleDesc.Count,desc.SampleDesc.Quality};const auto submission=cp.GetCurrentSubmission();if(auto found=pool.cache.Find(key,submission))return found;
 if(auto failed=pool.retry_after.find(key);failed!=pool.retry_after.end() && submission<failed->second)return {};
 desc.Format=DXGI_FORMAT_R8_UINT;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
 auto fail=[&](){pool.retry_after[key]=submission>UINT64_MAX-16?UINT64_MAX:submission+16;return std::shared_ptr<StencilTransferScratch>{};};
 const auto allocation=device->GetResourceAllocationInfo(0,1,&desc);
 if(!pool.cache.MakeRoom(allocation.SizeInBytes,submission,cp.GetCompletedSubmission()))return {};
 auto scratch=std::make_shared<StencilTransferScratch>();D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
 if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&scratch->texture))))return fail();
 D3D12_DESCRIPTOR_HEAP_DESC descriptors{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};if(FAILED(device->CreateDescriptorHeap(&descriptors,IID_PPV_ARGS(&scratch->descriptors))))return fail();scratch->rtv=scratch->descriptors->GetCPUDescriptorHandleForHeapStart();
 D3D12_RENDER_TARGET_VIEW_DESC view{};view.Format=DXGI_FORMAT_R8_UINT;view.ViewDimension=desc.SampleDesc.Count==1?D3D12_RTV_DIMENSION_TEXTURE2D:D3D12_RTV_DIMENSION_TEXTURE2DMS;device->CreateRenderTargetView(scratch->texture.Get(),&view,scratch->rtv);if(!pool.cache.Insert(key,scratch,allocation.SizeInBytes,submission))return {};pool.retry_after.erase(key);
 REXLOG_INFO("Native stencil scratch {}x{} samples{}; {} bytes retained of 256 MiB budget",desc.Width,desc.Height,desc.SampleDesc.Count,pool.cache.used());return scratch;
}
void em2::ShutdownStencilTransfers(const void* owner){std::lock_guard lock(mutex);pools.erase(owner);}
uint64_t em2::StencilValueTransferMeshes(){return meshes;}
void em2::CountStencilValueTransfer(){
 if(++meshes==1)REXLOG_INFO("Native R8 stencil value transfers active");
 else if((meshes&255)==1)REXLOG_DEBUG("Native R8 stencil value transfers: {} meshes",meshes);
}
uint64_t em2::FusedDepthStencilTransferMeshes(){return fused_meshes;}
void em2::CountFusedDepthStencilTransfer(){
 if(++fused_meshes==1)REXLOG_INFO("Native fused depth/stencil transfers active");
 else if((fused_meshes&255)==1)REXLOG_DEBUG("Native fused depth/stencil transfers: {} meshes",fused_meshes);
}
uint64_t em2::StencilScratchClearOmissions(){return clear_omissions;}
void em2::CountStencilScratchClearOmission(){++clear_omissions;}
uint64_t em2::StencilPlaneBarrierTransfers(){return plane_barrier_transfers;}
void em2::CountStencilPlaneBarrierTransfer(){++plane_barrier_transfers;}
uint64_t em2::StencilCopyCommands(){return copy_commands;}
uint64_t em2::StencilCopyCommandsSaved(){return copy_commands_saved;}
void em2::CountStencilCopyCommands(size_t issued,size_t saved){copy_commands+=issued;copy_commands_saved+=saved;}
size_t em2::CoalesceStencilCopyRectangles(std::vector<D3D12_RECT>& rectangles){
 const auto original=rectangles.size();if(original<2)return 0;
 for(const auto& r:rectangles)if(r.right<=r.left || r.bottom<=r.top)return 0;
 // Merge only equal-height horizontal spans, then equal-width vertical spans.
 // Each union is still a rectangle; L-shapes and resolve-clear holes survive.
 for(bool vertical:{false,true}){
  std::sort(rectangles.begin(),rectangles.end(),[=](const auto& a,const auto& b){
   return vertical?std::tie(a.left,a.right,a.top,a.bottom)<std::tie(b.left,b.right,b.top,b.bottom):std::tie(a.top,a.bottom,a.left,a.right)<std::tie(b.top,b.bottom,b.left,b.right);
  });
  size_t count=0;
  for(size_t i=0;i<rectangles.size();++i){const auto r=rectangles[i];
   if(count){auto& previous=rectangles[count-1];
    if(vertical?previous.left==r.left && previous.right==r.right && r.top<=previous.bottom:previous.top==r.top && previous.bottom==r.bottom && r.left<=previous.right){
     if(vertical)previous.bottom=std::max(previous.bottom,r.bottom);else previous.right=std::max(previous.right,r.right);continue;
    }
   }
   rectangles[count++]=r;
  }
  rectangles.resize(count);
 }
 return original-rectangles.size();
}
bool em2::TestStencilCopyCoalescing(){
 const auto csr=_mm_getcsr();struct Restore{unsigned csr;~Restore(){_mm_setcsr(csr);}}restore{csr};
 auto coverage=[](const auto& rectangles){uint64_t bits=0;for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)for(const auto& r:rectangles)if(LONG(x)>=r.left && LONG(x)<r.right && LONG(y)>=r.top && LONG(y)<r.bottom){bits|=uint64_t(1)<<(y*8+x);break;}return bits;};
 std::vector<D3D12_RECT> rectangles={{0,0,4,2},{4,0,8,2},{0,2,8,4}};
 if(CoalesceStencilCopyRectangles(rectangles)!=2 || rectangles.size()!=1 || rectangles[0].bottom!=4)return false;
 uint32_t random=0x31415926;uint64_t removed=0;
 for(unsigned test=0;test<50000;++test){rectangles.clear();
  const unsigned count=test%13;
  for(unsigned i=0;i<count;++i){auto next=[&](){random=random*1664525u+1013904223u;return (random>>16)%8;};const auto x=next(),y=next(),right=x+1+next()%(8-x),bottom=y+1+next()%(8-y);rectangles.push_back({LONG(x),LONG(y),LONG(right),LONG(bottom)});}
  const auto expected=coverage(rectangles);const auto saved=CoalesceStencilCopyRectangles(rectangles);removed+=saved;
  if(coverage(rectangles)!=expected || rectangles.size()+saved!=count){REXLOG_ERROR("Stencil copy union changed in fixture {}",test);return false;}
 }
 rectangles={{0,0,2,2},{1,1,1,2}};const auto malformed=rectangles;
 if(CoalesceStencilCopyRectangles(rectangles)!=0 || std::memcmp(rectangles.data(),malformed.data(),sizeof(D3D12_RECT)*2))return false;
 REXLOG_INFO("Stencil copy coalescing: 50000 independent bitmap unions, adjacent/full-row merging and malformed-input preservation passed; {} redundant copy rectangles removed",removed);return removed!=0;
}
