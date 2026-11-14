#include "native_launch_defaults.h"
#include "native_empty_free.h"
#include "online_native_menu.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <array>
#include <cstring>
#include <stdexcept>
#include <rex/cvar.h>

PPC_EXTERN_FUNC(__imp__sub_829977A8);
PPC_EXTERN_FUNC(__imp__sub_8294D278);
REXCVAR_DEFINE_BOOL(em2_probe_face_target_defaults,false,"Debug","Exact private native-menu Room only: seed divergent stale fallback points before the FaceTarget constructor");
REXCVAR_DEFINE_BOOL(em2_probe_face_target_unfixed,false,"Debug","Exact private FaceTarget fault-injection only: retain original undefined fallback for causal comparison");
namespace {
void ConstructFaceTarget(PPCContext& ctx,uint8_t* base,PPCFunc* original,bool define_fallback){
    const auto object=ctx.r3.u32;
    original(ctx,base);
    // ORIGINAL8294D278 defines target ID+44, angle+64 and speed+68 but never
    // point+48..63. Null ID + FLT_MAX angle makes ORIGINAL8294D358 consume
    // that point. Define only this entity-reference constructor's unused
    // fallback; authored point/angle constructors retain their original data.
    // Do it after the original so an aliased target-ID source is copied first.
    if(define_fallback)std::memset(base+object+48,0,16);
}
void Construct(PPCContext& ctx,uint8_t* base,PPCFunc* original){
    // The original 176-byte constructor initializes fields through byte130,
    // but leaves both VMX vectors at offsets144/160 as allocator residue.
    // Its update reads these fields for launch direction and facing.
    // Initialize before the original so subsequent native constructor and
    // subclass assignments retain ownership.
    std::memset(base+ctx.r3.u32+144,0,32);
    original(ctx,base);
}
struct Probe {uint32_t object=0;unsigned calls=0;bool initialized=false,throws=false;};
thread_local Probe* probe=nullptr;
void OriginalFixture(PPCContext& ctx,uint8_t* base){
    auto& p=*probe;++p.calls;
    p.initialized=true;for(unsigned i=0;i<32;++i)p.initialized&=base[p.object+144+i]==0;
    // Original/subclass writes must survive, even when r3 becomes a result.
    PPC_STORE_U32(p.object+160,0x3f800000);PPC_STORE_U32(p.object+164,0x40000000);
    ctx.r3.u64=0x12345678;ctx.r11.u64=0x98765432;ctx.f1.u64=0x4008000000000000;
    ctx.v0.u32[0]=0x13579bdf;_mm_setcsr(_mm_getcsr()|0x8040);SetLastError(0x2468);
    if(p.throws)throw std::runtime_error("native launch constructor fixture");
}
}
PPC_FUNC_IMPL(sub_829977A8){Construct(ctx,base,__imp__sub_829977A8);}
PPC_FUNC_IMPL(sub_8294D278){
    auto* room=em2::net::PrivateNativeMenuRoom();
    const auto object=ctx.r3.u32;
    const bool inject=room && REXCVAR_GET(em2_probe_face_target_defaults) && PPC_LOAD_U32(ctx.r4.u32)==0 &&
        !(ctx.r4.u32>=object+48 && ctx.r4.u32<object+64);
    if(inject){
        // Deliberate allocation residue, not a runtime correction or actor
        // override. Only the exact validated isolated QA owner authorizes it.
        std::memset(base+object+48,0,16);
        PPC_STORE_U32(object+56,(room->local_peer()&1)?0:0xBF800000u);
    }
    const bool fixed=!inject || !REXCVAR_GET(em2_probe_face_target_unfixed);
    ConstructFaceTarget(ctx,base,__imp__sub_8294D278,fixed);
    if(inject){
        static thread_local unsigned logged=0;
        if(logged++<32){const auto csr=_mm_getcsr();const auto error=GetLastError();
            REXLOG_INFO("Native FaceTarget constructor QA: local {} stale-z {:08X} fallback-z {:08X} fixed {}; constructor default only",room->local_peer(),(room->local_peer()&1)?0:0xBF800000u,PPC_LOAD_U32(object+56),fixed);
            _mm_setcsr(csr);SetLastError(error);
        }
    }
}

bool em2::TestNativeLaunchDefaults(){
    if(!TestNativeEmptyFree())return false;
    struct Restore {unsigned fp=_mm_getcsr();DWORD error=GetLastError();Probe* saved=probe;
        ~Restore(){_mm_setcsr(fp);SetLastError(error);probe=saved;}}restore;
    unsigned checks=0;
    auto check=[&](bool okay,const char* why){++checks;if(!okay)REXLOG_ERROR("Native launch defaults: {}",why);return okay;};
    for(uint8_t poison:{uint8_t(0),uint8_t(0xff),uint8_t(0x42),uint8_t(0x7f),uint8_t(0xcd)})for(bool throws:{false,true}){
        alignas(16) std::array<uint8_t,512> memory;memory.fill(poison);auto* base=memory.data();
        constexpr uint32_t object=64;Probe state{object,0,false,throws};probe=&state;
        PPCContext ctx{};ctx.r3.u64=object;ctx.r1.u64=448;ctx.r13.u64=0x11223344;
        auto expected=ctx;auto expected_memory=memory;
        std::memset(expected_memory.data()+object+144,0,32);
        Probe reference{object,0,false,throws};probe=&reference;bool reference_threw=false;
        try{OriginalFixture(expected,expected_memory.data());}catch(const std::runtime_error&){reference_threw=true;}
        const auto expected_fp=_mm_getcsr();const auto expected_error=GetLastError();
        probe=&state;bool did_throw=false;
        try{Construct(ctx,base,OriginalFixture);}catch(const std::runtime_error&){did_throw=true;}
        if(!check(state.calls==1 && state.initialized,"original sees initialized vectors and executes exactly once") ||
           !check(did_throw==reference_threw,"original exception propagates unchanged") ||
           !check(!std::memcmp(&ctx,&expected,sizeof(ctx)) && _mm_getcsr()==expected_fp && GetLastError()==expected_error,
               "original PPC registers, VMX/FPR, host FP and LastError remain authoritative") ||
           !check(memory==expected_memory,"only default vectors change; later native writes and allocation redzones survive"))return false;
    }
    REXLOG_INFO("Native launch defaults passed {} constructor-boundary checks over five allocator patterns and returned/thrown callbacks; full NPC gameplay requires a live capture",checks);
    // Constructor contract: preserve the native target ID and complete return
    // environment, while defining only its otherwise unreadable fallback.
    for(uint8_t poison:{uint8_t(0),uint8_t(0xBF),uint8_t(0x7F),uint8_t(0xCD)})for(bool alias:{false,true}){
        alignas(16) std::array<uint8_t,512> memory;memory.fill(poison);auto* base=memory.data();constexpr uint32_t object=64;
        PPC_STORE_U32(alias?object+48:400,0x12345678);
        PPCContext actual{};actual.r3.u64=object;actual.r4.u64=alias?object+48:400;actual.r1.u64=480;actual.fpscr.InitHost();
        const auto constructor=+[](PPCContext& c,uint8_t* base){const auto object=c.r3.u32;PPC_STORE_U32(object+44,PPC_LOAD_U32(c.r4.u32));PPC_STORE_U32(object+64,0x7F7FFFFF);PPC_STORE_U32(object+68,0x7F7FFFFF);c.r3.u64=0x13579;c.r12.u64=0x2468;c.f1.u64=0x4008000000000000;_mm_setcsr(_mm_getcsr()|0x8040);SetLastError(337);};
        auto expected_memory=memory;auto expected=actual;constructor(expected,expected_memory.data());const auto expected_fp=_mm_getcsr();const auto expected_error=GetLastError();std::memset(expected_memory.data()+object+48,0,16);
        ConstructFaceTarget(actual,base,constructor,true);
        if(!check(memory==expected_memory && PPC_LOAD_U32(object+44)==0x12345678,"fallback initialization preserves aliased ID, native assignments and redzones") ||
           !check(!std::memcmp(&actual,&expected,sizeof actual) && _mm_getcsr()==expected_fp && GetLastError()==expected_error,"face constructor preserves original CPU/FP/error result"))return false;
    }
    REXLOG_INFO("FaceTarget fallback constructor checks passed across allocator patterns and aliased target input; live fault-injection comparison required");
    return true;
}
