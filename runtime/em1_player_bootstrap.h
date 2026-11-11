#pragma once
#include <array>
#include <cstdint>
#include <string>
struct PPCContext;
namespace em2::em1 {
struct NativeCollisionAttachment;
enum class NativePlayerBootstrapStatus {Pending,Ready,Failed};
struct NativePlayerBootstrap {
  uint32_t manager=0,entity=0,handle=0,camera=0,proxy=0,input=0,motion=0;
  uint32_t attempts=0;
  bool prepared=false,placed=false,camera_requested=false,controls_requested=false;
  bool created=false,teleport_requested=false;
};
// Bounded, opt-in game-thread bootstrap after an isolated source scene loads.
// Reuses the real player or recreates it through the stock PlayerManager factory.
// Native spawn/transform/camera/input calls only; no synthesized gameplay input,
// generic placeholder actor, stale descriptor, or save/profile mutation.
// Owner resets state on every scene change and stops calling after Ready/Failed.
NativePlayerBootstrapStatus BootstrapNativeCourtyardPlayer(PPCContext&,uint8_t*,
    NativePlayerBootstrap&,std::string& diagnostic);
struct NativeStandingSample {
  std::array<float,3> position{},velocity{};
  uint32_t ground_state=0;
};
struct NativeStandingObservation {
  uint32_t samples=0,last_tick=UINT32_MAX,world=0,tail_count=0,tail_next=0;
  float first_world_time=0,world_time=0;
  std::array<NativeStandingSample,32> tail{};
  bool complete=false,standing=false;
};
// Call initially at completed_physics_ticks=0, then after native ticks through
// 120. Caller may pass observation.samples each callback: unchanged world time
// is ignored before allocation, so only distinct native step times are sampled.
// Never count high-rate render callbacks as physics ticks. Samples only:
// no position, velocity, input, animation, gravity or controller changes.
// Returns false for invalid/stale observations; a valid completed observation
// may have standing=false, with the measured reason in diagnostic. Acceptance
// requires actual world-time advancement and a settled final32-sample window.
bool ObserveNativeCourtyardStanding(PPCContext&,uint8_t*,
    const NativePlayerBootstrap&,uint32_t completed_physics_ticks,
    NativeStandingObservation&,std::string& diagnostic);
// Optional bounded read-only body/filter linkage snapshot; no policy changes.
bool TraceNativeCourtyardCollisionState(PPCContext&,uint8_t*,
    const NativePlayerBootstrap&,const NativeCollisionAttachment&,std::string& error);
bool SampleNativeCourtyardLightPosition(PPCContext&,uint8_t*,
    uint32_t& entity,std::array<float,3>& position,uint32_t* script_identity=nullptr);
}
