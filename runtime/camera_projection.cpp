#include "ppc_context.h"
#include "camera_projection.h"
#include "camera_timing.h"
#include "pc_settings.h"
#include "online.h"
#include "online_runtime.h"
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <immintrin.h>
#include <optional>
#include <stdexcept>
#include <utility>
#include <windows.h>

PPC_EXTERN_FUNC(__imp__sub_828510C8);
PPC_EXTERN_FUNC(__imp__sub_8290E320);
PPC_EXTERN_FUNC(sub_8290E320);
namespace {
struct ProjectionScope {uint32_t controller=0;float width=1;};
thread_local ProjectionScope projection_scope;
bool NormalOwnedCamera(uint32_t controller,uint8_t* base) {
    const auto manager=PPC_LOAD_U32(0x833DAB60);
    if(!controller || !manager)return false;
    const auto record=controller==PPC_LOAD_U32(manager+28)?manager:
        controller==PPC_LOAD_U32(manager+348)?manager+320:0u;
    if(!record || !PPC_LOAD_U32(record+20) || PPC_LOAD_U32(record+32)!=0)return false;
    if(PPC_LOAD_U32(controller)!=0x82112778)return false;
    const auto mode=PPC_LOAD_U32(controller+24);
    if(!mode || PPC_LOAD_U32(mode)!=0x82126C98 || PPC_LOAD_U32(mode+20)!=controller)return false;
    return !PPC_LOAD_U32(controller+28) && !PPC_LOAD_U32(controller+32) && !PPC_LOAD_U32(controller+36);
}
std::optional<float> WiderFov(double degrees,float width) {
    const auto csr=_mm_getcsr();
    struct Restore {unsigned csr;~Restore(){_mm_setcsr(csr);}} restore{csr};
    if(!std::isfinite(degrees) || degrees<=0 || degrees>=179 || !std::isfinite(width) || width<=1 || width>2)return {};
    constexpr double radians=3.14159265358979323846/180;
    const auto result=2*std::atan(std::tan(degrees*radians*.5)*double(width))/radians;
    if(!std::isfinite(result))return {};
    return float(result);
}
void CameraUpdate(PPCContext& ctx,uint8_t* base,float width,PPCFunc* update,bool synchronized=false) {
    const bool enabled=!synchronized && std::isfinite(width) && width>1 && width<=2;
    if(!enabled && !projection_scope.controller){update(ctx,base);return;}
    struct Restore {ProjectionScope previous=projection_scope;~Restore(){projection_scope=previous;}} restore;
    projection_scope=enabled?ProjectionScope{ctx.r3.u32,width}:ProjectionScope{};
    update(ctx,base);
}
}
bool em2::IsNormalGameplayCamera(uint32_t controller,uint8_t* base) {
    return NormalOwnedCamera(controller,base);
}
PPC_FUNC_IMPL(sub_828510C8) {
    em2::ObserveNativeCameraUpdate(ctx,base);
    const auto width=em2::GameplayViewWidth();
    bool synchronized=false;
    if(width>1){
        struct Restore {unsigned csr=_mm_getcsr();~Restore(){_mm_setcsr(csr);}} restore;
        synchronized=em2::net::GameplayInputGate().active() || em2::net::NativeOnlineTiming().fixed_step;
    }
    CameraUpdate(ctx,base,width,__imp__sub_828510C8,synchronized);
}
PPC_FUNC_IMPL(sub_8290E320) {
    __imp__sub_8290E320(ctx,base);
    // This call feeds the camera entity's native FOV attribute setter (virtual
    // +276), which then updates the camera projection. Leave authored/cached
    // pose data untouched so widening cannot accumulate on subsequent ticks.
    if(ctx.lr!=0x828515A8 || projection_scope.width<=1 ||
       !NormalOwnedCamera(projection_scope.controller,base))return;
    if(const auto fov=WiderFov(ctx.f1.f64,projection_scope.width))ctx.f1.f64=double(*fov);
}

namespace {
constexpr uint32_t fixture_pose=0x10000;
void ProjectionFixture(PPCContext& ctx,uint8_t* base) {
    ctx.r3.u64=fixture_pose;ctx.lr=0x828515A8;
    sub_8290E320(ctx,base);
}
void ThrowProjectionFixture(PPCContext&,uint8_t*) {throw std::runtime_error("projection scope test");}
}
bool em2::TestNativeCameraProjection() {
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
    struct Restore {uint8_t* base;unsigned csr=_mm_getcsr();ProjectionScope previous=projection_scope;
        ~Restore(){projection_scope=previous;_mm_setcsr(csr);if(base)VirtualFree(base,0,MEM_RELEASE);}} restore{base};
    if(!base || !VirtualAlloc(base,0x20000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0x833D0000,0x10000,MEM_COMMIT,PAGE_READWRITE))return false;
    constexpr uint32_t controller=0x2000,mode=0x4000,manager=0x6000,second=0x8000,second_mode=0xA000;
    auto setup=[&]{
        std::memset(base,0,0x20000);PPC_STORE_U32(0x833DAB60,manager);
        PPC_STORE_U32(manager+28,controller);PPC_STORE_U32(manager+348,second);
        PPC_STORE_U32(manager+20,1);PPC_STORE_U32(manager+340,2);
        for(auto pair:{std::pair(controller,mode),std::pair(second,second_mode)}){
            PPC_STORE_U32(pair.first,0x82112778);PPC_STORE_U32(pair.first+24,pair.second);
            PPC_STORE_U32(pair.second,0x82126C98);PPC_STORE_U32(pair.second+20,pair.first);
        }
    };
    auto check=[](bool value,const char* reason){if(!value)REXLOG_ERROR("Camera projection test: {}",reason);return value;};
    setup();unsigned comparisons=0;
    for(unsigned rounding=0;rounding<4;++rounding)for(unsigned flush=0;flush<4;++flush)
    for(float width:{1.f,1.1f,1.25f,1.5f,1.75f})for(float authored:{5.f,30.f,45.f,60.f,80.f,100.f,140.f,150.f}){
        PPC_STORE_U32(fixture_pose+64,std::bit_cast<uint32_t>(authored));
        const auto control=0x1F80|(rounding<<13)|((flush&1)?0x8000:0)|((flush&2)?0x40:0);
        _mm_setcsr(control);
        PPCContext initial{};initial.r3.u64=controller;initial.r31.u64=0x12345678;initial.fpscr.InitHost();
        auto expected=initial;expected.r3.u64=fixture_pose;expected.lr=0x828515A8;
        __imp__sub_8290E320(expected,base);const auto expected_csr=_mm_getcsr();
        _mm_setcsr(control);auto actual=initial;
        CameraUpdate(actual,base,width,ProjectionFixture);
        const auto actual_fov=actual.f1.f64;actual.f1=expected.f1;
        if(!check(!std::memcmp(&actual,&expected,sizeof(actual)) && _mm_getcsr()==expected_csr &&
            PPC_LOAD_U32(fixture_pose+64)==std::bit_cast<uint32_t>(authored),"projection changes only returned FOV, preserves FP state and never rewrites the authored pose"))return false;
        if(width==1){if(!check(actual_fov==authored,"Original must return the exact native FOV"))return false;}
        else{
            const auto desired=WiderFov(authored,width);
            if(!check(desired && actual_fov==double(*desired) && actual_fov>=authored && actual_fov<180,
                      "normal owned camera must use the configured geometric width"))return false;
            {
                constexpr double rad=3.14159265358979323846/180;
                const auto ratio=std::tan(actual_fov*rad*.5)/std::tan(double(authored)*rad*.5);
                if(!check(std::abs(ratio-width)<3e-6,"frustum width must scale geometrically, including native zoom values"))return false;
            }
        }
        ++comparisons;
    }
    // Every guard must preserve the complete native getter result.
    for(unsigned gate=0;gate<13;++gate){
        setup();PPC_STORE_U32(fixture_pose+64,std::bit_cast<uint32_t>(60.f));
        auto owner=controller;
        if(gate==0)PPC_STORE_U32(0x833DAB60,0);
        if(gate==1)owner=0xC000;
        if(gate==2)PPC_STORE_U32(controller,0);
        if(gate==3)PPC_STORE_U32(controller+24,0);
        if(gate==4)PPC_STORE_U32(mode,0);
        if(gate==5)PPC_STORE_U32(mode+20,second);
        if(gate>=6 && gate<=8)PPC_STORE_U32(controller+28+(gate-6)*4,0xD000);
        if(gate==11)PPC_STORE_U32(manager+20,0);
        if(gate==12)PPC_STORE_U32(manager+32,1);
        PPCContext actual{};actual.fpscr.InitHost();actual.r3.u64=fixture_pose;actual.lr=gate==9?0x1234:0x828515A8;
        auto expected=actual;__imp__sub_8290E320(expected,base);const auto expected_csr=_mm_getcsr();
        projection_scope=gate==10?ProjectionScope{}:ProjectionScope{owner,1.5f};
        sub_8290E320(actual,base);
        if(!check(!std::memcmp(&actual,&expected,sizeof(actual)) && _mm_getcsr()==expected_csr,
                  "unowned, scripted, transition, foreign-callsite and unscoped cameras keep their native FOV"))return false;
    }
    setup();projection_scope={controller,1.5f};
    for(uint32_t bits:{0u,0x80000000u,0xBF800000u,0x7F800000u,0xFF800000u,0x7FC12345u,0x7F812345u}){
        PPC_STORE_U32(fixture_pose+64,bits);
        PPCContext actual{};actual.fpscr.InitHost();actual.r3.u64=fixture_pose;actual.lr=0x828515A8;
        auto expected=actual;__imp__sub_8290E320(expected,base);const auto expected_csr=_mm_getcsr();
        sub_8290E320(actual,base);
        if(!check(!std::memcmp(&actual,&expected,sizeof(actual)) && _mm_getcsr()==expected_csr,"invalid authored FOV remains exactly native"))return false;
    }
    setup();PPC_STORE_U32(fixture_pose+64,std::bit_cast<uint32_t>(60.f));
    projection_scope={controller,1.5f};
    PPCContext nested{};nested.r3.u64=second;nested.fpscr.InitHost();
    CameraUpdate(nested,base,1.25f,ProjectionFixture);
    if(!check(nested.f1.f64==double(*WiderFov(60,1.25f)) && projection_scope.controller==controller && projection_scope.width==1.5f,
              "second player's projection is independent and restores the outer camera scope"))return false;
    nested.r3.u64=second;
    CameraUpdate(nested,base,1.f,ProjectionFixture);
    if(!check(nested.f1.f64==60 && projection_scope.controller==controller && projection_scope.width==1.5f,
              "nested Original camera must not inherit the outer player's widening"))return false;
    nested.r3.u64=second;
    CameraUpdate(nested,base,1.75f,ProjectionFixture,true);
    if(!check(nested.f1.f64==60 && projection_scope.controller==controller && projection_scope.width==1.5f,
              "synchronized sessions retain native projection without inheriting an offline scope"))return false;
    projection_scope={second,1.25f};PPCContext ctx{};ctx.r3.u64=controller;
    try{CameraUpdate(ctx,base,1.5f,ThrowProjectionFixture);return false;}catch(const std::runtime_error&){}
    if(!check(projection_scope.controller==second && projection_scope.width==1.25f,"unwinding restores the previous camera scope"))return false;
    REXLOG_INFO("Native offline gameplay projection passed {} normal FOV/control-mode comparisons, 13 ownership/script/callsite gates, invalid authored values, independent nested players, synchronized-session bypass and scope unwinding; actual rendered framing still needs gameplay acceptance",comparisons);
    return true;
}
