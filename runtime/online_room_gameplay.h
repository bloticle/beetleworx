#pragma once
#include "online_room.h"
#include <atomic>
#include <memory>
struct PPCContext;
namespace rex::input {class InputSystem;}
namespace em2::net {
class RoomBootstrapGate;
enum class RoomGameplayAdmission {Inactive,Waiting,Ready,Stopped,WrongThread};
// First two RETAIL humans. Owns no transport/controls/RNG lease: the retained
// native Room and its raw gate outlive this adapter through gameplay cutover.
// Configure is explicit; ordinary/demo launches never opt in implicitly.
class NativeRoomGameplay {
public:
    NativeRoomGameplay();
    ~NativeRoomGameplay();
    NativeRoomGameplay(const NativeRoomGameplay&)=delete;
    NativeRoomGameplay& operator=(const NativeRoomGameplay&)=delete;
    bool Configure(Room&,RoomBootstrapGate&,rex::input::InputSystem&,unsigned physical_user);
    void Detach();
    bool Readiness(PPCContext&,uint8_t*);
    RoomGameplayAdmission Prepare(PPCContext&,uint8_t*,RoomFrame&);
    bool Commit(const RoomFrame&,PPCContext&,uint8_t*);
    // Called only AFTER StartTick has established the agreed native tick.
    bool Deliver(PPCContext&,uint8_t*);
    // Called only AFTER original native post-update and a real observation.
    bool Complete(const std::array<uint8_t,32>& digest,PPCContext&,uint8_t*);
    void ObservationFailed();
    bool active()const;
    bool halted()const;
    bool waiting()const;
    bool completion_pending()const;
    uint64_t committed_tick()const;
    const std::string& error()const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
NativeRoomGameplay* CurrentNativeRoomGameplay()noexcept;
bool NativeRoomGameplayReadiness(PPCContext&,uint8_t*);
bool NativeRoomGameplayHalted()noexcept;
bool NativeRoomGameplayWaiting()noexcept;
}
