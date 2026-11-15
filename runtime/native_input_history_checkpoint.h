#pragma once
#include <cstdint>
#include <memory>
#include <string>
struct PPCContext;
namespace em2 {
// Trusted local native device history/action latch component. Does not restore
// remapping/online source clocks, dispatch actions or cover every input system.
class NativeInputHistoryCheckpoint {
public:
    NativeInputHistoryCheckpoint();~NativeInputHistoryCheckpoint();
    NativeInputHistoryCheckpoint(const NativeInputHistoryCheckpoint&)=delete;
    NativeInputHistoryCheckpoint& operator=(const NativeInputHistoryCheckpoint&)=delete;
    bool Capture(uint8_t*,uint64_t epoch,std::string&);
    bool Restore(uint8_t*,uint64_t epoch,std::string&)const;
    bool Matches(uint8_t*,uint64_t epoch,std::string&)const;
    bool TestButtonEdges(PPCContext&,uint8_t*,uint64_t epoch,std::string&)const;
    bool TestRoundTrip(uint8_t*,uint64_t epoch,std::string&)const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
