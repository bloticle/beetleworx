#pragma once
#include <cstdint>
#include <memory>
#include <string>
struct PPCContext;
namespace em2::net {struct Identity;}
namespace em2 {
void BeginNativeOnlineRandom(const net::Identity&);
void EndNativeOnlineRandom();
// Loader-thread only. Seed the next real native rand once; never restore the
// previous startup stream. An explicit native srand remains authoritative.
void BeginNativeOnlineWorldRandom(uint32_t seed);
class VisualRandomScope {
public:
    VisualRandomScope();~VisualRandomScope();
    VisualRandomScope(const VisualRandomScope&)=delete;
    VisualRandomScope& operator=(const VisualRandomScope&)=delete;
};
// Trusted local owner-thread CRT/presentation streams and pending seed state.
// Does not cover independent engine generators or worker-thread randomness.
class NativeRandomCheckpoint {
public:
    NativeRandomCheckpoint();~NativeRandomCheckpoint();
    NativeRandomCheckpoint(const NativeRandomCheckpoint&)=delete;
    NativeRandomCheckpoint& operator=(const NativeRandomCheckpoint&)=delete;
    bool Capture(PPCContext&,uint8_t*,uint64_t epoch,std::string&);
    bool Restore(PPCContext&,uint8_t*,uint64_t epoch,std::string&)const;
    bool Matches(PPCContext&,uint8_t*,uint64_t epoch,std::string&)const;
    bool TestRoundTrip(PPCContext&,uint8_t*,uint64_t epoch,std::string&)const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
bool TestNativeRandom();
// Initialized native owner PTD only. Uses the original srand and verifies the
// owner and resulting state; preserves the caller's complete PPC context.
bool SeedNativeSpawnReplay(PPCContext&,uint8_t*,uint32_t seed);
}
