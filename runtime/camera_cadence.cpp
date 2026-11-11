#include "camera_cadence.h"
#include "camera_projection.h"
#include "camera_targets.h"
#include "camera_look.h"
#include "controls.h"
#include "guest_read_batch.h"
#include "native_engine.h"
#include "mouse_look.h"
#include "online.h"
#include "online_runtime.h"
#include "pc_settings.h"
#include "simulation.h"
#include <rex/cvar.h>
#include <rex/kernel/kernel_state.h>
#include <rex/input/input_system.h>
#include <rex/ui/window_win.h>
#include <rex/logging.h>
#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <immintrin.h>
#include <limits>
#include <optional>
#include <stdexcept>

PPC_EXTERN_FUNC(sub_82A166D8);
PPC_EXTERN_FUNC(sub_831D6708);
PPC_EXTERN_FUNC(sub_82732A18);
PPC_EXTERN_FUNC(__imp__sub_827B58D0);
PPC_EXTERN_FUNC(__imp__sub_82A69948);
PPC_EXTERN_FUNC(sub_8275C160);
PPC_EXTERN_FUNC(__imp__sub_82A5D278);
PPC_EXTERN_FUNC(__imp__sub_82769490);
REXCVAR_DEFINE_BOOL(em2_present_camera,false,"Debug","Experimental offline normal camera at presentation rate with native stick action processing");
REXCVAR_DEFINE_BOOL(em2_trace_camera_background,false,"Debug","Trace-only neutral background native camera cadence; requires present_camera and trace_camera and never samples or consumes input");
REXCVAR_DECLARE(bool,em2_trace_camera);

namespace {
struct TestAdmission {bool permitted=true,sample_available=true,background=false;em2::LiveLookInput input{};};
thread_local const TestAdmission* test_admission=nullptr;
enum class Admission {None,Focused,BackgroundTrace};
thread_local bool camera_input_allowed=true;
bool Readable(uint32_t address,size_t size,uint8_t* base){
    return address>=4096 && uint64_t(address)+size<=0x100000000ull && em2::GuestReadBatch::Readable(base+address,size);
}
float Float(uint32_t address,uint8_t* base){return std::bit_cast<float>(PPC_LOAD_U32(address));}
struct RestoreContext {
    PPCContext& ctx;PPCContext saved;unsigned csr=_mm_getcsr();DWORD error=GetLastError();
    explicit RestoreContext(PPCContext& context):ctx(context),saved(context){}
    ~RestoreContext(){ctx=saved;_mm_setcsr(csr);SetLastError(error);}
};
struct RestoreEnvironment {
    unsigned csr=_mm_getcsr();DWORD error=GetLastError();
    void Restore(){_mm_setcsr(csr);SetLastError(error);}
    void Capture(){csr=_mm_getcsr();error=GetLastError();}
    ~RestoreEnvironment(){Restore();}
};
struct Ownership {
    uint32_t manager=0,registry=0,player=0,controller=0,mode=0,camera_handle=0,camera_entity=0,camera_node=0;
    std::array<uint32_t,2> components{};
    bool operator==(const Ownership&)const=default;
};
std::optional<Ownership> InspectOwnership(uint32_t controller,uint8_t* base){
    em2::GuestReadBatch reads;
    const auto manager=PPC_LOAD_U32(0x833DAB60),profile=PPC_LOAD_U32(0x833DC87C),registry=PPC_LOAD_U32(0x833DA7E4);
    if(!Readable(manager,640,base) || !Readable(profile,28,base) || !registry ||
        PPC_LOAD_U32(profile+24)!=1 || PPC_LOAD_U32(manager+28)!=controller ||
        !Readable(controller,960,base) || !em2::IsNormalGameplayCamera(controller,base))return {};
    // The parent controller has independent output override paths after its
    // normal-camera call. They retain the original simulation cadence.
    for(unsigned offset:{452u,568u,576u,578u,952u})if(PPC_LOAD_U8(controller+offset))return {};
    const auto mode=PPC_LOAD_U32(controller+24);
    if(!Readable(mode,2816,base))return {};
    return Ownership{manager,registry,PPC_LOAD_U32(manager+20),controller,mode,PPC_LOAD_U32(controller+20)};
}
bool InspectCameraEntity(Ownership& owner,PPCContext& ctx,uint8_t* base){
    RestoreContext restore(ctx);
    const auto scratch=(ctx.r1.u32-4096)&~15u;
    if(!Readable(scratch,4096,base))return false;
    ctx.r1.u64=scratch;ctx.r3.u64=owner.registry;ctx.r4.u64=owner.controller+20;
    // Same handle resolution as the controller's output path.
    sub_82732A18(ctx,base);
    const auto entity=ctx.r3.u32;
    em2::GuestReadBatch reads;
    if(!Readable(entity,92,base) || PPC_LOAD_U32(entity)!=0x8208D908 || PPC_LOAD_U32(entity+44)!=2)return false;
    const auto table=PPC_LOAD_U32(entity+36);
    if(!Readable(table,8,base))return false;
    const std::array<uint32_t,2> vtables{0x82090340,0x820D8970},updates{0x827D8778,0x8290CC70};
    std::array<uint32_t,2> components{};
    for(unsigned i=0;i<2;++i){
        components[i]=PPC_LOAD_U32(table+i*4);
        if(!Readable(components[i],144,base) || PPC_LOAD_U32(components[i])!=vtables[i] ||
            PPC_LOAD_U32(vtables[i]+40)!=updates[i])return false;
    }
    const auto node=PPC_LOAD_U32(components[1]+80);
    if(!Readable(node,512,base))return false;
    // Both audited callbacks ignore the saved input context pointers passed by
    // 827C9308. The second callback obtains this exact native NiCamera and
    // constructs its own update process through 82552F00.
    owner.camera_entity=entity;owner.camera_node=node;owner.components=components;
    return true;
}
bool Focused(){
    auto* kernel=rex::kernel::kernel_state();
    auto* input=kernel?kernel->input_system():nullptr;
    auto* window=input?input->window():nullptr;
    if(!window || !window->HasFocus())return false;
    const auto hwnd=static_cast<rex::ui::Win32Window*>(window)->hwnd();
    return hwnd && GetForegroundWindow()==hwnd && !IsIconic(hwnd);
}
bool TraceBackgroundAllowed(bool diagnostic,bool trace,bool present,bool pending,const std::array<uint32_t,4>& cached){
    return diagnostic && trace && present && !pending && std::all_of(cached.begin(),cached.end(),[](uint32_t value){return value==0;});
}
Admission AdmitCamera(uint32_t mode,uint8_t* base){
    if(test_admission)return test_admission->permitted?(test_admission->background?Admission::BackgroundTrace:Admission::Focused):Admission::None;
    if(PPC_LOAD_U8(0x833FCA86) || em2::ControlBindingsOpen() ||
        em2::net::GameplayInputGate().active() || em2::net::NativeOnlineTiming().fixed_step ||
        !em2::NativeConsoleWorldReady())return Admission::None;
    if(Focused())return Admission::Focused;
    const std::array<uint32_t,4> cached={PPC_LOAD_U32(mode+268),PPC_LOAD_U32(mode+272),PPC_LOAD_U32(mode+280),PPC_LOAD_U32(mode+284)};
    return TraceBackgroundAllowed(REXCVAR_GET(em2_trace_camera_background),REXCVAR_GET(em2_trace_camera),REXCVAR_GET(em2_present_camera),
        em2::MouseLookHasPendingMotion(),cached)?Admission::BackgroundTrace:Admission::None;
}
constexpr std::array<unsigned,14> discrete_look_ids{368,372,384,388,392,396,400,404,2352,2356,2360,2364,2368,2372};
bool GenericLookAction(uint32_t mode,uint32_t action,uint8_t* base){
    if(!Readable(mode,2376,base) || !Readable(action,4,base))return false;
    const auto controller=PPC_LOAD_U32(mode+20),manager=PPC_LOAD_U32(0x833DAB60);
    if(!Readable(controller,20,base) || PPC_LOAD_U32(controller+16)!=0 || !Readable(manager,712,base))return false;
    PPCContext selected{};selected.r3.u64=manager;selected.r4.u64=0;
    __imp__sub_82769490(selected,base);
    const auto id=PPC_LOAD_U32(action);
    if(!Readable(selected.r3.u32,4,base) || id!=PPC_LOAD_U32(selected.r3.u32))return false;
    // These named button actions have discrete mode changes and authored
    // per-dispatch pitch steps. Only the continuous look action may run here.
    for(unsigned offset:discrete_look_ids)
        if(id==PPC_LOAD_U32(mode+offset))return false;
    return true;
}
struct FreshLook {
    em2::LiveLookInput input;
    std::optional<em2::NativeCameraLookBinding> binding;
};
bool SampleFreshLook(uint32_t mode,uint8_t* base,FreshLook& result){
    result={};
    if(test_admission){
        if(!test_admission->sample_available)return false;
        result.input=test_admission->input;
    }else{
        auto* kernel=rex::kernel::kernel_state();auto* input=kernel?kernel->input_system():nullptr;
        if(!input || !em2::SampleLiveLook(*input,0,result.input))return false;
    }
    em2::NativeCameraLookBinding binding;
    if(em2::NativeCameraLookBindingFor(mode,base,binding) && GenericLookAction(mode,binding.action,base)){
        result.binding=binding;return true;
    }
    // Cursor-style Rotation has native event generation on the world clock.
    // Continue integrating that untouched cached intent at the view rate;
    // never substitute the differently curved RotationFixed stick action or
    // switch the camera clock just because the physical stick is nonzero.
    return true;
}
void DispatchFreshLook(PPCContext& ctx,uint8_t* base,uint32_t mode,const em2::NativeCameraLookResult& look,
                       PPCFunc* listener=__imp__sub_82A5D278){
    RestoreContext restore(ctx);
    ctx.r3.u64=mode;ctx.r4.u64=look.event;ctx.r5.u64=look.action;
    ctx.f1.f64=double(look.x);ctx.f2.f64=double(look.y);
    // Keep native user selection, scripted restrictions and activity handling.
    // The continuous-action guard excludes the handler's discrete pitch steps.
    listener(ctx,base);
}
struct Cadence {
    bool allowed=false,ready=false,replaying=false;
    uint64_t frame=0,seed_frame=0,rendered_frame=0,target_tick=0;
    double elapsed=0,last_absolute=0;
    Ownership owner;
    std::array<uint64_t,2> arguments{};
    PPCFunc* update=nullptr;
    em2::CameraTargetHistory targets;
    em2::CameraTargetPose target;
};
thread_local Cadence cadence;
void ClearOwnership(){
    cadence.ready=false;cadence.owner={};cadence.update=nullptr;cadence.targets.Reset();cadence.target={};
    cadence.arguments={};cadence.target_tick=0;cadence.last_absolute=0;
}
bool CurrentOwnership(PPCContext& ctx,uint8_t* base){
    const auto inspected=InspectOwnership(cadence.owner.controller,base);
    if(!inspected)return false;
    auto owner=*inspected;
    return InspectCameraEntity(owner,ctx,base) && owner==cadence.owner;
}
bool ReadTarget(PPCContext& ctx,uint8_t* base,em2::CameraTargetPose& target){
    RestoreContext restore(ctx);
    const auto scratch=(ctx.r1.u32-4096)&~15u;
    if(!Readable(scratch,4096,base))return false;
    ctx.r1.u64=scratch;ctx.r3.u64=cadence.owner.mode+64;sub_82A166D8(ctx,base);
    const auto source=ctx.r3.u32;
    if(!Readable(source,88,base))return false;
    const auto handle=PPC_LOAD_U32(source+84);
    ctx.r3.u64=source;sub_831D6708(ctx,base);
    const auto entity=ctx.r3.u32;
    em2::GuestReadBatch reads;
    if(!Readable(entity,92,base) || !handle)return false;
    const auto types=PPC_LOAD_U32(entity+88),components=PPC_LOAD_U32(entity+36);
    const auto type=PPC_LOAD_U8(0x833DBC3C);
    if(!Readable(types,unsigned(type)+1,base))return false;
    const auto slot=PPC_LOAD_U8(types+type);
    if(slot==255 || !Readable(components,unsigned(slot)*4+4,base))return false;
    const auto entry=PPC_LOAD_U32(components+unsigned(slot)*4);
    if(entry<8 || !Readable(entry-8,184,base))return false;
    target={cadence.owner.registry,handle,entity,entry-8,{}};
    for(unsigned axis=0;axis<3;++axis){target.position[axis]=Float(target.component+172+axis*4,base);if(!std::isfinite(target.position[axis]))return false;}
    // Physical integration completes after the actor's scenegraph update.
    // Follow the owned visible root's actual endpoints, not next-tick physics.
    return em2::ReadCameraVisualTarget(target,base);
}
class CameraClockScope {
    uint8_t* base_;uint32_t delta_;
public:
    CameraClockScope(uint8_t* base,float delta):base_(base),delta_(PPC_LOAD_U32(0x833FCA54)){
        PPC_STORE_U32(0x833FCA54,std::bit_cast<uint32_t>(delta));
    }
    ~CameraClockScope(){auto* base=base_;PPC_STORE_U32(0x833FCA54,delta_);}
};
void BeginFrame(bool requested,bool fixed,int cap,double elapsed){
    ++cadence.frame;
    cadence.allowed=requested && fixed && (cap==0 || cap>30) && std::isfinite(elapsed) && elapsed>0 && elapsed<=.1;
    cadence.elapsed=elapsed;
    if(!cadence.allowed)ClearOwnership();
}
}

void em2::BeginCameraPresentationFrame(bool fixed,double elapsed_seconds){
    BeginFrame(REXCVAR_GET(em2_present_camera),fixed,FrameLimit(),elapsed_seconds);
}
void em2::ResetCameraPresentation(){cadence={};ResetNativeCameraLookBindings();}
bool em2::HandleCameraPresentationUpdate(PPCContext& ctx,uint8_t* base,PPCFunc* native_update){
    if(cadence.replaying || !cadence.allowed || ctx.lr!=0x82A14CE0)return false;
    RestoreEnvironment environment;
    const auto inspected=InspectOwnership(ctx.r3.u32,base);
    const auto admission=inspected?AdmitCamera(inspected->mode,base):Admission::None;
    FreshLook look;
    if(!inspected || admission==Admission::None || (admission==Admission::Focused && !SampleFreshLook(inspected->mode,base,look))){
        if(cadence.ready && cadence.owner.controller==ctx.r3.u32)ClearOwnership();
        return false;
    }
    auto owner=*inspected;
    if(!InspectCameraEntity(owner,ctx,base))return false;
    if(cadence.ready && owner==cadence.owner){
        cadence.arguments={ctx.r5.u64,ctx.r6.u64};cadence.update=native_update;
        return true;
    }
    ClearOwnership();
    cadence.owner=owner;cadence.arguments={ctx.r5.u64,ctx.r6.u64};cadence.update=native_update;
    // One complete original call establishes native mode state, FOV and node
    // outputs before any simulation update is deferred.
    environment.Restore();
    try{native_update(ctx,base);}catch(...){ClearOwnership();throw;}
    environment.Capture();
    if(!CurrentOwnership(ctx,base)){ClearOwnership();return true;}
    cadence.ready=true;cadence.seed_frame=cadence.frame;
    cadence.last_absolute=Float(0x833FCA4C,base);
    em2::NativeCameraLookBinding binding;
    const bool bound=em2::NativeCameraLookBindingFor(owner.mode,base,binding);
    const bool continuous=bound && GenericLookAction(owner.mode,binding.action,base);
    PPCContext selected{};selected.r3.u64=owner.manager;selected.r4.u64=0;__imp__sub_82769490(selected,base);
    const auto selected_id=PPC_LOAD_U32(selected.r3.u32),action_id=bound?PPC_LOAD_U32(binding.action):0;
    const auto slot=PPC_LOAD_U32(owner.controller+16);
    uint32_t actual_selected=0;
    if(slot<=1 || slot==UINT32_MAX){selected.r3.u64=owner.manager;selected.r4.u64=slot;__imp__sub_82769490(selected,base);actual_selected=PPC_LOAD_U32(selected.r3.u32);}
    unsigned discrete=0;for(auto offset:discrete_look_ids)if(bound && action_id==PPC_LOAD_U32(owner.mode+offset)){discrete=offset;break;}
    REXLOG_INFO("Experimental native camera seeded: controller {:08X}, mode {:08X}, camera {:08X}, node {:08X}; native input bound {}, continuous {}, action {:08X} UID {:08X}, selected UID {:08X}, discrete offset {}, native slot {}, actual selected UID {:08X}",owner.controller,owner.mode,owner.camera_entity,owner.camera_node,bound,continuous,binding.action,action_id,selected_id,discrete,int32_t(slot),actual_selected);
    return true;
}
void em2::CaptureCameraPresentationTarget(PPCContext& ctx,uint8_t* base,uint64_t tick){
    if(!cadence.ready || !cadence.allowed)return;
    CameraTargetPose target;
    if(!CurrentOwnership(ctx,base) || !ReadTarget(ctx,base,target)){ClearOwnership();return;}
    double distance_squared=0;
    for(unsigned axis=0;axis<3;++axis){const double distance=double(target.position[axis])-cadence.target.position[axis];distance_squared+=distance*distance;}
    // Large relocation never sweeps the camera through intermediate geometry.
    // Normal target changes are independently invalidated by full handle and
    // component identity in CameraTargetHistory.
    cadence.targets.Capture(target,tick,distance_squared>32.*32.);
    cadence.target=target;cadence.target_tick=tick;
}
bool em2::PresentationOwnsCameraNode(uint32_t object){
    return cadence.allowed && cadence.ready && object==cadence.owner.camera_node;
}
bool em2::CameraPresentationInputAllowed(){return camera_input_allowed;}
void em2::RenderPresentationCamera(PPCContext& ctx,uint8_t* base,double alpha){
    if(!cadence.allowed || !cadence.ready || cadence.replaying || cadence.rendered_frame==cadence.frame ||
        cadence.seed_frame==cadence.frame)return;
    RestoreContext restore(ctx);
    if(!CurrentOwnership(ctx,base)){ClearOwnership();return;}
    const auto admission=AdmitCamera(cadence.owner.mode,base);
    FreshLook look;
    if(admission==Admission::None || (admission==Admission::Focused && !SampleFreshLook(cadence.owner.mode,base,look)) ||
        !cadence.targets.Sample(cadence.target,cadence.target_tick,alpha)){
        ClearOwnership();return;
    }
    const double scale=Float(0x833C01C0,base),scene=Float(0x833FCA4C,base);
    const double delta=cadence.elapsed*scale*double(EffectiveSimulationSpeedPercent())/100.;
    const double absolute=scene+alpha*scale/30.;
    if(!std::isfinite(delta) || delta<=0 || delta>.25 || !std::isfinite(absolute) || absolute<cadence.last_absolute){ClearOwnership();return;}
    cadence.rendered_frame=cadence.frame;
    CameraClockScope clock(base,float(delta));
    CameraTargetQueryScope target(cadence.targets,cadence.target,cadence.target_tick,alpha);
    struct ReplayScope {
        bool previous=camera_input_allowed;
        explicit ReplayScope(bool allow_input){cadence.replaying=true;camera_input_allowed=allow_input;}
        ~ReplayScope(){cadence.replaying=false;camera_input_allowed=previous;}
    }replay(admission==Admission::Focused);
    if(look.binding){
        em2::NativeCameraLookResult action;
        try{
            if(!ReplayNativeCameraLookAction(*look.binding,look.input,ctx,base,action)){ClearOwnership();return;}
            DispatchFreshLook(ctx,base,cadence.owner.mode,action);
        }catch(...){ClearOwnership();throw;}
        // A native input gate can change modes or detach a camera. Never run
        // the saved controller on that replaced ownership after dispatch.
        if(!CurrentOwnership(ctx,base)){ClearOwnership();return;}
    }
    // All current native physics and script state stays in the live render
    // context. In particular, never resurrect the simulation call's stack/TLS.
    ctx.r3.u64=cadence.owner.controller;ctx.r5.u64=cadence.arguments[0];ctx.r6.u64=cadence.arguments[1];ctx.f1.f64=absolute;
    // Native OnLook owns its caches; mouse counts continue through the timing
    // wrapper once during this real controller update.
    try{cadence.update(ctx,base);}catch(...){ClearOwnership();throw;}
    cadence.last_absolute=absolute;
}

namespace {
struct CameraFixture {
    unsigned calls=0,nested_calls=0;
    uint32_t stack=0,tls=0;
    double delta_sum=0,last_time=-1;
    float target_x=0;
    bool monotonic=true,throwing=false,nested=false,input_allowed=true;
};
thread_local CameraFixture camera_fixture;
void FixtureCameraUpdate(PPCContext& ctx,uint8_t* base){
    auto& fixture=camera_fixture;
    ++fixture.calls;fixture.stack=ctx.r1.u32;fixture.tls=ctx.r13.u32;
    fixture.input_allowed=em2::CameraPresentationInputAllowed();
    fixture.delta_sum+=Float(0x833FCA54,base);
    fixture.monotonic&=ctx.f1.f64>=fixture.last_time;fixture.last_time=ctx.f1.f64;
    const auto saved=ctx;
    ctx.r3.u64=0xA000;ctx.r4.u64=ctx.r1.u32+512;sub_8275C160(ctx,base);
    fixture.target_x=Float(saved.r1.u32+512,base);ctx=saved;
    if(fixture.nested){++fixture.nested_calls;em2::RenderPresentationCamera(ctx,base,.5);}
    ctx.r3.u64=0x12345678;ctx.f1.f64=123;ctx.lr=0x11111111;
    _mm_setcsr(0x5F80);SetLastError(0x9876);
    if(fixture.throwing)throw std::runtime_error("injected native camera failure");
}
}
bool em2::TestCameraPresentation(){
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
    struct Restore {
        uint8_t* base;Cadence saved=cadence;const TestAdmission* admission=test_admission;
        CameraFixture fixture=camera_fixture;unsigned csr=_mm_getcsr();DWORD error=GetLastError();
        ~Restore(){cadence=saved;test_admission=admission;camera_fixture=fixture;_mm_setcsr(csr);SetLastError(error);if(base)VirtualFree(base,0,MEM_RELEASE);}
    }restore{base};
    if(!base || !VirtualAlloc(base,0x40000,MEM_COMMIT,PAGE_READWRITE) ||
        !VirtualAlloc(base+0x82000000,0x10000,MEM_COMMIT,PAGE_READWRITE) ||
        !VirtualAlloc(base+0x82090000,0x10000,MEM_COMMIT,PAGE_READWRITE) ||
        !VirtualAlloc(base+0x820D0000,0x10000,MEM_COMMIT,PAGE_READWRITE) ||
        !VirtualAlloc(base+0x833C0000,0x40000,MEM_COMMIT,PAGE_READWRITE) ||
        !VirtualAlloc(base+PPC_IMAGE_BASE+PPC_IMAGE_SIZE,4096,MEM_COMMIT,PAGE_READWRITE))return false;
    unsigned checks=0;
    auto check=[&](bool condition,const char* reason){++checks;if(!condition)REXLOG_ERROR("Presentation camera test: {}",reason);return condition;};
    constexpr uint32_t manager=0x2400,profile=0x2000,controller=0x3000,mode=0x4000,entity=0x5000;
    constexpr uint32_t registry=0x10000,buckets=0x11000,code=uint32_t(PPC_CODE_BASE);
    PPC_LOOKUP_FUNC(base,code)=__imp__sub_827B58D0;PPC_LOOKUP_FUNC(base,code+4)=__imp__sub_82A69948;
    auto setup=[&]{
        std::memset(base,0,0x40000);cadence={};camera_fixture={};
        PPC_STORE_U32(0x833DAB60,manager);PPC_STORE_U32(0x833DC87C,profile);PPC_STORE_U32(0x833DA7E4,registry);
        PPC_STORE_U32(profile+24,1);PPC_STORE_U32(manager+20,3);PPC_STORE_U32(manager+28,controller);
        PPC_STORE_U32(controller,0x82112778);PPC_STORE_U32(controller+20,1);PPC_STORE_U32(controller+24,mode);
        PPC_STORE_U32(mode,0x82126C98);PPC_STORE_U32(mode+20,controller);
        PPC_STORE_U32(entity,0x8208D908);PPC_STORE_U32(entity+36,0x5800);PPC_STORE_U32(entity+44,2);
        PPC_STORE_U32(0x5800,0x6008);PPC_STORE_U32(0x5804,0x6808);
        PPC_STORE_U32(0x6008,0x82090340);PPC_STORE_U32(0x6808,0x820D8970);
        PPC_STORE_U32(0x82090340+40,0x827D8778);PPC_STORE_U32(0x820D8970+40,0x8290CC70);PPC_STORE_U32(0x6808+80,0x8000);
        PPC_STORE_U32(registry+4,code);PPC_STORE_U32(registry+8,code+4);PPC_STORE_U32(registry+16,buckets);PPC_STORE_U32(registry+20,4);
        const auto key=0x13000u,node=0x12000u;PPC_STORE_U32(key,1);
        PPCContext hash{};hash.r3.u64=key;__imp__sub_827B58D0(hash,base);
        PPC_STORE_U32(node,1);PPC_STORE_U32(node+4,entity);PPC_STORE_U32(buckets+(hash.r3.u32%4)*4,node);
        PPC_STORE_U32(0xA000+88,0xC800);PPC_STORE_U32(0xA000+36,0xC900);PPC_STORE_U8(0xC802,0);PPC_STORE_U8(0x833DBC3C,2);
        PPC_STORE_U32(0xC900,0xC008);
        for(unsigned axis=0;axis<3;++axis)PPC_STORE_U32(0xC000+172+axis*4,std::bit_cast<uint32_t>(float(14+axis)));
        PPC_STORE_U32(0x82002060,0);PPC_STORE_U32(0x833FCA54,std::bit_cast<uint32_t>(1.f/30));
        PPC_STORE_U32(0x833FCA4C,std::bit_cast<uint32_t>(1.f));PPC_STORE_U32(0x833C01C0,std::bit_cast<uint32_t>(1.f));
    };
    TestAdmission admission;test_admission=&admission;setup();
    PPCContext update{};update.fpscr.InitHost();update.r1.u64=0x3E000;update.r13.u64=0x2345;
    update.r3.u64=controller;update.r5.u64=0x18000;update.r6.u64=0x19000;update.f1.f64=1;update.lr=0x82A14CE0;
    const auto original=update;
    PPC_STORE_U32(0xB000,0x12345678);
    PPC_STORE_U32(manager+704,0x12345678);
    if(!check(GenericLookAction(mode,0xB000,base),"continuous look identity is distinct from discrete camera buttons"))return false;
    PPC_STORE_U32(manager+44,2);PPC_STORE_U32(manager+708,0x87654321);
    if(!check(!GenericLookAction(mode,0xB000,base),"native alternate input selection rejects an inactive continuous action"))return false;
    PPC_STORE_U32(manager+708,0x12345678);
    if(!check(GenericLookAction(mode,0xB000,base),"continuous look follows the original engine's alternate input selector"))return false;
    PPC_STORE_U32(manager+44,0);PPC_STORE_U32(controller+16,1);
    if(!check(!GenericLookAction(mode,0xB000,base),"unverified secondary-user action remapping retains native cadence"))return false;
    PPC_STORE_U32(controller+16,0);
    for(unsigned offset:discrete_look_ids){
        PPC_STORE_U32(mode+offset,0x12345678);
        if(!check(!GenericLookAction(mode,0xB000,base),"discrete pitch/mode actions cannot run at presentation rate"))return false;
        PPC_STORE_U32(mode+offset,0);
    }
    {
        const auto saved=update;const auto csr=_mm_getcsr();const auto error=GetLastError();
        em2::NativeCameraLookResult look{.25f,-.75f,0xB100,0xB000};
        DispatchFreshLook(update,base,mode,look,+[](PPCContext& call,uint8_t* base){
            PPC_STORE_U32(0xB200,call.r3.u32);PPC_STORE_U32(0xB204,call.r4.u32);PPC_STORE_U32(0xB208,call.r5.u32);
            PPC_STORE_U32(0xB20C,std::bit_cast<uint32_t>(float(call.f1.f64)));PPC_STORE_U32(0xB210,std::bit_cast<uint32_t>(float(call.f2.f64)));
            call.r3.u64=0xDEADBEEF;call.f1.f64=99;SetLastError(123);_mm_setcsr(_mm_getcsr()^0x2000);
        });
        if(!check(PPC_LOAD_U32(0xB200)==mode && PPC_LOAD_U32(0xB204)==look.event && PPC_LOAD_U32(0xB208)==look.action &&
            Float(0xB20C,base)==look.x && Float(0xB210,base)==look.y && !std::memcmp(&update,&saved,sizeof(update)) &&
            _mm_getcsr()==csr && GetLastError()==error,"normal look handler receives current mode/event/action and native axes without leaking caller/FP/environment changes"))return false;
        try{DispatchFreshLook(update,base,mode,look,+[](PPCContext& call,uint8_t*){call.r3.u64=99;SetLastError(123);throw std::runtime_error("look dispatch unwind");});return false;}
        catch(const std::runtime_error&){}
        if(!check(!std::memcmp(&update,&saved,sizeof(update)) && _mm_getcsr()==csr && GetLastError()==error,"normal input handler exceptions restore the complete render caller"))return false;
    }
    constexpr std::array<unsigned,5> parent_overrides{452,568,576,578,952};
    for(unsigned guard=0;guard<11;++guard){
        setup();
        if(guard==0)PPC_STORE_U32(profile+24,2);
        if(guard==1)PPC_STORE_U32(manager+28,controller+4);
        if(guard==2)PPC_STORE_U32(mode,0);
        if(guard==3)PPC_STORE_U32(mode+20,controller+4);
        if(guard>=4 && guard<=8)PPC_STORE_U8(controller+parent_overrides[guard-4],1);
        if(guard==9)PPC_STORE_U32(controller+28,0xB000);
        if(guard==10)PPC_STORE_U32(manager+32,1);
        if(!check(!InspectOwnership(controller,base),"scripted, pending, secondary, co-op and output-override camera modes stay native"))return false;
    }
    setup();auto owner=InspectOwnership(controller,base);
    if(!check(owner && InspectCameraEntity(*owner,update,base) && owner->camera_node==0x8000,"actual native registry resolver admits the audited two-component camera entity"))return false;
    for(unsigned guard=0;guard<5;++guard){
        setup();owner=InspectOwnership(controller,base);
        if(guard==0)PPC_STORE_U32(entity+44,3);
        if(guard==1)PPC_STORE_U32(0x6008,0x820D8970);
        if(guard==2)PPC_STORE_U32(0x82090340+40,0x8290CC70);
        if(guard==3)PPC_STORE_U32(0x6808+80,0);
        if(guard==4)PPC_STORE_U32(entity,0x8208D900);
        if(!check(owner && !InspectCameraEntity(*owner,update,base),"additional components and changed callbacks/nodes cannot replay saved context arguments"))return false;
    }
    setup();
    for(unsigned guard=0;guard<8;++guard){
        std::array<uint32_t,4> cached{};
        if(guard>=4)cached[guard-4]=0x3F000000;
        if(!check(!TraceBackgroundAllowed(guard!=0,guard!=1,guard!=2,guard==3,cached),
            "background diagnostics require explicit opt-in, tracing, cadence, no pending mouse counts and four exact neutral axes"))return false;
    }
    if(!check(TraceBackgroundAllowed(true,true,true,false,{}),"neutral traced background cadence is admitted without input sampling"))return false;
    BeginFrame(true,true,60,1./60);update.lr=0x12345678;
    if(!check(!HandleCameraPresentationUpdate(update,base,FixtureCameraUpdate) && camera_fixture.calls==0,"foreign native controller callsites stay synchronous"))return false;
    update=original;
    for(unsigned fps:{40u,60u,90u,120u,144u,240u}){
        setup();update=original;BeginFrame(true,true,int(fps),1./fps);
        if(!check(HandleCameraPresentationUpdate(update,base,FixtureCameraUpdate) && cadence.ready && camera_fixture.calls==1,"one complete native bootstrap runs before deferral"))return false;
        CameraTargetPose target{registry,3,0xA000,0xC000,{10,20,30}};
        cadence.targets.Capture(target,1);target.position={14,24,34};cadence.targets.Capture(target,2);cadence.target=target;cadence.target_tick=2;
        RenderPresentationCamera(update,base,0);
        if(!check(camera_fixture.calls==1,"bootstrap frame does not advance the camera twice"))return false;
        for(unsigned frame=1;frame<=fps;++frame){
            BeginFrame(true,true,int(fps),1./fps);update=original;
            const double time=1.+double(frame)/fps;
            const unsigned tick=unsigned(std::floor(double(frame)*30/fps));
            const double phase=double(frame)*30/fps-tick;
            PPC_STORE_U32(0x833FCA4C,std::bit_cast<uint32_t>(float(1.+double(tick)/30)));
            if(frame%2==0 && !check(HandleCameraPresentationUpdate(update,base,FixtureCameraUpdate) && camera_fixture.calls==frame,"world update defers without a duplicate native camera call"))return false;
            update.r1.u64=0x3D000;update.r13.u64=0x6789;
            const auto saved=update;const auto csr=_mm_getcsr();const auto error=GetLastError();const auto delta=PPC_LOAD_U32(0x833FCA54);
            RenderPresentationCamera(update,base,phase);RenderPresentationCamera(update,base,phase);
            if(!check(camera_fixture.calls==frame+1 && camera_fixture.stack==0x3D000 && camera_fixture.tls==0x6789 &&
                !std::memcmp(&saved,&update,sizeof(update)) && _mm_getcsr()==csr && GetLastError()==error && PPC_LOAD_U32(0x833FCA54)==delta,
                "each presentation runs once with the current stack/TLS and restores full caller/frame clock state"))return false;
            if(!check(std::abs(camera_fixture.last_time-time)<2e-6 && std::abs(camera_fixture.target_x-float(10+4*phase))<1e-5,
                "native timestamp is monotonic and camera target matches the character render phase"))return false;
        }
        if(!check(camera_fixture.monotonic && std::abs(camera_fixture.delta_sum-(1.+1./30))<2e-5,"all tested presentation rates integrate one second without changing the 30-Hz world"))return false;
    }
    // Admission/reset and exceptional calls use the same public path as the
    // rendering wrapper, with injected host focus/input permission only.
    setup();update=original;BeginFrame(true,true,60,1./60);HandleCameraPresentationUpdate(update,base,FixtureCameraUpdate);
    CameraTargetPose target{registry,3,0xA000,0xC000,{14,15,16}};
    cadence.targets.Capture(target,1);cadence.target=target;cadence.target_tick=1;
    BeginFrame(true,true,60,1./60);camera_fixture.nested=true;update=original;
    RenderPresentationCamera(update,base,.5);
    if(!check(camera_fixture.calls==2 && camera_fixture.nested_calls==1 && !cadence.replaying,"nested renders cannot advance the native camera again"))return false;
    BeginFrame(true,true,60,1./60);camera_fixture.throwing=true;update=original;
    const auto saved=update;const auto csr=_mm_getcsr(),delta=PPC_LOAD_U32(0x833FCA54);const auto error=GetLastError();
    try{RenderPresentationCamera(update,base,.75);return false;}catch(const std::runtime_error&){}
    if(!check(!cadence.ready && !cadence.replaying && !std::memcmp(&saved,&update,sizeof(update)) && _mm_getcsr()==csr && GetLastError()==error && PPC_LOAD_U32(0x833FCA54)==delta,
        "native callback failure restores caller, clock and recursion scopes and drops retained ownership"))return false;
    setup();admission.background=true;admission.sample_available=false;update=original;
    BeginFrame(true,true,60,1./60);HandleCameraPresentationUpdate(update,base,FixtureCameraUpdate);
    cadence.targets.Capture(target,1);cadence.target=target;cadence.target_tick=1;
    BeginFrame(true,true,60,1./60);RenderPresentationCamera(update,base,.5);
    if(!check(camera_fixture.calls==2 && !camera_fixture.input_allowed && CameraPresentationInputAllowed(),
        "traced neutral background replay bypasses SDK sampling and scopes mouse consumption off without leaking that state"))return false;
    admission.background=false;admission.sample_available=true;
    setup();admission.input={25000,-12000};update=original;
    const std::array<uint32_t,4> cached_intent{0x3E800000,0xBF000000,0x3F400000,0xBF800000};
    unsigned cache_index=0;for(unsigned offset:{268u,272u,280u,284u})PPC_STORE_U32(mode+offset,cached_intent[cache_index++]);
    FreshLook sampled;
    if(!check(SampleFreshLook(mode,base,sampled) && sampled.input==admission.input && !sampled.binding,
        "a valid nonzero pad sample keeps native cursor-style input when no fresh direct-stick action is eligible"))return false;
    BeginFrame(true,true,60,1./60);HandleCameraPresentationUpdate(update,base,FixtureCameraUpdate);
    cadence.targets.Capture(target,1);cadence.target=target;cadence.target_tick=1;
    for(unsigned frame=1;frame<=60;++frame){
        BeginFrame(true,true,60,1./60);PPC_STORE_U32(0x833FCA4C,std::bit_cast<uint32_t>(float(1.+double(frame)/60)));
        update=original;RenderPresentationCamera(update,base,.5);
    }
    cache_index=0;bool caches_unchanged=true;
    for(unsigned offset:{268u,272u,280u,284u})caches_unchanged&=PPC_LOAD_U32(mode+offset)==cached_intent[cache_index++];
    if(!check(cadence.ready && camera_fixture.calls==61 && caches_unchanged,
        "cursor-style pad movement retains 60 native camera updates without erasing, remapping or redispatching cached native look input"))return false;
    admission.input={};
    for(unsigned guard=0;guard<7;++guard){
        setup();update=original;BeginFrame(true,true,60,1./60);HandleCameraPresentationUpdate(update,base,FixtureCameraUpdate);
        if(guard==0)BeginFrame(false,true,60,1./60);
        if(guard==1)BeginFrame(true,false,60,1./60);
        if(guard==2)BeginFrame(true,true,30,1./60);
        if(guard==3)BeginFrame(true,true,60,.3);
        if(guard==4)BeginFrame(true,true,60,0);
        if(guard==5){admission.permitted=false;BeginFrame(true,true,60,1./60);RenderPresentationCamera(update,base,.5);admission.permitted=true;}
        if(guard==6){admission.sample_available=false;BeginFrame(true,true,60,1./60);RenderPresentationCamera(update,base,.5);admission.sample_available=true;}
        if(!check(!cadence.ready && !PresentationOwnsCameraNode(0x8000),"disable, native timing, cap, stall, pause, focus and unavailable input samples release camera ownership"))return false;
    }
    REXLOG_INFO("Native presentation camera passed {} ownership/roster, actual registry, 40/60/90/120/144/240-Hz cadence, target-phase, timestamp, native look dispatch, discrete-action exclusion, caller/clock, nesting and unwind checks; focused input feel still requires gameplay validation",checks);
    return true;
}
