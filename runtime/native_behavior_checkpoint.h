#pragma once
#include "native_component_schema.h"
#include <memory>
struct PPCContext;
namespace em2 {
// Trusted local, owner-thread native hkb state. Explicit Release is required.
// Not portable state or a complete rollback-world adapter. Restoring calls native
// graph deactivation/activation; its event effects are not isolated yet.
class NativeBehaviorCheckpoint {
public:
    NativeBehaviorCheckpoint();
    ~NativeBehaviorCheckpoint();
    NativeBehaviorCheckpoint(const NativeBehaviorCheckpoint&)=delete;
    NativeBehaviorCheckpoint& operator=(const NativeBehaviorCheckpoint&)=delete;
    bool Capture(PPCContext&,uint8_t*,const NativeEntityOwnership&,uint64_t,std::string&);
    bool ValidateOwnership(uint8_t*,uint64_t,std::string&)const;
    bool Restore(PPCContext&,uint8_t*,uint64_t,std::string&)const;
    bool Release(PPCContext&,uint8_t*);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
