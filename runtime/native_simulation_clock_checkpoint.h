#pragma once
#include <cstdint>
#include <memory>
#include <string>
struct PPCContext;
namespace em2 {
// Local scalar game clock and its fixed-step owner state; not OS/media time.
class NativeSimulationClockCheckpoint {
public:
    NativeSimulationClockCheckpoint();~NativeSimulationClockCheckpoint();
    NativeSimulationClockCheckpoint(const NativeSimulationClockCheckpoint&)=delete;
    NativeSimulationClockCheckpoint& operator=(const NativeSimulationClockCheckpoint&)=delete;
    bool Capture(uint8_t*,uint64_t epoch,std::string&);
    bool Restore(uint8_t*,uint64_t epoch,std::string&)const;
    bool Matches(uint8_t*,uint64_t epoch,std::string&)const;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
// Explicit private diagnostic only. Records/reuses core fixed-clock counter
// reads for one original/replayed update; never overrides other QPC callers.
bool BeginPrivateNativeClockRecord(uint8_t*,uint64_t epoch,std::string&);
bool SealPrivateNativeClockRecord(uint8_t*,uint64_t epoch,std::string&);
bool BeginPrivateNativeClockReplay(uint8_t*,uint64_t epoch,std::string&);
bool FinishPrivateNativeClockReplay(uint8_t*,uint64_t epoch,std::string&);
bool RunPrivateNativeReplayUpdate(PPCContext&,uint8_t*,std::string&);
}
