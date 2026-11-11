#pragma once
#include "em1_kfm_closure.h"
#include "em1_native_resources.h"
namespace em2::em1 {
struct NativeAnimationBundle {
  NativeModelResource animation;
  std::vector<NativeModelResource> models;
  std::vector<NativeKeyframeResource> keyframes;
};
// Complete original dependency ownership. Release manager before its files.
// This creates a native animation graph, but never attaches it to a world.
bool PublishNativeAnimationBundle(PPCContext&,uint8_t*,uint32_t,const KfmClosure&,NativeAnimationBundle&,std::string&);
bool ReleaseNativeAnimationBundle(PPCContext&,uint8_t*,NativeAnimationBundle&,std::string&);
}
