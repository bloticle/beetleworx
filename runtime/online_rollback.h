#pragma once
#include "online.h"
#include <span>

namespace em2::net {
using RollbackBytes=std::vector<uint8_t>;
struct RollbackScope {
    std::array<uint8_t,16> session{};
    std::array<uint8_t,32> schema{};
    uint64_t world_epoch=0,roster_revision=0;
    uint16_t players_mask=0;
    bool operator==(const RollbackScope&)const=default;
};
struct RollbackInput {
    uint64_t tick=0;
    uint16_t players_mask=0,predicted_mask=0;
    std::array<Pad,16> players{};
};
struct RollbackCommand {unsigned peer=0;uint64_t tick=0;Pad pad;};
enum class RollbackStep {Live,Replay};
enum class RollbackRole {Host,Client};
enum class RollbackResult {Ready,Advanced,Accepted,Corrected,Duplicate,Waiting,TooOld,NeedFullState,Rejected,WrongThread,Faulted};

// Do not claim these capabilities for a checkpoint, transform-only snapshot,
// guest-memory dump, or input history. All are required before activation.
enum RollbackCapability:uint32_t {
    FullSimulationState=1,StableEntityIdentity=2,FixedStepReplay=4,
    SuppressedReplaySideEffects=8,WorldEpochFence=16,
    RequiredRollbackCapabilities=31
};
class RollbackWorld {
public:
    virtual ~RollbackWorld()=default;
    virtual uint32_t Capabilities()const noexcept=0;
    // Lossless simulation state, including physics/contact state, Lua/BSQ,
    // RNG, native input interpreters, entity lifetimes and deferred gameplay.
    virtual bool Capture(RollbackBytes&)=0;
    virtual bool Restore(std::span<const uint8_t>)=0;
    // Re-interpret held pads in the restored world; do not replay precomputed
    // Jump/action edges. Replay must suppress audio, file writes and packets.
    virtual bool Step(const RollbackInput&,RollbackStep)=0;
};
struct RollbackLimits {
    uint16_t history_frames=120,max_prediction=8,future_inputs=32;
    uint32_t snapshot_bytes=8*1024*1024,history_bytes=64*1024*1024;
};
struct RollbackSnapshot {
    RollbackScope scope;
    // State BEFORE this tick. No skipped frame is marked as simulated.
    uint64_t tick=0,revision=0;
    std::array<Pad,16> previous_inputs{};
    RollbackBytes state;
};
struct RollbackStats {uint64_t restores=0,replayed_frames=0,corrections=0,rejected=0;};
// Versioned, bounded, SHA-256 checked envelope. Integrity is not authentication:
// the transport must admit snapshots only from the authenticated room host.
std::optional<RollbackBytes> EncodeRollbackSnapshot(const RollbackSnapshot&,uint32_t maximum_bytes);
std::optional<RollbackSnapshot> DecodeRollbackSnapshot(std::span<const uint8_t>,uint32_t maximum_bytes);

// Owner-thread host authority/client prediction. The native adapter is an
// explicit dependency: existing lockstep gameplay does not enable this class.
class RollbackSession {
public:
    RollbackSession(RollbackWorld&,RollbackRole,RollbackLimits={});
    ~RollbackSession();
    RollbackSession(const RollbackSession&)=delete;
    RollbackSession& operator=(const RollbackSession&)=delete;
    RollbackResult Begin(const RollbackScope&,uint64_t first_tick=0);
    // Late join/rejoin installs the authenticated host's CURRENT world directly.
    // The room handshake supplies the exact expected world/roster scope.
    RollbackResult BeginFromAuthoritative(const RollbackScope&,std::span<const uint8_t>);
    RollbackResult SubmitInput(unsigned peer,uint64_t tick,const Pad&);
    RollbackResult SubmitInputs(std::span<const RollbackCommand>);
    // Host lifecycle fence at the next unsimulated tick. Suspended peers have
    // neutral held controls; queued future commands are discarded, not replayed
    // when the connection returns. Completed history remains immutable.
    RollbackResult SetPeerConnected(unsigned peer,bool connected);
    RollbackResult Advance();
    // Corrections restore the host state and replay all retained newer input.
    // A failed transaction restores the pre-correction world or faults closed.
    RollbackResult AcceptAuthoritative(std::span<const uint8_t>);
    std::optional<RollbackBytes> ExportAuthoritative()const;
    uint64_t next_tick()const;
    uint64_t authority_revision()const;
    size_t retained_bytes()const;
    RollbackStats stats()const;
    RollbackScope scope()const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
bool TestOnlineRollback();
}
