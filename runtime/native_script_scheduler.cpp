#include "native_script_scheduler.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/kernel/kernel_state.h>
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <unordered_set>
#include <cstring>
#include <bit>
#include <stdexcept>
PPC_EXTERN_FUNC(__imp__sub_826DEF58); // Nonallocating raw integer table lookup.
PPC_EXTERN_FUNC(__imp__sub_8272C3C8); // Raw integer table assignment, native GC barrier.
PPC_EXTERN_FUNC(__imp__sub_82719758); // Original registry reference allocation.
PPC_EXTERN_FUNC(__imp__sub_827197E8); // Original registry unref/free-list maintenance.
PPC_EXTERN_FUNC(__imp__sub_82706B80); // Original compiler-settings initialization.
PPC_EXTERN_FUNC(__imp__sub_82732770); // Original scheduled coroutine resume.
PPC_EXTERN_FUNC(__imp__sub_82730810); // Original yielded wait extraction.
PPC_EXTERN_FUNC(__imp__sub_826F6C20);
namespace em2 {namespace {
bool Readable(uint8_t* base,uint32_t at,size_t size){return base && at>=4096 && size && uint64_t(at)+size<=0x100000000ull && GuestReadBatch::Readable(base+at,size);}
void Need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
uint32_t Word(const uint8_t* p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
struct Calls {
    PPCContext& ctx;PPCContext saved;uint8_t* base;unsigned csr=_mm_getcsr();DWORD error=GetLastError();uint32_t low=0;
    std::array<uint8_t,32768> bytes{};
    Calls(PPCContext& c,uint8_t* b):ctx(c),saved(c),base(b){Need(c.r1.u32>=65536 && Readable(base,c.r1.u32-32768,32768),"Native scheduler requires initialized owner stack");low=c.r1.u32-32768;std::memcpy(bytes.data(),base+low,bytes.size());}
    ~Calls(){std::memcpy(base+low,bytes.data(),bytes.size());ctx=saved;_mm_setcsr(csr);SetLastError(error);}
    void Reset(){ctx=saved;ctx.r1.u64=low+16384;}
    uint64_t Get(uint32_t state,uint32_t global,uint32_t ref){Reset();ctx.r3.u64=state;ctx.r4.u64=global+228;ctx.r5.u64=ref;__imp__sub_826DEF58(ctx,base);return ctx.r3.u64;}
};
uint32_t Global(uint8_t* base,uint32_t state){Need(Readable(base,state,128),"Native scheduler VM unavailable");const auto global=PPC_LOAD_U32(state+8);Need(Readable(base,global,236) && (PPC_LOAD_U32(global+232)&15)==5,"Native scheduler registry unavailable");return global;}
void Thread(uint8_t* base,uint32_t state,uint32_t global){Need(Readable(base,state,128) && PPC_LOAD_U32(state+8)==global,"Scheduled coroutine belongs to another VM");}
}
bool ReadNativeScriptScheduler(PPCContext& ctx,uint8_t* base,uint32_t state,uint32_t manager,NativeScriptScheduler& out,std::string& error)try{
    error.clear();Calls calls(ctx,base);GuestReadBatch inspection;
    NativeScriptScheduler sample;sample.manager=manager;sample.global=Global(base,state);sample.registry=PPC_LOAD_U32(sample.global+228);sample.thread=GetCurrentThreadId();
    Need(Readable(base,manager,88) && !PPC_LOAD_U32(manager+84),"Native scheduler is unavailable or inside coroutine execution");
    sample.array=PPC_LOAD_U32(manager+72);const auto count=PPC_LOAD_U32(manager+76);sample.capacity=PPC_LOAD_U32(manager+80);
    Need(count<=4096 && count<=sample.capacity && sample.capacity<=65536 && (!count || Readable(base,sample.array,size_t(count)*132)),"Native scheduler task bounds invalid");
    std::unordered_set<uint32_t> references;sample.tasks.reserve(count);
    for(unsigned i=0;i<count;++i){
        NativeScheduledScript task;task.address=sample.array+i*132;task.reference=PPC_LOAD_U32(task.address);task.coroutine=PPC_LOAD_U32(task.address+4);
        Need(task.reference>1 && task.reference<=0x7fffffff && references.insert(task.reference).second,"Native scheduler registry identity is invalid or duplicated");Thread(base,task.coroutine,sample.global);
        Need(task.coroutine!=state,"Native scheduler inspection cannot include its running caller");
        const auto value=calls.Get(state,sample.global,task.reference);
        Need((uint32_t(value)&15)==8 && uint32_t(value>>32)==task.coroutine,"Native scheduler coroutine and registry reference disagree");
        std::memcpy(task.bytes.data(),base+task.address,task.bytes.size());sample.tasks.push_back(task);
    }
    Need(Global(base,state)==sample.global && PPC_LOAD_U32(sample.global+228)==sample.registry && PPC_LOAD_U32(manager+72)==sample.array &&
        PPC_LOAD_U32(manager+76)==count && PPC_LOAD_U32(manager+80)==sample.capacity && !PPC_LOAD_U32(manager+84),"Native scheduler changed during inspection");
    out=std::move(sample);return true;
}catch(const std::exception& ex){error=ex.what();return false;}
bool RebindNativeScheduledScripts(PPCContext& ctx,uint8_t* base,uint32_t state,const NativeScriptScheduler& expected,
    std::span<const NativeScheduledScriptReplacement> replacements,std::string& error)try{
    error.clear();NativeScriptScheduler current;
    Need(expected.thread==GetCurrentThreadId() && ReadNativeScriptScheduler(ctx,base,state,expected.manager,current,error) && current==expected,"Native scheduler changed or belongs to another owner/VM");
    Need(!replacements.empty() && replacements.size()<=current.tasks.size(),"Native scheduler replacement batch bounds invalid");
    Calls calls(ctx,base);const auto storage=PPC_LOAD_U32(state+48),bottom=PPC_LOAD_U32(state+40),top=PPC_LOAD_U32(state+36);
    Need(storage<=bottom && bottom<=top && uint64_t(top)+8<=PPC_LOAD_U32(state+44) && Readable(base,top,8),"Native scheduler root stack unavailable");
    struct Prepared {unsigned index;uint32_t value,type;std::array<uint8_t,12> scheduling;};std::vector<Prepared> prepared;prepared.reserve(replacements.size());std::unordered_set<unsigned> selected;
    for(const auto& replacement:replacements){
        Need(replacement.task<current.tasks.size() && selected.insert(replacement.task).second,"Scheduled task replacement missing or duplicated");
        const auto slot=replacement.rooted_thread_slot;
        Need(slot>=bottom && (slot-storage)%8==0 && uint64_t(slot)+8<=top && Readable(base,slot,8) && (PPC_LOAD_U32(slot+4)&15)==8,"Scheduled replacement is not a rooted coroutine");
        const auto value=PPC_LOAD_U32(slot);Thread(base,value,current.global);
        Need(value!=state,"Cannot replace a scheduled task with its running caller");
        Need((Word(replacement.scheduling.data())&0x7f800000)!=0x7f800000 && replacement.scheduling[4]<=1 && replacement.scheduling[5]<=1 && Word(replacement.scheduling.data()+8)<=65536,"Invalid restored scheduling state");
        prepared.push_back({replacement.task,value,PPC_LOAD_U32(slot+4),replacement.scheduling});
    }
    // Existing registry keys are updated through native rawseti, retaining the
    // VM's GC barrier. No arbitrary network pointers or native names are copied.
    struct Pop {uint8_t* base;uint32_t state,offset;~Pop(){PPC_STORE_U32(state+36,PPC_LOAD_U32(state+48)+offset);}}pop{base,state,top-storage};
    for(const auto& item:prepared){
        const auto at=PPC_LOAD_U32(state+36);PPC_STORE_U32(at,item.value);PPC_STORE_U32(at+4,item.type);PPC_STORE_U32(state+36,at+8);
        calls.Reset();ctx.r3.u64=state;ctx.r4.s64=-10000;ctx.r5.u64=current.tasks[item.index].reference;__imp__sub_8272C3C8(ctx,base);
        Need(PPC_LOAD_U32(state+36)==top,"Native scheduler registry assignment did not balance its caller stack");
        PPC_STORE_U32(current.tasks[item.index].address+4,item.value);
        std::memcpy(base+current.tasks[item.index].address+96,item.scheduling.data(),12);
    }
    return true;
}catch(const std::exception& ex){error=ex.what();return false;}
bool TestNativeScriptScheduler(PPCContext& ctx,uint8_t* base,uint32_t state,uint32_t original_slot,uint32_t restored_slot,uint32_t gc_slot){
    try{
        Calls calls(ctx,base);const auto global=Global(base,state);Need((PPC_LOAD_U32(original_slot+4)&15)==8 && (PPC_LOAD_U32(restored_slot+4)&15)==8,"Native scheduler fixture threads missing");
        auto* memory=rex::kernel::kernel_state()->memory();const auto scratch=memory->SystemHeapAlloc(256,16);Need(Readable(base,scratch,256),"Native scheduler fixture allocation failed");
        struct Free {rex::memory::Memory* memory;uint32_t at;~Free(){memory->SystemHeapFree(at);}}free{memory,scratch};std::memset(base+scratch,0,256);
        const auto manager=scratch,task=scratch+96,initial_top=PPC_LOAD_U32(state+36),initial_offset=initial_top-PPC_LOAD_U32(state+48);
        PPC_STORE_U32(initial_top,PPC_LOAD_U32(original_slot));PPC_STORE_U32(initial_top+4,8);PPC_STORE_U32(state+36,initial_top+8);
        calls.Reset();ctx.r3.u64=state;ctx.r4.s64=-10000;__imp__sub_82719758(ctx,base);const auto reference=ctx.r3.u32;
        struct Unref {Calls& calls;uint32_t state,ref;bool released=false;~Unref(){if(released)return;calls.Reset();calls.ctx.r3.u64=state;calls.ctx.r4.s64=-10000;calls.ctx.r5.u64=ref;__imp__sub_827197E8(calls.ctx,calls.base);}}unref{calls,state,reference};
        Need(reference>1 && PPC_LOAD_U32(state+36)==PPC_LOAD_U32(state+48)+initial_offset,"Original scheduler reference allocation failed");
        PPC_STORE_U32(manager+72,task);PPC_STORE_U32(manager+76,1);PPC_STORE_U32(manager+80,1);PPC_STORE_U32(task,reference);PPC_STORE_U32(task+4,PPC_LOAD_U32(original_slot));
        calls.Reset();ctx.r3.u64=task+8;__imp__sub_82706B80(ctx,base);PPC_STORE_U32(task+96,std::bit_cast<uint32_t>(7.f));PPC_STORE_U8(task+101,1);
        std::array<uint8_t,12> scheduling{};std::memcpy(scheduling.data(),base+task+96,12);PPC_STORE_U32(task+96,0);
        NativeScriptScheduler expected;std::string error;if(!ReadNativeScriptScheduler(ctx,base,state,manager,expected,error))throw std::runtime_error(error);
        NativeScheduledScriptReplacement replacement{0,restored_slot,scheduling};auto invalid=replacement;invalid.rooted_thread_slot=0;
        Need(!RebindNativeScheduledScripts(ctx,base,state,expected,std::span(&invalid,1),error) && !std::memcmp(base+task,expected.tasks[0].bytes.data(),132),"Unrooted scheduler replacement changed the task");
        if(!RebindNativeScheduledScripts(ctx,base,state,expected,std::span(&replacement,1),error))throw std::runtime_error(error);
        const auto restored=PPC_LOAD_U32(task+4);Need(restored==PPC_LOAD_U32(restored_slot) && Word(base+task+96)==std::bit_cast<uint32_t>(7.f) && uint32_t(calls.Get(state,global,reference)>>32)==restored,"Native scheduler registry/timer rebind mismatch");
        // Remove the restored coroutine's stack root, retaining its only native
        // registry reference; collect using the separate fixture closure.
        const auto end_offset=restored_slot-PPC_LOAD_U32(state+48);PPC_STORE_U32(state+36,restored_slot);
        const auto at=PPC_LOAD_U32(state+36);PPC_STORE_U32(at,PPC_LOAD_U32(gc_slot));PPC_STORE_U32(at+4,9);PPC_STORE_U32(state+36,at+8);
        calls.Reset();ctx.r3.u64=state;ctx.r4.u64=ctx.r5.u64=ctx.r6.u64=0;__imp__sub_826F6C20(ctx,base);Need(!ctx.r3.s32 && PPC_LOAD_U32(state+36)==PPC_LOAD_U32(state+48)+end_offset,"Scheduler registry-root GC fixture failed");
        calls.Reset();ctx.r3.u64=task;ctx.r4.u64=0;__imp__sub_82732770(ctx,base);
        if(!PPC_LOAD_U32(task+4))unref.released=true; // Original completion already unreferenced it.
        calls.Reset();ctx.r3.u64=task;ctx.r4.u64=task+96;__imp__sub_82730810(ctx,base);
        Need(PPC_LOAD_U32(task+4)==restored && PPC_LOAD_U32(task+96)==std::bit_cast<uint32_t>(17.f),"Restored scheduled coroutine did not resume from the captured yield");
        REXLOG_INFO("Native scheduler binding passed: original registry ref/rawseti/GC barrier, restored thread survives full GC, captured wait 7 restored, original scheduler resume yields 17, invalid source rejected before writes; allocated task record, no live scheduler replaced");return true;
    }catch(const std::exception& ex){REXLOG_ERROR("Native scheduler binding fixture failed: {}",ex.what());return false;}
}
}
