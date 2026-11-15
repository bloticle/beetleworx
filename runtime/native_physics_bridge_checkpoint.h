#pragma once
#include "native_entity_probe.h"
#include <memory>
#include <string>
namespace em2 {
// EM2's scalar per-body physics userdata. Does not copy owner/array pointers,
// reconstruct contact queues or capture associated scene graph/controller state.
class NativePhysicsBridgeCheckpoint {
public:
    NativePhysicsBridgeCheckpoint();~NativePhysicsBridgeCheckpoint();
    NativePhysicsBridgeCheckpoint(const NativePhysicsBridgeCheckpoint&)=delete;
    NativePhysicsBridgeCheckpoint& operator=(const NativePhysicsBridgeCheckpoint&)=delete;
    bool Capture(uint8_t*,const NativeEntityOwnership&,uint64_t epoch,std::string&);
    bool Restore(uint8_t*,uint64_t epoch,std::string&)const;
    bool Matches(uint8_t*,uint64_t epoch,std::string&)const;
    bool TestRoundTrip(uint8_t*,uint64_t epoch,std::string&)const;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
