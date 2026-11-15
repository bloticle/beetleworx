#pragma once
#include <cstdint>
#include <memory>
#include <string>
struct PPCContext;
namespace em2 {
// Local component checkpoint for the registry's ordered deferred-destruction
// NiPointer queue. Does not undo teardown callbacks or resurrect entities.
class NativeDeferredEntityCheckpoint {
public:
    NativeDeferredEntityCheckpoint();
    ~NativeDeferredEntityCheckpoint();
    NativeDeferredEntityCheckpoint(const NativeDeferredEntityCheckpoint&)=delete;
    NativeDeferredEntityCheckpoint& operator=(const NativeDeferredEntityCheckpoint&)=delete;
    bool Capture(PPCContext&,uint8_t*,uint32_t registry,uint64_t epoch,std::string&);
    // Completed owner boundary; same epoch, registry owners and audited native
    // entity lifetime revision. Full world owner restores other state separately.
    bool Restore(PPCContext&,uint8_t*,uint32_t registry,uint64_t epoch,std::string&)const;
    bool Matches(uint8_t*,uint32_t registry,uint64_t epoch,std::string&)const;
    bool Release(PPCContext&,uint8_t*);
    unsigned size()const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
bool TestNativeDeferredEntityCheckpoint(PPCContext&,uint8_t*);
}
