#pragma once
#include "online_lobby.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace rex::input { class InputSystem; }

namespace em2::net {
struct CheckpointBundle;
struct Pad {
    uint16_t buttons=0;
    uint8_t left_trigger=0,right_trigger=0;
    int16_t left_x=0,left_y=0,right_x=0,right_y=0;
    // Session v9 metadata, never an emulated controller button. A native menu
    // requests a common paused simulation until both peers have resumed.
    bool menu_pause=false;
    bool operator==(const Pad&) const = default;
};
struct Frame {
    uint64_t tick=0;
    std::array<Pad,2> players{};
    bool operator==(const Frame&) const = default;
};
enum class StartupPolicy:uint8_t {Prelaunch=0,NativeMenu=1};
enum class InitializationPolicy:uint8_t {Original=0,SharedNativeV1=1};
struct Identity {
    std::array<uint8_t,32> build{},content{},checkpoint{};
    std::array<uint8_t,16> session{};
    uint64_t first_tick=0;
    uint8_t input_delay=2;
    StartupPolicy startup_policy=StartupPolicy::Prelaunch;
    InitializationPolicy initialization_policy=InitializationPolicy::Original;
    bool operator==(const Identity&) const = default;
};
enum class State {Idle,Listening,Connecting,Handshaking,Transferring,Ready,Disconnected,Error};
enum class LoadPhase:uint8_t {Begin,End};
struct LoadPoint {
    uint64_t sequence=0,tick=0;
    LoadPhase phase=LoadPhase::Begin;bool success=false;
    std::array<char,96> destination{};
    bool operator==(const LoadPoint&) const = default;
};

// Single-owner, nonblocking two-player input session. Ready means a compatible
// input channel, not that an EM2 world has been loaded or synchronized. The
// game must consume one agreed Frame per simulation tick and validate world
// determinism before this can be exposed as online gameplay.
class RouterMapping;
class Session {
public:
    Session();
    ~Session();
    Session(const Session&)=delete;
    Session& operator=(const Session&)=delete;
    bool Host(const Identity&,std::string_view numeric_address,uint16_t port=0);
    bool Join(const Identity&,std::string_view numeric_address,uint16_t port);
    // Optional pre-game checkpoint phase. Join may report a different or missing
    // local save; only this explicit mode negotiates the host's checkpoint.
    // Build/content/session/timing still match exactly. Pack on a worker first.
    bool HostWithCheckpoint(const Identity&,std::string_view numeric_address,uint16_t port,std::shared_ptr<const CheckpointBundle>);
    // Reserve the native lobby's port before file verification. The socket
    // accepts application traffic only after the verified identity is supplied.
    bool ReserveHostListener(std::string_view numeric_address,uint16_t port);
    bool StartReservedHost(const Identity&,std::shared_ptr<const CheckpointBundle>);
    bool JoinWithCheckpoint(const Identity&,std::string_view numeric_address,uint16_t port);
    // Immutable complete download for a worker to validate/install. Gameplay
    // remains blocked until installation is confirmed and acknowledged.
    std::shared_ptr<const std::vector<uint8_t>> DownloadedCheckpoint() const;
    bool ConfirmCheckpointInstalled(bool success);
    bool ArmLobbyStart();
    bool CanRequestLobbyStart() const;
    bool RequestLobbyStart();
    bool LobbyStartConfirmed() const;
    // Host validates each request against its owned roster; clients cannot select
    // another peer's identity. Queue success is distinct from host acceptance.
    bool RenameLobbyPlayer(std::string_view name);
    bool SetLobbyAppearance(LobbyCharacter,uint8_t color);
    bool SetLobbyReady(bool ready);
    const LobbySnapshot& lobby() const;
    LobbyResult last_lobby_result() const;
    bool lobby_request_pending() const;
    void Poll(std::chrono::steady_clock::time_point now=std::chrono::steady_clock::now());
    bool SubmitLocal(uint64_t tick,const Pad&);
    bool TakeFrame(Frame&);
    bool SubmitStateDigest(uint64_t completed_tick,const std::array<uint8_t,32>&);
    bool SubmitLoadPoint(const LoadPoint&);
    bool LoadPointAgreed(const LoadPoint&) const;
    std::optional<uint64_t> verified_tick() const;
    void Disconnect();
    bool KeepRouterMapping(std::unique_ptr<RouterMapping>&);
    State state() const;
    uint16_t port() const;
    unsigned local_player() const;
    uint64_t next_tick() const;
    const Identity& identity() const;
    const std::string& error() const;
private:
    friend bool TestOnlineSession();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
bool TestOnlineSession();
enum class Admission {Inactive,Waiting,Ready,Stopped};
enum class InputWait {None,Peer,LocalController,Verification};
struct PlayerObservation {uint32_t mode=0;std::array<float,3> position{};};
// Diagnostic fingerprint of player modes/positions only, not a full world hash.
std::optional<std::array<uint8_t,32>> HashPlayerObservations(uint64_t,const std::array<PlayerObservation,2>&);
struct RandomObservation {
    uint64_t calls=0,seeds=0,sequence=0xcbf29ce484222325ull;
    uint32_t last_state=0;
    void Record(bool reseed,uint32_t caller,uint32_t state,uint32_t value) {
        if(reseed)++seeds;else ++calls;
        last_state=state;
        for(uint32_t word:{uint32_t(reseed),caller,state,value})
            for(unsigned shift=32;shift;shift-=8) {sequence^=uint8_t(word>>(shift-8));sequence*=0x100000001b3ull;}
    }
};
struct GameplayObservation {
    struct CharacterPhysics {
        std::array<uint32_t,4> id{};
        bool active=false;
        uint32_t state=0;
        std::array<uint32_t,3> position{},velocity{};
        bool operator==(const CharacterPhysics&)const=default;
    };
    struct World {
        std::array<char,128> level{},state{},interior{};
        bool operator==(const World&) const = default;
    } world;
    std::array<PlayerObservation,2> players{};
    // P0 current/max life, P1 current/max life; paint/thinner/electric current/max.
    std::array<uint32_t,4> life{};
    std::array<float,6> ammo{};
    // Only CRT calls on the simulation thread during this tick. This is a
    // diagnostic stream checksum, not coverage of all engine/worker RNGs.
    RandomObservation random;
    // Native Room gameplay additionally proves all recognized character proxies,
    // including NPCs. GUID order is canonical; guest pointers never participate.
    bool includes_character_physics=false;
    std::vector<CharacterPhysics> characters;
    std::array<std::array<uint32_t,4>,2> player_ids{};
};
std::optional<std::array<uint8_t,32>> HashGameplayObservation(uint64_t,const GameplayObservation&);
// Simulation-thread owner. Session and InputSystem must outlive attachment.
// Attach only after compatible worlds/native co-op have been established.
class InputGate {
public:
    InputGate()=default;
    ~InputGate();
    InputGate(const InputGate&)=delete;
    InputGate& operator=(const InputGate&)=delete;
    bool Attach(Session&,rex::input::InputSystem&,unsigned local_device);
    void Detach();
    void SetNativeJoinPending(bool pending){native_join_pending_=pending;}
    void Poll(std::chrono::steady_clock::time_point now=std::chrono::steady_clock::now());
    Admission Prepare(Frame&);
    bool Commit(const Frame&);
    bool Complete(const std::array<uint8_t,32>& observation_digest);
    bool completion_pending() const {return completion_pending_;}
    uint64_t committed_tick() const {return committed_tick_;}
    void ObservationFailed();
    bool NativeLoadPoint(std::string_view destination,LoadPhase,bool success=false);
    void CancelNativeLoads() noexcept {cancel_loads_.store(true,std::memory_order_release);}
    // UI/worker policy queries never inspect the borrowed Session pointer.
    bool active() const {return active_snapshot_.load(std::memory_order_acquire);}
    bool waiting() const {return waiting_;}
    InputWait wait_reason() const {return wait_reason_;}
    const std::string& error() const {return error_;}
private:
    void Stop(const char*);
    Session* session_=nullptr;rex::input::InputSystem* input_=nullptr;
    std::atomic<bool> active_snapshot_{false};
    Identity identity_{};unsigned device_=0;uint64_t tick_=0;
    bool stopped_=false,prepared_=false,exhausted_=false,waiting_=false;
    bool completion_pending_=false,native_join_pending_=false;
    std::optional<uint64_t> local_pause_tick_;
    bool local_pause_=false;
    InputWait wait_reason_=InputWait::None;
    uint64_t committed_tick_=0;
    uint64_t load_sequence_=0;
    std::atomic<uint32_t> owner_thread_{0};
    std::atomic<bool> cancel_loads_{false};
    Frame pending_{};std::string error_;
};
InputGate& GameplayInputGate();
bool TestOnlineGameplay();
void UpdateOnlineQASession(uint8_t* base);
bool RunCheckpointTransferDiagnostic();
bool PrepareNativeOnlineQASession();
}
