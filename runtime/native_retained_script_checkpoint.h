#pragma once
#include <cstdint>
#include <memory>
#include <string>
struct PPCContext;
namespace em2 {
// Trusted-local script checkpoint with original LuaObject GC roots. Native
// task/holder lifetimes must remain stable; no network import or full rollback.
class NativeRetainedScriptCheckpoint {
public:
    NativeRetainedScriptCheckpoint();
    ~NativeRetainedScriptCheckpoint();
    NativeRetainedScriptCheckpoint(const NativeRetainedScriptCheckpoint&)=delete;
    NativeRetainedScriptCheckpoint& operator=(const NativeRetainedScriptCheckpoint&)=delete;
    bool Prepare(PPCContext&,uint8_t*,uint32_t state,uint32_t values,unsigned count,uint64_t epoch,std::string&);
    bool Capture(PPCContext&,uint8_t*,uint64_t epoch,std::string&);
    bool Restore(PPCContext&,uint8_t*,uint64_t epoch,std::string&)const;
    bool Verify(PPCContext&,uint8_t*,uint64_t epoch,std::string&)const;
    bool RememberReplaySchedule(PPCContext&,uint8_t*,uint64_t epoch,std::string&);
    bool MatchesReplaySchedule(PPCContext&,uint8_t*,uint64_t epoch,std::string&)const;
    bool Release(PPCContext&,uint8_t*);
    bool Faulted(uint8_t*)const;
    unsigned tasks()const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
