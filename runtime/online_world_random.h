#pragma once
#include "online.h"
#include <atomic>
#include <cstdint>
#include <optional>
#include <string_view>
namespace em2::net {
struct NativeInitializationSeed {uint32_t value=0;uint64_t sequence=0;};
// Transport-independent initialization identity. Arm before guest startup,
// bind after joining the prelaunch poller, release after guest threads stop.
class NativeInitializationContext {
public:
    bool Arm(const Identity&);
    bool BindThread();
    void Fail() noexcept {failed_.store(true,std::memory_order_release);}
    void Release();
    std::optional<NativeInitializationSeed> World(std::string_view destination);
    std::optional<NativeInitializationSeed> Player(uint32_t character,uint32_t mode,bool agreed_timeline_active);
private:
    friend bool TestNativeWorldInitializationSeed();
    Identity identity_{};
    uint64_t world_sequence_=0;
    std::array<uint64_t,2> player_sequences_{};
    uint32_t thread_=0;
    bool armed_=false;
    std::atomic<bool> failed_{false};
    bool Eligible()const;
};
// Snapshot once when Host/Join begins. The negotiated session policy, rather
// than this mutable debug preference, owns initialization after adoption.
InitializationPolicy ConfiguredNativeInitializationPolicy();
uint32_t NativeWorldInitializationSeed(const Identity&,std::string_view destination,uint64_t sequence);
uint32_t NativePlayerInitializationSeed(const Identity&,uint32_t slot,uint32_t mode,uint64_t sequence);
// Called at the audited native loader boundary, on the retained owner thread.
bool PrepareNativeOnlineWorldRandom(std::string_view destination);
bool PrepareNativeOnlinePlayerRandom(uint32_t slot,uint32_t mode);
bool TestNativeWorldInitializationSeed();
}
