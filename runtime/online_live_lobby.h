#pragma once
#include "online_rollback.h"
#include <chrono>

namespace em2::net {
using RejoinTicket=std::array<uint8_t,32>;
enum class LivePresence {PendingSnapshot,Active,Reconnecting,Leaving};
enum class LiveLobbyResult {Accepted,Closed,Full,Invalid,UnknownPlayer,WrongState,Expired,StaleSnapshot,ColorInUse};
struct LiveAdmission {
    uint8_t slot=0;uint64_t player_id=0;
    RejoinTicket ticket{}; // Private reconnect credential; never a displayed room code.
};
struct LiveParticipant {
    LobbyPlayer profile;
    uint64_t player_id=0;
    LivePresence presence=LivePresence::PendingSnapshot;
};
struct LiveJoinSnapshot {LiveAdmission admission;std::shared_ptr<const RollbackBytes> snapshot;};

// Host-owned admission for a running world. No title-menu or Continue operation.
// Native avatar creation/destruction and room transport consume these explicit
// transitions; a roster row alone never asserts that an avatar exists.
class LiveLobby {
public:
    using Clock=std::chrono::steady_clock;
    explicit LiveLobby(uint8_t capacity=16,std::chrono::seconds reconnect_grace=std::chrono::seconds(120));
    ~LiveLobby();
    LiveLobby(const LiveLobby&)=delete;
    LiveLobby& operator=(const LiveLobby&)=delete;
    LiveLobbyResult Open(RollbackSession& running_host,std::string_view host_name);
    LiveLobbyResult Publish(std::span<const uint8_t> host_snapshot);
    LiveLobbyResult Join(std::string_view name,LiveAdmission&);
    LiveLobbyResult Drop(unsigned slot,Clock::time_point);
    LiveLobbyResult Resume(unsigned slot,const RejoinTicket&,Clock::time_point,LiveAdmission&);
    std::optional<LiveJoinSnapshot> SnapshotFor(unsigned slot);
    LiveLobbyResult Acknowledge(unsigned slot,const RejoinTicket&,const RollbackScope&,uint64_t tick,uint64_t revision);
    LiveLobbyResult Leave(unsigned slot);
    LiveLobbyResult Kick(unsigned slot);
    // Release the slot/color only after a host snapshot proves avatar removal.
    LiveLobbyResult CommitDeparture(unsigned slot);
    void Expire(Clock::time_point);
    LiveLobbyResult SetAppearance(unsigned slot,LobbyCharacter,uint8_t color);
    // Call at every owner-thread simulation boundary, before accepting/stepping
    // commands. Pending, dropped and departing avatars receive neutral controls.
    LiveLobbyResult PrepareHostFrame(RollbackSession&);
    std::vector<LiveParticipant> players()const;
    bool open()const;
    void Close();
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
bool TestLiveOnlineLobby();
}
