#include "built_shaders.h"
#include "hdr_scene_resolve.h"
#include "scene_ambient.h"
#include "hdr_guest_view.h"
#include "hdr_scene_cache.h"
#include "material_capture.h"
#include "hdr_scene_target.h"
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/deferred_command_list.h>
#include <rex/graphics/shared_memory.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/logging.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <map>
#include <mutex>
#include <optional>
#include <vector>
#include <immintrin.h>
namespace {
using Cache=rex::graphics::d3d12::D3D12RenderTargetCache;
using Info=rex::graphics::draw_util::ResolveInfo;
using Key=std::array<uint32_t,9>;
using Microsoft::WRL::ComPtr;
Key MakeKey(const Info& i){return {i.copy_dest_base,i.copy_dest_info.value,i.copy_dest_coordinate_info.packed,i.coordinate_info.width_div_8,i.height_div_8,i.coordinate_info.draw_resolution_scale_x,i.coordinate_info.draw_resolution_scale_y,i.copy_dest_extent_start,i.copy_dest_extent_length};}
struct Constants {std::array<uint32_t,4> output,resolve,source,region,options;};
struct Work {std::shared_ptr<em2::HdrSceneTarget> source;Constants constants;};
struct Fp{unsigned csr=_mm_getcsr();~Fp(){_mm_setcsr(csr);}};
}
namespace em2 {
struct HdrResolveContext {
    rex::graphics::SharedMemory& memory;
    rex::graphics::SharedMemory::GlobalWatchHandle watch=nullptr;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>,2> pipelines;
    std::array<std::map<Key,std::shared_ptr<HdrResolvedScene>>,3> images;
    std::array<uint64_t,3> epoch{1,1,1};
    HdrGuestViewPass guest_encoder;
    explicit HdrResolveContext(rex::graphics::SharedMemory& m):memory(m){
        watch=memory.RegisterGlobalWatch([](const std::unique_lock<std::recursive_mutex>&,void* opaque,uint32_t first,uint32_t last,bool){
            auto& self=*static_cast<HdrResolveContext*>(opaque);
            for(unsigned layer=0;layer<self.images.size();++layer){bool changed=false;
            for(auto& [key,image]:self.images[layer]){const auto& i=image->info_;if(image->valid_ && uint64_t(first)<uint64_t(i.copy_dest_extent_start)+i.copy_dest_extent_length && uint64_t(last)>=i.copy_dest_extent_start){image->valid_=false;changed=true;}}
            if(changed)++self.epoch[layer];}
        },this);
    }
    ~HdrResolveContext(){if(watch)memory.UnregisterGlobalWatch(watch);}
    bool Initialize(ID3D12Device* device){
        if(root)return true;
        
        // Native bindless one-use descriptors may be recycled separately.
        // Each source/destination therefore needs an independent table.
        const D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0}};
        D3D12_ROOT_PARAMETER parameters[3]{};parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants={0,0,sizeof(Constants)/4};
        for(unsigned i=0;i<2;++i){parameters[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[i+1].DescriptorTable={1,ranges+i};}
        const D3D12_ROOT_SIGNATURE_DESC desc{3,parameters};ComPtr<ID3DBlob> serialized,error;
        ComPtr<ID3D12RootSignature> new_root;std::array<ComPtr<ID3D12PipelineState>,2> new_pipelines;
        if(FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error)) || FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&new_root))))return false;
        for(unsigned ms=0;ms<2;++ms){
            const auto* shader=em2::GetBuiltShader(ms?em2::BuiltShader::HdrMSResolve:em2::BuiltShader::HdrResolve);if(!shader)return false;
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=new_root.Get();pso.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
            if(FAILED(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&new_pipelines[ms]))))return false;
        }
        root=std::move(new_root);pipelines=std::move(new_pipelines);return true;
    }
};
}
namespace {
std::mutex contexts_mutex;
std::map<const Cache*,std::shared_ptr<em2::HdrResolveContext>> contexts;
struct TextureState {
 const Cache* owner=nullptr;std::weak_ptr<em2::HdrResolveContext> context;uint64_t attachment=1;
 struct Batch{size_t count;uint64_t attachment,epoch;};std::map<const void*,Batch> batches;
};
std::map<const rex::graphics::TextureCache*,TextureState> texture_states;
std::atomic<size_t> texture_state_count{0};
uint32_t next_srv_identity=1; //18 reserved native SRV-key bits; never wrap/reuse.

std::shared_ptr<em2::HdrResolveContext> GetContext(const Cache* cache){std::lock_guard lock(contexts_mutex);auto it=contexts.find(cache);return it==contexts.end()?nullptr:it->second;}
}
std::shared_ptr<em2::HdrResolvedScene> em2::RecordSceneResolve(Cache* cache,rex::graphics::d3d12::D3D12CommandProcessor& cp,rex::graphics::SharedMemory& memory,const Info& info,std::span<const HdrResolveOwner> owners,SceneResolveLayer layer){
    Fp fp;const bool surface=layer==SceneResolveLayer::kMaterialSurface;const bool material=layer!=SceneResolveLayer::kHdr;const auto slot=size_t(layer);if(slot>=3 || !(material?HasMaterialCapture(cache):HasHdrSceneCache(cache)) || (surface && !HasMaterialSurfaceCapture(cache)))return {};
    namespace x=rex::graphics::xenos;
    const auto& e=info.color_edram_info;const auto& coordinate=info.coordinate_info;
    const uint32_t sx=coordinate.draw_resolution_scale_x,sy=coordinate.draw_resolution_scale_y,log=uint32_t(e.msaa_samples);
    const uint32_t width=coordinate.width_div_8*8,height=info.height_div_8*8;
    if(info.IsCopyingDepth() || e.is_depth || e.format_is_64bpp || (e.format!=0 && e.format!=1) || log>2 || !e.pitch_tiles || !width || !height || !sx || !sy || uint64_t(width)*sx>16384 || uint64_t(height)*sy>16384 ||
       e.fill_half_pixel_offset || info.copy_dest_info.copy_dest_format!=x::ColorFormat(x::TextureFormat::k_8_8_8_8) || !info.copy_dest_extent_length || uint64_t(info.copy_dest_extent_start)+info.copy_dest_extent_length>0x20000000ull || owners.empty() || owners.size()>512)return {};
    if(e.format==1 && !material && GetHdrSceneEncoding(cache)!=HdrSceneEncoding::kNativeLinear)return {};
    const auto sample_select=uint32_t(info.copy_dest_coordinate_info.copy_sample_select);
    if(sample_select>6)return {};
    uint32_t mask=sample_select<4?1u<<sample_select:sample_select==4?3:sample_select==5?12:15;
    if(mask&~((1u<<(1u<<log))-1))return {};
    uint32_t base,row_width,rows,pitch;info.GetCopyEdramTileSpan(base,row_width,rows,pitch);
    if(!row_width || !rows || row_width>pitch || uint64_t(row_width)*rows>2048)return {};
    const uint32_t tile_width=log>=2?40:80,tile_height=log>=1?8:16;
    const uint32_t offset_x=coordinate.edram_offset_x_div_8*8,offset_y=coordinate.edram_offset_y_div_8*8;
    std::vector<uint8_t> covered(size_t(row_width)*rows);std::vector<Work> work;work.reserve(owners.size());std::optional<bool> encoding;std::optional<HdrSceneEncoding> colour_encoding;std::optional<HdrGuestTransfer> guest_transfer;std::shared_ptr<const SceneViewIdentity> view;bool matching_view=true;
    for(const auto& owner:owners){
        if(!owner.legacy || !owner.pitch || owner.samples_log2!=log || !owner.width_tiles || !owner.height_tiles)return {};
        auto owner_view=FindHdrSceneView(cache,owner.legacy);if(work.empty())view=owner_view;else if(view!=owner_view)matching_view=false;
        if(!owner_view || owner_view->frame!=cp.GetCurrentFrame() || owner_view->submission!=cp.GetCurrentSubmission() || owner_view->width!=width*sx || owner_view->height!=height*sy || log)matching_view=false;
        const uint32_t ox=owner.offset%pitch,oy=owner.offset/pitch;
        if(uint64_t(ox)+owner.width_tiles>row_width || uint64_t(oy)+owner.height_tiles>rows)return {};
        bool srgb=false;std::shared_ptr<HdrSceneTarget> target;HdrSceneEncoding source_encoding=HdrSceneEncoding::kNativeLinear;HdrGuestTransfer source_transfer=HdrGuestTransfer::kRaw;if(surface)target=FindMaterialSurfaceSource(cache,owner.legacy).target;else if(material)target=FindMaterialCaptureSource(cache,owner.legacy).target;else {auto binding=FindHdrSceneSource(cache,owner.legacy,srgb);target=std::move(binding.target);source_encoding=binding.encoding;source_transfer=binding.guest_transfer;}if(!target || (encoding && *encoding!=srgb) || (colour_encoding && *colour_encoding!=source_encoding) || (guest_transfer && *guest_transfer!=source_transfer))return {};encoding=srgb;colour_encoding=source_encoding;guest_transfer=source_transfer;
        const auto desc=target->texture()->GetDesc();const auto samples=desc.SampleDesc.Count;
        if(samples!=(1u<<log) && !(log==1 && samples==4))return {};
        // Validate every covered tile against the actual resource, before any
        // commands or descriptors are emitted. No out-of-bounds black fallback.
        for(uint32_t y=oy;y<oy+owner.height_tiles;++y)for(uint32_t z=ox;z<ox+owner.width_tiles;++z){
            if(covered[y*row_width+z]++)return {};
            const uint32_t tile=(base+y*pitch+z-owner.base)&2047;
            const uint32_t left=std::max(z*tile_width,offset_x%tile_width),right=std::min((z+1)*tile_width,offset_x%tile_width+width);
            const uint32_t top=std::max(y*tile_height,offset_y%tile_height),bottom=std::min((y+1)*tile_height,offset_y%tile_height+height);
            // AO uses the original raster's absolute pixel grid. Only a
            // one-to-one source-to-resolve mapping may retain its camera tag.
            // A relocated tile, crop, multisample resolve or mixed owner still
            // gets an HDR image, but cannot authorize scene-light composition.
            if((tile%owner.pitch)*tile_width+left-z*tile_width!=left-offset_x%tile_width || (tile/owner.pitch)*tile_height+top-y*tile_height!=top-offset_y%tile_height)matching_view=false;
            if(right<=left || bottom<=top || uint64_t((tile%owner.pitch)*tile_width+right-z*tile_width)*sx>desc.Width || uint64_t((tile/owner.pitch)*tile_height+bottom-y*tile_height)*sy>desc.Height)return {};
        }
        const uint32_t left=std::max(ox*tile_width,offset_x%tile_width)-(offset_x%tile_width),top=std::max(oy*tile_height,offset_y%tile_height)-(offset_y%tile_height);
        const uint32_t right=std::min((ox+owner.width_tiles)*tile_width,offset_x%tile_width+width)-(offset_x%tile_width),bottom=std::min((oy+owner.height_tiles)*tile_height,offset_y%tile_height+height)-(offset_y%tile_height);
        Work w;w.source=std::move(target);w.constants={{width*sx,height*sy,sx,sy},{e.base_tiles,e.pitch_tiles,offset_x,offset_y},{owner.base,owner.pitch,log,samples},{left*sx,top*sy,(right-left)*sx,(bottom-top)*sy},{mask,info.copy_dest_info.copy_dest_swap,surface?0u:uint32_t(int32_t(info.copy_dest_info.copy_dest_exp_bias)),surface?1u:0u}};work.push_back(std::move(w));
    }
    if(std::find(covered.begin(),covered.end(),uint8_t(0))!=covered.end())return {};
    auto context=GetContext(cache);if(!context){context=std::make_shared<HdrResolveContext>(memory);if(!context->watch)return {};std::lock_guard lock(contexts_mutex);contexts.emplace(cache,context);}
    if(&context->memory!=&memory)return {};
    auto* device=cp.GetD3D12Provider().GetDevice();if(!context->Initialize(device))return {};
    auto& images=context->images[slot];const auto key=MakeKey(info);auto it=images.find(key);std::shared_ptr<HdrResolvedScene> image;
    if(it!=images.end())image=it->second;
    else {
        if(images.size()>=64)return {};
        image=std::shared_ptr<HdrResolvedScene>(new HdrResolvedScene);D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=width*sx;desc.Height=height*sy;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const auto bytes=device->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;if(!bytes || bytes==UINT64_MAX || !(material?ReserveMaterialCaptureMemory(cache,bytes):ReserveHdrSceneMemory(cache,bytes)))return {};
        struct Reservation{const Cache* cache;uint64_t bytes;bool material;~Reservation(){if(bytes){if(material)ReleaseMaterialCaptureMemory(cache,bytes);else ReleaseHdrSceneMemory(cache,bytes);}}}reservation{cache,bytes,material};
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&image->texture_))))return {};
        const D3D12_DESCRIPTOR_HEAP_DESC views{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
        if(FAILED(device->CreateDescriptorHeap(&views,IID_PPV_ARGS(&image->uav_))))return {};
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=desc.Format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device->CreateUnorderedAccessView(image->texture_.Get(),nullptr,&uav,image->uav_->GetCPUDescriptorHandleForHeapStart());
        {std::lock_guard lock(contexts_mutex);if(next_srv_identity<=0x3ffff)image->srv_identity_=next_srv_identity++;}
        image->layer_=layer;images.emplace(key,image);reservation.bytes=0;
    }
    image->guest_transfer_=*guest_transfer;image->guest_revision_=0;image->lighting_applied_=false;image->view_=matching_view?view:nullptr;image->encoding_=*colour_encoding;image->submission_=cp.GetCurrentSubmission();image->frame_=cp.GetCurrentFrame();++image->revision_;image->info_=info;image->srgb_=*encoding;image->valid_=false;++context->epoch[slot];
    std::vector<rex::ui::d3d12::util::DescriptorCpuGpuHandlePair> descriptors(work.size()*2);
    if(!cp.RequestOneUseSingleViewDescriptors(uint32_t(descriptors.size()),descriptors.data()))return {};
    for(size_t i=0;i<work.size();++i){device->CopyDescriptorsSimple(1,descriptors[i*2].first,work[i].source->srv(),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);device->CopyDescriptorsSimple(1,descriptors[i*2+1].first,image->uav_->GetCPUDescriptorHandleForHeapStart(),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);}
    const auto read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    cp.PushTransitionBarrier(image->texture(),read,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto& list=cp.GetDeferredCommandList();list.D3DSetComputeRootSignature(context->root.Get());
    for(size_t i=0;i<work.size();++i){auto& w=work[i];cp.PushTransitionBarrier(w.source->texture(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);cp.SubmitBarriers();
        cp.SetExternalPipeline(context->pipelines[log!=0].Get());list.D3DSetComputeRoot32BitConstants(0,sizeof(Constants)/4,&w.constants,0);list.D3DSetComputeRootDescriptorTable(1,descriptors[i*2].second);list.D3DSetComputeRootDescriptorTable(2,descriptors[i*2+1].second);list.D3DDispatch((w.constants.region[2]+7)/8,(w.constants.region[3]+7)/8,1);
        cp.PushTransitionBarrier(w.source->texture(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    cp.PushTransitionBarrier(image->texture(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,read);cp.SubmitBarriers();return image;
}
std::shared_ptr<em2::HdrResolvedScene> em2::RecordHdrSceneResolve(Cache* cache,rex::graphics::d3d12::D3D12CommandProcessor& cp,rex::graphics::SharedMemory& memory,const Info& info,std::span<const HdrResolveOwner> owners){return RecordSceneResolve(cache,cp,memory,info,owners,SceneResolveLayer::kHdr);}
std::shared_ptr<em2::HdrResolvedScene> em2::RecordMaterialSceneResolve(Cache* cache,rex::graphics::d3d12::D3D12CommandProcessor& cp,rex::graphics::SharedMemory& memory,const Info& info,std::span<const HdrResolveOwner> owners){return RecordSceneResolve(cache,cp,memory,info,owners,SceneResolveLayer::kMaterialAmbient);}
std::shared_ptr<em2::HdrResolvedScene> em2::RecordMaterialSurfaceResolve(Cache* cache,rex::graphics::d3d12::D3D12CommandProcessor& cp,rex::graphics::SharedMemory& memory,const Info& info,std::span<const HdrResolveOwner> owners){return RecordSceneResolve(cache,cp,memory,info,owners,SceneResolveLayer::kMaterialSurface);}
void em2::CommitSceneResolve(Cache* cache,const std::shared_ptr<HdrResolvedScene>& image,bool copied,SceneResolveLayer layer){if(!image || image->layer_!=layer)return;auto context=GetContext(cache);if(context){auto& images=context->images[size_t(layer)];auto it=images.find(MakeKey(image->info_));if(it!=images.end() && it->second==image){image->valid_=copied;++context->epoch[size_t(layer)];}}}
void em2::CommitHdrSceneResolve(Cache* cache,const std::shared_ptr<HdrResolvedScene>& image,bool copied){CommitSceneResolve(cache,image,copied,SceneResolveLayer::kHdr);}
void em2::CommitMaterialSceneResolve(Cache* cache,const std::shared_ptr<HdrResolvedScene>& image,bool copied){CommitSceneResolve(cache,image,copied,SceneResolveLayer::kMaterialAmbient);}
void em2::CommitMaterialSurfaceResolve(Cache* cache,const std::shared_ptr<HdrResolvedScene>& image,bool copied){CommitSceneResolve(cache,image,copied,SceneResolveLayer::kMaterialSurface);}
std::shared_ptr<const em2::HdrResolvedScene> em2::FindHdrSceneResolve(Cache* cache,const Info& info){auto context=GetContext(cache);if(!context)return {};auto& images=context->images[0];auto it=images.find(MakeKey(info));return it!=images.end() && it->second->valid_?it->second:nullptr;}
std::shared_ptr<const em2::HdrResolvedScene> em2::FindMaterialSceneResolve(Cache* cache,const Info& info){auto context=GetContext(cache);if(!context)return {};auto& images=context->images[1];auto it=images.find(MakeKey(info));return it!=images.end() && it->second->valid_?it->second:nullptr;}
std::shared_ptr<const em2::HdrResolvedScene> em2::FindMaterialSurfaceResolve(Cache* cache,const Info& info){auto context=GetContext(cache);if(!context)return {};auto& images=context->images[2];auto it=images.find(MakeKey(info));return it!=images.end() && it->second->valid_?it->second:nullptr;}
void em2::ShutdownMaterialSceneResolves(const Cache* cache){auto context=GetContext(cache);if(context){rex::thread::global_critical_region region;auto lock=region.Acquire();for(size_t slot=1;slot<context->images.size();++slot){for(auto& [key,image]:context->images[slot])image->valid_=false;context->images[slot].clear();++context->epoch[slot];}}}
void em2::ShutdownHdrSceneResolves(const Cache* cache){
    // Watch teardown takes the native global memory lock. Never hold the
    // registry mutex while acquiring it (lookups use the opposite order).
    std::shared_ptr<HdrResolveContext> retired;
    {std::lock_guard lock(contexts_mutex);auto it=contexts.find(cache);if(it!=contexts.end()){retired=std::move(it->second);contexts.erase(it);for(auto& [texture,state]:texture_states)if(state.owner==cache){state.owner=nullptr;state.context.reset();++state.attachment;}}}
}

void em2::AttachHdrSceneTextureCache(const Cache* cache,const rex::graphics::TextureCache* texture){
 std::lock_guard lock(contexts_mutex);auto it=contexts.find(cache);if(it==contexts.end())return;
 auto& state=texture_states[texture];texture_state_count.store(texture_states.size(),std::memory_order_release);if(state.context.lock()!=it->second){state.owner=cache;state.context=it->second;++state.attachment;}
}
std::shared_ptr<const em2::HdrResolvedScene> em2::FindSceneTexture(const rex::graphics::TextureCache* texture,const HdrTextureQuery& query,SceneResolveLayer layer){
 if(size_t(layer)>=3)return {};
 std::shared_ptr<HdrResolveContext> context;{std::lock_guard lock(contexts_mutex);auto it=texture_states.find(texture);if(it==texture_states.end())return {};context=it->second.context.lock();}if(!context)return {};
 std::shared_ptr<const HdrResolvedScene> result;
 for(const auto& [key,image]:context->images[size_t(layer)]){const auto& i=image->info_;const auto& d=i.copy_dest_coordinate_info;
  if(!image->valid_ || (image->srgb_ && image->encoding_!=HdrSceneEncoding::kNativeLinear) || !image->srv_identity_ || i.copy_dest_base!=query.base || i.copy_dest_info.copy_dest_array || d.offset_x_div_8 || d.offset_y_div_8 ||
   i.coordinate_info.width_div_8*8!=query.width || i.height_div_8*8!=query.height || d.pitch_aligned_div_32!=query.pitch || uint32_t(i.copy_dest_info.copy_dest_endian)!=query.endian ||
   i.coordinate_info.draw_resolution_scale_x!=query.scale_x || i.coordinate_info.draw_resolution_scale_y!=query.scale_y)continue;
  // Multiple currently valid representations are ambiguous, even if their
  // bounding address overlaps. Never guess which one a fetch means.
  if(result)return {};result=image;
 }
 return result;
}
std::shared_ptr<const em2::HdrResolvedScene> em2::FindHdrSceneTexture(const rex::graphics::TextureCache* texture,const HdrTextureQuery& query){return FindSceneTexture(texture,query,SceneResolveLayer::kHdr);}
std::shared_ptr<const em2::HdrResolvedScene> em2::FindMaterialSceneTexture(const rex::graphics::TextureCache* texture,const HdrTextureQuery& query){return FindSceneTexture(texture,query,SceneResolveLayer::kMaterialAmbient);}
std::shared_ptr<const em2::HdrResolvedScene> em2::FindMaterialSurfaceTexture(const rex::graphics::TextureCache* texture,const HdrTextureQuery& query){return FindSceneTexture(texture,query,SceneResolveLayer::kMaterialSurface);}
bool em2::HdrTextureKeyBatchCurrent(const rex::graphics::TextureCache* texture,const void* keys,size_t count){
 std::lock_guard lock(contexts_mutex);auto it=texture_states.find(texture);if(it==texture_states.end())return true;auto& state=it->second;auto context=state.context.lock();
 auto batch=state.batches.find(keys);return batch!=state.batches.end() && batch->second.count==count && batch->second.attachment==state.attachment && batch->second.epoch==(context?context->epoch[0]:0);
}
void em2::RememberHdrTextureKeyBatch(const rex::graphics::TextureCache* texture,const void* keys,size_t count){
 std::lock_guard lock(contexts_mutex);auto it=texture_states.find(texture);if(it==texture_states.end())return;auto& state=it->second;auto context=state.context.lock();
 if(state.batches.size()>=256 && !state.batches.contains(keys))state.batches.clear();
 state.batches[keys]={count,state.attachment,context?context->epoch[0]:0};
}
void em2::ForgetHdrTextureCache(const rex::graphics::TextureCache* texture){std::lock_guard lock(contexts_mutex);texture_states.erase(texture);texture_state_count.store(texture_states.size(),std::memory_order_release);}
bool em2::HasHdrTextureStates(){return texture_state_count.load(std::memory_order_acquire)!=0;}

namespace {
using PublishCP=rex::graphics::d3d12::D3D12CommandProcessor;using PublishList=rex::graphics::d3d12::DeferredCommandList;
template<class Tag,auto Member>struct PublishAccess{friend auto MemberOf(Tag){return Member;}};
struct PublishStream{friend auto MemberOf(PublishStream);};template struct PublishAccess<PublishStream,&PublishList::command_stream_>;
struct PublishBarriers{friend auto MemberOf(PublishBarriers);};template struct PublishAccess<PublishBarriers,&PublishCP::barriers_>;
}
bool em2::PublishSceneAmbientResult(Cache* cache,PublishCP& cp,const std::shared_ptr<const HdrResolvedScene>& colour,const std::shared_ptr<const SceneAmbientBindings>& composition){
    if(!colour || !composition || !colour->valid_ || colour->layer_!=SceneResolveLayer::kHdr || colour->lighting_applied_ || colour->frame_!=cp.GetCurrentFrame() || colour->submission_!=cp.GetCurrentSubmission() || !composition->can_publish_to(*colour))return false;
    auto context=GetContext(cache);if(!context)return false;auto& images=context->images[0];auto owner=images.find(MakeKey(colour->info_));if(owner==images.end() || owner->second.get()!=colour.get())return false;
    auto* destination=colour->texture();auto* source=composition->result();if(!destination || !source || destination==source)return false;
    const auto a=destination->GetDesc(),b=source->GetDesc();
    if(a.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || a.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT || a.SampleDesc.Count!=1 || a.MipLevels!=1 || a.DepthOrArraySize!=1 || a.Width!=b.Width || a.Height!=b.Height || a.Format!=b.Format || a.Dimension!=b.Dimension || b.SampleDesc.Count!=1 || b.MipLevels!=1 || b.DepthOrArraySize!=1)return false;
    ComPtr<ID3D12Device> device;if(FAILED(destination->GetDevice(IID_PPV_ARGS(&device))) || device.Get()!=cp.GetD3D12Provider().GetDevice())return false;
    auto& list=cp.GetDeferredCommandList();auto& stream=list.*MemberOf(PublishStream{});const auto pending=(cp.*MemberOf(PublishBarriers{})).size();
    // The native writer stores uintmax_t words. Deliberately overreserve for
    // its pending barrier packet plus both fixed copy barrier packets. After
    // this point SubmitBarriers and these three commands cannot grow storage.
    const auto room=stream.max_size()-stream.size();if(room<256 || pending>(room-256)/sizeof(D3D12_RESOURCE_BARRIER))return false;
    stream.reserve(stream.size()+256+pending*sizeof(D3D12_RESOURCE_BARRIER));cp.SubmitBarriers();
    constexpr auto read=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_BARRIER barriers[2]{};for(auto& barrier:barriers){barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;barrier.Transition.StateBefore=read;}
    barriers[0].Transition.pResource=source;barriers[0].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;barriers[1].Transition.pResource=destination;barriers[1].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
    list.D3DResourceBarrier(2,barriers);list.D3DCopyResource(destination,source);
    for(auto& barrier:barriers)std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list.D3DResourceBarrier(2,barriers);
    auto& image=*owner->second;image.lighting_applied_=true;++image.revision_;++context->epoch[0];return true;
}

ID3D12Resource* em2::HdrResolvedScene::guest_texture()const{if(guest_transfer_==HdrGuestTransfer::kRaw)return texture();return !guest_disabled_ && guest_view_ && guest_revision_==revision_?guest_view_->result():nullptr;}
uint32_t em2::HdrResolvedScene::guest_srv_identity()const{return guest_transfer_==HdrGuestTransfer::kRaw?srv_identity_:guest_texture()?guest_srv_identity_:0;}
bool em2::RefreshHdrSceneGuestView(Cache* cache,PublishCP& cp,const std::shared_ptr<HdrResolvedScene>& image){
    if(!image || !image->valid_ || image->layer_!=SceneResolveLayer::kHdr || image->frame_!=cp.GetCurrentFrame() || image->submission_!=cp.GetCurrentSubmission())return false;
    if(image->guest_transfer_==HdrGuestTransfer::kRaw)return true;if(image->guest_disabled_)return false;
    auto context=GetContext(cache);if(!context)return false;const auto found=context->images[0].find(MakeKey(image->info_));if(found==context->images[0].end() || found->second!=image)return false;
    if(image->guest_revision_==image->revision_)return true;
    try{
        auto* device=cp.GetD3D12Provider().GetDevice();if(!context->guest_encoder.Initialize(device)){image->guest_disabled_=true;return false;}
        if(!image->guest_view_){auto d=image->texture()->GetDesc();d.Alignment=0;d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;const auto bytes=device->GetResourceAllocationInfo(0,1,&d).SizeInBytes;if(!bytes || bytes==UINT64_MAX || !ReserveHdrSceneMemory(cache,bytes)){image->guest_disabled_=true;return false;}
            struct Reservation{const Cache* cache;uint64_t bytes;~Reservation(){if(bytes)ReleaseHdrSceneMemory(cache,bytes);}}reservation{cache,bytes};auto view=HdrGuestViewBindings::Create(image->texture());if(!view){image->guest_disabled_=true;return false;}uint32_t identity=0;{std::lock_guard lock(contexts_mutex);if(next_srv_identity<=0x3ffff)identity=next_srv_identity++;}if(!identity){image->guest_disabled_=true;return false;}image->guest_view_=std::move(view);image->guest_srv_identity_=identity;reservation.bytes=0;
        }
        if(!context->guest_encoder.Record(cp,*image->guest_view_,image->guest_transfer_,int32_t(image->info_.copy_dest_info.copy_dest_exp_bias))){image->guest_disabled_=true;return false;}
        image->guest_revision_=image->revision_;return true;
    }catch(const std::exception& e){image->guest_disabled_=true;REXLOG_WARN("HDR encoded guest view unavailable: {}",e.what());return false;}
}
