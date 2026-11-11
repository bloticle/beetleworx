#pragma once
#include "online_movement_frame.h"
#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace em2::net {
// Native movement values, not raw stick axes. Ground-plane basis is captured
// with the command; no receiving peer consults its local camera for this input.
using RoomCommand=RoomMovement;
enum class RoomState {Idle,Listening,Connecting,Lobby,Loading,Running,Disconnected,Error};
// Single-thread-owned direct TCP transport. Up to 16 peers is a transport
// capability only; Host capacity MUST come from the actual gameplay adapter.
// Caller supplies a verified, already-installed common checkpoint identity.
// No checkpoint transfer, relay, NAT traversal or native actor creation here.
class Room {
public:
    Room();
    ~Room();
    Room(const Room&)=delete;
    Room& operator=(const Room&)=delete;
    bool Host(const Identity&,std::string_view numeric_address,uint16_t port,
        uint8_t gameplay_capacity,std::string_view name="Player 1");
    bool Join(const Identity&,std::string_view numeric_address,uint16_t port,std::string_view name);
    void Poll(std::chrono::steady_clock::time_point now=std::chrono::steady_clock::now());
    bool Rename(std::string_view);
    bool SetAppearance(LobbyCharacter,uint8_t color);
    bool SetReady(bool);
    bool Start();
    // Start only freezes the roster and enters Loading. The caller explicitly
    // asserts native world load + actor admission with a common nonzero token.
    // No input/frame is admitted until all frozen peers assert the same token.
    // Repeating the same local token is idempotent; changing it is rejected.
    bool WorldReady(const std::array<uint8_t,32>& canonical_world_token);
    // Submit exactly next_submit_tick(), which includes negotiated input delay.
    // The first input_delay ticks contain a common neutral command. Success
    // queues intent; only a validated TakeFrame authorizes a simulation tick.
    bool SubmitLocal(uint64_t tick,const RoomCommand&);
    bool TakeFrame(RoomFrame&);
    void Disconnect();
    RoomState state()const;
    uint16_t port()const;
    uint64_t local_peer()const;
    uint64_t next_tick()const;
    uint64_t next_submit_tick()const;
    const Identity& identity()const;
    const LobbySnapshot& lobby()const;
    LobbyResult last_lobby_result()const;
    bool profile_pending()const;
    const std::string& error()const;
private:
    friend bool TestOnlineRoom();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
bool TestOnlineRoom();
}
