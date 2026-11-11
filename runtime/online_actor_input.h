#pragma once
#include <array>
#include <cstdint>
struct PPCContext;
namespace em2::net {
// Owned EXTRA actors only; the two retail PlayerManager records stay native.
struct OwnedActorInput {
    uint64_t peer=0,generation=0;
    uint32_t registry=0,handle=0,entity=0,controller=0,listener=0;
    bool enabled=false;
};
struct ActorLookBasis {
    uint64_t peer=0,generation=0,tick=0;
    std::array<float,3> right{1,0,0},forward{0,0,1};
};
// Values must be produced by the native action interpreter, not raw stick axes.
struct NativeActorAction {uint32_t event=0,action=0;float x=0,y=0;};
enum class ActorInputResult { Applied, Unregistered, Stale, Disabled, WrongThread,
    InvalidInput, InvalidBasis, BasisRouteUnverified, Nested };
// All mutations/replay belong to the initialized game thread. Resolve the
// handle through the native registry and validate the actual typed controller.
bool RegisterOwnedActorInput(uint64_t peer,uint32_t handle,PPCContext&,uint8_t* base,OwnedActorInput&);
bool SetOwnedActorInputEnabled(uint64_t peer,bool enabled);
// Keep disabled/quarantined through deferred native destruction; unregister
// only after native input listeners are removed. This does not destroy actors.
void UnregisterOwnedActorInput(uint64_t peer);
// Native world teardown only; not a substitute for deactivating live extras.
void ResetOwnedActorInputs();
bool OwnedActorInputFor(uint64_t peer,PPCContext&,uint8_t* base,OwnedActorInput&);
// Deliberately rejects until the future per-peer NativeMotion/look-basis route
// is audited. A valid vector pair alone cannot authorize use of a retail camera.
ActorInputResult ReplayOwnedActorAction(uint64_t peer,const NativeActorAction&,const ActorLookBasis&,PPCContext&,uint8_t* base);
// Call first in the existing native Mickey/base input wrappers. True means
// the registered extra's ordinary broadcast was consumed without delivery.
bool SuppressOwnedActorInput(PPCContext&,uint8_t* base);
bool TestOwnedActorInput();
}
