#pragma once
#include "online_movement_frame.h"
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace em2::net {
struct CheckpointBundle;
struct RoomCheckpointPeer {uint64_t peer=0;std::size_t total=0,received=0;bool installed=false;};
struct RoomBootstrapFrame {
    uint64_t tick=0;
    uint16_t players_mask=0;
    std::array<Pad,kOnlineRoomCapacity> players{};
    bool operator==(const RoomBootstrapFrame&)const=default;
};
// Native movement values, not raw stick axes. Ground-plane basis is captured
// with the command; no receiving peer consults its local camera for this input.
using RoomCommand=RoomMovement;
enum class RoomState {Idle,Listening,Connecting,Transferring,Installing,Lobby,Loading,Bootstrapping,Running,Disconnected,Error};
// Single-thread-owned direct TCP transport. Up to 16 peers is a transport
// capability only; Host capacity MUST come from the actual gameplay adapter.
// Host/Join require a common installed checkpoint. Explicit WithCheckpoint
// bootstrap transports a shared bundle; only caller workers install it.
// No relay, NAT traversal or native actor creation here.
class Room {
public:
    Room();
    ~Room();
    Room(const Room&)=delete;
    Room& operator=(const Room&)=delete;
    bool Host(const Identity&,std::string_view numeric_address,uint16_t port,
        uint8_t gameplay_capacity,std::string_view name="Player 1");
    bool Join(const Identity&,std::string_view numeric_address,uint16_t port,std::string_view name);
    // Explicit bootstrap mode. Caller prepares one immutable verified bundle
    // on a worker. Only this Join permits missing/different local checkpoint;
    // all other identity fields still match exactly. No filesystem I/O here.
    bool HostWithCheckpoint(const Identity&,std::string_view numeric_address,uint16_t port,
        uint8_t gameplay_capacity,std::string_view name,std::shared_ptr<const CheckpointBundle>);
    bool JoinWithCheckpoint(const Identity&,std::string_view numeric_address,uint16_t port,std::string_view name);
    // Worker must run InstallCheckpoint(bytes, identity().checkpoint, NEW path)
    // and then report the result on this room's owning thread. A successful
    // installation is mutually acknowledged before Ready/Start is permitted.
    std::shared_ptr<const std::vector<uint8_t>> DownloadedCheckpoint()const;
    bool ConfirmCheckpointInstalled(bool success);
    bool checkpoint_ready()const;
    std::vector<RoomCheckpointPeer> checkpoint_peers()const;
    void Poll(std::chrono::steady_clock::time_point now=std::chrono::steady_clock::now());
    bool Rename(std::string_view);
    bool SetAppearance(LobbyCharacter,uint8_t color);
    bool SetReady(bool);
    bool Start();
    // Explicit alternate start: synchronized native raw-Pad bootstrap, never
    // interpreted Movement. Keeps the original negotiated gameplay delay.
    bool StartNativeBootstrap();
    // Negotiated mode remains true after gameplay activation. bootstrap_tick
    // names the current raw round; normal next_tick is the gameplay timeline.
    bool native_bootstrap()const;
    uint64_t bootstrap_tick()const;
    bool BootstrapWorldReady(const std::array<uint8_t,32>& loaded_world_token);
    bool SubmitBootstrapPad(uint64_t tick,const Pad&);
    bool PeekBootstrapFrame(RoomBootstrapFrame&)const;
    bool TakeBootstrapFrame(RoomBootstrapFrame&);
    // After consuming AND completing this native tick. All peers must report
    // equal nonzero observations; gameplay starts at tick+1 only when every
    // ready vote is true. Otherwise one ordered raw round is authorized.
    bool CompleteBootstrap(uint64_t tick,bool ready_for_gameplay,const std::array<uint8_t,32>& observation_digest);
    // Start only freezes the roster and enters Loading. The caller explicitly
    // asserts native world load + actor admission with a common nonzero token.
    // No input/frame is admitted until all frozen peers assert the same token.
    // Repeating the same local token is idempotent; changing it is rejected.
    bool WorldReady(const std::array<uint8_t,32>& canonical_world_token);
    // Submit exactly next_submit_tick(), which includes negotiated input delay.
    // The first gameplay input_delay ticks contain a common neutral command
    // (after the final raw round when native bootstrap was selected). Success
    // queues intent; only a validated TakeFrame authorizes a simulation tick.
    bool SubmitLocal(uint64_t tick,const RoomCommand&);
    // Inspect without advancing the transport timeline. Native admission may
    // need to wait; only TakeFrame commits consumption of this same tick.
    bool PeekFrame(RoomFrame&)const;
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
    bool JoinInternal(const Identity&,std::string_view,uint16_t,std::string_view,bool checkpoint);
    bool StartInternal(bool native_bootstrap);
    bool WorldReadyInternal(const std::array<uint8_t,32>&,bool native_bootstrap);
};
bool TestOnlineRoom();
}
