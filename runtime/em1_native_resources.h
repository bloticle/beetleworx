#pragma once
#include "em1_model_closure.h"
#include "em1_keyframe.h"
#include "em1_kfm.h"
#include <span>
#include <string_view>
struct PPCContext;
namespace em2::em1 {
// Native game-owned resource reference. Release on the same engine thread,
// before its pack manager is destroyed. The root is borrowed from the handle.
// This does not attach the root to a scene or admit collision into the world.
bool NativeModelPublicationActive();
struct NativeModelResource {
  std::shared_ptr<RvzSource> source;
  std::string origin_key, native_name;
  uint32_t manager=0, record=0, handle=0, root=0;
};
std::string ReserveNativeModelName();
struct NativeRawSceneResource {
  NativeModelResource owner;
  uint32_t data=0;
};
struct NativeHavokResource {
  NativeModelResource owner;
  uint32_t wrapper=0;
};
struct NativeTextureResource { NativeModelResource owner; };
// Array/sequence pointers are borrowed from the original KF loader cache.
// Use RetainNativeKeyframe to acquire another engine-owned reference.
struct NativeKeyframeResource { NativeModelResource owner;uint32_t array=0;std::vector<uint32_t> sequences; };
std::string ReserveNativeKeyframeName();
bool PublishNativeKeyframe(PPCContext&,uint8_t*,uint32_t,std::shared_ptr<RvzSource>,const KeyframeResource&,
    NativeKeyframeResource&,std::string&,std::string_view reserved_name={});
bool RetainNativeKeyframe(PPCContext&,uint8_t*,const NativeKeyframeResource&,NativeKeyframeResource&,std::string&);
bool ReleaseNativeKeyframe(PPCContext&,uint8_t*,NativeKeyframeResource&,std::string&);
std::string ReserveNativeAnimationManagerName();
bool PublishNativeAnimationManager(PPCContext&,uint8_t*,uint32_t,std::shared_ptr<RvzSource>,const BoundKfmResource&,
    std::string_view native_name,NativeModelResource&,std::string&);
bool SelectNativeAnimationSequence(PPCContext&,uint8_t*,const NativeModelResource&,uint32_t sequence,std::string&);
bool UpdateNativeAnimation(PPCContext&,uint8_t*,const NativeModelResource&,float absolute_time,std::string&);
// NIF texture roots live in the original NiStream object cache, not in a
// scene-graph shared handle. The typed record owns the cached texture lifetime.
bool PublishNativeTexture(PPCContext&,uint8_t*,uint32_t,const ModelClosure&,
    NativeTextureResource&,std::string&);
bool ReleaseNativeTexture(PPCContext&,uint8_t*,NativeTextureResource&,std::string&);
// Publishes an already-converted native pack through the original HKX factory.
// Root interpretation belongs to the caller; a convex root is not a full body.
bool PublishNativeHavok(PPCContext&,uint8_t*,uint32_t manager,
    std::shared_ptr<RvzSource>,std::string_view origin,std::span<const uint8_t>,
    NativeHavokResource&,std::string& error);
bool ReleaseNativeHavok(PPCContext&,uint8_t*,NativeHavokResource&,std::string& error);
bool RetainNativeRawScene(PPCContext&, uint8_t*, uint32_t manager,
    std::shared_ptr<RvzSource>, std::string_view origin, std::string_view name,
    std::span<const uint8_t> expected, NativeRawSceneResource&, std::string& error);
bool PublishNativeRawScene(PPCContext&, uint8_t*, uint32_t manager,
    std::shared_ptr<RvzSource>, std::string_view origin, std::string_view name,
    std::span<const uint8_t> bytes, NativeRawSceneResource&, std::string& error);
bool ReleaseNativeRawScene(PPCContext&, uint8_t*, NativeRawSceneResource&, std::string& error);
bool PublishNativeModel(PPCContext&, uint8_t* base, uint32_t pack_manager,
                        const ModelClosure&, NativeModelResource&, std::string& error,
                        std::string_view reserved_name = {});
bool ReleaseNativeModel(PPCContext&, uint8_t* base, NativeModelResource&, std::string& error);
}
