#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
struct PPCContext;
namespace em2 {
// Issued/invalidation by the native world's owner. This is a local script-graph
// checkpoint, not complete world state and not a network bytecode import API.
struct NativeScriptCheckpointScope {
    uint64_t world=0,vm=0,permanents=0;
    bool operator==(const NativeScriptCheckpointScope&)const=default;
};
// A native serializer allocation failure can leave GC state unsafe. The VM
// owner must stop using/discard that VM; a failed capture is not recovery.
bool LocalNativeScriptVmFaulted(uint8_t*,uint32_t state) noexcept;
class NativeScriptCheckpoint {
public:
    ~NativeScriptCheckpoint();
    NativeScriptCheckpoint(const NativeScriptCheckpoint&)=delete;
    size_t size()const;
private:
    NativeScriptCheckpoint();
    struct Impl;std::unique_ptr<Impl> impl_;
    friend std::unique_ptr<NativeScriptCheckpoint> CaptureLocalNativeScriptCheckpoint(PPCContext&,uint8_t*,uint32_t,uint32_t,uint32_t,const NativeScriptCheckpointScope&,std::string&);
    friend bool RestoreLocalNativeScriptCheckpoint(PPCContext&,uint8_t*,uint32_t,uint32_t,const NativeScriptCheckpointScope&,const NativeScriptCheckpoint&,uint32_t&,std::string&);
};
// Arguments are currently rooted TValue addresses on this VM's stack. The
// capture permanence table maps objects->stable tokens; restore maps tokens->
// current objects. Native allocations may move the stack; callers must re-read
// stack addresses after either call. Capture leaves the original top intact.
std::unique_ptr<NativeScriptCheckpoint> CaptureLocalNativeScriptCheckpoint(PPCContext&,uint8_t* base,uint32_t state,
    uint32_t permanence_slot,uint32_t root_closure_slot,const NativeScriptCheckpointScope&,std::string& error);
// On success leaves one GC-rooted restored Lua closure on the VM stack and
// returns its slot; it does not execute that closure or replace world owners.
// Only snapshots created locally by Capture can be passed here. Network data
// must never be fed to HKS unpersist without validating program/native refs.
bool RestoreLocalNativeScriptCheckpoint(PPCContext&,uint8_t* base,uint32_t state,uint32_t permanence_slot,
    const NativeScriptCheckpointScope&,const NativeScriptCheckpoint&,uint32_t& closure_slot,std::string& error);
// Explicit private original-VM test: permanence/inverse tables, mutable closure
// and separate full-GC closure, plus original/restored suspended coroutines.
// Rebinds only allocated native holder and scheduler-record fixtures.
bool TestLocalNativeScriptCheckpoint(PPCContext&,uint8_t*,uint32_t,uint32_t,unsigned);
bool TestNativeScriptBulkCheckpoint(PPCContext&,uint8_t*,uint32_t,uint32_t,unsigned);
}
