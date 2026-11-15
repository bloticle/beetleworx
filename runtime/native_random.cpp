#include "native_random.h"
#include "guest_read_batch.h"
#include "online_behavior_trace.h"
#include "simulation.h"
#include "online.h"
#include "online_runtime.h"
#include <vector>
#include <bit>
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <array>
#include <cstring>
#include <immintrin.h>
#include <windows.h>

PPC_EXTERN_FUNC(__imp__sub_82B6DDC8);
PPC_EXTERN_FUNC(__imp__KeTlsGetValue);
PPC_EXTERN_FUNC(__imp__sub_827B3298);
PPC_EXTERN_FUNC(__imp__sub_827B3C60);
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(bool,em2_probe_room_world_damage_events);
REXCVAR_DECLARE(bool,em2_trace_online_random_owners);
REXCVAR_DECLARE(uint32_t,em2_trace_online_random_start_tick);
PPC_EXTERN_FUNC(__imp__sub_82B6DD98);
PPC_EXTERN_FUNC(__imp__sub_82B6FFE8);
PPC_EXTERN_FUNC(__imp__sub_830F2450);
PPC_EXTERN_FUNC(__imp__sub_83232F20);
PPC_EXTERN_FUNC(__imp__sub_830CEC40);
PPC_EXTERN_FUNC(__imp__sub_828A4FB0);
PPC_EXTERN_FUNC(__imp__sub_828A4EF0);
PPC_EXTERN_FUNC(__imp__sub_82973700);
PPC_EXTERN_FUNC(__imp__sub_827CC8A0);
PPC_EXTERN_FUNC(__imp__sub_82792878);
REXCVAR_DEFINE_BOOL(em2_isolate_visual_random,true,"Gameplay","Separate presentation CRT randomness from gameplay randomness");
namespace {
thread_local unsigned visual_depth=0;
thread_local bool visual_initialized=false;
thread_local uint32_t visual_state=0;
// Only set by the same-executable regression fixture. It substitutes TLS lookup,
// not the original native generator or seed setter.
thread_local uint32_t test_ptd=0;
struct OnlineSeed {bool pending=false;uint32_t value=0;};
thread_local OnlineSeed online_seed;
thread_local OnlineSeed world_seed;
uint32_t SessionSeed(const em2::net::Identity& identity){
    uint32_t value=2166136261u;
    for(auto byte:identity.session){value^=byte;value*=16777619u;}
    for(auto byte:identity.checkpoint){value^=byte;value*=16777619u;}
    return value;
}
void InitializeOnlineState(uint8_t* base,uint32_t ptd,bool random_call,bool eligible){
    if(ptd && world_seed.pending){
        // A shared world load owns initialization before agreed ticks exist.
        // Preserve the native CRT algorithm, all draws, and explicit srand.
        const auto previous=PPC_LOAD_U32(ptd+20),seed=world_seed.value;
        if(random_call)PPC_STORE_U32(ptd+20,seed);
        world_seed.pending=false;online_seed.pending=false;
        struct Restore{unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Restore(){_mm_setcsr(csr);SetLastError(error);}}restore;
        try{REXLOG_INFO("Native online world RNG first use: thread {}, PTD {:08X}, previous {:08X}, queued seed {:08X}, native rand {}",GetCurrentThreadId(),ptd,previous,seed,random_call);}catch(...){}
        return;
    }
    if(!eligible || !ptd || !online_seed.pending)return;
    // An explicit native srand remains authoritative if it is the first use.
    // Otherwise initialize only the native CRT field read by the real rand.
    if(random_call)PPC_STORE_U32(ptd+20,online_seed.value);
    online_seed.pending=false;
}

struct Call {
    Call* previous;
    bool isolate;
    uint32_t ptd=0,canonical=0;
    uint8_t* base;
    static thread_local Call* current;
    explicit Call(uint8_t* memory,bool presentation=false):previous(current),isolate((visual_depth || presentation) && REXCVAR_GET(em2_isolate_visual_random)),base(memory){current=this;}
    ~Call() {
        if(ptd) {
            visual_state=PPC_LOAD_U32(ptd+20);visual_initialized=true;
            PPC_STORE_U32(ptd+20,canonical);
        }
        current=previous;
    }
};
thread_local Call* Call::current=nullptr;
void SelectDialog(PPCContext& ctx,uint8_t* base,bool online){
    // SequenceEventRandomDialog calls this selector from 82973998. Its
    // shuffle/sequential/nonrepeat choice controls dialogue variants; keep
    // other users of the shared selector on the gameplay random stream.
    if(online && ctx.lr==0x8297399C){em2::VisualRandomScope scope;__imp__sub_82973700(ctx,base);}
    else __imp__sub_82973700(ctx,base);
}
void EmitParticles(PPCContext& ctx,uint8_t* base,bool online,PPCFunc* native=__imp__sub_830F2450){
    // NiPSEmitter's lifetime/position/appearance randomization must not advance
    // gameplay CRT state when local particle acceptance/capacity differs.
    // Execute the complete original emitter, including its derived callbacks.
    // RibbonEmitter overrides the SAME virtual emission slot (+76), so its
    // complete original83232F20 must receive this boundary as well. Wrapping
    // only the base implementation misses its direct830CEC40 randomness.
    if(online){em2::VisualRandomScope scope;native(ctx,base);}
    else native(ctx,base);
}
void UpdateWind(PPCContext& ctx,uint8_t* base,bool online,PPCFunc* native){
    // This generator publishes wind values into NiFloat/float-array material
    // extra data (828A4D08 -> 825D7740). Local presentation cadence must not
    // advance the simulation's CRT stream. All native calculations and
    // material writes still execute; offline behavior remains the retail path.
    if(online){em2::VisualRandomScope scope;native(ctx,base);}
    else native(ctx,base);
}
void ConstructScriptResource(PPCContext& ctx,uint8_t* base,bool online,PPCFunc* native){
    if(online){em2::VisualRandomScope scope;native(ctx,base);}
    else native(ctx,base);
}
bool TestRetailScriptResourceRandom(){
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));if(!base)return false;
    struct Release{uint8_t* base;~Release(){VirtualFree(base,0,MEM_RELEASE);}}release{base};
    if(!VirtualAlloc(base,65536,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0x8203D000,4096,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0x833CD000,4096,MEM_COMMIT,PAGE_READWRITE))return false;
    test_ptd=0x1000;visual_depth=0;REXCVAR_SET(em2_isolate_visual_random,true);
    constexpr uint32_t local_seed=0x12345678,canonical=0xABCDEF01;
    std::vector<uint8_t> before(65536),expected_memory(65536);unsigned cases=0;
    for(bool online:{false,true})for(bool named:{false,true})for(uint32_t references:{0u,3u}){
        std::memset(base,0,65536);PPC_STORE_U32(0x3000,named?0x3108:0);PPC_STORE_U32(0x3100,references);
        PPC_STORE_U32(0x8203D278,0);PPC_STORE_U32(0x833CDD8C,0);
        PPC_STORE_U32(test_ptd+20,online?canonical:local_seed);
        PPCContext input{};input.r1.u64=0xF000;input.r3.u64=0x2000;input.r4.u64=0x3000;input.fpscr.InitHost();
        const auto fp=_mm_getcsr();SetLastError(0xA505);std::memcpy(before.data(),base,before.size());
        PPC_STORE_U32(test_ptd+20,local_seed);auto expected=input;__imp__sub_82792878(expected,base);
        const auto expected_fp=_mm_getcsr();const auto expected_error=GetLastError();
        if(online)PPC_STORE_U32(test_ptd+20,canonical);
        std::memcpy(expected_memory.data(),base,expected_memory.size());
        const auto expected_count=PPC_LOAD_U32(0x833CDD8C);
        std::memcpy(base,before.data(),before.size());PPC_STORE_U32(0x833CDD8C,0);
        visual_state=local_seed;visual_initialized=true;auto actual=input;_mm_setcsr(fp);SetLastError(0xA505);
        ConstructScriptResource(actual,base,online,__imp__sub_82792878);
        if(std::memcmp(&actual,&expected,sizeof(actual)) || std::memcmp(base,expected_memory.data(),expected_memory.size()) ||
           PPC_LOAD_U32(0x833CDD8C)!=expected_count || _mm_getcsr()!=expected_fp || GetLastError()!=expected_error || visual_depth)return false;
        ++cases;
    }
    REXLOG_INFO("Native BSQ parameter constructor passed {} original full-context/64KiB/FP/error comparisons: GUID output, reference counts, native object counter, offline passthrough and online gameplay RNG preservation",cases);
    return true;
}
bool TestRetailWindRandom(){
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));if(!base)return false;
    struct Release{uint8_t* p;~Release(){VirtualFree(p,0,MEM_RELEASE);}}release{base};
    if(!VirtualAlloc(base,65536,MEM_COMMIT,PAGE_READWRITE))return false;
    for(const uint32_t page:{0x82002000u,0x82003000u,0x82055000u,0x833FC000u})
        if(!VirtualAlloc(base+page,4096,MEM_COMMIT,PAGE_READWRITE))return false;
    PPC_STORE_U32(0x82002060,0);PPC_STORE_U32(0x82003F48,0x3F800000);
    PPC_STORE_U32(0x82055E14,0x38000100);PPC_STORE_U32(0x82055E18,0x38800100);
    PPC_STORE_U32(0x833FCA70,std::bit_cast<uint32_t>(1.f/30));
    constexpr uint32_t seed=0x12345678,canonical=0xABCDEF01;
    std::vector<uint8_t> before(65536),expected_memory(65536);unsigned cases=0;
    test_ptd=0x1000;visual_depth=0;REXCVAR_SET(em2_isolate_visual_random,true);
    for(PPCFunc* native:{__imp__sub_828A4FB0,__imp__sub_828A4EF0})
    for(const bool online:{false,true})for(const bool enabled:{false,true})for(const bool transition:{false,true})
    for(const bool expired:{false,true})for(const unsigned count:{0u,1u}){
        std::memset(base,0,65536);PPC_STORE_U8(0x2000+88,enabled);PPC_STORE_U8(0x2000+72,transition);
        for(unsigned i=1;i<=17;++i)PPC_STORE_U32(0x2000+i*4,std::bit_cast<uint32_t>(float(i)*.125f));
        PPC_STORE_U32(0x2000+76,std::bit_cast<uint32_t>(expired?2.f:0.f));PPC_STORE_U32(0x2000+80,std::bit_cast<uint32_t>(1.f));
        PPC_STORE_U32(0x2000+92,0x2F00);PPC_STORE_U32(0x2000+96,count);PPC_STORE_U32(0x2F00,0x3000);PPC_STORE_U32(0x3000,1);
        for(unsigned i=1;i<=25;++i){const auto object=0x4000+i*64;PPC_STORE_U32(0x3000+i*4,object);PPC_STORE_U32(object+16,2);PPC_STORE_U32(object+20,object+32);}
        for(unsigned i=104;i<=180;i+=4)PPC_STORE_U32(0x3000+i,std::bit_cast<uint32_t>(float(i)*.005f));
        PPC_STORE_U32(test_ptd+20,seed);visual_state=seed;visual_initialized=true;
        PPCContext input{};input.r1.u64=0xF000;input.r3.u64=0x2000;input.fpscr.InitHost();const auto csr=_mm_getcsr();
        std::memcpy(before.data(),base,before.size());auto expected=input;native(expected,base);
        const auto expected_csr=_mm_getcsr(),next=PPC_LOAD_U32(test_ptd+20);std::memcpy(expected_memory.data(),base,expected_memory.size());
        if(online){const auto encoded=_byteswap_ulong(canonical);std::memcpy(expected_memory.data()+test_ptd+20,&encoded,4);}
        std::memcpy(base,before.data(),before.size());if(online)PPC_STORE_U32(test_ptd+20,canonical);
        visual_state=seed;visual_initialized=true;_mm_setcsr(csr);auto actual=input;UpdateWind(actual,base,online,native);
        if(std::memcmp(&actual,&expected,sizeof(actual)) || std::memcmp(base,expected_memory.data(),expected_memory.size()) || _mm_getcsr()!=expected_csr || visual_depth ||
           visual_state!=(online?next:seed) || PPC_LOAD_U32(test_ptd+20)!=(online?canonical:next))return false;
        ++cases;
    }
    REXLOG_INFO("Retail wind RNG bridge passed {} full-context/64KiB/FP comparisons: disabled/active, transition/effect, expired/live, zero/one material binding; native float and array setters, online stream preservation and offline passthrough",cases);
    return true;
}
bool TestRetailDialogRandom(){
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));if(!base)return false;
    struct Release{uint8_t* p;~Release(){VirtualFree(p,0,MEM_RELEASE);}}release{base};
    if(!VirtualAlloc(base,65536,MEM_COMMIT,PAGE_READWRITE))return false;
    std::vector<uint8_t> before(65536),expected_memory(65536);unsigned cases=0;
    test_ptd=0x1000;visual_depth=0;REXCVAR_SET(em2_isolate_visual_random,true);
    for(const bool online:{false,true})for(const bool exact:{false,true})for(uint32_t mode=0;mode<3;++mode)
    for(const uint32_t count:{1u,5u})for(unsigned used=0;used<3;++used){
        const bool isolated=online && exact;
        constexpr uint32_t seed=0x12345678,canonical=0xABCDEF01;
        std::memset(base,0,65536);PPC_STORE_U32(test_ptd+20,isolated?canonical:seed);
        PPC_STORE_U32(0x2008,mode);PPC_STORE_U32(0x200C,0x3000);PPC_STORE_U32(0x2010,count);PPC_STORE_U32(0x2018,0xFFFFFFFF);
        for(uint32_t i=0;i<count;++i){const auto child=0x4000+i*64;PPC_STORE_U32(0x3000+i*4,child);PPC_STORE_U8(child+40,used==2 || (used==1 && (i%2)));}
        visual_state=seed;visual_initialized=true;
        for(unsigned step=0;step<8;++step){
            PPCContext input{};input.r1.u64=0xF000;input.r3.u64=0x2000;input.lr=exact?0x8297399C:0x82973998;input.fpscr.InitHost();
            const auto input_csr=_mm_getcsr();SetLastError(0xA504);std::memcpy(before.data(),base,65536);
            if(isolated)PPC_STORE_U32(test_ptd+20,visual_state);
            auto expected=input;__imp__sub_82973700(expected,base);
            const auto csr=_mm_getcsr(),next=PPC_LOAD_U32(test_ptd+20);const auto error=GetLastError();
            if(isolated)PPC_STORE_U32(test_ptd+20,canonical);
            std::memcpy(expected_memory.data(),base,65536);std::memcpy(base,before.data(),65536);
            auto actual=input;_mm_setcsr(input_csr);SetLastError(0xA504);SelectDialog(actual,base,online);
            if(std::memcmp(&actual,&expected,sizeof(actual)) || std::memcmp(base,expected_memory.data(),65536) ||
               _mm_getcsr()!=csr || GetLastError()!=error || visual_depth || (isolated && visual_state!=next)){
                REXLOG_ERROR("Native dialogue selection differs: online {}, exact {}, mode {}, count {}, used {}, step {}",online,exact,mode,count,used,step);return false;
            }
            ++cases;
        }
    }
    REXLOG_INFO("Native random dialogue passed {} original selector/context/64KiB/FP/error comparisons: shuffle/sequential/nonrepeat, 1/5 choices, partial/full reset, online isolation and unaudited/offline passthrough",cases);
    return true;
}
bool TestRetailParticleRandom(){
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));if(!base)return false;
    struct Release{uint8_t* base;~Release(){VirtualFree(base,0,MEM_RELEASE);}}release{base};
    for(const uint32_t page:{0u,0x82002000u,0x82003000u,0x82055000u,0x821E3000u}){
        if(!VirtualAlloc(base+page,page?4096:65536,MEM_COMMIT,PAGE_READWRITE))return false;
    }
    test_ptd=0x1000;visual_depth=0;REXCVAR_SET(em2_isolate_visual_random,true);
    const uint32_t visual_seed=0x12345678,canonical=0xABCDEF01;
    std::vector<uint8_t> before(65536),expected_memory(65536);
    for(const bool online:{false,true})for(const unsigned count:{0u,1u,3u,17u}){
        std::memset(base,0,65536);PPC_STORE_U32(0x2000+56,std::bit_cast<uint32_t>(-1.0f));
        // Real retail expired-particle path consumes lifetime RNG and rejects
        // each particle before callbacks. No replacement emitter or rand.
        PPC_STORE_U32(test_ptd+20,visual_seed);visual_state=visual_seed;visual_initialized=true;
        PPCContext input{};input.r1.u64=0xF000;input.r3.u64=0x2000;input.r4.u64=0x3000;input.r6.u64=count;input.fpscr.InitHost();
        const auto input_csr=_mm_getcsr();auto expected=input;
        std::memcpy(before.data(),base,before.size());__imp__sub_830F2450(expected,base);
        const auto expected_csr=_mm_getcsr();const auto native_next=PPC_LOAD_U32(test_ptd+20);std::memcpy(expected_memory.data(),base,expected_memory.size());
        if(online){const auto encoded=_byteswap_ulong(canonical);std::memcpy(expected_memory.data()+test_ptd+20,&encoded,4);}
        std::memcpy(base,before.data(),before.size());if(online)PPC_STORE_U32(test_ptd+20,canonical);
        visual_state=visual_seed;visual_initialized=true;_mm_setcsr(input_csr);auto actual=input;
        EmitParticles(actual,base,online);
        if(std::memcmp(&actual,&expected,sizeof(actual)) || std::memcmp(base,expected_memory.data(),expected_memory.size()) || _mm_getcsr()!=expected_csr || visual_depth ||
           visual_state!=(online?native_next:visual_seed) || PPC_LOAD_U32(test_ptd+20)!=(online?canonical:native_next))return false;
    }
    // Exercise the actual random-float routine reached by RibbonEmitter and
    // the complete derived override's missing-system return. Original native
    // code supplies both reference results, including its stack/FP effects.
    PPC_STORE_U32(0x82055E14,0x38000100);PPC_STORE_U32(0x82003EA0,0x3F000000);
    for(PPCFunc* native:{__imp__sub_830CEC40,__imp__sub_83232F20})for(const bool online:{false,true}){
        std::memset(base,0,65536);PPC_STORE_U32(test_ptd+20,visual_seed);
        PPC_STORE_U32(0x2000+20,std::bit_cast<uint32_t>(3.25f));PPC_STORE_U32(0x2000+24,std::bit_cast<uint32_t>(.75f));
        PPCContext input{};input.r1.u64=0xF000;input.r3.u64=0x2000;input.r4.u64=0;input.lr=0x832331A4;input.f1.f64=.0333333;input.fpscr.InitHost();
        const auto input_csr=_mm_getcsr();const auto input_error=GetLastError();auto expected=input;
        std::memcpy(before.data(),base,before.size());native(expected,base);
        const auto expected_csr=_mm_getcsr();const auto expected_error=GetLastError();const auto next=PPC_LOAD_U32(test_ptd+20);
        std::memcpy(expected_memory.data(),base,expected_memory.size());
        if(online){const auto encoded=_byteswap_ulong(canonical);std::memcpy(expected_memory.data()+test_ptd+20,&encoded,4);}
        std::memcpy(base,before.data(),before.size());if(online)PPC_STORE_U32(test_ptd+20,canonical);
        visual_state=visual_seed;visual_initialized=true;_mm_setcsr(input_csr);SetLastError(input_error);auto actual=input;
        EmitParticles(actual,base,online,native);
        if(std::memcmp(&actual,&expected,sizeof(actual)) || std::memcmp(base,expected_memory.data(),expected_memory.size()) ||
            _mm_getcsr()!=expected_csr || GetLastError()!=expected_error || visual_depth ||
            visual_state!=(online?next:visual_seed) || PPC_LOAD_U32(test_ptd+20)!=(online?canonical:next))return false;
    }
    REXLOG_INFO("Retail particle RNG bridge passed 0/1/3/17 emissions: full native caller/64KiB memory/FP output, online canonical-state preservation and offline passthrough; live allocation/callback behavior tested separately");return true;
}
}
void em2::BeginNativeOnlineRandom(const net::Identity& identity){online_seed={true,SessionSeed(identity)};}
void em2::EndNativeOnlineRandom(){online_seed={};world_seed={};}
void em2::BeginNativeOnlineWorldRandom(uint32_t seed){world_seed={true,seed};}
em2::VisualRandomScope::VisualRandomScope(){++visual_depth;}
em2::VisualRandomScope::~VisualRandomScope(){--visual_depth;}

PPC_FUNC_IMPL(sub_830F2450){EmitParticles(ctx,base,em2::net::SynchronizedSessionActive());}
PPC_FUNC_IMPL(sub_83232F20){EmitParticles(ctx,base,em2::net::SynchronizedSessionActive(),__imp__sub_83232F20);}
PPC_FUNC_IMPL(sub_828A4FB0){UpdateWind(ctx,base,em2::net::SynchronizedSessionActive(),__imp__sub_828A4FB0);}
PPC_FUNC_IMPL(sub_828A4EF0){UpdateWind(ctx,base,em2::net::SynchronizedSessionActive(),__imp__sub_828A4EF0);}
PPC_FUNC_IMPL(sub_82973700){SelectDialog(ctx,base,ctx.lr==0x8297399C && em2::net::SynchronizedSessionActive());}

// These constructors create BSQ resource/parameter objects, not world actors.
// Their base constructor generates four random GUID words. Asynchronous BSQ
// completion can construct them between agreed ticks (Autotopia's intro creates
// 55 objects), so those local resource IDs must not consume gameplay randomness.
// Preserve the entire original constructor and GUID algorithm on the existing
// local stream. Actual actor construction/cloning retains the canonical stream.
PPC_FUNC_IMPL(sub_827CC8A0){
    ConstructScriptResource(ctx,base,em2::net::SynchronizedSessionActive(),__imp__sub_827CC8A0);
}
PPC_FUNC_IMPL(sub_82792878){
    ConstructScriptResource(ctx,base,em2::net::SynchronizedSessionActive(),__imp__sub_82792878);
}

PPC_FUNC_IMPL(sub_82B6FFE8) {
    const auto caller=ctx.lr;
    if(test_ptd)ctx.r3.u64=test_ptd;else __imp__sub_82B6FFE8(ctx,base);
    auto* call=Call::current;
    if(call && !call->isolate && (caller==0x82B6DDD8 || caller==0x82B6DDB0))
        InitializeOnlineState(base,ctx.r3.u32,caller==0x82B6DDD8,em2::InAgreedSimulationTick());

    // Exact CRT rand/srand lookup return addresses. Other CRT users share this
    // TLS object but must never have their state swapped by a nested helper.
    if(call && call->isolate && !call->ptd && ctx.r3.u32 &&
       (caller==0x82B6DDD8 || caller==0x82B6DDB0)) {
        call->ptd=ctx.r3.u32;call->canonical=PPC_LOAD_U32(call->ptd+20);
        if(!visual_initialized){visual_state=call->canonical;visual_initialized=true;}
        PPC_STORE_U32(call->ptd+20,visual_state);
    }
}
namespace {
void RunNativeRandom(PPCContext& ctx,uint8_t* base,bool online) {
    const auto caller=ctx.lr;
    // VocalizationComponent's original update uses this draw only to choose
    // its chatter cooldown (82B4EDF0 -> timer +184). Sound completion and
    // local presentation can schedule it differently between peers. Isolate
    // this exact draw, not the component or its event/voice dispatches.
    Call call(base,online && caller==0x82B4EEA0);
    __imp__sub_82B6DDC8(ctx,base);
    if(!call.isolate){
        em2::net::ObserveNativeBehaviorRandom(uint32_t(caller),ctx,base);
        em2::RecordNativeRandom(false,uint32_t(caller),ctx.r11.u32,ctx.r3.u32,&ctx,base);
    }
}
}
PPC_FUNC_IMPL(sub_82B6DDC8) {RunNativeRandom(ctx,base,ctx.lr==0x82B4EEA0 && em2::net::SynchronizedSessionActive());}
PPC_FUNC_IMPL(sub_82B6DD98) {
    const auto caller=ctx.lr,seed=ctx.r3.u64;
    Call call(base);
    __imp__sub_82B6DD98(ctx,base);
    if(!call.isolate)em2::RecordNativeRandom(true,uint32_t(caller),uint32_t(seed),0,&ctx,base);
}

namespace {
struct RandomHost {
    PPCContext& ctx;PPCContext saved;DWORD error=GetLastError();unsigned csr=_mm_getcsr();
    explicit RandomHost(PPCContext& c):ctx(c),saved(c){}
    ~RandomHost(){ctx=saved;SetLastError(error);_mm_setcsr(csr);}
};
struct RandomOwner {uint32_t slot=0,ptd=0,id=0;bool operator==(const RandomOwner&)const=default;};
bool ReadRandomOwner(PPCContext& ctx,uint8_t* base,RandomOwner& owner){
    if(!base || visual_depth || Call::current)return false;
    uint32_t ptd=test_ptd,slot=0;
    if(!ptd){
        constexpr uint32_t CrtTlsIndex=0x833AB240; // original 82B6FF10 lis/lwz
        if(!em2::GuestReadBatch::Readable(base+CrtTlsIndex,4))return false;
        slot=PPC_LOAD_U32(CrtTlsIndex);
        {em2::GuestReadBatch::CallbackBoundary boundary;ctx.r3.u64=slot;__imp__KeTlsGetValue(ctx,base);ptd=ctx.r3.u32;}
    }
    // Lookup only: never allocate a missing CRT PTD as checkpoint preparation.
    if(ptd<4096 || uint64_t(ptd)+24>0x100000000ull || !em2::GuestReadBatch::Readable(base+ptd,24))return false;
    owner={slot,ptd,PPC_LOAD_U32(ptd)};return true;
}
}
struct em2::NativeRandomCheckpoint::Impl {
    uint8_t* base=nullptr;uint64_t epoch=0;uint32_t thread=0,state=0,visual=0;
    RandomOwner owner;bool initialized=false,isolate=false,fixture=false;
    OnlineSeed online,world;
    bool Check(PPCContext& ctx,uint8_t* memory,uint64_t expected,std::string& error)const{
        RandomOwner current;
        if(!epoch || expected!=epoch || memory!=base || thread!=GetCurrentThreadId() || fixture!=(test_ptd!=0) ||
            isolate!=REXCVAR_GET(em2_isolate_visual_random) || !ReadRandomOwner(ctx,memory,current) || current!=owner){
            error="Native random checkpoint owner/TLS/policy/world epoch changed";return false;
        }return true;
    }
};
em2::NativeRandomCheckpoint::NativeRandomCheckpoint():impl_(std::make_unique<Impl>()){}
em2::NativeRandomCheckpoint::~NativeRandomCheckpoint()=default;
bool em2::NativeRandomCheckpoint::Capture(PPCContext& ctx,uint8_t* base,uint64_t epoch,std::string& error){
    RandomHost host(ctx);GuestReadBatch reads;RandomOwner owner;
    if(!epoch || impl_->epoch || !ReadRandomOwner(ctx,base,owner)){error="Native random capture requires an initialized owner PTD outside random/visual calls";return false;}
    auto& s=*impl_;s.base=base;s.owner=owner;s.epoch=epoch;s.thread=GetCurrentThreadId();s.state=PPC_LOAD_U32(owner.ptd+20);
    s.visual=visual_state;s.initialized=visual_initialized;s.online=online_seed;s.world=world_seed;s.fixture=test_ptd!=0;s.isolate=REXCVAR_GET(em2_isolate_visual_random);
    error.clear();return true;
}
bool em2::NativeRandomCheckpoint::Restore(PPCContext& ctx,uint8_t* base,uint64_t epoch,std::string& error)const{
    RandomHost host(ctx);GuestReadBatch reads;const auto& s=*impl_;if(!s.Check(ctx,base,epoch,error))return false;
    PPC_STORE_U32(s.owner.ptd+20,s.state);visual_state=s.visual;visual_initialized=s.initialized;online_seed=s.online;world_seed=s.world;
    error.clear();return true;
}
bool em2::NativeRandomCheckpoint::Matches(PPCContext& ctx,uint8_t* base,uint64_t epoch,std::string& error)const{
    RandomHost host(ctx);GuestReadBatch reads;const auto& s=*impl_;if(!s.Check(ctx,base,epoch,error))return false;
    if(PPC_LOAD_U32(s.owner.ptd+20)!=s.state || visual_state!=s.visual || visual_initialized!=s.initialized ||
        online_seed.pending!=s.online.pending || online_seed.value!=s.online.value || world_seed.pending!=s.world.pending || world_seed.value!=s.world.value){
        error="Native random streams or pending initialization differ from checkpoint";return false;
    }
    error.clear();return true;
}
bool em2::NativeRandomCheckpoint::TestRoundTrip(PPCContext& ctx,uint8_t* base,uint64_t epoch,std::string& error)const{
    RandomHost host(ctx);if(!Matches(ctx,base,epoch,error))return false;
    struct Undo {const NativeRandomCheckpoint& cp;PPCContext& ctx;uint8_t* base;uint64_t epoch;~Undo(){std::string ignored;cp.Restore(ctx,base,epoch,ignored);}}undo{*this,ctx,base,epoch};
    std::array<std::array<uint32_t,48>,2> output{};
    for(unsigned pass=0;pass<2;++pass){
        for(unsigned i=0;i<32;++i){ctx=host.saved;__imp__sub_82B6DDC8(ctx,base);output[pass][i]=ctx.r3.u32;}
        for(unsigned i=32;i<48;++i){VisualRandomScope visual;ctx=host.saved;sub_82B6DDC8(ctx,base);output[pass][i]=ctx.r3.u32;}
        online_seed={ !online_seed.pending,online_seed.value^0xA531C9D7u };
        world_seed={ !world_seed.pending,world_seed.value^0x7254B1E3u };
        visual_initialized=!visual_initialized;visual_state^=0x6A29D083u;
        const auto online=online_seed,world=world_seed;const auto initialized=visual_initialized;
        const auto changed=PPC_LOAD_U32(impl_->owner.ptd+20),visual=visual_state;
        if(Restore(ctx,base,epoch+1,error) || PPC_LOAD_U32(impl_->owner.ptd+20)!=changed || visual_state!=visual || visual_initialized!=initialized ||
            online_seed.pending!=online.pending || online_seed.value!=online.value || world_seed.pending!=world.pending || world_seed.value!=world.value){error="Random checkpoint wrong epoch changed a native stream";return false;}
        if(!Restore(ctx,base,epoch,error) || !Matches(ctx,base,epoch,error))return false;
    }
    if(output[0]!=output[1]){error="Native CRT/presentation random outputs differ after restore";return false;}
    REXLOG_INFO("Native random checkpoint roundtrip passed: 32 original CRT draws and 16 presentation-scope draws reproduced (isolation {}), mutated pending seeds/visual initialization restored, wrong epoch rejected; owner thread only, other engine generators not covered",impl_->isolate);
    error.clear();return true;
}

bool em2::TestNativeRandom() {
    std::array<uint8_t,65536> memory{};auto* base=memory.data();
    struct Restore {
        uint32_t ptd=test_ptd,state=visual_state;unsigned depth=visual_depth,csr=_mm_getcsr();
        bool initialized=visual_initialized,enabled=REXCVAR_GET(em2_isolate_visual_random);
        OnlineSeed seed=online_seed,world=world_seed;
        ~Restore(){test_ptd=ptd;visual_state=state;visual_depth=depth;visual_initialized=initialized;online_seed=seed;world_seed=world;
            REXCVAR_SET(em2_isolate_visual_random,enabled);_mm_setcsr(csr);}
    } restore;
    test_ptd=0x1000;visual_depth=0;world_seed={};REXCVAR_SET(em2_isolate_visual_random,true);
    net::Identity identity;identity.session[0]=42;identity.checkpoint[0]=7;
    const auto shared_seed=SessionSeed(identity);auto different=identity;different.session[0]^=1;
    if(shared_seed==SessionSeed(different))return false;
    memory.fill(0x69);PPC_STORE_U32(test_ptd+20,123);const auto before=memory;
    BeginNativeOnlineRandom(identity);InitializeOnlineState(base,test_ptd,true,false);
    if(memory!=before || !online_seed.pending)return false;
    InitializeOnlineState(base,test_ptd,true,true);
    auto expected_memory=before;__builtin_memcpy(expected_memory.data()+test_ptd+20,base+test_ptd+20,4);
    if(memory!=expected_memory || PPC_LOAD_U32(test_ptd+20)!=shared_seed || online_seed.pending)return false;
    PPC_STORE_U32(test_ptd+20,456);InitializeOnlineState(base,test_ptd,true,true);
    if(PPC_LOAD_U32(test_ptd+20)!=456)return false;
    BeginNativeOnlineRandom(identity);InitializeOnlineState(base,test_ptd,false,true);
    if(online_seed.pending || PPC_LOAD_U32(test_ptd+20)!=456)return false;
    EndNativeOnlineRandom();
    PPCContext input{};input.r1.u64=0xF000;input.lr=0x12345678;input.r31.u64=0xDEADBEEF;input.fpscr.InitHost();
    for(const auto caller:{0x82B4EEA0u,0x82B4EE9Cu,0x82973848u})for(const bool online:{false,true}){
        constexpr uint32_t seed=0x12345678,canonical=0xABCDEF01;
        const bool isolated=online && caller==0x82B4EEA0;
        memory.fill(0);PPC_STORE_U32(test_ptd+20,seed);
        auto expected=input;expected.lr=caller;__imp__sub_82B6DDC8(expected,base);
        const auto next=PPC_LOAD_U32(test_ptd+20),csr=_mm_getcsr();
        if(isolated)PPC_STORE_U32(test_ptd+20,canonical);
        const auto expected_memory=memory;
        memory.fill(0);PPC_STORE_U32(test_ptd+20,isolated?canonical:seed);
        visual_state=seed;visual_initialized=true;auto actual=input;actual.lr=caller;
        RunNativeRandom(actual,base,online);
        if(memory!=expected_memory || std::memcmp(&expected,&actual,sizeof(actual)) || _mm_getcsr()!=csr ||
           visual_depth || visual_state!=(isolated?next:seed)){
            REXLOG_ERROR("Vocalization timer RNG differs from native output or changed an unaudited caller");return false;
        }
    }
    {
        constexpr uint32_t seed=0x346789AB;
        memory.fill(0);PPC_STORE_U32(test_ptd+20,seed);
        auto original=input;__imp__sub_82B6DDC8(original,base);
        const auto expected=memory;const auto csr=_mm_getcsr();
        memory.fill(0);PPC_STORE_U32(test_ptd+20,0xDEADBEEF);
        BeginNativeOnlineRandom(identity);BeginNativeOnlineWorldRandom(seed);
        auto wrapped=input;sub_82B6DDC8(wrapped,base);
        if(memory!=expected || std::memcmp(&original,&wrapped,sizeof(original)) || _mm_getcsr()!=csr ||
           world_seed.pending || online_seed.pending){REXLOG_ERROR("Native world initialization differs from original seeded CRT rand");return false;}
        const auto first_state=PPC_LOAD_U32(test_ptd+20);
        wrapped=input;sub_82B6DDC8(wrapped,base);
        if(PPC_LOAD_U32(test_ptd+20)!=first_state*214013u+2531011u){REXLOG_ERROR("Native world RNG was reseeded or restored after initialization");return false;}
        BeginNativeOnlineWorldRandom(seed);auto authored=input;authored.r3.u32=789;
        sub_82B6DD98(authored,base);
        if(world_seed.pending || PPC_LOAD_U32(test_ptd+20)!=789){REXLOG_ERROR("Native world RNG replaced an explicit authored srand");return false;}
    }
    for(uint32_t seed:{0u,1u,0x12345678u,0xFFFFFFFFu}) {
        memory.fill(0);PPC_STORE_U32(test_ptd+20,seed);
        auto original=input;__imp__sub_82B6DDC8(original,base);
        const auto expected=memory;const auto csr=_mm_getcsr();
        memory.fill(0);PPC_STORE_U32(test_ptd+20,seed);
        auto wrapped=input;sub_82B6DDC8(wrapped,base);
        if(memory!=expected || std::memcmp(&original,&wrapped,sizeof(original)) || _mm_getcsr()!=csr) {
            REXLOG_ERROR("Native rand forwarding differs from original");return false;
        }
        // Inside presentation, compare every guest byte/register against the
        // native generator running on that visual seed, with only canonical
        // PTD state restored. Other CRT/TLS fields must not be changed.
        memory.fill(0);PPC_STORE_U32(test_ptd+20,seed);
        original=input;__imp__sub_82B6DDC8(original,base);
        PPC_STORE_U32(test_ptd+20,0xABCDEF01);
        const auto visual_expected=memory;
        memory.fill(0);PPC_STORE_U32(test_ptd+20,0xABCDEF01);
        visual_state=seed;visual_initialized=true;wrapped=input;
        {VisualRandomScope scope;sub_82B6DDC8(wrapped,base);}
        if(memory!=visual_expected || std::memcmp(&original,&wrapped,sizeof(original)) || _mm_getcsr()!=csr) {
            REXLOG_ERROR("Visual rand differs from native register/memory/FP contract");return false;
        }
        memory.fill(0);original=input;original.r3.u64=seed;__imp__sub_82B6DD98(original,base);
        const auto seeded=memory;
        memory.fill(0);wrapped=input;wrapped.r3.u64=seed;sub_82B6DD98(wrapped,base);
        if(memory!=seeded || std::memcmp(&original,&wrapped,sizeof(original)))return false;
        memory.fill(0);PPC_STORE_U32(test_ptd+20,seed);wrapped=input;
        REXCVAR_SET(em2_isolate_visual_random,false);
        {VisualRandomScope scope;sub_82B6DDC8(wrapped,base);}
        REXCVAR_SET(em2_isolate_visual_random,true);
        if(memory!=expected)return false;
    }
    std::array<uint32_t,120> baseline{};
    for(unsigned presentations:{0u,1u,2u,4u,16u}) {
        memory.fill(0);visual_initialized=false;PPC_STORE_U32(test_ptd+20,0);
        for(unsigned tick=0;tick<baseline.size();++tick) {
            auto context=input;sub_82B6DDC8(context,base);
            if(!presentations)baseline[tick]=context.r3.u32;
            else if(baseline[tick]!=context.r3.u32){REXLOG_ERROR("Presentation count changed gameplay CRT sequence");return false;}
            const auto canonical=PPC_LOAD_U32(test_ptd+20);
            for(unsigned frame=0;frame<presentations;++frame) {
                VisualRandomScope scope;
                const auto before=visual_initialized?visual_state:canonical;
                const auto next=before*214013u+2531011u;
                context=input;sub_82B6DDC8(context,base);
                if(context.r3.u32!=((next>>16)&32767) || PPC_LOAD_U32(test_ptd+20)!=canonical) {
                    REXLOG_ERROR("Presentation rand did not preserve canonical state/native output");return false;
                }
                {VisualRandomScope nested;context=input;context.r3.u64=tick+frame;sub_82B6DD98(context,base);}
                if(visual_state!=tick+frame || PPC_LOAD_U32(test_ptd+20)!=canonical)return false;
            }
        }
    }
    if(!TestRetailParticleRandom() || !TestRetailWindRandom() || !TestRetailDialogRandom() || !TestRetailScriptResourceRandom())return false;
    REXLOG_INFO("Native CRT tests passed: full-context forwarding and identical gameplay sequence across 0/1/2/4/16 presentations per tick, nested scopes, visual reseeding and exact online-only vocalization cooldown isolation");
    return true;
}

// Bounded private diagnosis of template spawns around a captured random mismatch.
// Observes original calls; does not isolate RNG or change spawn eligibility.
PPC_FUNC_IMPL(sub_827B3C60){
    uint64_t tick=0;
    const bool trace=base && REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_probe_room_world_damage_events) && em2::CurrentAgreedSimulationTick(tick);
    if(!trace){__imp__sub_827B3C60(ctx,base);return;}
    const auto component=ctx.r3.u32,anchor=ctx.r5.u32,caller=uint32_t(ctx.lr);
    const auto selector=ctx.r4.s32;const auto flag=ctx.r6.u32;const bool collection=ctx.r7.u32!=0;
    std::array<uint32_t,4> owner{},anchor_id{};bool valid=false,anchor_valid=false,random_valid=false;
    uint32_t count_before=0,choice_before=0,used_before=0,random_before=0;RandomOwner random_owner;
    const auto readable=[&](uint32_t address,size_t size){return address>=4096 && uint64_t(address)+size<=0x100000000ull && em2::GuestReadBatch::Readable(base+address,size);};
    const auto identity=[&](uint32_t entity,std::array<uint32_t,4>& id){
        if(!readable(entity,20) || PPC_LOAD_U32(entity)!=0x8208D908)return false;
        for(unsigned i=0;i<4;++i)id[i]=PPC_LOAD_U32(entity+4+i*4);return true;
    };
    {
        RandomHost preserve(ctx);em2::GuestReadBatch reads;
        if(readable(component,148)){
            const auto attached=PPC_LOAD_U32(component+48);
            valid=attached && attached<=UINT32_MAX-8 && identity(attached+8,owner);
            count_before=PPC_LOAD_U32(component+104);choice_before=PPC_LOAD_U32(component+140);used_before=PPC_LOAD_U32(component+144);
        }
        anchor_valid=identity(anchor,anchor_id);
        random_valid=!online_seed.pending && !world_seed.pending && ReadRandomOwner(ctx,base,random_owner);
        if(random_valid)random_before=PPC_LOAD_U32(random_owner.ptd+20);
        REXLOG_INFO("Private native spawn batch begin: tick {} caller {:08X}, owner-valid {} GUID {:08X}/{:08X}/{:08X}/{:08X}, anchor-valid {} GUID {:08X}/{:08X}/{:08X}/{:08X}, selector {}, flag {}, collection {}, count {}, choice {}, used {:08X}, CRT-valid {} state {:08X}; observation only",
            tick,caller,valid,owner[0],owner[1],owner[2],owner[3],anchor_valid,anchor_id[0],anchor_id[1],anchor_id[2],anchor_id[3],selector,flag,collection,count_before,choice_before,used_before,random_valid,random_before);
    }
    __imp__sub_827B3C60(ctx,base);
    {
        RandomHost preserve(ctx);em2::GuestReadBatch reads;RandomOwner after;std::array<uint32_t,4> result{},current_owner{};
        const bool owner_unchanged=valid && readable(component,148) && [&]{const auto attached=PPC_LOAD_U32(component+48);return attached && attached<=UINT32_MAX-8 && identity(attached+8,current_owner) && current_owner==owner;}();
        const bool output_valid=identity(ctx.r3.u32,result);
        const bool same_random=random_valid && ReadRandomOwner(ctx,base,after) && after==random_owner && !online_seed.pending && !world_seed.pending;
        REXLOG_INFO("Private native spawn batch end: tick {} caller {:08X}, owner GUID {:08X}/{:08X}/{:08X}/{:08X}, owner-unchanged {}, count {}, choice {}, used {:08X}, result-valid {} GUID {:08X}/{:08X}/{:08X}/{:08X}, CRT-valid {} state {:08X}; observation only",
            tick,caller,owner[0],owner[1],owner[2],owner[3],owner_unchanged,owner_unchanged?PPC_LOAD_U32(component+104):0,owner_unchanged?PPC_LOAD_U32(component+140):0,owner_unchanged?PPC_LOAD_U32(component+144):0,output_valid,result[0],result[1],result[2],result[3],same_random,same_random?PPC_LOAD_U32(after.ptd+20):0);
    }
}
PPC_FUNC_IMPL(sub_827B3298){
    uint64_t tick=0;const uint64_t start=REXCVAR_GET(em2_trace_online_random_start_tick);
    const bool operation=REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_probe_room_world_damage_events) && em2::CurrentAgreedSimulationTick(tick);
    const auto component=ctx.r3.u32,anchor=ctx.r4.u32,prototype=ctx.r5.u32,caller=uint32_t(ctx.lr);
    const std::array<uint32_t,3> flags{ctx.r6.u32,ctx.r7.u32,ctx.r8.u32};
    std::array<uint32_t,4> owner_id{},anchor_id{},prototype_id{};bool owner_valid=false,anchor_valid=false,random_valid=false;
    RandomOwner random_owner;uint32_t random_before=0;
    if(operation){
        RandomHost preserve(ctx);em2::GuestReadBatch reads;
        const auto readable=[&](uint32_t address,size_t size){return address>=4096 && uint64_t(address)+size<=0x100000000ull && em2::GuestReadBatch::Readable(base+address,size);};
        const auto identity=[&](uint32_t entity,std::array<uint32_t,4>& id){
            if(!readable(entity,20) || PPC_LOAD_U32(entity)!=0x8208D908)return false;
            for(unsigned i=0;i<4;++i)id[i]=PPC_LOAD_U32(entity+4+i*4);return true;
        };
        if(readable(component,52)){
            const auto attached=PPC_LOAD_U32(component+48);
            if(attached && attached<=UINT32_MAX-8)owner_valid=identity(attached+8,owner_id);
        }
        anchor_valid=identity(anchor,anchor_id);
        if(readable(prototype,20))for(unsigned i=0;i<4;++i)prototype_id[i]=PPC_LOAD_U32(prototype+4+i*4);
        random_valid=!online_seed.pending && !world_seed.pending && ReadRandomOwner(ctx,base,random_owner);
        if(random_valid)random_before=PPC_LOAD_U32(random_owner.ptd+20);
    }
    if(REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_trace_online_random_owners) &&
        em2::CurrentAgreedSimulationTick(tick) && tick>=start && tick-start<2){
        const auto error=GetLastError();const auto csr=_mm_getcsr();
        std::string name="<unreadable>";
        const auto ref=ctx.r5.u32;
        if(ref>=4096 && em2::GuestReadBatch::Readable(base+ref,4)){
            const auto text=PPC_LOAD_U32(ref);
            if(text>=4096 && em2::GuestReadBatch::Readable(base+text,96)){
                const auto* p=reinterpret_cast<const char*>(base+text);
                if(const auto* end=static_cast<const char*>(std::memchr(p,0,96)))name.assign(p,end);
            }
        }
        REXLOG_INFO("Native template spawn random trace: tick {} caller {:08X} component {:08X} template '{}' flags {}/{}/{}",tick,ctx.lr,ctx.r3.u32,name,ctx.r6.u32,ctx.r7.u32,ctx.r8.u32);
        SetLastError(error);_mm_setcsr(csr);
    }
    __imp__sub_827B3298(ctx,base);
    if(operation){
        RandomHost preserve(ctx);em2::GuestReadBatch reads;RandomOwner after;std::array<uint32_t,4> output{};
        const auto entity=ctx.r3.u32;
        const bool output_valid=entity>=4096 && uint64_t(entity)+20<=0x100000000ull && em2::GuestReadBatch::Readable(base+entity,20) && PPC_LOAD_U32(entity)==0x8208D908;
        if(output_valid)for(unsigned i=0;i<4;++i)output[i]=PPC_LOAD_U32(entity+4+i*4);
        const bool same_random=random_valid && ReadRandomOwner(ctx,base,after) && after==random_owner && !online_seed.pending && !world_seed.pending;
        const auto random_after=same_random?PPC_LOAD_U32(after.ptd+20):0;
        REXLOG_INFO("Private native spawner operation: tick {} caller {:08X} component {:08X}, owner-valid {} GUID {:08X}/{:08X}/{:08X}/{:08X}, anchor-valid {} GUID {:08X}/{:08X}/{:08X}/{:08X}, template ID {:08X}/{:08X}/{:08X}/{:08X}, flags {}/{}/{}, result-valid {} entity {:08X} GUID {:08X}/{:08X}/{:08X}/{:08X}, CRT-valid {} states {:08X}/{:08X}; observation only",
            tick,caller,component,owner_valid,owner_id[0],owner_id[1],owner_id[2],owner_id[3],anchor_valid,anchor_id[0],anchor_id[1],anchor_id[2],anchor_id[3],
            prototype_id[0],prototype_id[1],prototype_id[2],prototype_id[3],flags[0],flags[1],flags[2],output_valid,entity,output[0],output[1],output[2],output[3],same_random,random_before,random_after);
    }
}
