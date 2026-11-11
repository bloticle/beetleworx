#pragma once
#include <cstdint>
struct PPCContext;
namespace em2::net {
// The native controller owns the NiPointer, not this bookkeeping record.
// Release before removing the input registration/native actor. World teardown
// releases native components; never dereference an old-world lease afterward.
struct ActorMovementReference {
    uint64_t peer=0,generation=0;
    uint32_t registry=0,controller=0,reference=0;
    bool active=false;
};
enum class ActorReferenceResult { Attached, Released, AlreadyReplaced, Invalid,
    Stale, ExistingReference, NativeFailure };
ActorReferenceResult AttachOwnedActorMovementReference(uint64_t peer,uint32_t camera_handle,
    PPCContext&,uint8_t* base,ActorMovementReference&);
ActorReferenceResult ReleaseOwnedActorMovementReference(ActorMovementReference&,PPCContext&,uint8_t* base);
// Called from the owned-input fixture after its two actors have been registered.
bool TestOwnedActorMovementReferences(PPCContext&,uint8_t* base,uint64_t first_peer,uint64_t second_peer);
}
