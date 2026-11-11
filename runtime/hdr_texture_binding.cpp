#include "hdr_scene_resolve.h"
#include "scene_depth_view.h"
#include "gpu_constants_internal.h"
#include "scene_lighting.h"
#include <rex/graphics/d3d12/texture_cache.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <optional>
namespace {
using TextureCache=rex::graphics::d3d12::D3D12TextureCache;
using ShaderBinding=rex::graphics::d3d12::D3D12Shader::TextureBinding;
using BaseTextureCache=rex::graphics::TextureCache;
template<class Tag,auto Member>struct Access {friend auto MemberOf(Tag){return Member;}};
template<class Tag,class Type>struct TypeAccess {friend auto TypeOf(Tag){return std::type_identity<Type>{};}};
struct FetchInfoTag{friend auto MemberOf(FetchInfoTag);};template struct Access<FetchInfoTag,&BaseTextureCache::BindingInfoFromFetchConstant>;
struct KeyTag{friend auto TypeOf(KeyTag);};template struct TypeAccess<KeyTag,BaseTextureCache::TextureKey>;
struct ActiveTag{friend auto MemberOf(ActiveTag);};template struct Access<ActiveTag,&BaseTextureCache::GetValidTextureBinding>;
struct SyncTag{friend auto MemberOf(SyncTag);};template struct Access<SyncTag,&BaseTextureCache::texture_bindings_in_sync_>;
struct RegistersTag{friend auto MemberOf(RegistersTag);};template struct Access<RegistersTag,&BaseTextureCache::register_file>;
struct HostTextureTag{friend auto TypeOf(HostTextureTag);};template struct TypeAccess<HostTextureTag,TextureCache::D3D12Texture>;
struct ResourceStateTag{friend auto MemberOf(ResourceStateTag);};template struct Access<ResourceStateTag,&TextureCache::D3D12Texture::resource_state_>;
struct UnormTag{friend auto MemberOf(UnormTag);};template struct Access<UnormTag,static_cast<DXGI_FORMAT(TextureCache::*)(rex::graphics::xenos::TextureFormat,uint32_t,uint32_t)const>(&TextureCache::GetDXGIUnormFormat)>;
struct CacheTag{friend auto MemberOf(CacheTag);};template struct Access<CacheTag,&rex::graphics::d3d12::D3D12CommandProcessor::texture_cache_>;
// A resolved image may replace only a complete, single-level unsigned 2D
// texture. Gamma/signed interpretation needs coordinated shader changes.
template<class Binding>std::optional<em2::HdrTextureQuery> Query(const Binding* binding,const ShaderBinding& host,uint32_t sx,uint32_t sy,bool loaded_scale=true){
    namespace x=rex::graphics::xenos;
    if(!binding || !binding->texture || host.is_signed || host.dimension!=x::FetchOpDimension::k2D)return {};
    const auto& k=binding->key;
    if(!k.is_valid || k.dimension!=x::DataDimension::k2DOrStacked || k.depth_or_array_size_minus_1 || !k.tiled || !k.base_page || k.mip_page || k.mip_max_level || k.packed_mips || k.signed_separate ||
       k.format!=x::TextureFormat::k_8_8_8_8 || binding->swizzled_signs || (loaded_scale && bool(k.scaled_resolve)!=(sx>1 || sy>1)))return {};
    return em2::HdrTextureQuery{uint32_t(k.base_page)<<12,k.GetWidth(),k.GetHeight(),k.pitch,uint32_t(k.endianness),sx,sy};
}
D3D12_SHADER_RESOURCE_VIEW_DESC View(uint32_t swizzle){
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};v.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
    v.Shader4ComponentMapping=swizzle|D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES;
    // Native DXBC uses Texture2DArray for ordinary 2D guest textures.
    v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;v.Texture2DArray.MipLevels=1;v.Texture2DArray.ArraySize=1;return v;
}
}
std::shared_ptr<const em2::SceneDepthView> em2::AcquireSceneDepthView(TextureCache& cache,const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,std::shared_ptr<const SceneDepthView> previous){
    namespace x=rex::graphics::xenos;
    if(fetch.type!=x::FetchConstantType::kTexture || fetch.dimension!=x::DataDimension::k2DOrStacked || fetch.stacked || fetch.mip_min_level || fetch.mip_max_level || fetch.exp_adjust)return {};
    rex::thread::global_critical_region region;auto lock=region.Acquire();
    const auto actual=(cache.*MemberOf(RegistersTag{}))().GetTextureFetch(0);
    if(std::memcmp(&actual,&fetch,sizeof(fetch)) || !(cache.*MemberOf(SyncTag{})&1u))return {};
    auto* binding=(cache.*MemberOf(ActiveTag{}))(0);
    if(!binding || !binding->texture || (binding->swizzled_signs&3u) || binding->key.depth_or_array_size_minus_1 || binding->key.mip_max_level)return {};
    using Key=decltype(TypeOf(KeyTag{}))::type;Key expected;uint8_t signs=0;
    MemberOf(FetchInfoTag{})(fetch,expected,&signs);expected.scaled_resolve=binding->key.scaled_resolve;
    if(expected!=binding->key || signs!=binding->swizzled_signs || binding->texture->base_outdated(lock))return {};
    using Host=decltype(TypeOf(HostTextureTag{}))::type;
    auto* texture=static_cast<Host*>(binding->texture);
    const auto read=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if((texture->*MemberOf(ResourceStateTag{})&read)!=read)return {};
    ShaderBinding host{};host.dimension=x::FetchOpDimension::k2D;
    auto query=Query(binding,host,cache.draw_resolution_scale_x(),cache.draw_resolution_scale_y());
    auto hdr=query?FindHdrSceneTexture(&cache,*query):nullptr;
    if(hdr && !hdr->guest_texture())hdr.reset();
    ID3D12Resource* resource=hdr?hdr->guest_texture():texture->resource();if(!resource)return {};
    const auto desc=resource->GetDesc();
    if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize!=1 || desc.MipLevels!=1 || desc.SampleDesc.Count!=1)return {};
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=hdr?DXGI_FORMAT_R16G16B16A16_FLOAT:(cache.*MemberOf(UnormTag{}))(binding->key.format,binding->key.GetWidth(),binding->key.GetHeight());
    if(view.Format==DXGI_FORMAT_UNKNOWN)return {};
    view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;view.Shader4ComponentMapping=binding->host_swizzle|D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES;
    view.Texture2DArray.MipLevels=view.Texture2DArray.ArraySize=1;
    if(previous && previous->resource()==resource && !std::memcmp(&previous->description_,&view,sizeof(view)) && previous->hdr_owner_==hdr){texture->MarkAsUsed();return previous;}
    Microsoft::WRL::ComPtr<ID3D12Device> device;if(FAILED(resource->GetDevice(IID_PPV_ARGS(&device))))return {};
    const auto allocation=device->GetResourceAllocationInfo(0,1,&desc);if(allocation.SizeInBytes>64ull*1024*1024)return {};
    auto result=std::make_shared<SceneDepthView>();result->resource_=resource;result->description_=view;result->hdr_owner_=std::move(hdr);
    const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    if(FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&result->heap_))))return {};
    device->CreateShaderResourceView(resource,&view,result->srv());texture->MarkAsUsed();return result;
}
std::shared_ptr<const em2::SceneDepthView> em2::AcquireCurrentSceneDepthView(Em2CommandProcessor& cp,const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,std::shared_ptr<const SceneDepthView> previous){
    if(!cp.IsGraphicsWorker())return {};
    // Optional rendering data must not turn a host allocation failure after an
    // accepted native draw into a title failure. HRESULT allocation failures
    // already decline in AcquireSceneDepthView.
    try{auto* cache=(cp.*MemberOf(CacheTag{})).get();return cache?AcquireSceneDepthView(*cache,fetch,std::move(previous)):nullptr;}
    catch(const std::bad_alloc&){return {};}
}
std::shared_ptr<const em2::HdrResolvedScene> em2::FindHdrSwapScene(const BaseTextureCache* cache,const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch){
    // Both native painting and HDR output produce opaque alpha. Only the RGB
    // mapping matters here; games may fetch constant-one alpha for a backbuffer.
    if(!cache || (fetch.swizzle&0x1ff)!=0x88 || !HasHdrTextureStates())return {};
    using Key=decltype(TypeOf(KeyTag{}))::type;
    struct Binding {Key key;bool texture=true;uint8_t swizzled_signs=0;} binding;
    MemberOf(FetchInfoTag{})(fetch,binding.key,&binding.swizzled_signs);
    ShaderBinding host{};host.dimension=rex::graphics::xenos::FetchOpDimension::k2D;
    // The fetch decoder has not loaded a native texture, so scaled_resolve is
    // unset here. The authoritative HDR resolve lookup verifies exact scales.
    auto query=Query(&binding,host,cache->draw_resolution_scale_x(),cache->draw_resolution_scale_y(),false);
    return query?FindHdrSceneTexture(cache,*query):nullptr;
}
extern "C" void EM2OriginalWriteActiveTextureBindfulSRV(TextureCache*,const ShaderBinding&,D3D12_CPU_DESCRIPTOR_HANDLE);
extern "C" uint32_t EM2OriginalGetActiveTextureBindlessSRVIndex(TextureCache*,const ShaderBinding&);
extern "C" bool EM2OriginalAreActiveTextureSRVKeysUpToDate(const TextureCache*,const TextureCache::TextureSRVKey*,const ShaderBinding*,size_t);
extern "C" void EM2OriginalWriteActiveTextureSRVKeys(const TextureCache*,TextureCache::TextureSRVKey*,const ShaderBinding*,size_t);
extern "C" void EM2OriginalDestroyAllTextures(rex::graphics::TextureCache*,bool);
extern "C" ID3D12Resource* EM2OriginalRequestSwapTexture(TextureCache*,D3D12_SHADER_RESOURCE_VIEW_DESC&,rex::graphics::xenos::TextureFormat&);
ID3D12Resource* TextureCache::RequestSwapTexture(D3D12_SHADER_RESOURCE_VIEW_DESC& view,rex::graphics::xenos::TextureFormat& format){
    auto* original=EM2OriginalRequestSwapTexture(this,view,format);
    if(!original || !em2::SceneLightingEnabled())return original;
    rex::thread::global_critical_region region;auto lock=region.Acquire();
    const auto fetch=register_file().GetTextureFetch(0);auto scene=em2::FindHdrSwapScene(this,fetch);
    auto* game=dynamic_cast<em2::Em2CommandProcessor*>(&command_processor_);
    auto* replacement=game?game->RequestSceneSdrSwapTexture(scene):nullptr;
    if(!replacement)return original;
    view.Format=DXGI_FORMAT_R8G8B8A8_UNORM;return replacement;
}
void TextureCache::WriteActiveTextureBindfulSRV(const ShaderBinding& host,D3D12_CPU_DESCRIPTOR_HANDLE handle){
    if(!em2::HasHdrTextureStates()){EM2OriginalWriteActiveTextureBindfulSRV(this,host,handle);return;}
    rex::thread::global_critical_region region;auto lock=region.Acquire();
    auto* binding=GetValidTextureBinding(host.fetch_constant);auto query=Query(binding,host,draw_resolution_scale_x(),draw_resolution_scale_y());
    auto image=query?em2::FindHdrSceneTexture(this,*query):nullptr;
    if(image && (!image->guest_texture() || !image->guest_srv_identity()))image.reset();
    if(!image){EM2OriginalWriteActiveTextureBindfulSRV(this,host,handle);return;}
    binding->texture->MarkAsUsed();const auto view=View(binding->host_swizzle);
    command_processor_.GetD3D12Provider().GetDevice()->CreateShaderResourceView(image->guest_texture(),&view,handle);
}
uint32_t TextureCache::GetActiveTextureBindlessSRVIndex(const ShaderBinding& host){
    if(!em2::HasHdrTextureStates())return EM2OriginalGetActiveTextureBindlessSRVIndex(this,host);
    rex::thread::global_critical_region region;auto lock=region.Acquire();
    auto* binding=GetValidTextureBinding(host.fetch_constant);auto query=Query(binding,host,draw_resolution_scale_x(),draw_resolution_scale_y());
    auto image=query?em2::FindHdrSceneTexture(this,*query):nullptr;
    if(image && (!image->guest_texture() || !image->guest_srv_identity()))image.reset();
    if(!image)return EM2OriginalGetActiveTextureBindlessSRVIndex(this,host);
    auto* texture=static_cast<D3D12Texture*>(binding->texture);texture->MarkAsUsed();
    // Reserved bits of the native private descriptor-map key distinguish HDR
    // resource identity from legacy signed/swizzled views. No ABI/layout change.
    static_assert(sizeof(D3D12Texture::SRVDescriptorKey)==4);
    D3D12Texture::SRVDescriptorKey key;key.key=0x80000000u|(image->guest_srv_identity()<<13)|(binding->host_swizzle<<1);
    auto index=texture->GetSRVDescriptorIndex(key);if(index!=UINT32_MAX)return index;
    index=command_processor_.RequestPersistentViewBindlessDescriptor();
    if(index==UINT32_MAX)return EM2OriginalGetActiveTextureBindlessSRVIndex(this,host);
    const auto& provider=command_processor_.GetD3D12Provider();auto cpu=command_processor_.GetViewBindlessHeapCPUStart();cpu.ptr+=SIZE_T(index)*provider.GetViewDescriptorSize();
    const auto view=View(binding->host_swizzle);provider.GetDevice()->CreateShaderResourceView(image->guest_texture(),&view,cpu);
    // Native texture destruction releases these descriptors, before the
    // processor destroys its bindless heap. Never overwrite an in-flight view.
    texture->AddSRVDescriptorIndex(key,index);return index;
}
bool TextureCache::AreActiveTextureSRVKeysUpToDate(const TextureSRVKey* keys,const ShaderBinding* bindings,size_t count)const{
    if(!em2::HasHdrTextureStates())return EM2OriginalAreActiveTextureSRVKeysUpToDate(this,keys,bindings,count);
    rex::thread::global_critical_region region;auto lock=region.Acquire();
    return em2::HdrTextureKeyBatchCurrent(this,keys,count) && EM2OriginalAreActiveTextureSRVKeysUpToDate(this,keys,bindings,count);
}
void TextureCache::WriteActiveTextureSRVKeys(TextureSRVKey* keys,const ShaderBinding* bindings,size_t count)const{
    if(!em2::HasHdrTextureStates()){EM2OriginalWriteActiveTextureSRVKeys(this,keys,bindings,count);return;}
    rex::thread::global_critical_region region;auto lock=region.Acquire();
    EM2OriginalWriteActiveTextureSRVKeys(this,keys,bindings,count);em2::RememberHdrTextureKeyBatch(this,keys,count);
}
void rex::graphics::TextureCache::DestroyAllTextures(bool from_destructor){
    if(from_destructor)em2::ForgetHdrTextureCache(this);
    EM2OriginalDestroyAllTextures(this,from_destructor);
}
