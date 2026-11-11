#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
struct PPCContext;
namespace rex::memory {class Memory;}
namespace em2::em1 {
struct AuthoredStaticCollision;
struct NativeCollisionBodyOwner {uint32_t body=0,pack=0,pack_bytes=0;std::string name;};
struct NativeCollisionAttachment {
  NativeCollisionAttachment()=default;
  NativeCollisionAttachment(const NativeCollisionAttachment&)=delete;
  NativeCollisionAttachment& operator=(const NativeCollisionAttachment&)=delete;
  ~NativeCollisionAttachment();
  std::vector<NativeCollisionBodyOwner> objects;
  rex::memory::Memory* memory=nullptr;
  uint32_t world=0;
  bool world_reference=false;
};
// hkTransform column vectors followed by translation; no scale/shear. Main
// courtyard scene placement is identity. Original per-body poses are composed
// with this placement exactly once. Names/properties remain pack-backed.
inline constexpr std::array<float,16> kIdentityCollisionPlacement={
  1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
// Call only on an initialized game/loading thread with a valid PPC context.
// All functions are explicit: destructing this host ledger cannot call guest
// destructors. ReleaseNativeAuthoredCollision MUST run before its owner dies,
// before native runtime teardown, and before releasing scene-owned resources.
bool ConstructNativeAuthoredCollision(PPCContext&,uint8_t*,
    const AuthoredStaticCollision&,const std::array<float,16>& placement,
    NativeCollisionAttachment&,std::string& error);
uint32_t CurrentNativeCollisionWorld(uint8_t* base);
// Toggle world membership while retaining bodies, packs and the world owner.
bool SetNativeAuthoredCollisionActive(PPCContext&,uint8_t*,NativeCollisionAttachment&,bool active,std::string& error);
bool AdmitNativeAuthoredCollision(PPCContext&,uint8_t*,uint32_t world,
    NativeCollisionAttachment&,std::string& error);
// Bounded courtyard fixture: real world broadphase/query from the original
// player spawn, without moving the player or camera. Foreign hits are reported
// as existing-world occlusion, not misdiagnosed as authored-geometry mismatch.
bool ProbeNativeCollisionFloorRay(PPCContext&,uint8_t*,
    const NativeCollisionAttachment&,std::string& error);
// Removes bodies from their retained world, releases native body references,
// THEN frees borrowed shape/name/property pack buffers and releases world ref.
// A failed removal retains the ledger and backing storage for a later retry.
bool ReleaseNativeAuthoredCollision(PPCContext&,uint8_t*,
    NativeCollisionAttachment&,std::string& error);
}
