#include "native_launch_defaults.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <array>
#include <cstring>
#include <stdexcept>

PPC_EXTERN_FUNC(__imp__sub_829977A8);
namespace {
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

bool em2::TestNativeLaunchDefaults(){
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
    return true;
}
