#pragma once
#include "online_room.h"
#include "ppc_context.h"
#include <atomic>
#include <optional>

namespace em2::net {
enum class BootstrapAdmission {Inactive,Waiting,Ready,Gameplay,Stopped,WrongThread};
// Simulation-thread bridge for the negotiated native initialization phase.
// Room outlives this gate. Detach/destruction runs on that thread, or during
// shutdown after guest threads stop. The controls/RNG lease survives cutover;
// Gameplay means a gameplay adapter must take over before another native tick.
class RoomBootstrapGate {
public:
    ~RoomBootstrapGate(){Detach();}
    RoomBootstrapGate()=default;
    RoomBootstrapGate(const RoomBootstrapGate&)=delete;
    RoomBootstrapGate& operator=(const RoomBootstrapGate&)=delete;
    bool Attach(Room&);
    void Detach();
    bool SetJoining(bool);
    using ReadinessProbe=bool(*)(PPCContext&,uint8_t*);
    // Owning gameplay adapter checks current actor/binding admission here,
    // after native post-update. It must not dispatch controls or run a world.
    bool SetReadinessProbe(ReadinessProbe);
    bool CheckGameplayReadiness(PPCContext&,uint8_t*);
    void ObservationFailed();
    BootstrapAdmission Prepare(RoomBootstrapFrame&);
    bool Commit(const RoomBootstrapFrame&);
    bool Complete(bool ready_for_gameplay,const std::array<uint8_t,32>& observation_digest);
    bool PublishedRetailFrame(uint64_t& generation,Frame&)const;
    bool active()const{return active_.load(std::memory_order_acquire);}
    // Owner-thread identity check for completed-boundary adapters; no transfer
    // of transport or controller ownership is implied.
    bool Owns(const Room& room)const{return OnOwnerThread() && active() && room_==&room;}
    bool completion_pending()const{return completion_pending_;}
    uint64_t committed_tick()const{return committed_tick_;}
    bool joining()const{return joining_;}
    const std::string& error()const{return error_;}
private:
    bool Stop(std::string);
    bool OnOwnerThread()const;
    Room* room_=nullptr;
    Identity identity_{};
    uint32_t thread_=0;
    uint16_t roster_=0;
    uint64_t generation_=0,committed_tick_=0;
    std::optional<uint64_t> completed_;
    std::optional<RoomBootstrapFrame> prepared_;
    std::optional<uint64_t> submitted_;
    bool joining_=true,completion_pending_=false,stopped_=false,sampling_rebased_=false;
    ReadinessProbe readiness_=nullptr;
    std::string error_;
    std::atomic<bool> active_{false};
};
bool RoomBootstrapActive()noexcept;
bool RoomBootstrapHalted()noexcept;
RoomBootstrapGate* CurrentRoomBootstrapGate()noexcept;
bool TestRoomBootstrapGate();
}
