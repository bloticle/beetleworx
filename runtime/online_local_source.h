#pragma once
#include "online.h"
#include "online_local_movement.h"
namespace em2::net {
class Room;
class NativeRoutedPadSample {
public:
    NativeRoutedPadSample()=default;
    const Pad& pad()const{return pad_;}
    unsigned physical_user()const{return physical_user_;}
    unsigned native_user()const{return native_user_;}
    unsigned retail_slot()const{return retail_slot_;}
    uint64_t room_peer()const{return room_peer_;}
    uint64_t source_tick()const{return source_tick_;}
    bool operator==(const NativeRoutedPadSample&)const=default;
private:
    Pad pad_{};
    uint64_t route_generation_=0,controls_generation_=0,source_tick_=0,room_peer_=0;
    unsigned physical_user_=0,native_user_=0,retail_slot_=0;
    friend bool SampleNativeLocalSource(const Room&,rex::input::InputSystem&,uint64_t,PPCContext&,uint8_t*,NativeRoutedPadSample&);
};
// Explicit Room source route for peers1..16. Character selection chooses one
// of the two native interpreter profiles; it is not a controller/manager index.
// This source lease does not admit an owned actor or select its local camera.
// It may be armed at an
// owning bootstrap gate's completed boundary without sampling, or in Running.
// Sampling itself requires Running and the cutover sampler's admitted tick.
// The physical
// source remains a real bounded SDK user and is distinct from the validated
// canonical native user/device in the selected retail Movement binding.
// Existing unsigned-user producer APIs remain strict and unchanged.
bool ArmNativeLocalSource(const Room&,unsigned physical_user,const NativeLocalMovementBinding&,PPCContext&,uint8_t*);
void DisarmNativeLocalSource();
// Performs the physical poll itself through the existing once-per-tick,
// already-remapped sampler. No public API can wrap an arbitrary Pad as proof.
bool SampleNativeLocalSource(const Room&,rex::input::InputSystem&,uint64_t source_tick,PPCContext&,uint8_t*,NativeRoutedPadSample&);
// For typed producer overloads: validates the sealed sample against the current
// controls lease, complete native binding and most recent sampled source tick.
// Does not change the caller's physical-user label or consume the token, so
// Movement and Jump can use the identical physical sample for this tick.
bool ValidateNativeLocalSource(const Room&,const NativeRoutedPadSample&,const NativeLocalMovementBinding&,PPCContext&,uint8_t*);
// Called while a native producer fixture owns its image and real retail binding.
bool TestNativeLocalSource(PPCContext&,uint8_t*,const NativeLocalMovementBinding&);
}
