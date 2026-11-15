#include "native_script_checkpoint.h"
#include "native_retained_script_checkpoint.h"
#include "native_frame_checkpoint_probe.h"
#include "native_gameplay_component_checkpoint.h"
#include "native_script_owners.h"
#include "native_script_scheduler.h"
#include "native_script_scheduler_checkpoint.h"
#include "allocation_trace.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/kernel/kernel_state.h>
#include <rex/logging.h>
#include <rex/cvar.h>
#include <windows.h>
#include <immintrin.h>
#include <array>
#include <cstring>
#include <vector>
#include <bit>
#include <algorithm>
PPC_EXTERN_FUNC(__imp__sub_826EA2C0);
PPC_EXTERN_FUNC(__imp__sub_826F6C20);
PPC_EXTERN_FUNC(__imp__sub_822F0710);
PPC_EXTERN_FUNC(__imp__sub_826D25F0);
PPC_EXTERN_FUNC(__imp__sub_826D2668);
PPC_EXTERN_FUNC(__imp__sub_826DEF58);
PPC_EXTERN_FUNC(__imp__sub_82719758);
PPC_EXTERN_FUNC(__imp__sub_827197E8);
PPC_EXTERN_FUNC(__imp__sub_82706B80);
PPC_EXTERN_FUNC(__imp__sub_82732770);
PPC_EXTERN_FUNC(__imp__sub_82730810);
REXCVAR_DEFINE_UINT32(em2_probe_native_vm_root_mode,0,"Debug","Private root capture isolation: 0 complete, 1 globals, 2 registry; partial captures are never restored");
REXCVAR_DEFINE_STRING(em2_probe_native_vm_global,"","Debug","Private globals-only capture of one raw dotted path; does not restore it");
REXCVAR_DEFINE_BOOL(em2_probe_native_script_restore,false,"Debug","With private persistence probe only, immediately restore actual script roots/tasks at the same completed boundary; not frame rollback");
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(bool,em2_probe_native_physics_restore);
REXCVAR_DECLARE(bool,em2_probe_native_frame_scripts);

namespace em2 {
bool TestPrivateNativeScriptBacking(PPCContext&,uint8_t*);
namespace {
constexpr uint32_t MaximumBytes=8*1024*1024;
thread_local std::array<uint32_t,16> faulted_vms{};
thread_local bool faulted_vm_overflow=false;
bool Faulted(uint32_t global){return global && (faulted_vm_overflow || std::find(faulted_vms.begin(),faulted_vms.end(),global)!=faulted_vms.end());}
void Poison(uint32_t global){for(auto& entry:faulted_vms)if(entry==global)return;for(auto& entry:faulted_vms)if(!entry){entry=global;return;}faulted_vm_overflow=true;}
bool Readable(uint8_t* base,uint32_t address,size_t size){return base && address>=4096 && size && uint64_t(address)+size<=0x100000000ull && GuestReadBatch::Readable(PPC_RAW_ADDR(address),size);}
struct Call {
    PPCContext& ctx;PPCContext saved;uint8_t* base;uint32_t state=0,global=0,storage=0,top=0,bottom=0;
    int64_t call_delta=0;unsigned csr=_mm_getcsr();DWORD error=GetLastError();bool valid=false,keep=false;
    Call(PPCContext& c,uint8_t* b,uint32_t s):ctx(c),saved(c),base(b),state(s){
        if(!Readable(base,state,128) || c.r1.u32<32768 || !Readable(base,c.r1.u32-32768,32768))return;
        global=PPC_LOAD_U32(state+8);storage=PPC_LOAD_U32(state+48);
        const auto t=PPC_LOAD_U32(state+36),v=PPC_LOAD_U32(state+40),last=PPC_LOAD_U32(state+44);
        if(!Readable(base,global,704) || t<storage || v<storage || v>t || (t-storage)%8 || (v-storage)%8 || t-storage>MaximumBytes || uint64_t(t)+32>last || !Readable(base,storage,t-storage+32))return;
        top=t-storage;bottom=v-storage;call_delta=int64_t(PPC_LOAD_U32(state+20))-PPC_LOAD_U32(state+12);
        if(call_delta%16 || call_delta < -65536 || call_delta>16*1024*1024)return;
        valid=true;ctx.r1.u64=saved.r1.u32-32768;
    }
    bool Intact()const{
        if(!valid || !Readable(base,state,128) || PPC_LOAD_U32(state+8)!=global)return false;
        const auto relocated=PPC_LOAD_U32(state+48);
        return uint64_t(relocated)+top<=UINT32_MAX && PPC_LOAD_U32(state+40)==uint64_t(relocated)+bottom &&
            int64_t(PPC_LOAD_U32(state+20))-PPC_LOAD_U32(state+12)==call_delta;
    }
    uint32_t At(uint32_t offset)const{const auto p=uint64_t(PPC_LOAD_U32(state+48))+offset;return p<=UINT32_MAX?uint32_t(p):0;}
    bool Slot(uint32_t slot,uint32_t type,uint32_t& offset)const{
        if(!valid || slot<storage || uint64_t(slot)+8>uint64_t(storage)+top || (slot-storage)%8 || !Readable(base,slot,8) || (PPC_LOAD_U32(slot+4)&15)!=type)return false;
        offset=slot-storage;return true;
    }
    bool Push(uint32_t value,uint32_t type){
        const auto at=PPC_LOAD_U32(state+36);
        if(uint64_t(at)+8>PPC_LOAD_U32(state+44) || !Readable(base,at,8))return false;
        PPC_STORE_U32(at,value);PPC_STORE_U32(at+4,type);PPC_STORE_U32(state+36,at+8);return true;
    }
    bool Function(uint32_t function){
        ctx.r3.u64=state;ctx.r4.u64=function;ctx.r5.u64=ctx.r6.u64=ctx.r7.u64=0;__imp__sub_826EA2C0(ctx,base);
        return ctx.r3.u32 && Push(ctx.r3.u32,10);
    }
    bool Copy(uint32_t offset,uint32_t expected){
        const auto at=At(offset);return at && Readable(base,at,8) && (PPC_LOAD_U32(at+4)&15)==expected && Push(PPC_LOAD_U32(at),PPC_LOAD_U32(at+4));
    }
    bool Invoke(unsigned arguments=2){
        ctx.r3.u64=state;ctx.r4.u64=arguments;ctx.r5.u64=1;ctx.r6.u64=0;__imp__sub_826F6C20(ctx,base);
        return ctx.r3.s32==0 && Intact() && PPC_LOAD_U32(state+36)==uint64_t(At(top))+8;
    }
    std::string Error()const{
        if(!Intact())return {};
        const auto top=PPC_LOAD_U32(state+36);if(top<At(this->top)+8 || !Readable(base,top-8,8) || (PPC_LOAD_U32(top-4)&15)!=4)return {};
        const auto string=PPC_LOAD_U32(top-8);if(!Readable(base,string,12))return {};
        const auto size=std::min(PPC_LOAD_U32(string+4)&0x3fffffffu,512u);if(!size || !Readable(base,string+12,size))return {};
        std::string text(reinterpret_cast<const char*>(base+string+12),size);for(auto& c:text)if(uint8_t(c)<32 || uint8_t(c)>126)c=' ';return text;
    }
    void CheckFatalError(){if(ctx.r3.s32==-200 || ctx.r3.u32==4){Poison(global);return;}const auto text=Error();if(text.find("Out of memory")!=std::string::npos || text.find("not enough memory")!=std::string::npos)Poison(global);}
    ~Call(){if(valid && !keep && Intact()){const auto at=At(top);if(at)PPC_STORE_U32(state+36,at);}ctx=saved;_mm_setcsr(csr);SetLastError(error);}
};
bool Scope(const NativeScriptCheckpointScope& s){return s.world && s.vm && s.permanents;}
}
bool LocalNativeScriptVmFaulted(uint8_t* base,uint32_t state)noexcept{
    const auto error=GetLastError();const auto csr=_mm_getcsr();
    const bool faulted=Readable(base,state,12) && Faulted(PPC_LOAD_U32(state+8));
    _mm_setcsr(csr);SetLastError(error);return faulted;
}
struct NativeScriptCheckpoint::Impl {
    NativeScriptCheckpointScope scope;uint32_t global=0,thread=0;std::vector<uint8_t> bytes;
};
NativeScriptCheckpoint::NativeScriptCheckpoint():impl_(std::make_unique<Impl>()){}
NativeScriptCheckpoint::~NativeScriptCheckpoint()=default;
size_t NativeScriptCheckpoint::size()const{return impl_->bytes.size();}
std::unique_ptr<NativeScriptCheckpoint> CaptureLocalNativeScriptCheckpoint(PPCContext& ctx,uint8_t* base,uint32_t state,
    uint32_t permanence,uint32_t closure,const NativeScriptCheckpointScope& scope,std::string& error)try{
    error.clear();Call call(ctx,base,state);uint32_t permanent_offset=0,root_offset=0;
    if(call.valid && Faulted(call.global)){error="Native script VM is faulted after an allocation failure; discard/reload the VM before further use";return {};}
    if(!Scope(scope) || !call.Slot(permanence,5,permanent_offset) || !call.Slot(closure,9,root_offset)){error="Native script capture requires rooted permanence table and Lua closure at a valid owner boundary";return {};}
    if(!call.Function(0x82715730) || !call.Copy(permanent_offset,5) || !call.Copy(root_offset,9) || !call.Invoke()){
        call.CheckFatalError();error="Original HKS persist rejected the graph or did not restore its caller stack";const auto detail=call.Error();if(!detail.empty())error+=": "+detail;return {};
    }
    const auto slot=call.At(call.top);
    if(!Readable(base,slot,8) || (PPC_LOAD_U32(slot+4)&15)!=4){error="Native script capture returned no serialized string";return {};}
    const auto string=PPC_LOAD_U32(slot);if(!Readable(base,string,12)){error="Native script string is unreadable";return {};}
    const auto size=PPC_LOAD_U32(string+4)&0x3fffffff;
    if(!size || size>MaximumBytes || !Readable(base,string+12,size)){error="Native script snapshot exceeds its bounded storage";return {};}
    auto result=std::unique_ptr<NativeScriptCheckpoint>(new NativeScriptCheckpoint());
    result->impl_->scope=scope;result->impl_->global=call.global;result->impl_->thread=GetCurrentThreadId();
    const auto* bytes=reinterpret_cast<const uint8_t*>(PPC_RAW_ADDR(string+12));result->impl_->bytes.assign(bytes,bytes+size);return result;
}catch(...){error="Native script capture failed";return {};}
bool RestoreLocalNativeScriptCheckpoint(PPCContext& ctx,uint8_t* base,uint32_t state,uint32_t permanence,
    const NativeScriptCheckpointScope& scope,const NativeScriptCheckpoint& snapshot,uint32_t& closure,std::string& error)try{
    closure=0;error.clear();const auto& saved=*snapshot.impl_;Call call(ctx,base,state);uint32_t permanent_offset=0;
    if(call.valid && Faulted(call.global)){error="Native script VM is faulted after an allocation failure; restore cannot recover its GC state";return false;}
    if(!Scope(scope) || scope!=saved.scope || GetCurrentThreadId()!=saved.thread || !call.Slot(permanence,5,permanent_offset) || call.global!=saved.global){error="Native script snapshot belongs to a different VM, world, permanence map or owner thread";return false;}
    auto* kernel=rex::kernel::kernel_state();if(!kernel || !kernel->memory() || saved.bytes.empty() || saved.bytes.size()>MaximumBytes){error="Native script restore allocation context unavailable";return false;}
    struct Buffer {rex::memory::Memory& memory;uint32_t address;~Buffer(){if(address)memory.SystemHeapFree(address);}} buffer{*kernel->memory(),kernel->memory()->SystemHeapAlloc(saved.bytes.size(),16)};
    if(!buffer.address || !Readable(base,buffer.address,saved.bytes.size())){error="Could not allocate native script restore buffer";return false;}
    std::memcpy(PPC_RAW_ADDR(buffer.address),saved.bytes.data(),saved.bytes.size());
    if(!call.Function(0x8271B5D8) || !call.Copy(permanent_offset,5)){error="Could not root original HKS unpersist call";return false;}
    ctx.r3.u64=state;ctx.r4.u64=buffer.address;ctx.r5.u64=saved.bytes.size();__imp__sub_822F0710(ctx,base);
    if(!call.Invoke()){call.CheckFatalError();error="Original HKS unpersist rejected the local graph or did not restore its caller stack";return false;}
    const auto slot=call.At(call.top);
    if(!Readable(base,slot,8) || (PPC_LOAD_U32(slot+4)&15)!=9){error="Restored native script root is not a Lua closure";return false;}
    call.keep=true;closure=slot;return true;
}catch(...){closure=0;error="Native script restore failed";return false;}
struct NativeScriptSchedulerCheckpoint::Impl {
    NativeScriptSchedulerCheckpointScope scope;NativeScriptScheduler scheduler;
    std::unique_ptr<NativeScriptCheckpoint> graph;
    bool context_required=false;
};
NativeScriptSchedulerCheckpoint::NativeScriptSchedulerCheckpoint():impl_(std::make_unique<Impl>()){}
NativeScriptSchedulerCheckpoint::~NativeScriptSchedulerCheckpoint()=default;
unsigned NativeScriptSchedulerCheckpoint::tasks()const{return unsigned(impl_->scheduler.tasks.size());}
size_t NativeScriptSchedulerCheckpoint::size()const{return impl_->graph?impl_->graph->size():0;}
std::unique_ptr<NativeScriptSchedulerCheckpoint> CaptureLocalNativeSchedulerCheckpoint(PPCContext& ctx,uint8_t* base,uint32_t state,uint32_t manager,
    uint32_t permanence,uint32_t factory,const NativeScriptSchedulerCheckpointScope& scope,std::string& error,bool context_required)try{
    error.clear();Call call(ctx,base,state);uint32_t permanent_offset=0,factory_offset=0;
    if(call.valid && Faulted(call.global)){error="Native scheduler VM is faulted; no further capture is safe";return {};}
    if(!scope.tasks || !Scope(scope.scripts) || !call.Slot(permanence,5,permanent_offset) || !call.Slot(factory,9,factory_offset)){error="Scheduler graph requires rooted local permanence/factory values and nonzero owner epochs";return {};}
    NativeScriptScheduler scheduler;if(!ReadNativeScriptScheduler(ctx,base,state,manager,scheduler,error))return {};
    const auto top=PPC_LOAD_U32(state+36);const auto needed=uint64_t(scheduler.tasks.size()+1)*8;
    if(uint64_t(top)+needed>PPC_LOAD_U32(state+44) || !Readable(base,top,size_t(std::max<uint64_t>(needed,8)))){error="Native scheduler graph requires more reserved VM stack slots";return {};}
    if(!call.Copy(factory_offset,9)){error="Could not root scheduler graph factory";return {};}
    for(const auto& task:scheduler.tasks)if(!call.Push(task.coroutine,8)){error="Could not root every scheduled coroutine";return {};}
    if(!call.Invoke(unsigned(scheduler.tasks.size()))){call.CheckFatalError();error="Native scheduler graph factory failed: "+call.Error();return {};}
    auto graph=CaptureLocalNativeScriptCheckpoint(ctx,base,state,call.At(permanent_offset),call.At(call.top),scope.scripts,error);
    if(!graph)return {};
    NativeScriptScheduler current;if(!ReadNativeScriptScheduler(ctx,base,state,manager,current,error) || current!=scheduler){error="Native scheduler changed during graph capture";return {};}
    auto result=std::unique_ptr<NativeScriptSchedulerCheckpoint>(new NativeScriptSchedulerCheckpoint());
    result->impl_->scope=scope;result->impl_->scheduler=std::move(scheduler);result->impl_->graph=std::move(graph);result->impl_->context_required=context_required;return result;
}catch(const std::exception& ex){error=ex.what();return {};}
bool RestoreLocalNativeSchedulerCheckpoint(PPCContext& ctx,uint8_t* base,uint32_t state,uint32_t manager,uint32_t inverse,
    const NativeScriptSchedulerCheckpointScope& scope,const NativeScriptSchedulerCheckpoint& snapshot,std::string& error,uint32_t context_restorer)try{
    error.clear();Call call(ctx,base,state);uint32_t inverse_offset=0,restorer_offset=0;const auto& s=*snapshot.impl_;
    if(call.valid && Faulted(call.global)){error="Native scheduler VM is faulted; graph restore cannot recover its GC state";return false;}
    if(scope!=s.scope || !scope.tasks || !call.Slot(inverse,5,inverse_offset)){error="Scheduler graph scope or inverse permanence table changed";return false;}
    if(s.context_required && !context_restorer){error="This scheduler checkpoint requires its local VM context restorer";return false;}
    if(context_restorer && !call.Slot(context_restorer,9,restorer_offset)){error="Scheduler context restorer must be a rooted trusted local Lua closure";return false;}
    NativeScriptScheduler current;if(!ReadNativeScriptScheduler(ctx,base,state,manager,current,error))return false;
    if(current.manager!=s.scheduler.manager || current.global!=s.scheduler.global || current.registry!=s.scheduler.registry || current.thread!=s.scheduler.thread || current.tasks.size()!=s.scheduler.tasks.size()){
        error="Scheduler graph owner/VM/task inventory changed; task lifetime restoration required";return false;
    }
    for(unsigned i=0;i<current.tasks.size();++i){const auto& a=current.tasks[i];const auto& b=s.scheduler.tasks[i];
        if(a.reference!=b.reference || std::memcmp(a.bytes.data()+8,b.bytes.data()+8,88) || std::memcmp(a.bytes.data()+108,b.bytes.data()+108,24)){
            error="Scheduler task identity/compiler state changed; graph restore rejected";return false;
        }
    }
    if(uint64_t(PPC_LOAD_U32(state+36))+uint64_t(current.tasks.size()+3)*8>PPC_LOAD_U32(state+44)){error="Restored scheduler graph requires more reserved VM stack slots";return false;}
    uint32_t root=0;if(!RestoreLocalNativeScriptCheckpoint(ctx,base,state,call.At(inverse_offset),scope.scripts,*s.graph,root,error))return false;
    if(!call.Invoke(0) || (PPC_LOAD_U32(call.At(call.top)+4)&15)!=5){call.CheckFatalError();error="Restored scheduler graph returned no thread table: "+call.Error();return false;}
    std::vector<NativeScheduledScriptReplacement> replacements;replacements.reserve(current.tasks.size());
    for(unsigned i=0;i<current.tasks.size();++i){
        ctx.r3.u64=state;ctx.r4.u64=call.At(call.top);ctx.r5.u64=i+1;__imp__sub_826DEF58(ctx,base);
        if((ctx.r3.u32&15)!=8){error="Restored scheduler graph contains a non-thread entry";return false;}
        const auto slot=PPC_LOAD_U32(state+36);if(!call.Push(uint32_t(ctx.r3.u64>>32),ctx.r3.u32)){error="Could not root a restored scheduled coroutine";return false;}
        NativeScheduledScriptReplacement replacement;replacement.task=i;replacement.rooted_thread_slot=slot;
        std::memcpy(replacement.scheduling.data(),s.scheduler.tasks[i].bytes.data()+96,12);replacements.push_back(replacement);
    }
    if(!replacements.empty() && !RebindNativeScheduledScripts(ctx,base,state,current,replacements,error))return false;
    if(context_restorer){
        const auto before=PPC_LOAD_U32(state+36)-PPC_LOAD_U32(state+48);
        if(!call.Copy(restorer_offset,9) || !call.Copy(call.top,5)){error="Could not root scheduler context restore call";return false;}
        ctx.r3.u64=state;ctx.r4.u64=1;ctx.r5.u64=ctx.r6.u64=0;__imp__sub_826F6C20(ctx,base);
        if(ctx.r3.s32 || !call.Intact() || PPC_LOAD_U32(state+36)!=call.At(before)){call.CheckFatalError();error="Native scheduler context restore failed: "+call.Error();return false;}
    }
    return true;
}catch(const std::exception& ex){error=ex.what();return false;}
struct NativeRetainedScriptCheckpoint::Impl {
    uint32_t state=0,global=0,helpers=0,thread=0;unsigned constructed=0;uint64_t epoch=0;
    NativeScriptOwners baseline,rooted;NativeScriptScheduler scheduler;mutable NativeScriptScheduler bound;NativeScriptScheduler expected_replay;bool replay_expected=false;
    NativeScriptSchedulerCheckpointScope scope;std::unique_ptr<NativeScriptSchedulerCheckpoint> snapshot;
    bool Owner(uint8_t* base,uint64_t expected,std::string& error)const{
        NativeScriptOwners current;
        if(!helpers || constructed!=5 || !expected || expected!=epoch || thread!=GetCurrentThreadId() ||
            !Readable(base,state,128) || PPC_LOAD_U32(state+8)!=global || LocalNativeScriptVmFaulted(base,state) ||
            !ReadNativeScriptOwners(base,state,current,error) || current!=rooted){
            error="Retained script checkpoint VM/epoch/native holder lifetime changed";return false;
        }return true;
    }
};
NativeRetainedScriptCheckpoint::NativeRetainedScriptCheckpoint():impl_(std::make_unique<Impl>()){}
NativeRetainedScriptCheckpoint::~NativeRetainedScriptCheckpoint(){if(impl_->helpers)REXLOG_ERROR("Retained script checkpoint destroyed without owner-thread root release");}
unsigned NativeRetainedScriptCheckpoint::tasks()const{return impl_->snapshot?impl_->snapshot->tasks():0;}
bool NativeRetainedScriptCheckpoint::Faulted(uint8_t* base)const{return impl_->state && LocalNativeScriptVmFaulted(base,impl_->state);}
bool NativeRetainedScriptCheckpoint::Prepare(PPCContext& ctx,uint8_t* base,uint32_t state,uint32_t values,unsigned count,uint64_t epoch,std::string& error){
    auto& s=*impl_;Call call(ctx,base,state);
    if(s.helpers || !epoch || !call.valid || count!=11 || values<call.storage || !Readable(base,values,88) ||
        uint64_t(values)+88>uint64_t(call.storage)+call.top || Faulted(base)){
        error="Retained script helper preparation requires the trusted local rooted probe results";return false;
    }
    if(!ReadNativeScriptOwners(base,state,s.baseline,error) || !s.baseline.owners.empty()){
        error="Unadapted native gameplay LuaObject holders prevent retained script preparation";return false;
    }
    auto* kernel=rex::kernel::kernel_state();if(!kernel || !kernel->memory()){error="Retained script allocation context unavailable";return false;}
    s.state=state;s.global=call.global;s.thread=GetCurrentThreadId();s.epoch=epoch;
    s.helpers=kernel->memory()->SystemHeapAlloc(160,16);
    if(!Readable(base,s.helpers,160)){error="Retained script helper allocation failed";return false;}
    std::memset(base+s.helpers,0,160);const auto offset=values-call.storage,source=s.helpers+128;
    constexpr std::array<uint32_t,5> offsets{0,8,16,40,80};
    for(unsigned i=0;i<offsets.size();++i){
        const auto slot=call.At(offset+offsets[i]),type=i<2?5u:9u;
        if(!Readable(base,slot,8) || (PPC_LOAD_U32(slot+4)&15)!=type){error="Trusted script helper has the wrong type";Release(ctx,base);return false;}
        PPC_STORE_U32(source,state);PPC_STORE_U32(source+4,PPC_LOAD_U32(slot));PPC_STORE_U32(source+8,PPC_LOAD_U32(slot+4));
        ctx.r3.u64=s.helpers+i*24;ctx.r4.u64=source;__imp__sub_826D25F0(ctx,base);++s.constructed;
    }
    if(!ReadNativeScriptOwners(base,state,s.rooted,error) || s.rooted.owners.size()!=5){Release(ctx,base);error="Native retained helper GC roots were not installed";return false;}
    for(unsigned i=0;i<5;++i){const auto address=s.helpers+i*24;
        if(std::none_of(s.rooted.owners.begin(),s.rooted.owners.end(),[&](const auto& owner){return owner.address==address && owner.state==state;})){
            Release(ctx,base);error="Native retained helper owner is missing";return false;
        }
    }
    s.scope={{epoch,s.global,s.helpers},epoch};
    REXLOG_INFO("Private retained script helpers prepared: five original LuaObject roots, no registry entries, world {}",epoch);error.clear();return true;
}
bool NativeRetainedScriptCheckpoint::Capture(PPCContext& ctx,uint8_t* base,uint64_t epoch,std::string& error){
    auto& s=*impl_;if(s.snapshot || !s.Owner(base,epoch,error))return false;
    Call call(ctx,base,s.state);if(!call.valid || !Readable(base,0x833DA7B8,4)){error="Retained script capture boundary unavailable";return false;}
    if(!call.Push(PPC_LOAD_U32(s.helpers+4),PPC_LOAD_U32(s.helpers+8)) || !call.Push(PPC_LOAD_U32(s.helpers+48+4),PPC_LOAD_U32(s.helpers+48+8))){error="Retained script capture helper stack unavailable";return false;}
    const auto permanence=call.At(call.top),factory=call.At(call.top+8);
    const auto manager=PPC_LOAD_U32(0x833DA7B8);
    if(!ReadNativeScriptScheduler(ctx,base,s.state,manager,s.scheduler,error))return false;
    s.bound=s.scheduler;
    s.snapshot=CaptureLocalNativeSchedulerCheckpoint(ctx,base,s.state,manager,permanence,factory,s.scope,error,true);
    if(!s.snapshot)return false;
    return s.Owner(base,epoch,error);
}
bool NativeRetainedScriptCheckpoint::Restore(PPCContext& ctx,uint8_t* base,uint64_t epoch,std::string& error)const{
    const auto& s=*impl_;if(!s.snapshot || !s.Owner(base,epoch,error))return false;
    NativeScriptScheduler current;
    if(!ReadNativeScriptScheduler(ctx,base,s.state,s.scheduler.manager,current,error) || current.tasks.size()!=s.bound.tasks.size()){
        error="Retained script task lifetime inventory changed";return false;
    }
    for(unsigned i=0;i<current.tasks.size();++i)if(current.tasks[i].reference!=s.bound.tasks[i].reference || current.tasks[i].coroutine!=s.bound.tasks[i].coroutine){
        error="Retained script coroutine identity changed; task slot reuse cannot authorize restoration";return false;
    }
    Call call(ctx,base,s.state);if(!call.valid || !call.Push(PPC_LOAD_U32(s.helpers+24+4),PPC_LOAD_U32(s.helpers+24+8)) ||
        !call.Push(PPC_LOAD_U32(s.helpers+72+4),PPC_LOAD_U32(s.helpers+72+8))){error="Retained script restore helper stack unavailable";return false;}
    if(!RestoreLocalNativeSchedulerCheckpoint(ctx,base,s.state,s.scheduler.manager,call.At(call.top),s.scope,*s.snapshot,error,call.At(call.top+8)))return false;
    if(!Verify(ctx,base,epoch,error))return false;
    if(!ReadNativeScriptScheduler(ctx,base,s.state,s.scheduler.manager,s.bound,error))return false;
    return true;
}
bool NativeRetainedScriptCheckpoint::Verify(PPCContext& ctx,uint8_t* base,uint64_t epoch,std::string& error)const{
    const auto& s=*impl_;if(!s.snapshot || !s.Owner(base,epoch,error))return false;
    Call call(ctx,base,s.state);if(!call.valid || !call.Push(PPC_LOAD_U32(s.helpers+96+4),PPC_LOAD_U32(s.helpers+96+8))){error="Retained script validator stack unavailable";return false;}
    ctx.r3.u64=s.state;ctx.r4.u64=ctx.r5.u64=ctx.r6.u64=0;__imp__sub_826F6C20(ctx,base);
    if(ctx.r3.s32 || !call.Intact() || PPC_LOAD_U32(s.state+36)!=call.At(call.top)){
        call.CheckFatalError();error="Retained applied VM-root validation failed: "+call.Error();return false;
    }
    NativeScriptScheduler current;
    if(!ReadNativeScriptScheduler(ctx,base,s.state,s.scheduler.manager,current,error) || current.tasks.size()!=s.scheduler.tasks.size()){error="Retained restored scheduler task inventory changed";return false;}
    for(unsigned i=0;i<current.tasks.size();++i){const auto& a=current.tasks[i];const auto& b=s.scheduler.tasks[i];
        if(a.reference!=b.reference || std::memcmp(a.bytes.data()+8,b.bytes.data()+8,124)){
            error="Retained restored task scheduling/compiler/owner metadata mismatch";return false;
        }
    }
    error.clear();return true;
}
bool NativeRetainedScriptCheckpoint::RememberReplaySchedule(PPCContext& ctx,uint8_t* base,uint64_t epoch,std::string& error){
    auto& s=*impl_;if(!s.Owner(base,epoch,error) || s.replay_expected)return false;
    if(!ReadNativeScriptScheduler(ctx,base,s.state,s.scheduler.manager,s.expected_replay,error))return false;
    s.replay_expected=true;return true;
}
bool NativeRetainedScriptCheckpoint::MatchesReplaySchedule(PPCContext& ctx,uint8_t* base,uint64_t epoch,std::string& error)const{
    const auto& s=*impl_;NativeScriptScheduler current;
    if(!s.replay_expected || !s.Owner(base,epoch,error) || !ReadNativeScriptScheduler(ctx,base,s.state,s.scheduler.manager,current,error))return false;
    if(current.tasks.size()!=s.expected_replay.tasks.size()){error="Replayed native scheduler task count differs";return false;}
    for(unsigned i=0;i<current.tasks.size();++i){const auto& a=current.tasks[i];const auto& b=s.expected_replay.tasks[i];
        if(a.reference!=b.reference || std::memcmp(a.bytes.data()+8,b.bytes.data()+8,124)){error="Replayed native script scheduling/compiler/owner metadata differs";return false;}
    }error.clear();return true;
}
bool NativeRetainedScriptCheckpoint::Release(PPCContext& ctx,uint8_t* base){
    auto& s=*impl_;if(!s.helpers)return true;
    if(s.thread!=GetCurrentThreadId() || Faulted(base))return false;
    Call call(ctx,base,s.state);if(!call.valid)return false;
    for(unsigned i=s.constructed;i-->0;){ctx.r3.u64=s.helpers+i*24;__imp__sub_826D2668(ctx,base);--s.constructed;}
    NativeScriptOwners after;std::string error;
    const auto unlinked=ReadNativeScriptOwners(base,s.state,after,error) && after==s.baseline &&
        std::none_of(after.owners.begin(),after.owners.end(),[&](const auto& owner){return owner.address>=s.helpers && owner.address<s.helpers+120;});
    if(!unlinked)return false;
    rex::kernel::kernel_state()->memory()->SystemHeapFree(s.helpers);s.helpers=0;s.snapshot.reset();return true;
}
static bool TestNativeSchedulerGraphRoundTrip(PPCContext& ctx,uint8_t* base,uint32_t state,uint32_t values){
    Call call(ctx,base,state);if(!call.valid || values<call.storage)return false;const auto offset=values-call.storage;
    auto* memory=rex::kernel::kernel_state()->memory();const auto scratch=memory->SystemHeapAlloc(512,16);
    if(!Readable(base,scratch,512))return false;std::memset(base+scratch,0,512);
    struct Fixture {
        PPCContext& ctx;uint8_t* base;rex::memory::Memory* memory;uint32_t state,scratch;std::array<uint32_t,2> refs{};
        ~Fixture(){for(auto ref:refs)if(ref){ctx.r3.u64=state;ctx.r4.s64=-10000;ctx.r5.u64=ref;__imp__sub_827197E8(ctx,base);}memory->SystemHeapFree(scratch);}
    }fixture{ctx,base,memory,state,scratch};
    const auto manager=scratch,array=scratch+96;
    PPC_STORE_U32(manager+72,array);PPC_STORE_U32(manager+76,2);PPC_STORE_U32(manager+80,2);
    for(unsigned i=0;i<2;++i){
        const auto slot=call.At(offset+24+i*8);if(!call.Copy(offset+24+i*8,8))return false;
        ctx.r3.u64=state;ctx.r4.s64=-10000;__imp__sub_82719758(ctx,base);fixture.refs[i]=ctx.r3.u32;
        const auto task=array+i*132;PPC_STORE_U32(task,fixture.refs[i]);PPC_STORE_U32(task+4,PPC_LOAD_U32(slot));
        ctx.r3.u64=task+8;__imp__sub_82706B80(ctx,base);PPC_STORE_U32(task+96,std::bit_cast<uint32_t>(7.f));PPC_STORE_U8(task+101,1);
    }
    std::string error;const NativeScriptSchedulerCheckpointScope scope{{1,1,1},1};
    auto snapshot=CaptureLocalNativeSchedulerCheckpoint(ctx,base,state,manager,call.At(offset),call.At(offset+16),scope,error,true);
    if(!snapshot || snapshot->tasks()!=2){REXLOG_ERROR("Native shared scheduler graph capture: {}",error);return false;}
    auto resume=[&](unsigned i,float expected){
        const auto task=array+i*132;ctx.r3.u64=task;ctx.r4.u64=0;__imp__sub_82732770(ctx,base);
        if(!PPC_LOAD_U32(task+4)){fixture.refs[i]=0;return false;}
        ctx.r3.u64=task;ctx.r4.u64=task+96;__imp__sub_82730810(ctx,base);
        return PPC_LOAD_U32(task+96)==std::bit_cast<uint32_t>(expected);
    };
    if(!resume(0,8) || !resume(1,18)){REXLOG_ERROR("Native shared scheduler graph original fixture did not share its upvalue");return false;}
    const auto before=PPC_LOAD_U32(array+4);auto wrong=scope;++wrong.tasks;
    if(RestoreLocalNativeSchedulerCheckpoint(ctx,base,state,manager,call.At(offset+8),scope,*snapshot,error) || PPC_LOAD_U32(array+4)!=before){REXLOG_ERROR("Native scheduler graph ignored its required context restorer");return false;}
    if(RestoreLocalNativeSchedulerCheckpoint(ctx,base,state,manager,call.At(offset+8),wrong,*snapshot,error) || PPC_LOAD_U32(array+4)!=before){REXLOG_ERROR("Native scheduler graph accepted wrong task epoch");return false;}
    for(unsigned repeat=0;repeat<2;++repeat){
        if(!RestoreLocalNativeSchedulerCheckpoint(ctx,base,state,manager,call.At(offset+8),scope,*snapshot,error,call.At(offset+40))){REXLOG_ERROR("Native scheduler graph restore: {}",error);return false;}
        if(PPC_LOAD_U32(array+96)!=std::bit_cast<uint32_t>(7.f) || PPC_LOAD_U32(array+132+96)!=std::bit_cast<uint32_t>(7.f) || !resume(0,8) || !resume(1,18)){
            REXLOG_ERROR("Native restored scheduler graph lost captured timing or shared upvalue aliases");return false;
        }
        const auto validation_top=PPC_LOAD_U32(state+36)-PPC_LOAD_U32(state+48);
        if(!call.Copy(offset+48,9))return false;
        ctx.r3.u64=state;ctx.r4.u64=ctx.r5.u64=ctx.r6.u64=0;__imp__sub_826F6C20(ctx,base);
        if(ctx.r3.s32 || !call.Intact() || PPC_LOAD_U32(state+36)!=call.At(validation_top)){REXLOG_ERROR("Native restored VM context did not share coroutine/global/registry aliases");return false;}
    }
    REXLOG_INFO("Native scheduler shared graph roundtrip passed: {} bytes, two native task fixtures, shared yields 8/18, global/registry aliases and added/deleted keys reproduced across two restores, waits reset to 7, wrong epoch and missing context restorer rejected; live scheduler not restored",snapshot->size());return true;
}
bool TestNativeScriptBulkCheckpoint(PPCContext& ctx,uint8_t* base,uint32_t state,uint32_t values,unsigned count){
    if(!TestPrivateNativeScriptBacking(ctx,base))return false;
    Call call(ctx,base,state);if(!call.valid || count!=3 || values<call.storage)return false;const auto offset=values-call.storage;
    std::string error;const NativeScriptCheckpointScope scope{1,1,1};
    auto snapshot=CaptureLocalNativeScriptCheckpoint(ctx,base,state,call.At(offset),call.At(offset+16),scope,error);
    if(!snapshot){REXLOG_ERROR("Native bulk script capture failed: {}",error);return false;}
    uint32_t restored=0;
    if(!RestoreLocalNativeScriptCheckpoint(ctx,base,state,call.At(offset+8),scope,*snapshot,restored,error) || !call.Invoke(0)){
        REXLOG_ERROR("Native bulk script restore failed: {} {}",error,call.Error());return false;
    }
    const auto slot=call.At(call.top);if((PPC_LOAD_U32(slot+4)&15)!=4)return false;const auto string=PPC_LOAD_U32(slot);
    if(!Readable(base,string,12) || (PPC_LOAD_U32(string+4)&0x3fffffffu)!=262144 || !Readable(base,string+12,262144))return false;
    for(unsigned i=0;i<262144;++i)if(base[string+12+i]!='Q')return false;
    REXLOG_INFO("Native bulk script roundtrip passed: {} serialized bytes, full 262144-byte payload checked; original persist/unpersist, not game state",snapshot->size());return true;
}
bool TestNativeSchedulerGraphCapture(PPCContext& ctx,uint8_t* base,uint32_t state,uint32_t values,unsigned count){
    Call call(ctx,base,state);if(!call.valid || count!=11 || values<call.storage || !Readable(base,values,88) || !Readable(base,0x833DA7B8,4))return false;
    const auto names=PPC_LOAD_U32(values+64);
    if((PPC_LOAD_U32(values+68)&15)==4 && Readable(base,names,12)){
        const auto length=PPC_LOAD_U32(names+4)&0x3fffffffu;
        if(length<=65536 && Readable(base,names+12,std::max(1u,length)))REXLOG_INFO("Native VM global root index: {}",std::string(reinterpret_cast<const char*>(base+names+12),length));
    }
    const auto offset=values-call.storage;if(!TestNativeSchedulerGraphRoundTrip(ctx,base,state,values))return false;
    std::string error;const NativeScriptSchedulerCheckpointScope scope{{CurrentNativeLoadGeneration(),1,1},1};
    const auto mode=REXCVAR_GET(em2_probe_native_vm_root_mode);if(mode>2)return false;
    const auto selected=REXCVAR_GET(em2_probe_native_vm_global);
    if(selected.size()>128 || (!selected.empty() && mode!=1))return false;
    for(auto c:selected)if(!((c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='_' || c=='.' || c=='@'))return false;
    if(mode){
        const auto top=PPC_LOAD_U32(state+36)-PPC_LOAD_U32(state+48);
        if(!call.Copy(offset+56,9) || !call.Push(std::bit_cast<uint32_t>(float(mode)),3))return false;
        const auto text=ctx.r1.u32+1024;if(!Readable(base,text,selected.size()+1))return false;
        std::memcpy(base+text,selected.c_str(),selected.size()+1);ctx.r3.u64=state;ctx.r4.u64=text;ctx.r5.u64=selected.size();__imp__sub_822F0710(ctx,base);
        ctx.r3.u64=state;ctx.r4.u64=2;ctx.r5.u64=ctx.r6.u64=0;__imp__sub_826F6C20(ctx,base);
        if(ctx.r3.s32 || !call.Intact() || PPC_LOAD_U32(state+36)!=call.At(top))return false;
    }
    NativeGameplayComponentCheckpoint gameplay;
    struct ReleaseGameplay {NativeGameplayComponentCheckpoint& value;PPCContext& ctx;uint8_t* base;~ReleaseGameplay(){value.Release(ctx,base);}}release_gameplay{gameplay,ctx,base};
    const bool combined=REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_probe_native_script_restore) && REXCVAR_GET(em2_probe_native_physics_restore) && !mode;
    if(combined && !gameplay.Capture(ctx,base,scope.scripts.world,error)){
        REXLOG_ERROR("Private combined gameplay capture failed: {}",error);return false;
    }
    auto snapshot=CaptureLocalNativeSchedulerCheckpoint(ctx,base,state,PPC_LOAD_U32(0x833DA7B8),call.At(offset),call.At(offset+16),scope,error,true);
    if(!snapshot){
        REXLOG_ERROR("Native LIVE VM root capture rejected: mode {} (0 complete/1 globals/2 registry), selected '{}', {}; VM faulted {}; no live state restored",mode,selected,error,LocalNativeScriptVmFaulted(base,state));
        return false;
    }
    if(mode){REXLOG_INFO("Native VM root isolation: mode {} selected '{}' captured {} bytes; diagnostic subset only, not a complete snapshot and not restored",mode,selected,snapshot->size());return false;}
    REXLOG_INFO("Native LIVE scheduler graph capture passed: {} actual tasks captured together, {} local bytes; live scheduler was not restored",snapshot->tasks(),snapshot->size());
    // Decode the complete local graph into detached objects, without rebinding
    // live tasks or applying VM roots. Check registry/coroutine aliases and that
    // native owners and the original VM table contents remain unchanged.
    const auto& saved=*snapshot->impl_;
    if(uint64_t(PPC_LOAD_U32(state+36))+uint64_t(saved.scheduler.tasks.size()+4)*8>PPC_LOAD_U32(state+44))return false;
    uint32_t decoded=0;
    if(!RestoreLocalNativeScriptCheckpoint(ctx,base,state,call.At(offset+8),scope.scripts,*saved.graph,decoded,error) ||
       !call.Invoke(0) || (PPC_LOAD_U32(call.At(call.top)+4)&15)!=5){
        call.CheckFatalError();REXLOG_ERROR("Native LIVE graph detached decode failed: {} {}",error,call.Error());return false;
    }
    const auto decoded_top=PPC_LOAD_U32(state+36)-PPC_LOAD_U32(state+48);
    if(!call.Copy(offset+72,9) || !call.Copy(call.top,5))return false;
    for(const auto& task:saved.scheduler.tasks){
        if(uint32_t(float(task.reference))!=task.reference || !call.Push(std::bit_cast<uint32_t>(float(task.reference)),3))return false;
    }
    ctx.r3.u64=state;ctx.r4.u64=saved.scheduler.tasks.size()+1;ctx.r5.u64=ctx.r6.u64=0;__imp__sub_826F6C20(ctx,base);
    if(ctx.r3.s32 || !call.Intact() || PPC_LOAD_U32(state+36)!=call.At(decoded_top)){
        call.CheckFatalError();REXLOG_ERROR("Native LIVE detached graph alias/original-root validation failed: {}",call.Error());return false;
    }
    NativeScriptScheduler unchanged;
    if(!ReadNativeScriptScheduler(ctx,base,state,saved.scheduler.manager,unchanged,error) || unchanged!=saved.scheduler){
        REXLOG_ERROR("Native LIVE scheduler changed while decoding detached graph: {}",error);return false;
    }
    REXLOG_INFO("Native LIVE graph detached decode passed: {} tasks, {} bytes, registry/thread aliases and unchanged original globals/registry/task ownership checked; no live restore or replay",snapshot->tasks(),snapshot->size());
    if(REXCVAR_GET(em2_probe_native_frame_scripts)){
        if(!REXCVAR_GET(em2_private_profile) || REXCVAR_GET(em2_probe_native_script_restore) || REXCVAR_GET(em2_probe_native_physics_restore)){
            REXLOG_ERROR("Private retained frame script test requires capture-only setup, not earlier mid-frame actual restores");return false;
        }
        if(!PreparePrivateNativeFrameScriptRoots(ctx,base,state,call.At(offset),count,error)){
            REXLOG_ERROR("Private retained frame script preparation failed: {}",error);return false;
        }
        return true;
    }
    if(!REXCVAR_GET(em2_probe_native_script_restore))return true;
    if(!REXCVAR_GET(em2_private_profile))return false;
    NativeScriptOwners holders;
    if(!ReadNativeScriptOwners(base,state,holders,error) || !holders.owners.empty()){
        REXLOG_ERROR("Private live script restore requires an empty native LuaObject holder list: {} holders; {}",holders.owners.size(),error);return false;
    }
    PPC_STORE_U32(state+36,call.At(call.top)); // Drop the detached graph root.
    // Once writes start, any failure stops this private process before native
    // simulation can resume with partially replaced ownership. No recovery is
    // implied: the complete world transaction is still a separate requirement.
    auto fatal=[&](const std::string& why){REXLOG_ERROR("Private live script restore stopped before simulation resumed: {}",why);spdlog::shutdown();ExitProcess(1);};
    if(combined && !gameplay.Matches(ctx,base,scope.scripts.world,error))fatal("Gameplay changed during script capture: "+error);
    if(!RestoreLocalNativeSchedulerCheckpoint(ctx,base,state,saved.scheduler.manager,call.At(offset+8),scope,*snapshot,error,call.At(offset+40)))fatal(error);
    const auto restored_top=PPC_LOAD_U32(state+36)-PPC_LOAD_U32(state+48);
    if(!call.Copy(offset+80,9))fatal("Missing trusted post-restore validator");
    ctx.r3.u64=state;ctx.r4.u64=ctx.r5.u64=ctx.r6.u64=0;__imp__sub_826F6C20(ctx,base);
    if(ctx.r3.s32 || !call.Intact() || PPC_LOAD_U32(state+36)!=call.At(restored_top))fatal("VM roots did not match applied snapshot: "+call.Error());
    NativeScriptScheduler restored;
    if(!ReadNativeScriptScheduler(ctx,base,state,saved.scheduler.manager,restored,error) || restored.tasks.size()!=saved.scheduler.tasks.size())fatal(error);
    unsigned replaced=0;
    for(unsigned i=0;i<restored.tasks.size();++i){
        const auto& a=restored.tasks[i];const auto& b=saved.scheduler.tasks[i];
        if(a.reference!=b.reference || std::memcmp(a.bytes.data()+8,b.bytes.data()+8,124))fatal("Native scheduling/compiler/owner state changed during immediate script restore");
        replaced+=a.coroutine!=b.coroutine;
    }
    if(combined){
        if(gameplay.Restore(ctx,base,scope.scripts.world+1,error))fatal("Combined component bundle accepted another world epoch");
        if(!gameplay.Matches(ctx,base,scope.scripts.world,error))fatal("Wrong-epoch rejection changed gameplay: "+error);
        if(!gameplay.TestRoundTrip(ctx,base,scope.scripts.world,error))fatal("Combined component mutation/restore: "+error);
        // Re-run the trusted VM validator after native reconstruction so this
        // tests their composition, rather than only each component separately.
        if(!call.Copy(offset+80,9))fatal("Missing post-composition script validator");
        ctx.r3.u64=state;ctx.r4.u64=ctx.r5.u64=ctx.r6.u64=0;__imp__sub_826F6C20(ctx,base);
        if(ctx.r3.s32 || !call.Intact() || PPC_LOAD_U32(state+36)!=call.At(restored_top))fatal("Combined restore changed VM roots: "+call.Error());
        REXLOG_INFO("Private combined native component restore passed: {} fields, {} bodies, {} scheduled scripts; common boundary, health/matrix/property box mutated and restored through bundle, wrong epoch rejected, final script roots and component payloads checked; no complete simulation frame replayed",gameplay.fields(),gameplay.bodies(),restored.tasks.size());
        if(!gameplay.Release(ctx,base))fatal("Combined native checkpoint release failed");
    }
    REXLOG_INFO("Private LIVE script immediate restore passed: {} tasks, {} replaced coroutine identities, {} bytes, global/registry bindings and native schedule/owner metadata checked; same boundary only, no simulation frame rewound",restored.tasks.size(),replaced,snapshot->size());return true;
}
bool TestLocalNativeScriptCheckpoint(PPCContext& ctx,uint8_t* base,uint32_t state,uint32_t values,unsigned count){
    Call call(ctx,base,state);if(!call.valid || count!=6 || values<call.storage)return false;
    const auto values_offset=values-call.storage;
    std::string error;const NativeScriptCheckpointScope scope{1,1,1};
    const auto before=ctx;const auto csr=_mm_getcsr();const auto last_error=GetLastError();
    auto snapshot=CaptureLocalNativeScriptCheckpoint(ctx,base,state,call.At(values_offset),call.At(values_offset+16),scope,error);
    if(!snapshot || !snapshot->size() || !call.Intact() || std::memcmp(&ctx,&before,sizeof(ctx)) || _mm_getcsr()!=csr || GetLastError()!=last_error){REXLOG_ERROR("Native script checkpoint bridge capture: {}",error);return false;}
    auto invoke=[&](uint32_t offset,int increment,int expected){
        const auto original_top=PPC_LOAD_U32(state+36)-PPC_LOAD_U32(state+48);
        if(!call.Copy(offset,9) || !call.Push(std::bit_cast<uint32_t>(float(increment)),3))return false;
        ctx.r3.u64=state;ctx.r4.u64=1;ctx.r5.u64=1;ctx.r6.u64=0;__imp__sub_826F6C20(ctx,base);
        if(ctx.r3.s32 || !call.Intact()){REXLOG_ERROR("Native script checkpoint test invoke status {} caller stack intact {}",ctx.r3.s32,call.Intact());return false;}
        const auto result=call.At(original_top);
        if(!Readable(base,result,8))return false;const auto type=PPC_LOAD_U32(result+4)&15,word=PPC_LOAD_U32(result);
        const bool matches=type==3 && std::bit_cast<float>(word)==float(expected);
        PPC_STORE_U32(state+36,result);return matches;
    };
    if(!invoke(values_offset+16,10,17)){REXLOG_ERROR("Native script checkpoint bridge could not mutate original fixture closure");return false;}
    uint32_t restored=0;const auto current_top=PPC_LOAD_U32(state+36);auto wrong=scope;++wrong.vm;
    if(RestoreLocalNativeScriptCheckpoint(ctx,base,state,call.At(values_offset+8),wrong,*snapshot,restored,error) || restored || PPC_LOAD_U32(state+36)!=current_top){REXLOG_ERROR("Native script checkpoint bridge accepted mismatched VM ownership");return false;}
    if(!RestoreLocalNativeScriptCheckpoint(ctx,base,state,call.At(values_offset+8),scope,*snapshot,restored,error)){REXLOG_ERROR("Native script checkpoint bridge restore: {}",error);return false;}
    const auto restored_offset=restored-PPC_LOAD_U32(state+48);
    if(!invoke(restored_offset,2,9) || !invoke(values_offset+16,0,17)){REXLOG_ERROR("Native script checkpoint bridge did not preserve independent restored upvalues");return false;}
    NativeScriptOwners baseline;
    if(!ReadNativeScriptOwners(base,state,baseline,error)){REXLOG_ERROR("Native script owner inventory: {}",error);return false;}
    auto* memory=rex::kernel::kernel_state()->memory();const auto scratch=memory->SystemHeapAlloc(96,16);
    if(!Readable(base,scratch,96))return false;std::memset(base+scratch,0,96);
    struct Owners {
        PPCContext& ctx;uint8_t* base;rex::memory::Memory* memory;uint32_t scratch;unsigned count=0;
        void Release(){for(unsigned i=count;i-->0;){ctx.r3.u64=scratch+i*24;__imp__sub_826D2668(ctx,base);}count=0;}
        ~Owners(){Release();memory->SystemHeapFree(scratch);}
    }owners{ctx,base,memory,scratch};
    const auto source=scratch+64;
    PPC_STORE_U32(source,state);PPC_STORE_U32(source+4,PPC_LOAD_U32(call.At(values_offset+16)));PPC_STORE_U32(source+8,9);
    for(unsigned i=0;i<2;++i){ctx.r3.u64=scratch+i*24;ctx.r4.u64=source;__imp__sub_826D25F0(ctx,base);++owners.count;}
    NativeScriptOwners expected;
    if(!ReadNativeScriptOwners(base,state,expected,error) || expected.owners.size()!=baseline.owners.size()+2)return false;
    const std::array<NativeScriptOwnerReplacement,2> replacements{{{scratch,call.At(restored_offset)},{scratch+24,call.At(restored_offset)}}};
    auto invalid=replacements;invalid[1].rooted_slot=0;
    NativeScriptOwners unchanged;
    if(RebindNativeScriptOwners(ctx,base,state,expected,invalid,error) || !ReadNativeScriptOwners(base,state,unchanged,error) || unchanged!=expected){REXLOG_ERROR("Native script batch accepted a missing root or partially rebound holders");return false;}
    if(!RebindNativeScriptOwners(ctx,base,state,expected,replacements,error)){REXLOG_ERROR("Native script owner rebind: {}",error);return false;}
    // Remove the restored stack root. Only the two original native LuaObject
    // holders now retain that closure while a separate closure forces full GC.
    PPC_STORE_U32(state+36,call.At(restored_offset));
    const auto gc_top=PPC_LOAD_U32(state+36)-PPC_LOAD_U32(state+48);
    if(!call.Copy(values_offset+24,9))return false;
    ctx.r3.u64=state;ctx.r4.u64=ctx.r5.u64=ctx.r6.u64=0;__imp__sub_826F6C20(ctx,base);
    if(ctx.r3.s32 || !call.Intact() || PPC_LOAD_U32(state+36)!=call.At(gc_top))return false;
    auto invoke_owner=[&](uint32_t owner,int increment,int expected_value){
        const auto offset=PPC_LOAD_U32(state+36)-PPC_LOAD_U32(state+48);
        if(!call.Push(PPC_LOAD_U32(owner+4),PPC_LOAD_U32(owner+8)))return false;
        const bool ok=invoke(offset,increment,expected_value);PPC_STORE_U32(state+36,call.At(offset));return ok;
    };
    if(!invoke_owner(scratch,1,10) || !invoke_owner(scratch+24,2,12) || !invoke(values_offset+16,0,17)){REXLOG_ERROR("Native rebound script holders lost GC roots/shared closure state");return false;}
    owners.Release();NativeScriptOwners after;
    if(!ReadNativeScriptOwners(base,state,after,error) || after!=baseline){REXLOG_ERROR("Native script owner fixture did not restore original owner list");return false;}
    REXLOG_INFO("Native script owner rebind passed: {} existing native holders inspected, two original LuaObject holders rebound to one restored closure, temporary stack root removed, full original GC, shared state 10 then 12, original remains 17, rejected batch leaves all holders unchanged; actual gameplay holders not rebound",baseline.owners.size());
    if(!TestNativeScriptScheduler(ctx,base,state,call.At(values_offset+32),call.At(values_offset+40),call.At(values_offset+24)))return false;
    NativeScriptScheduler scheduler;
    if(Readable(base,0x833DA7B8,4) && ReadNativeScriptScheduler(ctx,base,state,PPC_LOAD_U32(0x833DA7B8),scheduler,error))
        REXLOG_INFO("Native script scheduler actual inventory: {} registry-backed tasks; observation only, no live task rebound",scheduler.tasks.size());
    else REXLOG_INFO("Native script scheduler actual inventory unavailable: {}; no scheduler state accepted",error);
    REXLOG_INFO("Native script checkpoint bridge passed: original HKS capture/unpersist through protected native calls, {} bytes, independent upvalues, GC-rooted restored closure and rejected VM scope; trusted local graph only",snapshot->size());return true;
}
}
