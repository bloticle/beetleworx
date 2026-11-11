#include "ppc_recomp_shared.h"
#include "lowering_tests.h"
#include <rex/logging.h>
#include <stdexcept>
#include <malloc.h>
#define NOMINMAX
#include <Windows.h>

#define FIXTURE(name) extern "C" PPC_FUNC(__imp__fixture_##name)
FIXTURE(addc); FIXTURE(addme); FIXTURE(subfze); FIXTURE(lhzu); FIXTURE(lhau);
FIXTURE(lfsu); FIXTURE(stfsu); FIXTURE(lfdu); FIXTURE(stfdu); FIXTURE(vandc);
FIXTURE(pack_half2); FIXTURE(unpack_half2); FIXTURE(unpack_half4);
FIXTURE(counter_tail);
FIXTURE(jump);
FIXTURE(lvx); FIXTURE(stvx); FIXTURE(lvlx); FIXTURE(lvrx);
FIXTURE(lwarx); FIXTURE(stwcx); FIXTURE(ldarx); FIXTURE(stdcx);
FIXTURE(dcbz); FIXTURE(dcbzl);
FIXTURE(mftb);
static void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
bool em2::TestLowering() {
    // Match real guest-thread initialization and restore the caller's FP mode.
    // A zero-initialized FPSCR can otherwise unmask host exceptions when a VMX
    // fixture enables flush-to-zero, breaking the next native test suite.
    struct HostFPState {
        uint32_t value = _mm_getcsr();
        ~HostFPState() { _mm_setcsr(value); }
    } restore_fp;
    // Reserve guest address space, committing only two 64 KiB regions. This
    // verifies the SDK's physical alias without allocating 4 GiB of RAM.
    auto* base = static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100001000ull,MEM_RESERVE,PAGE_NOACCESS));
    if (!base) { REXLOG_ERROR("Instruction tests could not reserve guest address space"); return false; }
    if (!VirtualAlloc(base,65536,MEM_COMMIT,PAGE_READWRITE) ||
        !VirtualAlloc(base+0xE0000000ull,65536,MEM_COMMIT,PAGE_READWRITE)) {
        VirtualFree(base,0,MEM_RELEASE); REXLOG_ERROR("Instruction tests could not commit guest memory"); return false;
    }
    try {
        PPCContext ctx{};
        ctx.fpscr.InitHost();
        rex::chrono::Clock::set_guest_tick_frequency(50000000);
        auto tick_before=rex::chrono::Clock::QueryGuestTickCount();
        __imp__fixture_mftb(ctx,base);
        auto tick_after=rex::chrono::Clock::QueryGuestTickCount();
        check(ctx.r3.u64>=tick_before && ctx.r3.u64<=tick_after,"mftb used the host counter instead of Xbox guest time");
        constexpr uint32_t physical = 0xE0000040u;
        std::memset(base+0xE0000000ull,0xA5,512);
        PPC_STORE_U32(physical,0x12345678u);
        ctx.r4.u64=physical; ctx.r5.u64=0;
        __imp__fixture_lwarx(ctx,base);
        check(ctx.r3.u32==0x12345678u,"Physical lwarx omitted the host offset");
        ctx.r3.u64=0x87654321u; __imp__fixture_stwcx(ctx,base);
        check(ctx.cr0.eq && PPC_LOAD_U32(physical)==0x87654321u,"Physical stwcx failed");
        PPC_STORE_U64(physical,0x0123456789ABCDEFull);
        __imp__fixture_ldarx(ctx,base);
        check(ctx.r3.u64==0x0123456789ABCDEFull,"Physical ldarx failed");
        ctx.r3.u64=0xFEDCBA9876543210ull; __imp__fixture_stdcx(ctx,base);
        check(ctx.cr0.eq && PPC_LOAD_U64(physical)==0xFEDCBA9876543210ull,"Physical stdcx failed");
        for(unsigned i=0;i<16;++i) PPC_STORE_U8(physical+i,i+1);
        __imp__fixture_lvx(ctx,base);
        for(unsigned i=0;i<16;++i) check(ctx.v3.u8[15-i]==i+1,"Physical vector load or byte order failed");
        ctx.r4.u64=physical+64; __imp__fixture_stvx(ctx,base);
        for(unsigned i=0;i<16;++i) check(PPC_LOAD_U8(physical+64+i)==i+1,"Physical vector store failed");
        ctx.r4.u64=physical+3; __imp__fixture_lvlx(ctx,base);
        for(unsigned i=0;i<13;++i) check(ctx.v3.u8[15-i]==i+4,"Physical vector-left load failed");
        __imp__fixture_lvrx(ctx,base);
        for(unsigned i=0;i<3;++i) check(ctx.v3.u8[2-i]==i+1,"Physical vector-right load failed");
        std::memset(PPC_RAW_ADDR(0xE0000000u),0xCC,128);
        ctx.r4.u64=physical; __imp__fixture_dcbz(ctx,base);
        for(unsigned i=0;i<128;++i) check(PPC_LOAD_U8(0xE0000000u+i)==(i>=64&&i<96?0:0xCC),"Physical dcbz range failed");
        __imp__fixture_dcbzl(ctx,base);
        for(unsigned i=0;i<128;++i) check(PPC_LOAD_U8(0xE0000000u+i)==0,"Physical dcbzl failed");
        for(unsigned i=0;i<256;++i) check(base[0xE0000000ull+i]==0xA5,"A physical operation wrote to the unadjusted alias");
        ctx.r1.u64=3072; ctx.r31.u64=0x123456789ABCDEF0ull;
        __imp__fixture_jump(ctx,base);
        check(ctx.r3.u64==7 && ctx.r4.u64==0x1234 && ctx.r1.u64==3072 && ctx.r31.u64==0x123456789ABCDEF0ull,
              "Native longjmp did not unwind or restore guest registers");
        for (uint32_t count = 1; count <= 3; ++count) {
            std::memset(base+256, 0xCD, 4);
            ctx.r3.u64=256; ctx.r4.u64=0xFF; ctx.r5.u64=count;
            __imp__fixture_counter_tail(ctx,base);
            for (uint32_t i=0; i<4; ++i)
                check(base[256+i] == (i<count ? 0xFF : 0xCD), "Conditional counter return truncated a byte fill");
        }
        ctx.r4.u64=0xFFFFFFFF; ctx.r5.u64=1;
        __imp__fixture_addc(ctx,base);
        check(ctx.r3.u64==0x100000000ull && ctx.xer.ca==1,"addc carry failed");
        ctx.r4.u64=0; ctx.xer.ca=0; __imp__fixture_addme(ctx,base);
        check(ctx.r3.u64==UINT64_MAX && ctx.xer.ca==0,"addme zero carry failed");
        ctx.r4.u64=1; ctx.xer.ca=0; __imp__fixture_addme(ctx,base);
        check(ctx.r3.u64==0 && ctx.xer.ca==1,"addme carry failed");
        ctx.r4.u64=0; ctx.xer.ca=1; __imp__fixture_subfze(ctx,base);
        check(ctx.r3.u64==0 && ctx.xer.ca==1,"subfze carry failed");
        ctx.r4.u64=5; ctx.xer.ca=1; __imp__fixture_subfze(ctx,base);
        check(ctx.r3.s64==-5 && ctx.xer.ca==0,"subfze subtraction failed");
        PPC_STORE_U16(66,0xFEDCu); ctx.r4.u64=64; __imp__fixture_lhzu(ctx,base);
        check(ctx.r4.u64==66 && ctx.r3.u64==0xFEDC,"lhzu endian or update failed");
        ctx.r4.u64=64; __imp__fixture_lhau(ctx,base);
        check(ctx.r4.u64==66 && ctx.r3.s64==int16_t(0xFEDC),"lhau sign extension failed");
        PPCRegister bits{}; bits.f32=1.25f; PPC_STORE_U32(68,bits.u32);
        ctx.r4.u64=64; __imp__fixture_lfsu(ctx,base);
        check(ctx.f3.f64==1.25 && ctx.r4.u64==68,"lfsu failed");
        ctx.r4.u64=80; ctx.f3.f64=2.5; __imp__fixture_stfsu(ctx,base);
        bits.u32=PPC_LOAD_U32(84);
        check(bits.f32==2.5f && ctx.r4.u64==84,"stfsu failed");
        bits.f64=-3.75; PPC_STORE_U64(104,bits.u64);
        ctx.r4.u64=96; __imp__fixture_lfdu(ctx,base);
        check(ctx.f3.f64==-3.75 && ctx.r4.u64==104,"lfdu failed");
        ctx.r4.u64=112; ctx.f3.f64=7.125; __imp__fixture_stfdu(ctx,base);
        bits.u64=PPC_LOAD_U64(120);
        check(bits.f64==7.125 && ctx.r4.u64==120,"stfdu failed");
        for(int i=0;i<4;i++){ctx.v4.u32[i]=0xF0F0FF00;ctx.v5.u32[i]=0x55555555;}
        __imp__fixture_vandc(ctx,base);
        for(int i=0;i<4;i++)check(ctx.v3.u32[i]==(0xF0F0FF00u & ~0x55555555u),"vandc operands reversed");
        ctx.v4.f32[3]=1.0f; ctx.v4.f32[2]=-2.0f;
        ctx.v3.u32[0]=0x12345678;
        __imp__fixture_pack_half2(ctx,base);
        check(ctx.v3.u32[3]==0x3C00C000 && ctx.v3.u32[0]==0x12345678,"FLOAT16_2 pack layout failed");
        ctx.v4.u16[1]=0x3C00; ctx.v4.u16[0]=0xC000;
        __imp__fixture_unpack_half2(ctx,base);
        check(ctx.v3.f32[3]==1.0f && ctx.v3.f32[2]==-2.0f && ctx.v3.f32[1]==0.0f && ctx.v3.f32[0]==1.0f,"FLOAT16_2 unpack layout failed");
        ctx.v4.u16[0]=1; ctx.v4.u16[1]=0x7C00; ctx.v4.u16[2]=0xFC00; ctx.v4.u16[3]=0x8000;
        __imp__fixture_unpack_half4(ctx,base);
        check(ctx.v3.f32[0]==0x1p-24f && std::isinf(ctx.v3.f32[1]) && ctx.v3.f32[1]>0 && std::isinf(ctx.v3.f32[2]) && ctx.v3.f32[2]<0 && std::signbit(ctx.v3.f32[3]),"FLOAT16_4 special values failed");
        REXLOG_INFO("Generated instruction regressions passed");
    } catch(const std::exception& e){REXLOG_ERROR("Instruction regression failed: {}",e.what());VirtualFree(base,0,MEM_RELEASE);return false;}
    VirtualFree(base,0,MEM_RELEASE);
    return true;
}
