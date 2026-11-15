#pragma once
#include <cstdint>
#include "native_rigid_body_checkpoint.h"
#include <memory>
#include <string>
struct PPCContext;
namespace em2 {
// Trusted local bundle at one completed simulation-owner boundary. Does not
// include scripts; the owning transaction must coordinate the script snapshot.
// Not complete simulation state, portable peer state or a rollback adapter.
class NativeGameplayComponentCheckpoint {
public:
    NativeGameplayComponentCheckpoint();
    ~NativeGameplayComponentCheckpoint();
    NativeGameplayComponentCheckpoint(const NativeGameplayComponentCheckpoint&)=delete;
    NativeGameplayComponentCheckpoint& operator=(const NativeGameplayComponentCheckpoint&)=delete;
    bool Capture(PPCContext&,uint8_t*,uint64_t epoch,std::string&);
    bool Restore(PPCContext&,uint8_t*,uint64_t epoch,std::string&)const;
    bool Matches(PPCContext&,uint8_t*,uint64_t epoch,std::string&)const;
    // Private mutation probe delegates its final restore through the whole bundle.
    bool TestPhysicsBridgeRoundTrip(uint8_t*,uint64_t epoch,std::string&)const;
    std::string DescribePhysicsBody(uint8_t*,uint32_t body)const;
    bool SamplePhysicsPhase(uint8_t*,uint32_t object,unsigned phase,std::vector<NativePhysicsMotionSample>&,std::string&)const;
    bool TestInputRoundTrip(PPCContext&,uint8_t*,uint64_t epoch,std::string&)const;
    bool TestRandomRoundTrip(PPCContext&,uint8_t*,uint64_t epoch,std::string&)const;
    bool TestRoundTrip(PPCContext&,uint8_t*,uint64_t epoch,std::string&)const;
    bool Release(PPCContext&,uint8_t*);
    size_t fields()const;
    unsigned bodies()const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
