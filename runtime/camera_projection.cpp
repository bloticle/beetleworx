#include "ppc_context.h"
#include "camera_projection.h"
#include "camera_timing.h"
#include "camera_cadence.h"
#include "guest_read_batch.h"
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
struct ViewCamera {uint32_t manager=0,registry=0,controller=0,handle=0,entity=0,component=0,node=0;};
thread_local std::array<ViewCamera,2> view_cameras{};
thread_local uint32_t active_view_controller=0;
struct ViewHostState {DWORD error=GetLastError();unsigned csr=_mm_getcsr();~ViewHostState(){_mm_setcsr(csr);SetLastError(error);}};
bool ViewReadable(uint32_t address,size_t size,uint8_t* base){
    return address>=4096 && uint64_t(address)+size<=0x100000000ull && em2::GuestReadBatch::Readable(base+address,size);
}
int ViewControllerSlot(uint32_t controller,uint32_t manager,uint8_t* base){
    if(!ViewReadable(manager,352,base) || !ViewReadable(controller,28,base) || PPC_LOAD_U32(controller)!=0x82112778)return -1;
    if(PPC_LOAD_U32(manager+28)==controller)return 0;
    if(PPC_LOAD_U32(manager+348)==controller)return 1;
    return -1;
}
void ObserveViewEntity(uint32_t controller,uint32_t entity,uint8_t* base){
    ViewHostState preserve;
    em2::GuestReadBatch reads;
    const auto manager=PPC_LOAD_U32(0x833DAB60),registry=PPC_LOAD_U32(0x833DA7E4);
    const int slot=ViewControllerSlot(controller,manager,base);
    if(slot<0 || !registry || !PPC_LOAD_U32(controller+20) || !ViewReadable(entity,48,base) ||
        PPC_LOAD_U32(entity)!=0x8208D908 || PPC_LOAD_U32(entity+44)!=2)return;
    const auto table=PPC_LOAD_U32(entity+36);
    if(!ViewReadable(table,8,base))return;
    const auto component=PPC_LOAD_U32(table+4);
    if(!ViewReadable(component,84,base) || PPC_LOAD_U32(component)!=0x820D8970)return;
    const auto node=PPC_LOAD_U32(component+80);
    if(!ViewReadable(node,512,base))return;
    view_cameras[slot]={manager,registry,controller,PPC_LOAD_U32(controller+20),entity,component,node};
}
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
void em2::ResetGameplayViewCameras() noexcept {view_cameras={};}
bool em2::IsGameplayViewCameraNode(uint32_t node,uint8_t* base) noexcept {
    ViewHostState preserve;
    em2::GuestReadBatch reads;
    for(unsigned slot=0;slot<view_cameras.size();++slot){
        const auto& view=view_cameras[slot];
        if(view.node!=node || !node || PPC_LOAD_U32(0x833DAB60)!=view.manager || PPC_LOAD_U32(0x833DA7E4)!=view.registry ||
            ViewControllerSlot(view.controller,view.manager,base)!=int(slot) || PPC_LOAD_U32(view.controller+20)!=view.handle ||
            !ViewReadable(view.entity,48,base) || PPC_LOAD_U32(view.entity)!=0x8208D908 || PPC_LOAD_U32(view.entity+44)!=2 ||
            !ViewReadable(view.component,84,base) || PPC_LOAD_U32(view.component)!=0x820D8970 ||
            PPC_LOAD_U32(view.component+80)!=node)continue;
        const auto table=PPC_LOAD_U32(view.entity+36);
        if(!ViewReadable(table,8,base) || PPC_LOAD_U32(table+4)!=view.component)continue;
        return true;
    }
    return false;
}
namespace {
void ProjectedCamera(PPCContext& ctx,uint8_t* base){
    struct ViewScope {uint32_t saved=active_view_controller;~ViewScope(){active_view_controller=saved;}}view_scope;
    active_view_controller=ctx.r3.u32;
    em2::ObserveNativeCameraUpdate(ctx,base);
    const auto width=em2::GameplayViewWidth();
    bool synchronized=false;
    if(width>1){
        struct Restore {unsigned csr=_mm_getcsr();~Restore(){_mm_setcsr(csr);}} restore;
        synchronized=em2::net::GameplayInputGate().active() || em2::net::NativeOnlineTiming().fixed_step;
    }
    CameraUpdate(ctx,base,width,__imp__sub_828510C8,synchronized);
}
}
PPC_FUNC_IMPL(sub_828510C8) {
    if(!em2::HandleCameraPresentationUpdate(ctx,base,ProjectedCamera))ProjectedCamera(ctx,base);
}
PPC_FUNC_IMPL(sub_8290E320) {
    __imp__sub_8290E320(ctx,base);
    em2::ObserveNativeCameraEntity(ctx,base);
    if(ctx.lr==0x828515A8 && active_view_controller)
        ObserveViewEntity(active_view_controller,ctx.r30.u32,base);
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
        decltype(view_cameras) views=view_cameras;uint32_t active=active_view_controller;
        ~Restore(){projection_scope=previous;view_cameras=views;active_view_controller=active;_mm_setcsr(csr);if(base)VirtualFree(base,0,MEM_RELEASE);}} restore{base};
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
    setup();ResetGameplayViewCameras();
    const auto bind_view=[&](uint32_t owner,uint32_t entity,uint32_t component,uint32_t node){
        PPC_STORE_U32(0x833DA7E4,0x1234);PPC_STORE_U32(owner+20,entity);
        PPC_STORE_U32(entity,0x8208D908);PPC_STORE_U32(entity+44,2);PPC_STORE_U32(entity+36,entity+128);
        PPC_STORE_U32(entity+132,component);PPC_STORE_U32(component,0x820D8970);PPC_STORE_U32(component+80,node);
        ObserveViewEntity(owner,entity,base);
    };
    bind_view(controller,0xC000,0xD000,0xE000);bind_view(second,0x12000,0x13000,0x14000);
    if(!check(IsGameplayViewCameraNode(0xE000,base) && IsGameplayViewCameraNode(0x14000,base) &&
        !IsGameplayViewCameraNode(0xF000,base),"both exact owned display views interpolate while unrelated shadow/reflection cameras remain native"))return false;
    for(unsigned guard=0;guard<6;++guard){
        setup();ResetGameplayViewCameras();bind_view(controller,0xC000,0xD000,0xE000);
        if(guard==0)PPC_STORE_U32(0x833DA7E4,0x5678);
        if(guard==1)PPC_STORE_U32(manager+28,second);
        if(guard==2)PPC_STORE_U32(controller+20,0xC100);
        if(guard==3)PPC_STORE_U32(0xD000+80,0xF000);
        if(guard==4)PPC_STORE_U32(0xC000+132,0xD100);
        if(guard==5)ResetGameplayViewCameras();
        if(!check(!IsGameplayViewCameraNode(0xE000,base),"load, controller, handle, native component/node replacement and reset invalidate view ownership"))return false;
    }
    REXLOG_INFO("Native offline gameplay projection passed {} normal FOV/control-mode comparisons, 13 ownership/script/callsite gates, invalid authored values, independent nested players, synchronized-session bypass and scope unwinding; actual rendered framing still needs gameplay acceptance",comparisons);
    return true;
}
