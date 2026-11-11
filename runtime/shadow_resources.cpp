#include "shadow_resources.h"
#include "pc_settings.h"
#include "ppc_context.h"
#include "allocation_trace.h"
#include <rex/logging.h>
#include <rex/cvar.h>
#include "guest_read_batch.h"
#include <windows.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <immintrin.h>
#include <vector>
#include <type_traits>
#include <mutex>
#include <set>
#include <tuple>
#include <stdexcept>
#include <thread>

PPC_EXTERN_FUNC(__imp__sub_82546EA8);
PPC_EXTERN_FUNC(__imp__sub_82561130);
REXCVAR_DEFINE_BOOL(em2_trace_shadow_preparation,false,"Debug","Read-only native depth-shadow selection/enable/caller observations; does not change lights or casters");
namespace {
static_assert(std::is_trivially_copyable_v<em2::ShadowCreationTrace>);
thread_local em2::ShadowCreationTrace creation;
using CreationScope=em2::ShadowCreationAuditScope;
int CreationSlot(uint32_t caller) {
    switch(caller) {
    case 0x829BCFDC:return 0;
    case 0x829BD024:return 1;
    case 0x829BD06C:return 2;
    default:return -1;
    }
}
bool ShadowCreation(const PPCContext& ctx,uint8_t* base,int slot) {
    if(slot<0 || ctx.r30.u32<0x10000 || ctx.r30.u32>0xFFFFFB97 ||
       ctx.r5.u32!=ctx.r31.u32+192u+unsigned(slot)*16u ||
       !ctx.r3.u32 || ctx.r3.u32!=ctx.r4.u32 || ctx.r3.u32>65535 ||
       ctx.r6.u32 || ctx.r7.u32 || ctx.r8.u32!=3)return false;
    return PPC_LOAD_U32(ctx.r30.u32)==0x820FAB5C &&
        PPC_LOAD_U32(ctx.r5.u32)==9 && PPC_LOAD_U32(ctx.r5.u32+4)==3 &&
        !PPC_LOAD_U32(ctx.r5.u32+8);
}
void Create(PPCContext& ctx,uint8_t* base,unsigned selected,PPCFunc* factory,bool report) {
    const auto slot=CreationSlot(ctx.lr);
    const auto cached=ctx.r3.u32;
    const bool changed=selected && ShadowCreation(ctx,base,slot) && selected!=cached;
    if(changed)ctx.r3.u64=ctx.r4.u64=selected;
    factory(ctx,base);
    if(changed && report) {
        const auto csr=_mm_getcsr();
        REXLOG_INFO("Native shadow target {} creation: cached {} -> current menu {} pixels; native 2048 cap retained, target={:08X}",slot,cached,selected,ctx.r3.u32);
        _mm_setcsr(csr);
    }
}
}
void em2::ObserveNativeShadowPreparation(uint32_t caller,uint32_t owner,uint32_t selected,uint32_t camera_count,uint32_t cameras,uint8_t* base){
    if(!REXCVAR_GET(em2_trace_shadow_preparation))return;
    const auto error=GetLastError();const auto csr=_mm_getcsr();
    struct Restore {DWORD error;unsigned csr;~Restore(){SetLastError(error);_mm_setcsr(csr);}} restore{error,csr};
    auto readable=[&](uint32_t pointer,size_t size){return base && pointer>=4096 && uint64_t(pointer)+size<=0x100000000ull &&
        GuestReadBatch::Readable(PPC_RAW_ADDR(pointer),size);};
    if(!readable(owner,225) || PPC_LOAD_U32(owner)!=0x820FAB5C || !readable(selected,12))return;
    NativeShadowPreparation sample{caller,owner,PPC_LOAD_U8(owner+224)};
    if(caller==0x827A59D4){
        sample.camera_count=camera_count;sample.cameras=cameras;
        if(camera_count && camera_count<=64 && readable(cameras,size_t(camera_count)*4)){
            sample.first_camera=PPC_LOAD_U32(cameras);
            if(readable(sample.first_camera,4))sample.camera_type=PPC_LOAD_U32(sample.first_camera);
        }
    }
    for(unsigned i=0;i<3;++i)sample.selected[i]=PPC_LOAD_U32(selected+i*4);
    static std::mutex audit_mutex;static std::vector<NativeShadowPreparation> seen;
    {std::lock_guard lock(audit_mutex);
        if(seen.size()>=64 || std::find(seen.begin(),seen.end(),sample)!=seen.end())return;
        seen.push_back(sample);
    }
    REXLOG_INFO("Native depth-shadow preparation: caller={:08X} owner={:08X} enabled={} selected={:08X}/{:08X}/{:08X} camera_count={} first_camera={:08X} camera_type={:08X} cameras={:08X}; read-only selection, not allocated texture count",
        sample.caller,sample.owner,sample.enabled,sample.selected[0],sample.selected[1],sample.selected[2],sample.camera_count,sample.first_camera,sample.camera_type,sample.cameras);
}
em2::ShadowCreationAuditScope::ShadowCreationAuditScope(ShadowCreationTrace current):previous_(creation){creation=current;}
em2::ShadowCreationAuditScope::~ShadowCreationAuditScope(){creation=previous_;}
em2::ShadowCreationTrace em2::CurrentShadowCreation(){return creation;}
bool em2::ShadowCreationActive(){return creation.active;}
void em2::RunShadowCreationCrashProbe(bool physical) {
    CreationScope audit({true,0x12340000,1,1024,2048});
    RunAllocationCrashProbe(physical);
}
void em2::NoteShadowAllocationRequest(const AllocationTrace& allocation) {
    if(!creation.active || !allocation.active)return;
    ++creation.requests;creation.last_caller=allocation.caller;
    if(allocation.has_size)creation.requested_bytes+=allocation.requested;
    // Native 8272F5C8 sends kind13 directly to physical allocator8272F450.
    if(allocation.kind==13) {
        ++creation.physical_requests;
        if(allocation.has_size)creation.physical_requested_bytes+=allocation.requested;
    }
}
void em2::NoteShadowAllocationCompletion(const AllocationTrace& allocation,uint32_t result,uint32_t granted) {
    if(creation.active && allocation.active && allocation.kind==13 && result) {
        ++creation.physical_completed;creation.physical_granted_bytes+=granted;
    }
}
bool em2::ValidateShadowMetadataAllocation(uint32_t address,uint32_t bytes) {
    if(address>=4096 && uint64_t(address)+bytes<=0x100000000ull)return true;
    if(creation.active) {
        creation.failed_allocation_size=bytes;creation.failed_allocation_address=address;
        const auto csr=_mm_getcsr();
        REXLOG_WARN("Shadow metadata allocation rejected before native write: bytes={} address={:08X} owner={:08X} slot={}",bytes,address,creation.owner,creation.slot);
        _mm_setcsr(csr);
    }
    return false;
}
PPC_FUNC_IMPL(sub_82546EA8) {
    const auto slot=CreationSlot(ctx.lr);
    const unsigned player[]={128,256,512,1024,2048};
    const unsigned environment[]={512,1024,2048,4096};
    const auto selected=slot<0?0:slot==0?player[em2::PCSetting(4)]:environment[em2::PCSetting(5)];
    if(!ShadowCreation(ctx,base,slot)){Create(ctx,base,selected,__imp__sub_82546EA8,true);return;}
    CreationScope audit({true,ctx.r30.u32,unsigned(slot),ctx.r3.u32,selected});
    Create(ctx,base,selected,__imp__sub_82546EA8,true);
    const auto trace=creation;const auto csr=_mm_getcsr();
    REXLOG_INFO("Shadow creation audit: owner={:08X} slot={} cached={} requested={} native_cap=2048 target={:08X} root_requests={} root_requested_bytes={} physical_requests={} physical_completed={} physical_requested_bytes={} physical_granted_bytes={}",
        trace.owner,trace.slot,trace.cached_size,trace.requested_size,ctx.r3.u32,trace.requests,trace.requested_bytes,
        trace.physical_requests,trace.physical_completed,trace.physical_requested_bytes,trace.physical_granted_bytes);
    _mm_setcsr(csr);
}

PPC_FUNC_IMPL(sub_82561130) {
    // Observe only the native shadow preparation callsite, without changing its
    // arguments or callback order. A bounded identity set limits routine logs.
    const auto view=ctx.r3.u32,target=ctx.r4.u32,owner=ctx.r23.u32;
    if(ctx.lr==0x829BAE80 && view && owner && PPC_LOAD_U32(owner)==0x820FAB5C) {
        int slot=-1;for(int i=0;i<3;++i)if(PPC_LOAD_U32(owner+176+i*4)==target)slot=i;
        if(slot>=0) {
            const auto setter=PPC_LOAD_U32(view+56),getter=PPC_LOAD_U32(view+60),count=PPC_LOAD_U32(view+64);
            const auto csr=_mm_getcsr();
            static std::mutex mutex;static std::set<std::tuple<uint32_t,uint32_t,uint32_t,uint32_t,uint32_t>> seen;
            {std::lock_guard lock(mutex);
                const auto identity=std::tuple(owner,view,setter,getter,count);
                if(seen.size()<32 && seen.insert(identity).second)
                    REXLOG_INFO("Shadow binding audit: owner={:08X} view={:08X} target={:08X} slot={} setter={:08X} getter={:08X} count={:08X} flags={:04X}",
                        owner,view,target,slot,setter,getter,count,PPC_LOAD_U16(view+12));
            }
            _mm_setcsr(csr);
        }
    }
    __imp__sub_82561130(ctx,base);
}

bool em2::TestShadowResourceCreation() {
    const auto csr=_mm_getcsr();struct FP {unsigned saved;~FP(){_mm_setcsr(saved);}}fp{csr};
    auto check=[](bool ok,const char* description){if(!ok)REXLOG_ERROR("Shadow creation test: {}",description);return ok;};
    constexpr std::array<uint32_t,3> callers={0x829BCFDC,0x829BD024,0x829BD06C};
    unsigned checks=0;
    const auto original_creation=CurrentShadowCreation();
    {
        CreationScope outer({true,0x10000,2,1024,2048});
        AllocationTrace cpu{};cpu.active=true;cpu.has_size=true;cpu.requested=24;cpu.kind=0;
        AllocationTrace gpu=cpu;gpu.kind=13;gpu.requested=81920;gpu.caller=0x82654F48;
        NoteShadowAllocationRequest(cpu);NoteShadowAllocationRequest(gpu);
        NoteShadowAllocationCompletion(gpu,0xA0000000,86016);
        NoteShadowAllocationCompletion(gpu,0,86016);
        auto snapshot=CurrentShadowCreation();
        if(!check(snapshot.requests==2 && snapshot.requested_bytes==81944 && snapshot.physical_requests==1 &&
            snapshot.physical_completed==1 && snapshot.physical_requested_bytes==81920 && snapshot.physical_granted_bytes==86016 &&
            snapshot.last_caller==0x82654F48,"physical request/completion accounting excludes CPU metadata and failed allocations"))return false;
        try {CreationScope nested({true,0x20000,0,128,512});NoteShadowAllocationRequest(gpu);throw std::runtime_error("scope probe");}
        catch(const std::runtime_error&){}
        const auto restored=CurrentShadowCreation();
        if(!check(snapshot==restored,"nested creation unwinding restores outer crash snapshot"))return false;
        bool isolated=false;
        std::thread other([&] {
            const auto before=CurrentShadowCreation();
            {CreationScope scope({true,0x30000,1,512,1024});NoteShadowAllocationRequest(gpu);
                isolated=!before.active && CurrentShadowCreation().physical_requests==1;}
            isolated=isolated && CurrentShadowCreation()==before;
        });other.join();
        if(!check(isolated && CurrentShadowCreation()==snapshot,"allocation audit is isolated between native host threads"))return false;
        {CreationScope inactive({});NoteShadowAllocationRequest(gpu);
            if(!check(!CurrentShadowCreation().requests,"inactive creation does not collect allocations"))return false;}
    }
    if(!check(CurrentShadowCreation()==original_creation,"creation audit restores caller snapshot after completion"))return false;
    // Exercise both upgrades and downgrades across cached low/high choices.
    // Compare all registers, FP control and memory
    // against the native-factory delegate receiving only the new dimensions.
    for(unsigned slot=0;slot<3;++slot)for(unsigned cached: {128u,1024u,2048u,4096u})
    for(unsigned selected: {128u,256u,512u,1024u,2048u,4096u})for(unsigned guard=0;guard<14;++guard) {
        std::vector<uint8_t> expected_memory(0x11000,0xA5),actual_memory;
        auto* base=expected_memory.data();
        PPCContext input{};input.r1.u64=0xF00;input.r31.u64=0x400;input.r30.u64=0x10000;
        input.r5.u64=0x400+192+slot*16;input.r3.u64=input.r4.u64=cached;
        input.r8.u64=3;input.lr=callers[slot];input.fpscr.InitHost();
        PPC_STORE_U32(0x10000,0x820FAB5C);
        PPC_STORE_U32(input.r5.u32,9);PPC_STORE_U32(input.r5.u32+4,3);PPC_STORE_U32(input.r5.u32+8,0);
        switch(guard) {
        case 1:input.lr=0x829BCFD8;break;
        case 2:PPC_STORE_U32(0x10000,0x820FAB60);break;
        case 3:PPC_STORE_U32(input.r5.u32,8);break;
        case 4:PPC_STORE_U32(input.r5.u32+4,2);break;
        case 5:PPC_STORE_U32(input.r5.u32+8,1);break;
        case 6:input.r6.u64=1;break;
        case 7:input.r7.u64=1;break;
        case 8:input.r8.u64=2;break;
        case 9:input.r4.u64=512;break;
        case 10:input.r30.u64=0;break;
        case 11:input.r5.u64+=4;break;
        case 12:input.r3.u64=input.r4.u64=0;break;
        case 13:input.r30.u64=0xFFFFFFFC;break;
        }
        actual_memory=expected_memory;
        auto native=+[](PPCContext& ctx,uint8_t* base) {
            PPC_STORE_U32(0x100,ctx.r3.u32);PPC_STORE_U32(0x104,ctx.r4.u32);
            ctx.r3.u64=0x12345678;ctx.r9.u64=0xABCDEF;ctx.cr6.eq=1;
            _mm_setcsr(_mm_getcsr()^0x8000);
        };
        auto expected=input;if(!guard)expected.r3.u64=expected.r4.u64=selected;
        const auto before=_mm_getcsr();native(expected,base);const auto after=_mm_getcsr();
        _mm_setcsr(before);auto actual=input;Create(actual,actual_memory.data(),selected,native,false);
        if(!check(!std::memcmp(&expected,&actual,sizeof(actual)) && expected_memory==actual_memory && after==_mm_getcsr(),
            "current choice reaches all three constructor calls; unrelated callers, formats, modes and owners retain exact native context/memory/FP behavior"))return false;
        ++checks;
    }
    REXLOG_INFO("Shadow resource creation passed {} context/memory/FP cases: current menu overrides stale cached INI dimensions only at guarded shadow-constructor calls; native resource allocation and live replacement remain separate checks",checks);
    return TestShadowMetadataAllocationFailure();
}
