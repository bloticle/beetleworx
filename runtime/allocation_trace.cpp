#include "allocation_trace.h"
#include "shadow_resources.h"
#include "ppc_context.h"
#include "guest_read_batch.h"
#include <rex/logging.h>
#include <array>
#include <cstring>
#include <immintrin.h>
#include <stdexcept>
#include <thread>
#include <windows.h>
#include <rex/cvar.h>
#include <memory>
#include <new>
#include <cstdio>
#include <algorithm>
#include <vector>

REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DEFINE_BOOL(em2_trace_native_heap_history,false,"Debug","Private diagnostic only: retain bounded native allocation/free request history for teardown crashes");
PPC_EXTERN_FUNC(__imp__sub_8272EDC0);
PPC_EXTERN_FUNC(__imp__sub_8272F5C8);
namespace {
struct HeapEvent {uint64_t sequence=0;uint32_t pointer=0,size=0,kind=0,caller=0,thread=0;bool allocation=false;std::array<uint32_t,3> parents{};};
template<size_t Capacity> struct HeapHistory {
    std::array<HeapEvent,Capacity> events{};uint64_t next=0,allocations=0,frees=0;
    void Add(HeapEvent event){if(next==UINT64_MAX)return;event.sequence=++next;if(event.allocation)++allocations;else ++frees;events[(next-1)%Capacity]=event;}
    template<class Visit> void Find(uint32_t pointer,Visit visit)const{
        for(uint64_t n=0;n<std::min<uint64_t>(next,Capacity);++n){const auto& event=events[(next-1-n)%Capacity];
            const auto distance=event.pointer>pointer?uint64_t(event.pointer)-pointer:uint64_t(pointer)-event.pointer;
            if(distance<=16 || (event.allocation && event.pointer<=pointer && uint64_t(pointer)<uint64_t(event.pointer)+event.size))
                if(!visit(event))break;
        }
    }
};
constexpr size_t heap_capacity=131072;
SRWLOCK heap_lock=SRWLOCK_INIT;
std::unique_ptr<HeapHistory<heap_capacity>> heap_history;
thread_local em2::NativeFreeTrace current_free;
bool HeapHistoryEnabled(){return REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_trace_native_heap_history);}
std::array<uint32_t,3> HeapParents(const PPCContext& ctx,uint8_t* base){
    const auto error=GetLastError();const auto fp=_mm_getcsr();
    struct Restore {DWORD error;unsigned fp;~Restore(){SetLastError(error);_mm_setcsr(fp);}} restore{error,fp};
    em2::GuestReadBatch reads;std::array<uint32_t,3> result{};auto stack=ctx.r1.u32;
    for(auto& pc:result){
        if(!base || stack<4096 || stack>UINT32_MAX-4 || !em2::GuestReadBatch::Readable(PPC_RAW_ADDR(stack),4))break;
        const auto parent=PPC_LOAD_U32(stack);
        if(parent<=stack || parent-stack>1024*1024 || (parent&15) || !em2::GuestReadBatch::Readable(PPC_RAW_ADDR(parent-8),4))break;
        const auto candidate=PPC_LOAD_U32(parent-8);
        if(candidate<0x822f0000 || candidate>=0x83346908 || (candidate&3))break;
        pc=candidate;stack=parent;
    }
    return result;
}
void RecordHeapEvent(HeapEvent event){
    const auto error=GetLastError();const auto fp=_mm_getcsr();
    AcquireSRWLockExclusive(&heap_lock);
    if(!heap_history)heap_history.reset(new(std::nothrow) HeapHistory<heap_capacity>);
    if(heap_history){event.thread=GetCurrentThreadId();heap_history->Add(event);}
    ReleaseSRWLockExclusive(&heap_lock);_mm_setcsr(fp);SetLastError(error);
}
thread_local em2::AllocationTrace current;
thread_local std::array<char,64> load_level{};
thread_local uint64_t load_generation=0;
template<class Allocate> void Track(PPCContext& ctx,uint8_t* base,Allocate allocate) {
    const auto previous=current;
    current={};current.active=true;current.guest_base=reinterpret_cast<uintptr_t>(base);
    current.guest_context=reinterpret_cast<uintptr_t>(&ctx);
    current.heap=ctx.r3.u32;current.size_pointer=ctx.r4.u32;current.alignment_pointer=ctx.r5.u32;
    current.flags=ctx.r6.u32;current.kind=ctx.r7.u32;current.bucket=ctx.r8.u32;current.caller=uint32_t(ctx.lr);
    current.depth=previous.depth+1;
    struct Restore {em2::AllocationTrace previous;~Restore(){current=previous;}} restore{previous};
    if(current.size_pointer){current.requested=PPC_LOAD_U32(current.size_pointer);current.has_size=true;}
    if(current.alignment_pointer){current.alignment=PPC_LOAD_U32(current.alignment_pointer);current.has_alignment=true;}
    em2::NoteShadowAllocationRequest(current);
    const bool heap_trace=HeapHistoryEnabled();
    const auto parents=heap_trace?HeapParents(ctx,base):std::array<uint32_t,3>{};
    allocate(ctx,base);
    if(heap_trace && ctx.r3.u32 && current.has_size)
        RecordHeapEvent({0,ctx.r3.u32,PPC_LOAD_U32(current.size_pointer),current.kind,current.caller,0,true,parents});
    if(em2::ShadowCreationActive() && current.kind==13 && ctx.r3.u32 && current.has_size)
        em2::NoteShadowAllocationCompletion(current,ctx.r3.u32,PPC_LOAD_U32(current.size_pointer));
}
}
em2::AllocationTrace em2::CurrentNativeAllocation(){return current;}
em2::NativeFreeTrace em2::CurrentNativeFree(){return current_free;}
size_t em2::WriteNativeHeapHistory(char* out,size_t bytes,uint32_t pointer){
    if(!out || bytes<128 || !TryAcquireSRWLockShared(&heap_lock))return 0;
    size_t used=0;unsigned matches=0;
    if(heap_history){
        const auto append=[&](const char* text){const auto n=std::strlen(text);if(n>=bytes-used)return false;std::memcpy(out+used,text,n);used+=n;out[used]=0;return true;};
        char line[192];std::snprintf(line,sizeof(line),"native_heap_history target=%08X recorded=%llu capacity=%zu allocations=%llu frees=%llu newest-first requests\n",pointer,static_cast<unsigned long long>(heap_history->next),heap_capacity,static_cast<unsigned long long>(heap_history->allocations),static_cast<unsigned long long>(heap_history->frees));append(line);
        heap_history->Find(pointer,[&](const HeapEvent& event){
            std::snprintf(line,sizeof(line),"heap_event %c seq=%llu pointer=%08X size=%u kind=%u caller=%08X thread=%u parents=%08X/%08X/%08X\n",event.allocation?'A':'F',static_cast<unsigned long long>(event.sequence),event.pointer,event.size,event.kind,event.caller,event.thread,event.parents[0],event.parents[1],event.parents[2]);
            return append(line) && ++matches<32;
        });
    }
    ReleaseSRWLockShared(&heap_lock);return used;
}
bool em2::TestNativeHeapHistory(){
    HeapHistory<4> history;
    history.Add({0,0x1000,64,1,0x10,0,true});history.Add({0,0x1000,0,3,0x20,0,false});
    history.Add({0,0x1000,56,1,0x30,0,true});history.Add({0,0x9000,8,1,0x40,0,true});
    std::vector<HeapEvent> matches;history.Find(0x1008,[&](const auto& event){matches.push_back(event);return true;});
    if(matches.size()!=3 || matches[0].caller!=0x30 || matches[1].caller!=0x20 || matches[2].caller!=0x10)return false;
    history.Add({0,0xfffffff0,64,1,0x50,0,true});matches.clear();history.Find(0x1008,[&](const auto& e){matches.push_back(e);return true;});
    if(matches.size()!=2 || matches.back().caller!=0x20)return false;
    matches.clear();history.Find(0xfffffffc,[&](const auto& e){matches.push_back(e);return false;});
    if(matches.size()!=1 || matches[0].caller!=0x50)return false;
    std::array<uint8_t,16384> memory{};auto* base=memory.data();PPCContext ctx{};ctx.r1.u64=0x1000;
    PPC_STORE_U32(0x1000,0x1100);PPC_STORE_U32(0x10f8,0x82753284);
    PPC_STORE_U32(0x1100,0x1200);PPC_STORE_U32(0x11f8,0x829e6828);
    PPC_STORE_U32(0x1200,0x1300);PPC_STORE_U32(0x12f8,0x829ed310);
    const auto saved=ctx;const auto before=memory;const auto error=GetLastError();const auto fp=_mm_getcsr();
    if(HeapParents(ctx,base)!=std::array<uint32_t,3>{0x82753284,0x829e6828,0x829ed310} ||
       std::memcmp(&ctx,&saved,sizeof(ctx)) || memory!=before || GetLastError()!=error || _mm_getcsr()!=fp)return false;
    PPC_STORE_U32(0x1100,0x1000);
    if(HeapParents(ctx,base)!=std::array<uint32_t,3>{0x82753284,0,0})return false;
    ctx.r1.u64=0xfffffffc;
    if(HeapParents(ctx,base)!=std::array<uint32_t,3>{})return false;
    REXLOG_INFO("Native heap history passed bounded wrap, interior/array-prefix matching, allocation/free ordering, 64-bit range bounds, early-stop and bounded native parent-stack/preservation checks; diagnostic only, no native free intercepted by fixture");return true;
}
PPC_FUNC_IMPL(sub_8272EDC0){
    if(!HeapHistoryEnabled()){__imp__sub_8272EDC0(ctx,base);return;}
    const auto previous=current_free;
    struct Restore {em2::NativeFreeTrace saved;~Restore(){current_free=saved;}} restore{previous};
    current_free={true,ctx.r4.u32,ctx.r5.u32,uint32_t(ctx.lr)};
    RecordHeapEvent({0,ctx.r4.u32,ctx.r6.u32,ctx.r5.u32,uint32_t(ctx.lr),0,false,HeapParents(ctx,base)});
    __imp__sub_8272EDC0(ctx,base);
}

void em2::RecordNativeLoadLevel(std::string_view level) {
    if(load_generation!=UINT64_MAX)++load_generation;
    load_level.fill(0);const auto count=level.size()<load_level.size()-1?level.size():load_level.size()-1;
    if(count)std::memcpy(load_level.data(),level.data(),count);
}
std::array<char,64> em2::CurrentNativeLoadLevel(){return load_level;}
uint64_t em2::CurrentNativeLoadGeneration(){return load_generation;}
PPC_FUNC_IMPL(sub_8272F5C8){Track(ctx,base,__imp__sub_8272F5C8);}
bool em2::TestAllocationTrace() {
    const auto entry_csr=_mm_getcsr();struct RestoreFP {unsigned csr;~RestoreFP(){_mm_setcsr(csr);}} restore_fp{entry_csr};
    auto check=[](bool ok,const char* text){if(!ok)REXLOG_ERROR("Allocation trace test: {}",text);return ok;};
    const auto old_level=load_level;
    struct RestoreLevel {std::array<char,64> level;uint64_t generation;~RestoreLevel(){load_level=level;load_generation=generation;}} restore_level{old_level,load_generation};
    load_generation=0;
    RecordNativeLoadLevel("meanstreet_north.visit_1");
    if(!check(std::strcmp(CurrentNativeLoadLevel().data(),"meanstreet_north.visit_1")==0,"thread-local loading level is captured without guest access"))return false;
    const auto first_load=CurrentNativeLoadGeneration();
    RecordNativeLoadLevel("meanstreet_north.visit_1");
    if(!check(first_load==1 && CurrentNativeLoadGeneration()==2,
        "same-destination reload receives a fresh native owner-thread incarnation"))return false;
    uint64_t foreign_generation=UINT64_MAX;
    std::thread foreign([&]{foreign_generation=CurrentNativeLoadGeneration();RecordNativeLoadLevel("foreign-world");});foreign.join();
    if(!check(foreign_generation==0 && CurrentNativeLoadGeneration()==2 &&
        std::strcmp(CurrentNativeLoadLevel().data(),"meanstreet_north.visit_1")==0,
        "foreign native thread cannot replace the retained world incarnation"))return false;
    load_generation=UINT64_MAX;RecordNativeLoadLevel("exhausted-world");
    if(!check(CurrentNativeLoadGeneration()==UINT64_MAX,"load incarnation exhaustion never wraps into an old lease"))return false;
    load_generation=2;
    RecordNativeLoadLevel(std::string_view("abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrst",72));
    if(!check(CurrentNativeLoadLevel().back()==0 && std::strlen(CurrentNativeLoadLevel().data())==63,"long level context is safely bounded"))return false;
    alignas(16) std::array<uint8_t,4096> memory{};auto* base=memory.data();
    PPC_STORE_U32(128,131073);PPC_STORE_U32(132,256);PPC_STORE_U32(136,32);PPC_STORE_U32(140,16);
    PPCContext initial{};initial.r3.u64=2048;initial.r4.u64=128;initial.r5.u64=132;initial.r6.u64=0x400;
    initial.r7.u64=1;initial.r8.u64=3;initial.lr=0x82655144;initial.fpscr.InitHost();
    const auto original_csr=_mm_getcsr();
    const auto old=current;
    struct RestoreTrace {AllocationTrace trace;~RestoreTrace(){current=trace;}} restore_trace{old};current={};
    bool captured=false;
    auto native=[](PPCContext& ctx,uint8_t*){ctx.r3.u64=0x12345000;ctx.r12.u64=99;ctx.lr=0x11223344;_mm_setcsr(_mm_getcsr()^0x8000);};
    auto expected=initial;native(expected,base);const auto expected_csr=_mm_getcsr();_mm_setcsr(original_csr);
    auto actual=initial;
    Track(actual,base,[&](PPCContext& ctx,uint8_t* memory_base){
        const auto record=CurrentNativeAllocation();captured=record.active && record.guest_base==reinterpret_cast<uintptr_t>(base) && record.guest_context==reinterpret_cast<uintptr_t>(&ctx) &&
            record.heap==2048 && record.requested==131073 && record.alignment==256 && record.flags==0x400 &&
            record.kind==1 && record.bucket==3 && record.caller==0x82655144 && record.depth==1 && record.has_size && record.has_alignment;
        native(ctx,memory_base);
    });
    if(!check(captured && !CurrentNativeAllocation().active && !std::memcmp(&actual,&expected,sizeof(actual)) && _mm_getcsr()==expected_csr,
        "request capture preserves every native guest/FP result and clears after return"))return false;
    bool nested=false,outer_restored=false;actual=initial;
    Track(actual,base,[&](PPCContext& outer,uint8_t* memory_base){
        auto inner=outer;inner.r4.u64=136;inner.r5.u64=140;
        Track(inner,memory_base,[&](PPCContext&,uint8_t*){auto trace=CurrentNativeAllocation();nested=trace.depth==2 && trace.requested==32 && trace.alignment==16;});
        auto trace=CurrentNativeAllocation();outer_restored=trace.depth==1 && trace.requested==131073;
    });
    if(!check(nested && outer_restored && !CurrentNativeAllocation().active,"nested native allocation restores the enclosing request"))return false;
    try{Track(actual,base,[](PPCContext&,uint8_t*){throw std::runtime_error("fixture");});}catch(const std::runtime_error&){}
    if(!check(!CurrentNativeAllocation().active,"C++ exceptions do not leave a stale request"))return false;
    REXLOG_INFO("Allocation trace tests passed: endian request metadata, nested lifetime and exact guest/FP forwarding");return true;
}
void em2::RunAllocationCrashProbe(bool physical) {
    if(physical) {
        auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));
        if(!base || !VirtualAlloc(base,0x10000,MEM_COMMIT,PAGE_READWRITE) ||
           !VirtualAlloc(base+0x83390000,0x10000,MEM_COMMIT,PAGE_READWRITE) ||
           !VirtualAlloc(base+0x833D0000,0x10000,MEM_COMMIT,PAGE_READWRITE))throw std::runtime_error("Physical crash probe memory unavailable");
        constexpr uint32_t root=0x833D2760,arena=root+32128,mib=1024*1024;
        PPC_STORE_U32(0x2000,12*mib);PPC_STORE_U32(0x2004,0xBD000000);
        PPC_STORE_U32(0x8339A138,384*mib);PPC_STORE_U32(root+32156,369*mib);PPC_STORE_U32(root+32160,0xA0000000);
        PPC_STORE_U32(root+32200,15*mib);PPC_STORE_U32(root+32204,0xB0000000);
        PPC_STORE_U32(arena,0x821E3A70);PPC_STORE_U32(arena+8,0x5000);PPC_STORE_U32(arena+12,350*mib);PPC_STORE_U32(arena+20,369*mib);
        auto block=[&](uint32_t node,uint32_t offset,uint32_t bytes,bool available,uint32_t previous,uint32_t next) {
            PPC_STORE_U32(node,offset);PPC_STORE_U32(node+4,bytes);PPC_STORE_U8(node+11,available);PPC_STORE_U32(node+16,previous);PPC_STORE_U32(node+20,next);
        };
        block(0x5000,0,10*mib,true,0x5080,0x5040);block(0x5040,10*mib,350*mib,false,0x5000,0x5080);block(0x5080,360*mib,9*mib,true,0x5040,0x5000);
        DWORD old=0;if(!VirtualProtect(base,4096,PAGE_NOACCESS,&old))throw std::runtime_error("Physical crash probe guard unavailable");
        PPCContext ctx{};ctx.r1.u64=0xF000;ctx.r3.u64=root;ctx.r4.u64=0x2000;ctx.r5.u64=0x2004;ctx.r6.u64=0x40020000;ctx.r7.u64=13;ctx.lr=0x82654F48;
        RecordNativeLoadLevel("physical-arena-crash-probe");
        Track(ctx,base,[](PPCContext& native,uint8_t* base){native.r3.u64=0;native.r31.u64=4096;native.r10.u64=0x8206C514;native.lr=0x8272F528;native.ctr.u64=0x830FD0B0;PPC_STORE_U32(0,0x8206C514);});
        throw std::runtime_error("Physical crash probe did not fault");
    }
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,65536,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!base)throw std::runtime_error("Crash probe guest memory unavailable");
    PPC_STORE_U32(0x2000,1048599);PPC_STORE_U32(0x2004,256);
    PPC_STORE_U32(0x3000,0x1FFFF000);PPC_STORE_U32(0x3000+3028,0x1FFFF100);PPC_STORE_U8(0x3000+3045,1);
    PPC_STORE_U32(0x3000+2996,0x4000);PPC_STORE_U32(0x3000+3004,0x5000);
    PPC_STORE_U32(0x3000+3008,0xA1000000);PPC_STORE_U32(0x3000+3012,0xA1001000);
    PPC_STORE_U32(0x3000+3016,0x3000000);PPC_STORE_U32(0x3000+3020,0x2FFF000);
    std::memcpy(PPC_RAW_ADDR(0x4000),"probe-pool",11);
    DWORD old=0;if(!VirtualProtect(base,4096,PAGE_NOACCESS,&old))throw std::runtime_error("Crash probe guard unavailable");
    PPCContext ctx{};ctx.r3.u64=0x2800;ctx.r4.u64=0x2000;ctx.r5.u64=0x2004;
    ctx.r6.u64=0x400;ctx.r7.u64=1;ctx.r8.u64=3;ctx.lr=0x82655144;ctx.r1.u64=0xF000;
    RecordNativeLoadLevel("allocator-crash-probe");
    Track(ctx,base,[](PPCContext& native,uint8_t* base){
        native.r31.u64=0x3000;native.r10.u64=0x8206C56C;
        PPC_STORE_U32(0,0x8206C56C);
    });
    throw std::runtime_error("Crash probe did not fault");
}
