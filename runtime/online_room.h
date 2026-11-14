#pragma once
#include "online_movement_frame.h"
#include "online_voice_transport.h"
#include "online_room_history.h"
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace em2::net {
struct CheckpointBundle;
struct RoomCheckpointPeer {uint64_t peer=0;std::size_t total=0,received=0;bool installed=false;};
struct RoomChatMessage {uint64_t sequence=0,peer=0;std::string name,text;uint8_t color=0;bool operator==(const RoomChatMessage&)const=default;};
bool ValidRoomChatText(std::string_view);
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
// Owner-thread snapshot. Contains no invitation/session nonce, address, player
// name, chat, media key, save bytes or guest pointers. Not a restorable world.
struct RoomProofMismatch {
    bool bootstrap=false;uint64_t tick=0,peer=0;
    std::array<uint8_t,32> expected{},received{};
    bool operator==(const RoomProofMismatch&)const=default;
};
struct RoomDiagnosticSnapshot {
    RoomState phase=RoomState::Idle;bool host=false,native_bootstrap=false;
    uint64_t local_peer=0,next_consume=0,next_submit=0,raw_tick=0;
    uint16_t players_mask=0;uint8_t verification_window=1;
    std::optional<uint64_t> verified,pending;
    std::optional<RoomProofMismatch> mismatch;
    std::array<uint8_t,32> build{},content{},checkpoint{};
    bool operator==(const RoomDiagnosticSnapshot&)const=default;
};
std::string FormatRoomDiagnosticSnapshot(const RoomDiagnosticSnapshot&);
// Single-thread-owned direct TCP transport. Up to 16 peers is a transport
// capability only; Host capacity MUST come from the actual gameplay adapter.
// Host/Join require a common installed checkpoint. Explicit WithCheckpoint
// bootstrap transports a shared bundle; only caller workers install it.
// No relay, NAT traversal or native actor creation here.
class Room {
public:
    Room();
    // Retain the in-process outbound internet bridge across native menu handoff.
    void KeepInternetRelay(std::shared_ptr<class InternetRelay>);
    ~Room();
    Room(const Room&)=delete;
    Room& operator=(const Room&)=delete;
    // Exact Hello/Welcome agreement, immutable after Host/Join. One preserves
    // synchronous verification; at most eight completed worlds may await proof.
    bool SetGameplayVerificationWindow(uint8_t completed_worlds);
    uint8_t gameplay_verification_window()const;
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
    // Owner thread only. Host supplies sender/name/color; never simulation data.
    // Four-message burst, one token/second; bounded 64-message receive history.
    bool SendChat(std::string_view);
    std::vector<RoomChatMessage> TakeChatMessages();
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
    // A taken tick awaiting local Complete, or a full verification-credit
    // window, blocks both calls. No frame skips the ordered native timeline.
    bool PeekFrame(RoomFrame&)const;
    bool TakeFrame(RoomFrame&);
    // Exactly one taken frame awaits local Complete. Completed worlds retain
    // their exact nonzero digests until ordered all-peer consensus releases
    // bounded credit; this does not change native tick duration or digest.
    bool CompleteGameplay(uint64_t consumed_tick,const std::array<uint8_t,32>& observation_digest);
    bool gameplay_completion_pending()const;
    // Ordered native loader rendezvous for the consumed gameplay tick. End
    // must match its Begin, including nested destinations. No world restore
    // or new-world admission is implied by agreement. Submit each point once,
    // then Poll until LoadPointAgreed; repeating an agreed local point is safe.
    bool SubmitLoadPoint(const LoadPoint&);
    bool LoadPointAgreed(const LoadPoint&)const;
    // After the agreed successful outer End and the original native finish.
    // Discards the unverified transition tick and future input only after all
    // frozen peers assert the same loaded world; resumes raw native admission.
    bool RebootstrapLoadedWorld(const LoadPoint&,const std::array<uint8_t,32>& loaded_world);
    // Admission credit, distinct from the broad outstanding-proof query above.
    bool gameplay_frame_blocked()const;
    std::size_t pending_gameplay_verifications()const;
    std::optional<uint64_t> verified_gameplay_tick()const;
    // Read-only replay preparation. Contains consumed, locally completed AND
    // host-verified input only. It neither restores a native world nor replays
    // callbacks. Exact scope and complete contiguous range are mandatory.
    std::optional<RoomHistoryScope> gameplay_history_scope()const;
    RoomHistoryResult ReadGameplayHistory(const RoomHistoryScope&,uint64_t first,uint64_t last,std::vector<RoomHistoryEntry>&)const;
    RoomHistoryResult FingerprintGameplayHistory(const RoomHistoryScope&,uint64_t first,uint64_t last,std::string&)const;
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
    // Optional media channel, negotiated only after an admitted request. UDP
    // failure never changes Room gameplay state. No microphone/device access.
    bool RequestVoice();
    bool voice_ready()const;
    bool SendVoice(const VoicePacket&);
    std::vector<ReceivedVoice> TakeVoice();
    const std::string& error()const;
    // The first failure or disconnect freezes this before teardown; later
    // Disconnect calls cannot erase or replace its original proof context.
    RoomDiagnosticSnapshot diagnostic_snapshot()const;
private:
    friend bool TestOnlineRoom();
    friend bool TestOnlineRoomChat();
    friend bool TestOnlineRoomVoice();
    friend bool TestOnlineRoomLoads();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool JoinInternal(const Identity&,std::string_view,uint16_t,std::string_view,bool checkpoint);
    bool StartInternal(bool native_bootstrap);
    bool WorldReadyInternal(const std::array<uint8_t,32>&,bool native_bootstrap);
};
bool TestOnlineRoom();
bool TestOnlineRoomChat();
bool TestOnlineRoomVoice();
bool TestOnlineRoomLoads();
}
