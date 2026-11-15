#pragma once
#include "native_script_checkpoint.h"
#include <memory>
namespace em2 {
struct NativeScriptSchedulerCheckpointScope {
    NativeScriptCheckpointScope scripts;
    // Issued by the owner; invalidate on task creation/deletion/identity reuse.
    uint64_t tasks=0;
    bool operator==(const NativeScriptSchedulerCheckpointScope&)const=default;
};
class NativeScriptSchedulerCheckpoint {
public:
    ~NativeScriptSchedulerCheckpoint();
    NativeScriptSchedulerCheckpoint(const NativeScriptSchedulerCheckpoint&)=delete;
    unsigned tasks()const;
    size_t size()const;
private:
    NativeScriptSchedulerCheckpoint();
    struct Impl;std::unique_ptr<Impl> impl_;
    friend std::unique_ptr<NativeScriptSchedulerCheckpoint> CaptureLocalNativeSchedulerCheckpoint(PPCContext&,uint8_t*,uint32_t,uint32_t,uint32_t,uint32_t,const NativeScriptSchedulerCheckpointScope&,std::string&,bool);
    friend bool RestoreLocalNativeSchedulerCheckpoint(PPCContext&,uint8_t*,uint32_t,uint32_t,uint32_t,const NativeScriptSchedulerCheckpointScope&,const NativeScriptSchedulerCheckpoint&,std::string&,uint32_t);
    friend bool TestNativeSchedulerGraphCapture(PPCContext&,uint8_t*,uint32_t,uint32_t,unsigned);
};
// Factory is a trusted rooted Lua closure: (...threads) -> root Lua closure
// returning their ordered table. One graph preserves shared coroutine upvalues.
std::unique_ptr<NativeScriptSchedulerCheckpoint> CaptureLocalNativeSchedulerCheckpoint(PPCContext&,uint8_t*,uint32_t state,uint32_t manager,
    uint32_t permanence_slot,uint32_t factory_slot,const NativeScriptSchedulerCheckpointScope&,std::string&,bool context_restore_required=false);
// Same task-lifetime scope/inventory only. An optional trusted local restorer
// applies captured VM context; checkpoints can require it before any task writes.
// Native task creation/resurrection, userdata, other VM internals, world and
// replay side effects remain separate. Caller supplies transaction recovery.
bool RestoreLocalNativeSchedulerCheckpoint(PPCContext&,uint8_t*,uint32_t state,uint32_t manager,
    uint32_t inverse_permanence_slot,const NativeScriptSchedulerCheckpointScope&,const NativeScriptSchedulerCheckpoint&,std::string&,uint32_t local_context_restorer_slot=0);
bool TestNativeSchedulerGraphCapture(PPCContext&,uint8_t*,uint32_t,uint32_t,unsigned);
}
