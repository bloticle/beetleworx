#include "online_basic_input_test.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>
PPC_EXTERN_FUNC(__imp__sub_82783CE8);
PPC_EXTERN_FUNC(__imp__sub_82783208);
PPC_EXTERN_FUNC(__imp__sub_827CA160);
namespace {
struct Environment {unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(csr);SetLastError(error);}};
struct Preserve {
    PPCContext& context;std::array<uint8_t,sizeof(PPCContext)> saved{};uint8_t* base;
    std::array<uint8_t,8192> stack{};uint32_t begin=0;Environment environment;
    Preserve(PPCContext& ctx,uint8_t* memory):context(ctx),base(memory){
        std::memcpy(saved.data(),&ctx,sizeof(ctx));begin=(ctx.r1.u32&~15u)-8192;
        std::memcpy(stack.data(),base+begin,stack.size());
    }
    ~Preserve(){std::memcpy(base+begin,stack.data(),stack.size());std::memcpy(&context,saved.data(),sizeof(context));}
};
void ReferenceTransform(PPCContext& ctx,uint8_t* base){
    if(ctx.lr!=0x827CA1B4 || ctx.r4.u32!=0x833DA808 || ctx.r6.u32)
        throw std::runtime_error("Generic Basic fixture requires ORIGINAL NiEntity WorldTransform ABI");
    const auto entity=PPC_LOAD_U32(ctx.r3.u32+4);
    std::memcpy(base+ctx.r5.u32,base+entity+256,48);ctx.r3.u64=1;
}
}
bool em2::net::TestNativeGenericBasicInput(){
    Environment environment;
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
    struct Free {uint8_t* base;~Free(){if(base)VirtualFree(base,0,MEM_RELEASE);}} free{base};
    const auto path=std::filesystem::path(__FILE__).parent_path().parent_path()/"private/image.bin";
    std::error_code error;const auto size=std::filesystem::file_size(path,error);
    if(error || !base || !VirtualAlloc(base,0x50000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+PPC_IMAGE_BASE,size,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+PPC_IMAGE_BASE+PPC_IMAGE_SIZE,4096,MEM_COMMIT,PAGE_READWRITE))return false;
    std::ifstream image(path,std::ios::binary);if(!image.read(reinterpret_cast<char*>(base+PPC_IMAGE_BASE),size))return false;
    auto map=[&](uint32_t address,PPCFunc* function){
        const auto lookup=reinterpret_cast<uintptr_t>(&PPC_LOOKUP_FUNC(base,address));
        if(!VirtualAlloc(reinterpret_cast<void*>(lookup&~uintptr_t(4095)),4096,MEM_COMMIT,PAGE_READWRITE))return false;
        PPC_LOOKUP_FUNC(base,address)=function;return true;
    };
    if(!map(0x827CA160,__imp__sub_827CA160) || !map(uint32_t(PPC_CODE_BASE),ReferenceTransform))return false;
    auto native_string=[&](const char* value){const auto length=std::strlen(value)+1;
        const auto found=std::search(base+PPC_IMAGE_BASE,base+PPC_IMAGE_BASE+size,value,value+length);
        return found==base+PPC_IMAGE_BASE+size?0u:uint32_t(found-base);};
    const auto movement=native_string("Movement"),movement2=native_string("Movement2");
    if(!movement || !movement2)return false;
    constexpr uint32_t basic=0x10000,manager=0x3000,action=0x7000,unknown=0x7100;
    constexpr uint32_t reference=0x20000,frame=0x21000,components=0x22000,foreign=0x23000;
    PPC_STORE_U32(0x833DAB60,manager);PPC_STORE_U32(0x833DAD98,0x4000);
    PPC_STORE_U32(0x833DAEB8,movement);PPC_STORE_U32(0x833DAEC4,movement2);
    std::memcpy(base+unknown,"UnrelatedAction",16);
    // Layout comes from ORIGINAL constructor827863D8, without constructing a
    // character or admitting this generic component as Oswald.
    PPC_STORE_U32(basic,0x82080370);PPC_STORE_U32(basic+8,0x820801E0);PPC_STORE_U32(basic+64,0x820801D0);
    PPC_STORE_U8(basic+52,0x80); // Skip unrelated task/FSM forwarding only.
    PPC_STORE_U32(reference,0x8208D908);PPC_STORE_U32(reference+36,components);PPC_STORE_U32(reference+44,1);
    PPC_STORE_U32(components,frame);PPC_STORE_U32(frame,0x82090340);PPC_STORE_U32(frame+4,reference);
    PPC_STORE_U32(0x82090340+172,uint32_t(PPC_CODE_BASE));PPC_STORE_U32(basic+80,reference);
    for(unsigned i=0;i<12;++i)PPC_STORE_U32(reference+256+i*4,std::bit_cast<uint32_t>(i%5==0?1.f:0.f));
    auto put=[&](uint32_t p,float value){PPC_STORE_U32(p,std::bit_cast<uint32_t>(value));};
    auto value=[&](uint32_t p){return std::bit_cast<float>(PPC_LOAD_U32(p));};
    auto context=[] {PPCContext ctx{};ctx.fpscr.InitHost();ctx.r1.u64=0x4F000;ctx.lr=0x12345678;ctx.r13.u64=0x13572468;return ctx;};
    unsigned checks=0;auto check=[&](bool condition,const char* why){++checks;if(!condition)REXLOG_ERROR("Generic native Basic: {}",why);return condition;};
    if(!check(PPC_LOAD_U32(0x820801D0)==0x82783CE8 && PPC_LOAD_U32(0x820801E0+40)==0x827835A8 &&
        PPC_LOAD_U32(0x82080370+108)==0x827DDD88,"native image confirms generic listener, update face and reference setter"))return false;
    auto invoke=[&](uint32_t uid,float x,float y){auto ctx=context();PPC_STORE_U32(action,uid);ctx.r3.u64=basic+64;ctx.r5.u64=action;ctx.f1.f64=x;ctx.f2.f64=y;
        Preserve preserve(ctx,base);__imp__sub_82783CE8(ctx,base);};
    for(const auto xy:std::array<std::array<float,2>,4>{{{.5f,-.25f},{-.75f,.125f},{0,0},{1,-1}}}){
        invoke(movement2,xy[0],xy[1]);
        if(!check(value(basic+232)==xy[1] && value(basic+236)==xy[0],"ORIGINAL Movement2 callback preserves f1/f2 sign and native cache order"))return false;
    }
    put(basic+232,.25f);put(basic+236,-.5f);invoke(unknown,1,1);
    if(!check(value(basic+232)==.25f && value(basic+236)==-.5f,"unrelated UID does not modify native movement caches"))return false;
    PPC_STORE_U8(basic+240,1);invoke(movement2,1,1);
    if(!check(value(basic+232)==.25f && value(basic+236)==-.5f,
        "native movement latch240 skips cache replacement and retains previous intent rather than neutralizing it"))return false;
    PPC_STORE_U8(basic+240,0);PPC_STORE_U32(basic+48,foreign-8);invoke(movement2,.25f,-.5f);
    if(!check(value(basic+232)==-.5f && value(basic+236)==.25f,
        "mapped foreign owner without optional motion is NOT rejected by original callback; external lease validation remains necessary"))return false;
    PPC_STORE_U32(basic+48,0);
    for(const auto pitched:std::array<std::array<float,3>,2>{{{3,-12,4},{-3,12,-4}}}){
        for(unsigned i=0;i<3;++i)put(reference+256+32+i*4,pitched[i]);
        auto ctx=context();std::array<uint8_t,sizeof(PPCContext)> incoming{};std::memcpy(incoming.data(),&ctx,sizeof(ctx));
        std::vector<uint8_t> before(base,base+0x50000);
        _mm_setcsr(0x1F80);SetLastError(0x1234ABCD);std::array<float,3> direction{};
        {Preserve preserve(ctx,base);const auto copy=ctx.r1.u32-4096;
            // 83208's last Basic access is264+4. This is a private copy of
            // the original generic Basic subobject, never fullMickey+16.
            std::memcpy(base+copy,base+basic,268);ctx.r1.u64=copy-512;ctx.r3.u64=copy;
            __imp__sub_82783208(ctx,base);
            direction={value(copy+256),value(copy+260),value(copy+264)};
        }
        const auto last_error=GetLastError();const auto csr=_mm_getcsr();
        if(!check(std::abs(direction[0]-pitched[0]/5)<1e-6f && direction[1]==0 && std::abs(direction[2]-pitched[2]/5)<1e-6f,
            "ORIGINAL NiEntity property query plus Basic projection flattens pitch and preserves positive native forward"))return false;
        if(!check(std::memcmp(base,before.data(),before.size())==0 && std::memcmp(&ctx,incoming.data(),sizeof(ctx))==0 && csr==0x1F80 && last_error==0x1234ABCD,
            "private reference projection restores caller, guest scratch/controller/reference, FP and LastError"))return false;
    }
    // Demonstrate exactly what ORIGINAL83208 guards, without inventing native
    // registry/generation checks. A captured transition deliberately reuses its
    // stored basis; null unlatched reference also retains it and sets the latch.
    auto capture=[&](){auto ctx=context();Preserve preserve(ctx,base);ctx.r3.u64=basic;__imp__sub_82783208(ctx,base);};
    PPC_STORE_U8(basic+241,0);capture();const std::array<uint32_t,3> captured{PPC_LOAD_U32(basic+256),PPC_LOAD_U32(basic+260),PPC_LOAD_U32(basic+264)};
    PPC_STORE_U32(basic+80,0xDEAD0000);capture();
    if(!check(PPC_LOAD_U8(basic+241)==1 && PPC_LOAD_U32(basic+256)==captured[0] && PPC_LOAD_U32(basic+264)==captured[2],
        "captured native latch skips a replaced reference; it is not a lifetime/identity validator"))return false;
    PPC_STORE_U8(basic+241,0);PPC_STORE_U32(basic+80,0);capture();
    if(!check(PPC_LOAD_U8(basic+241)==1 && PPC_LOAD_U32(basic+256)==captured[0] && PPC_LOAD_U32(basic+264)==captured[2],
        "null native reference retains prior basis and latches; online producer must reject this fallback"))return false;
    REXLOG_INFO("Generic native Basic mechanics passed {} checks: ORIGINAL Movement2 callback and NiEntity reference projection only; no human Oswald identity, factory admission, contacts or playable multiplayer proof",checks);
    return true;
}
