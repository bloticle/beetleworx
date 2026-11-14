#include "ppc_context.h"
#include "camera_timing.h"
#include "guest_read_batch.h"
#include "physics_timing.h"
#include <chrono>
#include <algorithm>
#include "camera_projection.h"
#include "camera_cadence.h"
#include "camera_look.h"
#include "pc_settings.h"
#include "online.h"
#include "online_runtime.h"
#include "online_room_gameplay.h"
#include "controls.h"
#include "mouse_look.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <immintrin.h>
#include <limits>
#include <optional>
#include <windows.h>

PPC_EXTERN_FUNC(__imp__sub_83207330);
PPC_EXTERN_FUNC(__imp__sub_82A515D0);
PPC_EXTERN_FUNC(__imp__sub_82A517D0);
PPC_EXTERN_FUNC(__imp__sub_82A628A0);
PPC_EXTERN_FUNC(__imp__sub_82A5D278);
PPC_EXTERN_FUNC(__imp__sub_831C6930);
PPC_EXTERN_FUNC(__imp__sub_831C6878);
PPC_EXTERN_FUNC(EM2MousePitch);
PPC_EXTERN_FUNC(EM2TimedPitch);
REXCVAR_DECLARE(bool,em2_trace_camera);
REXCVAR_DEFINE_BOOL(em2_camera_timing,true,"Debug","Use elapsed time for the native player-camera input filters");
REXCVAR_DEFINE_BOOL(em2_camera_spring_timing,true,"Debug","Use elapsed time for stable normal-camera pitch springs; reference-rate calls retain native behavior");
REXCVAR_DEFINE_BOOL(em2_camera_pitch_threshold_timing,true,"Debug","Scale native pitch input/velocity thresholds by elapsed time on owned offline cameras");

namespace {
float ReadFloat(uint32_t address,uint8_t* base) {return std::bit_cast<float>(PPC_LOAD_U32(address));}
void WriteFloat(uint32_t address,float value,uint8_t* base) {PPC_STORE_U32(address,std::bit_cast<uint32_t>(value));}
struct CameraWindow {
    uint32_t identity=0,mode=0,minimum5=UINT32_MAX,maximum5=0,minimum6=UINT32_MAX,maximum6=0;
    uint64_t calls=0,invalid=0,paused=0;double total=0,minimum=.25,maximum=0;
    uint64_t source_samples=0,source_invalid=0;
    double source_min=std::numeric_limits<double>::infinity(),source_max=-std::numeric_limits<double>::infinity();
    double offset_min=std::numeric_limits<double>::infinity(),offset_max=-std::numeric_limits<double>::infinity();
    std::chrono::steady_clock::time_point started{};
    void Add(double dt,uint32_t a,uint32_t b) {
        ++calls;minimum5=std::min(minimum5,a);maximum5=std::max(maximum5,a);
        minimum6=std::min(minimum6,b);maximum6=std::max(maximum6,b);
        if(!std::isfinite(dt) || dt<0 || dt>.25){++invalid;return;}
        total+=dt;minimum=std::min(minimum,dt);maximum=std::max(maximum,dt);paused+=dt==0;
    }
    void Source(double absolute,double scene){
        if(!std::isfinite(absolute) || !std::isfinite(scene)){++source_invalid;return;}
        ++source_samples;source_min=std::min(source_min,absolute);source_max=std::max(source_max,absolute);
        const auto offset=absolute-scene;offset_min=std::min(offset_min,offset);offset_max=std::max(offset_max,offset);
    }
};
struct SpringProbe {
    uint32_t mode=0,call=0,slot=0,stiffness=0,damping=0,step=0,carry=0;
    uint64_t interval=0;
};
std::optional<SpringProbe> IdentifySpring(const PPCContext& ctx,uint8_t* base) {
    const uint32_t offset=ctx.lr==0x82A51944 || ctx.lr==0x82A61308?2632:
        ctx.lr==0x82A51B24?1376:ctx.lr==0x82A51BC0?1356:0;
    const auto mode=ctx.r31.u32;
    if(!offset || !mode || ctx.r3.u32!=mode+offset || PPC_LOAD_U32(mode)!=0x82126C98)return {};
    const auto controller=PPC_LOAD_U32(mode+20);if(!controller || PPC_LOAD_U32(controller+24)!=mode)return {};
    const auto slot=PPC_LOAD_U32(controller+16);if(slot>=2)return {};
    return SpringProbe{mode,uint32_t(ctx.lr),slot,PPC_LOAD_U32(ctx.r3.u32),PPC_LOAD_U32(ctx.r3.u32+4),
        PPC_LOAD_U32(0x833C0230),PPC_LOAD_U32(ctx.r3.u32+12),ctx.f2.u64};
}
std::optional<SpringProbe> CaptureSpring(const PPCContext& ctx,uint8_t* base) {
    if(!REXCVAR_GET(em2_trace_camera) && !em2::NativeTimingDiagnosticsEnabled())return {};
    return IdentifySpring(ctx,base);
}
struct SpringHistory {
    uint32_t mode=0,spring=0,velocity=0,correction=0;
    uint64_t canonical_velocity=0;bool retimed=false;
};
thread_local std::array<std::array<SpringHistory,3>,2> spring_history;
thread_local std::array<unsigned,2> last_recenter{};
unsigned SpringIndex(uint32_t call){return call==0x82A51B24?1:call==0x82A51BC0?2:0;}
struct SpringResult {float correction,velocity;};
// Fractional power of the native recurrence M, rather than a pose blend:
// [error'; velocity'] = [[1-k, d-1], [k, 1-d]] [error; velocity].
// M^(elapsed/reference) advances state at the actual interval and matches the
// authored recurrence at each reference tick. Negative real poles cannot
// define this continuous real flow and retain the native implementation.
std::optional<SpringResult> AdvanceSpring(double error,double velocity,double k,double damping,double steps) {
    if(!std::isfinite(error) || !std::isfinite(velocity) || !std::isfinite(k) || !std::isfinite(damping) ||
       !std::isfinite(steps) || steps<0 || steps>16 || k<0 || damping<0)return {};
    if(steps==0)return SpringResult{0,float(velocity)};
    if(damping>1)return {};
    const double center=(2-k-damping)*.5,determinant=1-damping,discriminant=center*center-determinant;
    double a=0,b=0;
    if(discriminant>=0) {
        const double root=std::sqrt(discriminant),high=center+root,low=center-root;
        if(high<=0 || low<0 || high>1+1e-12)return {};
        if(root>1e-8*std::max(1.,std::abs(center))) {
            const double p=std::pow(high,steps),q=std::pow(low,steps);
            b=(p-q)/(2*root);a=(p+q)*.5-b*center;
        }else {const double p=std::pow(center,steps);b=steps*p/center;a=p-b*center;}
    }else {
        const double imaginary=std::sqrt(-discriminant),radius=std::sqrt(determinant);
        if(!(radius>0) || radius>1+1e-12)return {};
        const double angle=std::atan2(imaginary,center),power=std::pow(radius,steps);
        b=power*std::sin(steps*angle)/imaginary;a=power*std::cos(steps*angle)-b*center;
    }
    const double next_error=(a+b*(1-k))*error+b*(damping-1)*velocity;
    const double next_velocity=b*k*error+(a+b*(1-damping))*velocity;
    const float correction=float(error-next_error),result_velocity=float(next_velocity);
    if(!std::isfinite(correction) || !std::isfinite(result_velocity))return {};
    return SpringResult{correction,result_velocity};
}
bool SpringTimingAllowed(uint32_t mode,uint8_t* base) {
    if(!REXCVAR_GET(em2_camera_spring_timing))return false;
    return em2::IsNormalGameplayCamera(PPC_LOAD_U32(mode+20),base) &&
        !em2::net::GameplayInputGate().active() && !em2::net::NativeOnlineTiming().fixed_step;
}
bool SpringUpdate(PPCContext& ctx,uint8_t* base) {
    if(!REXCVAR_GET(em2_camera_spring_timing)){spring_history={};last_recenter={};__imp__sub_831C6930(ctx,base);return false;}
    const auto identified=IdentifySpring(ctx,base);
    if(!identified || !SpringTimingAllowed(identified->mode,base)) {
        if(identified){for(auto& entry:spring_history[identified->slot])if(entry.mode==identified->mode)entry={};last_recenter[identified->slot]=0;}
        __imp__sub_831C6930(ctx,base);return false;
    }
    const auto input=*identified;const auto spring=ctx.r3.u32;
    std::optional<SpringResult> result;
    const auto csr=_mm_getcsr();
    _mm_setcsr(csr&~unsigned(0x8040)); // Native scalar spring loads disable FTZ/DAZ.
    const double dt=std::bit_cast<double>(input.interval),reference=std::bit_cast<float>(input.step);
    const double carry=std::bit_cast<float>(input.carry),error=ctx.f1.f64,threshold=ctx.f3.f64;
    if(std::isfinite(dt) && dt>=0 && dt<=.25 && std::isfinite(reference) && reference>0 && reference<=.25 &&
       std::isfinite(carry) && carry>=0 && carry<reference && std::isfinite(threshold) && threshold>=0 && dt!=reference) {
        result=AdvanceSpring(error,ReadFloat(spring+8,base),std::bit_cast<float>(input.stiffness),std::bit_cast<float>(input.damping),dt==0?0:(dt+carry)/reference);
        if(result && dt>0 && std::abs(error)<threshold)*result={0,0};
    }
    _mm_setcsr(csr);
    // Keep the native call and its ABI clobbers. Only the final spring state and
    // returned displacement are replaced; no extra camera update is replayed.
    __imp__sub_831C6930(ctx,base);
    const auto native_csr=_mm_getcsr();
    if(result){WriteFloat(spring+8,result->velocity,base);PPC_STORE_U32(spring+12,input.carry && dt==0?input.carry:0);ctx.f1.f64=result->correction;}
    auto& history=spring_history[input.slot][SpringIndex(input.call)];
    history={input.mode,spring,PPC_LOAD_U32(spring+8),std::bit_cast<uint32_t>(float(ctx.f1.f64)),
        std::bit_cast<uint64_t>(double(ReadFloat(spring+8,base))),result.has_value()};
    if(const auto index=SpringIndex(input.call))last_recenter[input.slot]=index;
    _mm_setcsr(native_csr);
    return result.has_value();
}
void SpringVelocity(PPCContext& ctx,uint8_t* base) {
    const uint32_t call=ctx.lr==0x82A51B04?0x82A51B24:ctx.lr==0x82A51BA0?0x82A51BC0:0;
    if(call && REXCVAR_GET(em2_camera_spring_timing)) {
        const auto lr=ctx.lr;ctx.lr=call;const auto identified=IdentifySpring(ctx,base);ctx.lr=lr;
        if(identified && SpringTimingAllowed(identified->mode,base)) {
            const auto index=last_recenter[identified->slot];
            const auto& history=spring_history[identified->slot][index];
            // +1176 is shared by the positive and negative recenter branches.
            // Carry the latest branch's canonical velocity across sign changes.
            if(index && history.retimed && history.mode==identified->mode && history.spring &&
                history.velocity==PPC_LOAD_U32(history.spring+8)) {
                const auto csr=_mm_getcsr();const auto provided=std::bit_cast<uint32_t>(float(ctx.f1.f64));_mm_setcsr(csr);
                if(provided==history.correction)ctx.f1.u64=history.canonical_velocity;
            }
        }
    }
    __imp__sub_831C6878(ctx,base);
}
void ObserveSpring(const SpringProbe& probe,bool retimed)noexcept {
    struct Restore {unsigned csr=_mm_getcsr();~Restore(){_mm_setcsr(csr);}}restore;
    try {
        struct Window {
            uint32_t mode=0;uint64_t calls=0,zero=0,short_interval=0,invalid=0,retimed=0;
            double total=0,dt_min=.25,dt_max=0,k_min=INFINITY,k_max=-INFINITY,d_min=INFINITY,d_max=-INFINITY;
            double step=0,carry=0;std::chrono::steady_clock::time_point started{};
        };
        static thread_local std::array<std::array<Window,4>,2> windows;
        const unsigned index=probe.call==0x82A51944?0:probe.call==0x82A51B24?1:probe.call==0x82A51BC0?2:3;
        auto& window=windows[probe.slot][index];const auto now=std::chrono::steady_clock::now();
        if(window.mode!=probe.mode || !window.calls){window={};window.mode=probe.mode;window.started=now;}
        ++window.calls;window.retimed+=retimed;const auto dt=std::bit_cast<double>(probe.interval);
        const double k=std::bit_cast<float>(probe.stiffness),d=std::bit_cast<float>(probe.damping),step=std::bit_cast<float>(probe.step);
        if(std::isfinite(dt) && dt>=0 && dt<=.25 && std::isfinite(k) && std::isfinite(d) && std::isfinite(step) && step>0) {
            window.total+=dt;window.zero+=dt==0;window.short_interval+=dt>0 && dt<step;
            window.dt_min=std::min(window.dt_min,dt);window.dt_max=std::max(window.dt_max,dt);
            window.k_min=std::min(window.k_min,k);window.k_max=std::max(window.k_max,k);
            window.d_min=std::min(window.d_min,d);window.d_max=std::max(window.d_max,d);
            window.step=step;window.carry=std::bit_cast<float>(probe.carry);
        }else ++window.invalid;
        const auto elapsed=std::chrono::duration<double>(now-window.started).count();
        if(elapsed>=5) {
            REXLOG_INFO("Native camera spring mode {:08X} call {:08X} P{}: {} actual calls / {:.3f}s, supplied dt sum {:.6f}, dt {:.6f}..{:.6f}, zero {}, below reference {}, invalid {}, k {:.6f}..{:.6f}, damping {:.6f}..{:.6f}, reference step {:.6f}, last carried {:.6f}, corrected calls {}",
                probe.mode,probe.call,probe.slot+1,window.calls,elapsed,window.total,window.dt_min,window.dt_max,window.zero,window.short_interval,window.invalid,
                window.k_min,window.k_max,window.d_min,window.d_max,window.step,window.carry,window.retimed);
            window={};
        }
    }catch(...){}
}
bool TestCameraWindow() {
    struct Restore {unsigned csr=_mm_getcsr();~Restore(){_mm_setcsr(csr);}}restore;
    CameraWindow sample;
    for(unsigned i=0;i<450;++i)sample.Add(1./90,i&1,0x1000+i);
    sample.Add(0,0,0x2000);sample.Add(NAN,0,0);sample.Add(-1,0,0);
    sample.Source(20,19);sample.Source(21,20);sample.Source(NAN,20);
    const bool okay=sample.calls==453 && sample.invalid==2 && sample.paused==1 &&
        std::abs(sample.total-5)<1e-10 && sample.minimum==0 && sample.maximum==1./90 &&
        sample.minimum5==0 && sample.maximum5==1 && sample.minimum6==0 && sample.maximum6==0x2000 &&
        sample.source_samples==2 && sample.source_invalid==1 && sample.source_min==20 && sample.source_max==21 && sample.offset_min==1 && sample.offset_max==1;
    if(!okay)REXLOG_ERROR("Camera timing diagnostic accumulation failed");return okay;
}
struct FilterHistory {
    uint32_t mode=0;
    float interval=0;
    std::array<uint32_t,2> output{};
};
// These contain no owned guest references and are consulted only during a live
// native camera call. Constructor resets/other writes invalidate cached values.
thread_local std::array<std::array<FilterHistory,3>,2> filter_history;
struct MouseScope {uint32_t mode=0;float pitch=0;};
thread_local MouseScope mouse_scope;
struct PitchThresholdFactors {double input=1,velocity=1;bool native()const{return input==1 && velocity==1;}};
thread_local PitchThresholdFactors pitch_threshold_scale;
PitchThresholdFactors PitchThresholdScale(const PPCContext& ctx,uint8_t* base) {
    if(ctx.lr!=0x82A63098 || !REXCVAR_GET(em2_camera_pitch_threshold_timing))return {};
    const auto mode=ctx.r3.u32;
    if(!mode || ctx.r30.u32!=mode || PPC_LOAD_U32(mode)!=0x82126C98 || !em2::IsNormalGameplayCamera(PPC_LOAD_U32(mode+20),base) ||
       em2::net::GameplayInputGate().active() || em2::net::NativeOnlineTiming().fixed_step)return {};
    const auto csr=_mm_getcsr();struct Restore {unsigned csr;~Restore(){_mm_setcsr(csr);}}restore{csr};
    // Parent82A628A0 retains its actual camera interval in f30. The recenter
    // spring separately consumes global833FCA54; do not conflate the clocks.
    const double dt=ctx.f30.f64,spring_dt=ReadFloat(0x833FCA54,base),reference=ReadFloat(0x833C0230,base);
    if(!std::isfinite(dt) || dt<=0 || dt>.25 || !std::isfinite(spring_dt) || spring_dt<=0 || spring_dt>.25 ||
       !std::isfinite(reference) || reference<=0 || reference>.25)return {};
    return {dt/reference,spring_dt/reference};
}
void CameraMouseScope(PPCContext& ctx,uint8_t* base,PPCFunc* update,bool activity=false) {
    struct Restore {MouseScope previous=mouse_scope;~Restore(){mouse_scope=previous;}}restore;
    struct RestoreActivity {
        uint32_t mode,controller;uint8_t idle;uint8_t* base;
        ~RestoreActivity(){
            // Check current ownership before reading a potentially retired mode.
            if(idle && em2::IsNormalGameplayCamera(controller,base) && PPC_LOAD_U32(controller+24)==mode && !PPC_LOAD_U8(mode+309))
                PPC_STORE_U8(mode+309,idle);
        }
    }restore_activity{ctx.r3.u32,activity?PPC_LOAD_U32(ctx.r3.u32+20):0u,activity?PPC_LOAD_U8(ctx.r3.u32+309):uint8_t(0),base};
    if(activity){PPC_STORE_U8(ctx.r3.u32+309,0);WriteFloat(ctx.r3.u32+12,0,base);}
    mouse_scope={ctx.r3.u32,0};update(ctx,base);
}
void NativeYawWithMouse(PPCContext& ctx,uint8_t* base,em2::MouseLookAngles angles) {
    if(mouse_scope.mode!=ctx.r3.u32)angles={};
    if(mouse_scope.mode==ctx.r3.u32)mouse_scope.pitch=angles.pitch;
    if(angles.yaw!=0) {
        const auto csr=_mm_getcsr();ctx.f1.f64=double(float(ctx.f1.f64+angles.yaw));_mm_setcsr(csr);
    }
    __imp__sub_82A515D0(ctx,base);
}
bool ManualPitchResetsVelocity(const PPCContext& ctx,uint8_t* base,bool mouse) {
    if(ctx.lr!=0x82A63098 || !REXCVAR_GET(em2_camera_spring_timing))return false;
    bool cached=false;for(const auto& player:spring_history)for(const auto& entry:player)cached|=entry.mode==ctx.r3.u32 && entry.retimed;
    if(!cached)return false;
    const auto csr=_mm_getcsr();struct Restore{unsigned csr;~Restore(){_mm_setcsr(csr);}}restore{csr};
    if(!(std::abs(ctx.f1.f64)>(mouse?0.:em2::NativePitchMotionThreshold(ReadFloat(0x821242AC,base)))))return false;
    const auto mode=ctx.r3.u32;
    const bool forced=ReadFloat(mode+1920,base)<ReadFloat(0x82125F98,base);
    const float first=ReadFloat(mode+1936,base),second=ReadFloat(mode+1940,base);
    const bool pending=(first>0 && ReadFloat(mode+1944,base)<first) || (second>0 && ReadFloat(mode+1948,base)<second);
    if(forced && !pending)return false;
    if(PPC_LOAD_U8(mode+1504))return false;
    return (!(ctx.r5.u32&255) && !PPC_LOAD_U8(mode+580)) || PPC_LOAD_U8(mode+584) || (PPC_LOAD_U32(mode+2052)&2);
}
void ClearManualPitchHistory(const PPCContext& ctx,uint8_t* base,bool mouse) {
    if(ManualPitchResetsVelocity(ctx,base,mouse))
        for(auto& player:spring_history)for(auto& entry:player)if(entry.mode==ctx.r3.u32)entry={};
}
void NativePitchWithMouse(PPCContext& ctx,uint8_t* base) {
    struct RestoreScale {PitchThresholdFactors previous=pitch_threshold_scale;~RestoreScale(){pitch_threshold_scale=previous;}}restore_scale;
    pitch_threshold_scale=PitchThresholdScale(ctx,base);
    // Native manual input explicitly cancels recenter momentum. Do not revive
    // cached velocity after that reset, or clear it for ignored/small inputs.
    if(ctx.lr==0x82A63098 && mouse_scope.mode==ctx.r3.u32 && mouse_scope.pitch!=0) {
        const auto csr=_mm_getcsr();ctx.f1.f64=double(float(ctx.f1.f64+mouse_scope.pitch));_mm_setcsr(csr);mouse_scope.pitch=0;
        ClearManualPitchHistory(ctx,base,true);
        EM2MousePitch(ctx,base);return;
    }
    ClearManualPitchHistory(ctx,base,false);
    if(!pitch_threshold_scale.native()){EM2TimedPitch(ctx,base);return;}
    __imp__sub_82A517D0(ctx,base);
}

int CameraSlot(uint32_t mode,uint8_t* base) {
    if(!mode || PPC_LOAD_U32(mode)!=0x82126C98) return -1;
    const auto controller=PPC_LOAD_U32(mode+20);
    if(!controller) return -1;
    const auto slot=PPC_LOAD_U32(controller+16);
    return slot<2?int(slot):-1;
}
bool ValidInterval(float dt) {return std::isfinite(dt) && dt>=0 && dt<=.25f;}
bool ValidBlend(float blend) {return std::isfinite(blend) && blend>=0 && blend<=1;}
float ReferenceRate(uint8_t* base) {
    const auto configured=PPC_LOAD_U32(0x833FCA80);
    return configured && configured<=240?float(configured):30.f;
}
float TimeBlend(float blend,float steps) {
    if(steps==1 || blend==0) return blend;
    if(steps==0) return 0;
    if(blend==1) return 1;
    return float(-std::expm1(double(steps)*std::log1p(-double(blend))));
}
int SelectResponse(int requested,bool synchronized) {
    return !synchronized && requested>=0 && requested<=2?requested:0;
}
int ActiveResponse() {
    const auto requested=em2::CameraResponse();
    if(!requested)return 0;
    // Camera orientation can affect aiming. Peers must retain the same native
    // response rather than applying independent local gameplay preferences.
    return SelectResponse(requested,em2::net::GameplayInputGate().active() || em2::net::NativeOnlineTiming().fixed_step);
}
float ResponseBlend(float blend,float steps,int response) {
    if(response==2)return steps==0?0.f:1.f;
    return TimeBlend(blend,steps*(response==1?2.f:1.f));
}

void AccelerateLook(PPCContext& ctx,uint8_t* base,int response=0) {
    const unsigned axis=ctx.lr==0x82A62E8C?0:ctx.lr==0x82A62EAC?1:2;
    const auto mode=ctx.r30.u32,filter=ctx.r3.u32;
    if(!REXCVAR_GET(em2_camera_timing)) {
        filter_history={};__imp__sub_83207330(ctx,base);return;
    }
    if(axis==2 || filter!=mode+1568+axis*16) {
        __imp__sub_83207330(ctx,base);return;
    }
    const auto slot=CameraSlot(mode,base);
    const float dt=float(ctx.f30.f64);
    const float acceleration=ReadFloat(filter,base),deceleration=ReadFloat(filter+4,base);
    if(slot<0 || !ValidInterval(dt) || !ValidBlend(acceleration) || !ValidBlend(deceleration)) {
        if(slot>=0)filter_history[slot][axis]={};
        __imp__sub_83207330(ctx,base);return;
    }
    if(response && !em2::IsNormalGameplayCamera(PPC_LOAD_U32(mode+20),base))response=0;
    // A paused clock must not apply the last nonzero look increment again.
    if(dt==0) {ctx.f1.f64=0;return;}
    const auto csr=_mm_getcsr();
    auto& history=filter_history[slot][axis];
    const auto cached=PPC_LOAD_U32(filter+12);
    if(ctx.f1.f64==0 && history.mode==mode && history.output[0]==cached && history.interval>0)
        WriteFloat(filter+12,ReadFloat(filter+12,base)*(dt/history.interval),base);
    const auto saved_acceleration=PPC_LOAD_U32(filter),saved_deceleration=PPC_LOAD_U32(filter+4);
    const auto configured=PPC_LOAD_U32(0x833FCA80);
    const float native_scale=configured?float(configured)*dt:1.f;
    const float steps=ReferenceRate(base)*dt;
    // The original helper multiplies its coefficients by native_scale. Supply
    // coefficients whose product is the elapsed-time exponential blend, then
    // restore the authored parameters. All sign/reversal behavior stays native.
    WriteFloat(filter,ResponseBlend(acceleration,steps,response)/native_scale,base);
    WriteFloat(filter+4,ResponseBlend(deceleration,steps,response)/native_scale,base);
    _mm_setcsr(csr);
    __imp__sub_83207330(ctx,base);
    PPC_STORE_U32(filter,saved_acceleration);PPC_STORE_U32(filter+4,saved_deceleration);
    history={mode,dt,{PPC_LOAD_U32(filter+12),0}};
}

void SmoothLook(PPCContext& ctx,uint8_t* base,int response=0) {
    const auto mode=ctx.r3.u32;
    if(!REXCVAR_GET(em2_camera_timing)) {
        filter_history={};__imp__sub_82A515D0(ctx,base);return;
    }
    if(ctx.lr!=0x82A62F80 || ctx.r30.u32!=mode) {
        __imp__sub_82A515D0(ctx,base);return;
    }
    const auto slot=CameraSlot(mode,base);
    const float dt=float(ctx.f30.f64),blend=float(ctx.f8.f64);
    if(slot<0 || !ValidInterval(dt) || !ValidBlend(blend)) {
        if(slot>=0)filter_history[slot][2]={};
        __imp__sub_82A515D0(ctx,base);return;
    }
    if(response && !em2::IsNormalGameplayCamera(PPC_LOAD_U32(mode+20),base))response=0;
    const auto csr=_mm_getcsr();
    auto& history=filter_history[slot][2];
    // At this exact call site f0/f13 are the preceding increments and f12/f11
    // the requested increments. The stock FMAs have just written +568/+572.
    const std::array<float,2> old={float(ctx.f0.f64),float(ctx.f13.f64)};
    const std::array<float,2> desired={float(ctx.f12.f64),float(ctx.f11.f64)};
    float ratio=1;
    if(history.mode==mode && history.interval>0 &&
       history.output[0]==std::bit_cast<uint32_t>(old[0]) &&
       history.output[1]==std::bit_cast<uint32_t>(old[1])) ratio=dt/history.interval;
    const float alpha=ResponseBlend(blend,ReferenceRate(base)*dt,response);
    for(unsigned axis=0;axis<2;++axis) {
        const float previous=old[axis]*ratio;
        const float result=dt==0?0:response==2?desired[axis]:float(double(float(desired[axis]-previous))*alpha+previous);
        WriteFloat(mode+568+axis*4,result,base);
        history.output[axis]=std::bit_cast<uint32_t>(result);
        if(axis==0)ctx.f1.f64=result;else ctx.f7.f64=result;
    }
    history.mode=mode;history.interval=dt;
    _mm_setcsr(csr);
    em2::MouseLookAngles mouse;
    // Single-player swaps retarget P0's camera; they do not select P1's camera.
    if(em2::CameraPresentationInputAllowed() && em2::MouseCameraEnabled() && mouse_scope.mode==mode && slot==0) {
        const bool allowed=dt>0 && !PPC_LOAD_U8(0x833FCA86) &&
            em2::IsNormalGameplayCamera(PPC_LOAD_U32(mode+20),base) &&
            !em2::net::GameplayInputGate().active() && !em2::net::NativeOnlineTiming().fixed_step;
        mouse=em2::TakeMouseLook(mode,allowed);
    }
    // Native yaw limits/recentering still run, followed by the original pitch,
    // follow and collision path. This changes only the two input filters.
    NativeYawWithMouse(ctx,base,mouse);
}
}

double em2::NativePitchMotionThreshold(double authored,bool velocity) noexcept {
    const auto scale=velocity?pitch_threshold_scale.velocity:pitch_threshold_scale.input;
    if(scale==1)return authored;
    const auto csr=_mm_getcsr();struct Restore {unsigned csr;~Restore(){_mm_setcsr(csr);}}restore{csr};
    return double(float(authored*scale));
}

void em2::ObserveNativeCameraUpdate(const PPCContext& ctx,uint8_t* base) noexcept {
    if(!REXCVAR_GET(em2_trace_camera) && !NativeTimingDiagnosticsEnabled())return;
    struct Restore {unsigned csr=_mm_getcsr();~Restore(){_mm_setcsr(csr);}}restore;
    try {
        static thread_local std::array<CameraWindow,9> samples;
        const auto controller=ctx.r3.u32;
        auto slot=std::find_if(samples.begin(),samples.end()-1,[&](const auto& sample){return sample.identity==controller;});
        if(slot==samples.end()-1)slot=std::find_if(samples.begin(),samples.end()-1,[](const auto& sample){return sample.identity==0;});
        const auto identity=slot==samples.end()-1?UINT32_MAX:controller;
        const auto now=std::chrono::steady_clock::now();
        if(slot->identity!=identity || !slot->calls){*slot={};slot->identity=identity;slot->started=now;}
        const auto mode=controller?PPC_LOAD_U32(controller+24):0;
        if(slot->calls && slot->mode!=mode){*slot={};slot->identity=identity;slot->started=now;}
        slot->mode=mode;
        // Audited828510C8 saves incoming f1 separately in f29, then loads
        // 833FCA54 into f30 and forwards that actual interval to camera modes.
        // Incoming f1 is not the camera timestep (it grows like absolute time).
        slot->Add(ReadFloat(0x833FCA54,base),ctx.r5.u32,ctx.r6.u32);
        slot->Source(ctx.f1.f64,ReadFloat(0x833FCA4C,base));
        const auto elapsed=std::chrono::duration<double>(now-slot->started).count();
        if(elapsed>=5) {
            REXLOG_INFO("Native camera dispatch {:08X} mode {:08X} vtable {:08X}: {} calls / {:.3f}s, valid dt sum {:.6f}, dt {:.6f}..{:.6f}, paused {}, invalid {}, r5 {:08X}..{:08X}, r6 {:08X}..{:08X} (audited core delta833FCA54, not incoming f1; FFFFFFFF pools overflow controllers, not render FPS)",
                slot->identity,slot->mode,mode?PPC_LOAD_U32(mode):0u,slot->calls,elapsed,slot->total,slot->minimum,slot->maximum,slot->paused,slot->invalid,
                slot->minimum5,slot->maximum5,slot->minimum6,slot->maximum6);
            REXLOG_INFO("Native camera absolute time {:08X}: {} samples, incoming f1 {:.9f}..{:.9f}, f1 minus scene833FCA4C {:.9f}..{:.9f}, invalid {}",
                slot->identity,slot->source_samples,slot->source_min,slot->source_max,slot->offset_min,slot->offset_max,slot->source_invalid);
            *slot={};slot->identity=identity;slot->started=now;
        }
    }catch(...){} // Diagnostics must not replace the native camera update.
}

void em2::ObserveNativeCameraEntity(const PPCContext& ctx,uint8_t* base) noexcept {
    if(!REXCVAR_GET(em2_trace_camera) || ctx.lr!=0x828515A8)return;
    struct Restore {DWORD error=GetLastError();unsigned csr=_mm_getcsr();~Restore(){SetLastError(error);_mm_setcsr(csr);}} restore;
    try{
        static thread_local std::array<uint64_t,16> seen{};
        const auto entity=ctx.r30.u32;if(!entity)return;
        const auto table=PPC_LOAD_U32(entity);if(!table)return;
        const auto components=PPC_LOAD_U32(entity+36),count=PPC_LOAD_U32(entity+44);
        const auto key=((uint64_t(entity)<<32)|table)^uint64_t(count);
        if(std::find(seen.begin(),seen.end(),key)!=seen.end())return;
        const auto slot=std::find(seen.begin(),seen.end(),uint64_t(0));if(slot==seen.end())return;
        *slot=key;
        REXLOG_INFO("Native camera entity {:08X}: vtable {:08X}, update40 {:08X}, absolute {:.9f}, argument5 {:08X}, argument6 {:08X}",
            entity,table,PPC_LOAD_U32(table+40),ctx.f29.f64,ctx.r19.u32,ctx.r18.u32);
        if(components && count<=64)for(uint32_t i=0;i<count;++i){
            const auto component=PPC_LOAD_U32(components+i*4);if(!component)continue;
            const auto vtable=PPC_LOAD_U32(component);if(!vtable)continue;
            REXLOG_INFO("Native camera entity component {:08X} {}/{}: interface {:08X}, vtable {:08X}, update40 {:08X}",
                entity,i,count,component,vtable,PPC_LOAD_U32(vtable+40));
        }
    }catch(...){}
}

PPC_FUNC_IMPL(sub_82A5D278) {
    if(ctx.lr==0x827C439C)
        em2::ObserveNativeCameraLookBinding(ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,base);
    if(em2::net::SuppressNativeRoomCameraLook(ctx,base))return;
    // Identify the engine action dispatcher without synthesizing input or
    // changing the native listener's arguments, side effects or ABI clobbers.
    if(REXCVAR_GET(em2_trace_camera)) {
        struct Restore {unsigned csr=_mm_getcsr();DWORD error=GetLastError();
            ~Restore(){_mm_setcsr(csr);SetLastError(error);}}restore;
        try {
            static thread_local std::array<std::array<uint32_t,3>,32> seen{};
            const std::array<uint32_t,3> key{ctx.r3.u32,uint32_t(ctx.lr),ctx.r5.u32};
            if(std::find(seen.begin(),seen.end(),key)==seen.end()) {
                const auto free=std::find(seen.begin(),seen.end(),std::array<uint32_t,3>{});
                if(free!=seen.end()) {
                    *free=key;
                    REXLOG_INFO("Native normal-camera look listener: mode {:08X}, caller {:08X}, event {:08X}, user {:08X}, axes {:.9f}/{:.9f}",
                        ctx.r3.u32,uint32_t(ctx.lr),ctx.r4.u32,ctx.r5.u32,ctx.f1.f64,ctx.f2.f64);
                    // 827C4310 forwards the action itself as r5 (828BB858 is
                    // an identity getter). Inspect its bounded binding table
                    // and authored curve rather than assume a stick mapping.
                    const auto action=ctx.r5.u32;
                    em2::GuestReadBatch reads;
                    if(ctx.lr==0x827C439C && action>=4096 && uint64_t(action)+436<=0x100000000ull &&
                        em2::GuestReadBatch::Readable(base+action,436)) {
                        const auto count=PPC_LOAD_U32(action+408);
                        REXLOG_INFO("Native camera action {:08X}: UID {:08X}, kind {}, curve mode {}, bindings {}, deadzone {:.9f}, saturation {:.9f}, exponent {:.9f}, gain {:.9f}",
                            action,PPC_LOAD_U32(action),PPC_LOAD_U32(action+80),PPC_LOAD_U32(action+432),count,
                            ReadFloat(action+416,base),ReadFloat(action+420,base),ReadFloat(action+424,base),ReadFloat(action+428,base));
                        if(count<=10)for(unsigned i=0;i<count;++i) {
                            const auto binding=action+88+i*32;
                            REXLOG_INFO("Native camera action {:08X} binding {}: device {}, key {:08X}, component {}, inverted {}",
                                action,i,PPC_LOAD_U32(binding),PPC_LOAD_U32(binding+4),PPC_LOAD_U32(binding+8),PPC_LOAD_U8(binding+16));
                        }
                    }
                }
            }
        }catch(...){}
    }
    __imp__sub_82A5D278(ctx,base);
}
void em2::ObserveNativeCameraInputDevice(uint32_t device,const PPCContext& ctx,uint8_t* base) noexcept {
    if(REXCVAR_GET(em2_trace_camera) && ctx.r3.u32 && device) {
        struct Restore {unsigned csr=_mm_getcsr();DWORD error=GetLastError();
            ~Restore(){_mm_setcsr(csr);SetLastError(error);}}restore;
        try {
            static thread_local std::array<uint32_t,8> seen{};
            if(std::find(seen.begin(),seen.end(),device)==seen.end()) {
                const auto free=std::find(seen.begin(),seen.end(),uint32_t(0));
                if(free!=seen.end()) {
                    *free=device;
                    const auto history=PPC_LOAD_U32(device+60);
                    if(history)for(unsigned axis=2;axis<4;++axis) {
                        const auto record=history+axis*32;
                        REXLOG_INFO("Native camera input device {:08X} axis {}: raw range {}..{}, published range {}..{}, inversion {:08X}",
                            device,axis,int32_t(PPC_LOAD_U32(record+16)),int32_t(PPC_LOAD_U32(record+20)),
                            int32_t(PPC_LOAD_U32(record+24)),int32_t(PPC_LOAD_U32(record+28)),PPC_LOAD_U32(device+68));
                    }
                }
            }
        }catch(...){}
    }
}
PPC_FUNC_IMPL(sub_83207330) {AccelerateLook(ctx,base,ActiveResponse());}
PPC_FUNC_IMPL(sub_82A515D0) {SmoothLook(ctx,base,ActiveResponse());}
PPC_FUNC_IMPL(sub_82A517D0) {NativePitchWithMouse(ctx,base);}
PPC_FUNC_IMPL(sub_831C6930) {
    const auto probe=CaptureSpring(ctx,base);
    const auto retimed=SpringUpdate(ctx,base);
    if(probe)ObserveSpring(*probe,retimed);
}
PPC_FUNC_IMPL(sub_831C6878) {SpringVelocity(ctx,base);}
PPC_FUNC_IMPL(sub_82A628A0) {
    bool activity=false;
    {
    struct RestoreMath {unsigned csr=_mm_getcsr();~RestoreMath(){_mm_setcsr(csr);}}restore_math;
    if(em2::CameraPresentationInputAllowed() && em2::MouseCameraEnabled() && ValidInterval(float(ctx.f3.f64)) && ctx.f3.f64>0 && !PPC_LOAD_U8(0x833FCA86) &&
        CameraSlot(ctx.r3.u32,base)==0 &&
        em2::IsNormalGameplayCamera(PPC_LOAD_U32(ctx.r3.u32+20),base) &&
        !em2::net::GameplayInputGate().active() && !em2::net::NativeOnlineTiming().fixed_step &&
        em2::MouseLookHasMotion(ctx.r3.u32)) {
        // Same native activity fields used by the look listener: mouse motion
        // must not be treated as idle stick input by automatic follow.
        activity=true;
    }
    }
    CameraMouseScope(ctx,base,__imp__sub_82A628A0,activity);
}

bool em2::TestNativeCameraTiming() {
    if(!TestMouseLook() || !TestCameraWindow())return false;
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));
    if(!base)return false;
    struct Restore {
        uint8_t* base;unsigned csr=_mm_getcsr();bool enabled=REXCVAR_GET(em2_camera_timing);
        decltype(filter_history) history=filter_history;
        MouseScope scope=mouse_scope;
        bool spring_enabled=REXCVAR_GET(em2_camera_spring_timing);decltype(spring_history) springs=spring_history;decltype(last_recenter) recenter=last_recenter;
        bool threshold_enabled=REXCVAR_GET(em2_camera_pitch_threshold_timing);PitchThresholdFactors thresholds=pitch_threshold_scale;
        ~Restore(){pitch_threshold_scale=thresholds;REXCVAR_SET(em2_camera_pitch_threshold_timing,threshold_enabled);last_recenter=recenter;spring_history=springs;REXCVAR_SET(em2_camera_spring_timing,spring_enabled);mouse_scope=scope;filter_history=history;REXCVAR_SET(em2_camera_timing,enabled);_mm_setcsr(csr);VirtualFree(base,0,MEM_RELEASE);}
    } restore{base};
    REXCVAR_SET(em2_camera_spring_timing,false);
    REXCVAR_SET(em2_camera_pitch_threshold_timing,false);
    for(auto address:{0u,0x82000000u,0x82040000u,0x82080000u,0x820F0000u,0x82120000u,0x82200000u,0x833C0000u,0x833D0000u,0x833F0000u})
        if(!VirtualAlloc(base+address,0x10000,MEM_COMMIT,PAGE_READWRITE))return false;
    WriteFloat(0x82002060,0,base);WriteFloat(0x82003F48,1,base);
    PPC_STORE_U64(0x8200FBE8,std::bit_cast<uint64_t>(0.0));
    PPC_STORE_U64(0x8200FBE0,std::bit_cast<uint64_t>(1.0));
    PPC_STORE_U64(0x82081AC0,std::bit_cast<uint64_t>(-1.0));
    WriteFloat(0x82125F98,std::numeric_limits<float>::max(),base);
    constexpr uint32_t mode=0x1000,filter=mode+1568;
    PPC_STORE_U32(mode,0x82126C98);PPC_STORE_U32(mode+20,0x3000);PPC_STORE_U32(0x3010,0);
    PPC_STORE_U32(0x3000,0x82112778);PPC_STORE_U32(0x3018,mode);
    PPC_STORE_U32(0x833DAB60,0x5000);PPC_STORE_U32(0x5014,1);PPC_STORE_U32(0x501C,0x3000);
    PPC_STORE_U32(0x833FCA80,30);
    WriteFloat(mode+1880,std::numeric_limits<float>::max(),base);
    REXCVAR_SET(em2_camera_timing,true);
    bool ok=true;
    auto check=[&](bool passed,const char* message){if(!passed){ok=false;REXLOG_ERROR("Native camera timing test: {}",message);}};
    auto input=[&](float dt,float value,unsigned axis=0) {
        PPCContext ctx{};ctx.fpscr.InitHost();ctx.r1.u64=0xF000;ctx.r3.u64=filter+axis*16;
        ctx.r30.u64=mode;ctx.f30.f64=dt;ctx.f1.f64=value;ctx.lr=axis?0x82A62EAC:0x82A62E8C;
        WriteFloat(0x833FCA54,dt,base);return ctx;
    };
    // Compare the hook against the real translated helper, including all
    // guest registers, stack writes and filter bytes, at the reference rate.
    for(unsigned slot=0;slot<2;++slot) for(unsigned axis=0;axis<2;++axis)
    for(float value:{-.05f,0.f,.05f}) for(float state:{-.4f,0.f,.4f}) {
        const auto address=filter+axis*16;PPC_STORE_U32(0x3010,slot);
        filter_history={};WriteFloat(address,.2f,base);WriteFloat(address+4,.3f,base);
        WriteFloat(address+8,state,base);WriteFloat(address+12,.05f,base);
        auto ctx=input(1.f/30,value,axis),native=ctx;
        std::array<uint8_t,0x10000> before{},expected{};
        std::memcpy(before.data(),base,before.size());const auto csr=_mm_getcsr();
        __imp__sub_83207330(native,base);const auto native_csr=_mm_getcsr();std::memcpy(expected.data(),base,expected.size());
        std::memcpy(base,before.data(),before.size());_mm_setcsr(csr);AccelerateLook(ctx,base);
        check(!std::memcmp(&native,&ctx,sizeof(ctx)) && !std::memcmp(expected.data(),base,expected.size()) && _mm_getcsr()==native_csr,
              "reference-rate acceleration must preserve original context, memory and FP behavior");
    }
    PPC_STORE_U32(0x3010,0);
    for(unsigned variant=0;variant<5;++variant) {
        filter_history={};WriteFloat(filter,.2f,base);WriteFloat(filter+4,.3f,base);
        WriteFloat(filter+8,.4f,base);WriteFloat(filter+12,.05f,base);
        auto ctx=input(1.f/60,.03f);
        if(variant==0)ctx.lr=0;
        if(variant==1)PPC_STORE_U32(mode,0x82126C9C);
        if(variant==2)PPC_STORE_U32(0x3010,2);
        if(variant==3)REXCVAR_SET(em2_camera_timing,false);
        if(variant==4)WriteFloat(filter,1.5f,base);
        auto native=ctx;std::array<uint8_t,0x10000> before{},expected{};
        std::memcpy(before.data(),base,before.size());const auto csr=_mm_getcsr();
        __imp__sub_83207330(native,base);const auto native_csr=_mm_getcsr();std::memcpy(expected.data(),base,expected.size());
        std::memcpy(base,before.data(),before.size());_mm_setcsr(csr);AccelerateLook(ctx,base);
        check(!std::memcmp(&native,&ctx,sizeof(ctx)) && !std::memcmp(expected.data(),base,expected.size()) && _mm_getcsr()==native_csr,
              "other callers/classes/slots, debug-off and unsupported coefficients must remain native");
        PPC_STORE_U32(mode,0x82126C98);PPC_STORE_U32(0x3010,0);REXCVAR_SET(em2_camera_timing,true);
    }
    for(int fps:{30,40,50,60,90,120,144,165,240,1000}) {
        filter_history={};WriteFloat(filter,.2f,base);WriteFloat(filter+4,.3f,base);
        WriteFloat(filter+8,0,base);WriteFloat(filter+12,0,base);
        const float dt=1.f/fps;
        for(int frame=0;frame<fps;++frame){auto ctx=input(dt,2*dt);AccelerateLook(ctx,base);}
        check(std::abs(ReadFloat(filter+8,base)-float(1-std::pow(.8,30)))<2e-5f,
              "acceleration state must have the same one-second response at all update rates");
        const auto state=ReadFloat(filter+8,base);
        // A changed interval on release must retain angular speed, not the
        // magnitude of an unrelated preceding frame's angular increment.
        auto released=input(1.f/60,0);AccelerateLook(released,base);
        const float expected=state*float(std::pow(.7,.5));
        check(std::abs(float(released.f1.f64)-2.f/60*expected)<2e-6f,
              "release across a frame-rate change must scale the remembered look increment");
        const auto cached=PPC_LOAD_U32(filter+12),held=PPC_LOAD_U32(filter+8);
        auto paused=input(0,0);AccelerateLook(paused,base);
        check(paused.f1.f64==0 && PPC_LOAD_U32(filter+12)==cached && PPC_LOAD_U32(filter+8)==held,
              "paused camera must not consume a remembered look increment or advance acceleration");
    }
    for(unsigned variant=0;variant<5;++variant) {
        filter_history={};auto ctx=input(variant?1.f/60:1.f/30,0);
        ctx.r3.u64=mode;ctx.lr=0x82A62F80;
        ctx.f0.f64=.01f;ctx.f13.f64=-.02f;ctx.f12.f64=.05f;ctx.f11.f64=.07f;ctx.f8.f64=variant==4?1.5f:.2f;
        ctx.f1.f64=float(float(ctx.f12.f64-ctx.f0.f64)*ctx.f8.f64+ctx.f0.f64);
        ctx.f7.f64=float(float(ctx.f11.f64-ctx.f13.f64)*ctx.f8.f64+ctx.f13.f64);
        WriteFloat(mode+568,float(ctx.f1.f64),base);WriteFloat(mode+572,float(ctx.f7.f64),base);
        if(variant==1)ctx.lr=0;
        if(variant==2)PPC_STORE_U32(mode,0x82126C9C);
        if(variant==3)REXCVAR_SET(em2_camera_timing,false);
        auto native=ctx;std::array<uint8_t,0x10000> before{},expected{};
        std::memcpy(before.data(),base,before.size());const auto csr=_mm_getcsr();
        __imp__sub_82A515D0(native,base);const auto native_csr=_mm_getcsr();std::memcpy(expected.data(),base,expected.size());
        std::memcpy(base,before.data(),before.size());_mm_setcsr(csr);SmoothLook(ctx,base);
        check(!std::memcmp(&native,&ctx,sizeof(ctx)) && !std::memcmp(expected.data(),base,expected.size()) && _mm_getcsr()==native_csr,
              "reference-rate smoothing and guarded fallbacks must preserve original yaw helper context/memory/FP behavior");
        PPC_STORE_U32(mode,0x82126C98);REXCVAR_SET(em2_camera_timing,true);
    }
    // Run the actual native yaw helper with unconstrained yaw. The preceding
    // two FMAs are recreated from their original registers at the hook site.
    for(int fps:{30,40,60,120,240,1000}) {
        filter_history={};WriteFloat(mode+568,0,base);WriteFloat(mode+572,0,base);WriteFloat(mode+1140,0,base);
        const float dt=1.f/fps;
        for(int frame=0;frame<fps;++frame) {
            auto ctx=input(dt,0);ctx.r3.u64=mode;ctx.lr=0x82A62F80;
            ctx.f0.f64=ReadFloat(mode+568,base);ctx.f13.f64=ReadFloat(mode+572,base);
            ctx.f12.f64=2*dt;ctx.f11.f64=-dt;ctx.f8.f64=.2f;
            ctx.f1.f64=float(float(ctx.f12.f64-ctx.f0.f64)*double(float(ctx.f8.f64))+ctx.f0.f64);
            ctx.f7.f64=float(float(ctx.f11.f64-ctx.f13.f64)*double(float(ctx.f8.f64))+ctx.f13.f64);
            WriteFloat(mode+568,float(ctx.f1.f64),base);WriteFloat(mode+572,float(ctx.f7.f64),base);
            SmoothLook(ctx,base);
        }
        const float response=float(1-std::pow(.8,30));
        check(std::abs(ReadFloat(mode+568,base)/dt-2*response)<3e-5f &&
              std::abs(ReadFloat(mode+572,base)/dt+response)<3e-5f,
              "secondary look filter must have the same velocity response at each update rate");
    }
    {
        filter_history={};WriteFloat(mode+568,0,base);WriteFloat(mode+572,0,base);
        float elapsed=0;
        for(float dt:{1.f/30,1.f/120,1.f/60,1.f/240}) {
            auto ctx=input(dt,0);ctx.r3.u64=mode;ctx.lr=0x82A62F80;
            ctx.f0.f64=ReadFloat(mode+568,base);ctx.f13.f64=ReadFloat(mode+572,base);
            ctx.f12.f64=2*dt;ctx.f11.f64=-dt;ctx.f8.f64=.2f;
            ctx.f1.f64=float(float(ctx.f12.f64-ctx.f0.f64)*ctx.f8.f64+ctx.f0.f64);
            ctx.f7.f64=float(float(ctx.f11.f64-ctx.f13.f64)*ctx.f8.f64+ctx.f13.f64);
            WriteFloat(mode+568,float(ctx.f1.f64),base);WriteFloat(mode+572,float(ctx.f7.f64),base);
            SmoothLook(ctx,base);elapsed+=dt;
            const float response=float(1-std::pow(.8,double(elapsed)*30));
            check(std::abs(ReadFloat(mode+568,base)/dt-2*response)<2e-6f &&
                  std::abs(ReadFloat(mode+572,base)/dt+response)<2e-6f,
                  "changing frame duration must rescale both preceding look increments before blending");
        }
    }
    unsigned guarded_responses=0;
    for(uint32_t address:{0x301Cu,0x3020u,0x3024u,0x5020u,0x5014u,0x501Cu,0x3000u}) {
        const auto original=PPC_LOAD_U32(address);
        PPC_STORE_U32(address,(address==0x5014 || address==0x501C)?0:1);
        for(int response:{1,2})for(bool smoothing:{false,true}) {
            filter_history={};WriteFloat(filter,.2f,base);WriteFloat(filter+4,.3f,base);
            WriteFloat(filter+8,.4f,base);WriteFloat(filter+12,.05f,base);
            auto ctx=input(1.f/30,.05f);
            if(smoothing) {
                ctx.r3.u64=mode;ctx.lr=0x82A62F80;
                ctx.f0.f64=.01f;ctx.f13.f64=-.02f;ctx.f12.f64=.05f;ctx.f11.f64=.07f;ctx.f8.f64=.2f;
                ctx.f1.f64=float(float(ctx.f12.f64-ctx.f0.f64)*ctx.f8.f64+ctx.f0.f64);
                ctx.f7.f64=float(float(ctx.f11.f64-ctx.f13.f64)*ctx.f8.f64+ctx.f13.f64);
                WriteFloat(mode+568,float(ctx.f1.f64),base);WriteFloat(mode+572,float(ctx.f7.f64),base);
            }
            auto native=ctx;std::array<uint8_t,0x10000> before{},expected{};
            std::memcpy(before.data(),base,before.size());const auto csr=_mm_getcsr();
            if(smoothing)__imp__sub_82A515D0(native,base);else __imp__sub_83207330(native,base);
            const auto native_csr=_mm_getcsr();std::memcpy(expected.data(),base,expected.size());
            std::memcpy(base,before.data(),before.size());_mm_setcsr(csr);
            if(smoothing)SmoothLook(ctx,base,response);else AccelerateLook(ctx,base,response);
            check(!std::memcmp(&native,&ctx,sizeof(ctx)) && !std::memcmp(expected.data(),base,expected.size()) && _mm_getcsr()==native_csr,
                "Scripted/disabled/foreign cameras must retain original native filter context, memory and FP state");
            ++guarded_responses;
        }
        PPC_STORE_U32(address,original);
    }
    unsigned response_steps=0;
    for(int response:{1,2})for(int fps:{30,40,60,120,240}) {
        filter_history={};WriteFloat(mode+568,0,base);WriteFloat(mode+572,0,base);WriteFloat(mode+1140,0,base);
        const float dt=1.f/fps;
        for(int frame=0;frame<fps;++frame) {
            auto ctx=input(dt,0);ctx.r3.u64=mode;ctx.lr=0x82A62F80;
            ctx.f0.f64=ReadFloat(mode+568,base);ctx.f13.f64=ReadFloat(mode+572,base);
            // A real press, reversal and release, not just a constant input.
            const float direction=frame<fps/3?1.f:frame<2*fps/3?-1.f:0.f;
            ctx.f12.f64=2*dt*direction;ctx.f11.f64=-dt*direction;ctx.f8.f64=.2f;
            ctx.f1.f64=float(float(ctx.f12.f64-ctx.f0.f64)*ctx.f8.f64+ctx.f0.f64);
            ctx.f7.f64=float(float(ctx.f11.f64-ctx.f13.f64)*ctx.f8.f64+ctx.f13.f64);
            WriteFloat(mode+568,float(ctx.f1.f64),base);WriteFloat(mode+572,float(ctx.f7.f64),base);
            const float old_x=float(ctx.f0.f64),old_y=float(ctx.f13.f64);
            SmoothLook(ctx,base,response);
            if(response==2)check(ReadFloat(mode+568,base)==2*dt*direction && ReadFloat(mode+572,base)==-dt*direction,
                "No smoothing must respond to press/reversal/release in the current native update");
            else {
                const auto factor=std::pow(.8,60.0*dt);
                check(std::abs(ReadFloat(mode+568,base)-(2*dt*direction+(old_x-2*dt*direction)*factor))<1e-6 &&
                      std::abs(ReadFloat(mode+572,base)-(-dt*direction+(old_y+dt*direction)*factor))<1e-6,
                    "Faster smoothing must retain the same doubled response rate across update intervals");
            }
            ++response_steps;
        }
        // Exercise the real acceleration helper as well as the downstream
        // smoothing helper, including direction changes and release tails.
        for(unsigned axis=0;axis<2;++axis) {
            const auto address=filter+axis*16;
            filter_history={};WriteFloat(address,.2f,base);WriteFloat(address+4,.3f,base);
            WriteFloat(address+8,0,base);WriteFloat(address+12,0,base);
            for(float value:{.05f,0.f,-.05f,0.f,.02f,-.02f,0.f}) {
                auto ctx=input(dt,value,axis);AccelerateLook(ctx,base,response);
                check(ReadFloat(address,base)==.2f && ReadFloat(address+4,base)==.3f,
                    "Response modes must restore both authored acceleration coefficients");
                if(response==2)check(std::abs(ctx.f1.f64-value)<1e-7,
                    "No smoothing must remove acceleration and release tails on both native axes");
                ++response_steps;
            }
        }
    }
    for(int requested:{-1,0,1,2,3}) {
        check(SelectResponse(requested,true)==0,"Online aiming must ignore local response preferences");
        check(SelectResponse(requested,false)==(requested>=0 && requested<=2?requested:0),"Offline response selection must stay bounded");
    }
    WriteFloat(0x8204A0FC,100000000.f,base);WriteFloat(0x8220AA2C,3.1415927410125732f,base);
    WriteFloat(0x833C0200,6.2831854820251465f,base);WriteFloat(0x820040F0,-3.1415927410125732f,base);
    WriteFloat(0x833C0230,.03333333507180214f,base);WriteFloat(0x820047F0,.0001f,base);WriteFloat(0x8200422C,.01f,base);
    WriteFloat(0x820077E4,.01745329238474369f,base);WriteFloat(0x821242AC,.001f,base);
    PPC_STORE_U32(mode+1788,0x7000);PPC_STORE_U32(mode+1792,0x7004);
    WriteFloat(0x7000,80,base);WriteFloat(0x7004,-80,base);
    WriteFloat(mode+1920,std::numeric_limits<float>::max(),base);PPC_STORE_U8(mode+580,0);
    unsigned mouse_steps=0;
    for(float increment:{.02f,float(3.14159265358979323846/14400)})for(int fps:{30,40,60,120,240}) {
        WriteFloat(mode+1140,0,base);WriteFloat(mode+1184,0,base);
        WriteFloat(mode+568,.123f,base);WriteFloat(mode+572,-.234f,base);
        for(unsigned frame=0;frame<10;++frame) {
            mouse_scope={mode,0};auto yaw=input(1.f/fps,0);yaw.r3.u64=mode;
            const MouseLookAngles angles{-increment*.5f,increment};
            NativeYawWithMouse(yaw,base,angles);WriteFloat(mode+1140,float(yaw.f1.f64),base);
            auto pitch=input(1.f/fps,0);pitch.r3.u64=mode;pitch.r5.u64=0;pitch.lr=0x82A63098;
            WriteFloat(mode+1144,0,base);NativePitchWithMouse(pitch,base);
            check(std::abs(ReadFloat(mode+1140,base)+increment*.5f*(frame+1))<2e-6f &&
                std::abs(float(pitch.f1.f64)-increment*(frame+1))<2e-6f && mouse_scope.pitch==0,
                "mouse angular displacement must enter real native yaw/pitch once, independently of tick duration");
            check(ReadFloat(mode+568,base)==.123f && ReadFloat(mode+572,base)==-.234f,
                "mouse deltas must never enter stick smoothing history");
            ++mouse_steps;
        }
        auto stopped_yaw=input(1.f/fps,0);stopped_yaw.r3.u64=mode;
        NativeYawWithMouse(stopped_yaw,base,{});
        auto stopped_pitch=input(1.f/fps,0);stopped_pitch.r3.u64=mode;stopped_pitch.lr=0x82A63098;
        WriteFloat(mode+1144,0,base);NativePitchWithMouse(stopped_pitch,base);
        check(std::abs(float(stopped_yaw.f1.f64)+increment*5)<2e-6f && std::abs(float(stopped_pitch.f1.f64)-increment*10)<2e-6f,
            "stopping mouse motion must retain the angle without a smoothing/release tail");
        // Native pitch limits remain authoritative even for large mouse deltas.
        mouse_scope={mode,2.f};auto pitch=input(1.f/fps,0);pitch.r3.u64=mode;pitch.lr=0x82A63098;
        WriteFloat(mode+1144,0,base);NativePitchWithMouse(pitch,base);
        check(std::abs(float(pitch.f1.f64)-80*.01745329238474369f)<2e-6f,"mouse pitch must respect original native angle limits");
    }
    // Execute the complete original forced-pitch path, not a replacement for
    // its spring. Keep the base orientation fixed and let +1184 accumulate the
    // aiming offset exactly as the parent routine does. This is a controlled
    // camera-mode fixture, not a captured in-level parameter set.
    unsigned spring_steps=0;std::array<float,4> spring_positions{};
    for(float stiffness:{.0025f,.01f,.04f})for(unsigned rate=0;rate<4;++rate) {
        const unsigned hz=30u<<rate;const float dt=1.f/hz,damping=float(2*std::sqrt(stiffness));
        WriteFloat(mode+1920,1,base);WriteFloat(mode+1932,0,base);
        WriteFloat(mode+1936,0,base);WriteFloat(mode+1940,0,base);PPC_STORE_U8(mode+1929,1);
        WriteFloat(mode+1144,0,base);WriteFloat(mode+1184,0,base);
        WriteFloat(mode+2632,stiffness,base);WriteFloat(mode+2636,damping,base);
        WriteFloat(mode+2640,0,base);WriteFloat(mode+2644,0,base);WriteFloat(0x833FCA54,dt,base);
        float position=0,velocity=0;
        for(unsigned frame=0;frame<hz/10;++frame) {
            const float error=float(1-position);
            const float damped=float(double(damping)*velocity);
            const float acceleration=float(double(stiffness)*error-damped);
            velocity=float(double(acceleration)+velocity);
            const float remaining=float(double(error)-velocity);
            const float correction=float(double(error)-remaining);
            position=float(double(position)+correction);
            auto pitch=input(dt,0);pitch.r3.u64=mode;pitch.r5.u64=0;pitch.f2.f64=0;
            __imp__sub_82A517D0(pitch,base);
            check(std::abs(ReadFloat(mode+1184,base)-position)<2e-6f &&
                std::abs(ReadFloat(mode+2640,base)-velocity)<2e-6f && ReadFloat(mode+2644,base)==0 &&
                PPC_LOAD_U8(mode+1928)==1,
                "complete original forced-pitch path reproduces one full spring recurrence per short-interval call");
            ++spring_steps;
        }
        if(stiffness==.01f)spring_positions[rate]=ReadFloat(mode+1184,base);
    }
    check(spring_positions[1]>spring_positions[0]+.01f && spring_positions[2]>spring_positions[1]+.01f &&
        spring_positions[3]>spring_positions[2]+.01f,"native forced-pitch counterexample advances farther at higher rates over the same tenth-second");
    for(float dt:{0.f,1.f/240,1.f/120,1.f/60,1.f/30}) {
        constexpr uint32_t spring=0x7800;
        WriteFloat(spring,.01f,base);WriteFloat(spring+4,.2f,base);WriteFloat(spring+8,0,base);WriteFloat(spring+12,0,base);
        PPCContext ctx{};ctx.fpscr.InitHost();ctx.r3.u64=spring;ctx.f1.f64=1;ctx.f2.f64=dt;ctx.f3.f64=.0001f;
        __imp__sub_831C6930(ctx,base);
        check(std::abs(ctx.f1.f64-.01)<2e-7 && ReadFloat(spring+8,base)==.01f && ReadFloat(spring+12,base)==0,
            "original spring executes its minimum full update even for zero/sub-reference intervals");
    }
    REXLOG_INFO("Native forced-pitch spring counterexample: {} complete original parent updates, stiffness0.01/damping0.2, tenth-second offsets at30/60/120/240Hz {:.9f}/{:.9f}/{:.9f}/{:.9f}; short/zero intervals still execute one full spring update. Controlled mode fixture with correction disabled, not measured live coefficients",spring_steps,spring_positions[0],spring_positions[1],spring_positions[2],spring_positions[3]);
    WriteFloat(0x820FAB58,2,base);
    unsigned recenter_steps=0;
    for(float stiffness:{.0025f,.01f,.04f})for(bool positive:{false,true})for(unsigned rate=0;rate<4;++rate) {
        const unsigned hz=30u<<rate;const float dt=1.f/hz,damping=float(2*std::sqrt(stiffness));
        WriteFloat(mode+1920,std::numeric_limits<float>::max(),base);PPC_STORE_U8(mode+580,1);PPC_STORE_U8(mode+584,0);
        PPC_STORE_U8(mode+1504,0);PPC_STORE_U8(mode+1505,0);PPC_STORE_U32(mode+2052,0);
        PPC_STORE_U32(mode+1392,0x7200);PPC_STORE_U32(mode+1372,0x7204);
        WriteFloat(0x7200,stiffness,base);WriteFloat(0x7204,stiffness,base);
        WriteFloat(mode+1396,stiffness,base);
        float position=positive?.5f:-.5f,velocity=0;
        WriteFloat(mode+1144,0,base);WriteFloat(mode+1184,position,base);WriteFloat(mode+1176,0,base);
        for(auto offset:{1356u,1376u})for(unsigned field=0;field<4;++field)WriteFloat(mode+offset+field*4,0,base);
        for(unsigned frame=0;frame<hz/10;++frame) {
            const float error=-position;
            const float damped=float(double(damping)*velocity);
            const float acceleration=float(double(stiffness)*error-damped);
            const float new_velocity=float(double(acceleration)+velocity);
            const float remaining=float(double(error)-new_velocity);
            velocity=float(double(error)-remaining); // Parent feeds returned correction into next call's velocity setter.
            position=float(double(position)+velocity);
            auto pitch=input(dt,0);pitch.r3.u64=mode;pitch.r5.u64=0;pitch.f2.f64=0;
            __imp__sub_82A517D0(pitch,base);
            check(std::abs(ReadFloat(mode+1184,base)-position)<2e-6f && std::abs(ReadFloat(mode+1176,base)-velocity)<2e-6f,
                "complete original positive/negative recenter paths feed the previous returned correction into the native velocity setter");
            ++recenter_steps;
        }
    }
    REXLOG_INFO("Native recenter velocity handoff reproduced across {} complete original parent steps: both signs, three stiffnesses,30/60/120/240Hz; previous returned correction, not per-second velocity, is passed through831C6878",recenter_steps);
    unsigned spring_observer_checks=0;
    {
        struct RestoreDiagnostics {
            bool trace=REXCVAR_GET(em2_trace_camera),automatic=NativeTimingDiagnosticsEnabled();
            ~RestoreDiagnostics(){REXCVAR_SET(em2_trace_camera,trace);SetNativeTimingPhysicsDiagnostics(automatic);}
        } restore_spring_diagnostics;
        for(unsigned enabled=0;enabled<3;++enabled)for(uint32_t call:{0x82A51944u,0x82A51B24u,0x82A51BC0u,0x82A61308u}) {
            REXCVAR_SET(em2_trace_camera,enabled==1);SetNativeTimingPhysicsDiagnostics(enabled==2);
            const auto offset=call==0x82A51B24?1376:call==0x82A51BC0?1356:2632;
            const auto spring=mode+offset;
            WriteFloat(spring,.01f,base);WriteFloat(spring+4,.2f,base);WriteFloat(spring+8,.003f,base);WriteFloat(spring+12,0,base);
            PPCContext native{};native.fpscr.InitHost();native.r3.u64=spring;native.r31.u64=mode;native.lr=call;
            native.f1.f64=.25;native.f2.f64=1.f/120;native.f3.f64=.0001f;auto actual=native;
            std::array<uint8_t,65536> before{},expected{};std::memcpy(before.data(),base,before.size());const auto csr=_mm_getcsr();
            __imp__sub_831C6930(native,base);const auto native_csr=_mm_getcsr();std::memcpy(expected.data(),base,expected.size());
            std::memcpy(base,before.data(),before.size());_mm_setcsr(csr);sub_831C6930(actual,base);
            check(!std::memcmp(&native,&actual,sizeof(actual)) && !std::memcmp(expected.data(),base,expected.size()) && _mm_getcsr()==native_csr,
                "spring observer preserves complete original context, guest memory and MXCSR at all four scoped call sites under disabled/explicit/automatic diagnostics");
            ++spring_observer_checks;
        }
        for(bool disabled:{true,false}) {
            REXCVAR_SET(em2_trace_camera,!disabled);SetNativeTimingPhysicsDiagnostics(false);
            constexpr uint32_t spring=0x7800;
            WriteFloat(spring,.01f,base);WriteFloat(spring+4,.2f,base);WriteFloat(spring+8,0,base);WriteFloat(spring+12,0,base);
            PPCContext ctx{};ctx.fpscr.InitHost();ctx.r3.u64=spring;ctx.r31.u64=0xFFFF0000;ctx.lr=disabled?0x82A51944:0;
            ctx.f1.f64=1;ctx.f2.f64=1.f/120;ctx.f3.f64=.0001f;sub_831C6930(ctx,base);
            check(std::abs(ctx.f1.f64-.01)<2e-7,"disabled/foreign spring diagnostics do not read an invalid camera-mode pointer");++spring_observer_checks;
        }
    }
    REXLOG_INFO("Native camera spring observer passed {} original-code context/memory/MXCSR and no-read checks; bounded per-player/per-call-site k/d/reference-step/carry/interval logging enabled with existing native timing diagnostics",spring_observer_checks);
    unsigned corrected_steps=0,reference_checks=0;double maximum_spring_error=0;
    auto spring_fixture=[&](unsigned kind,float stiffness,float initial_velocity=0) {
        spring_history={};last_recenter={};
        WriteFloat(mode+1920,kind?std::numeric_limits<float>::max():1.f,base);WriteFloat(mode+1932,0,base);
        WriteFloat(mode+1936,0,base);WriteFloat(mode+1940,0,base);PPC_STORE_U8(mode+1929,1);
        PPC_STORE_U8(mode+580,kind!=0);PPC_STORE_U8(mode+584,0);PPC_STORE_U8(mode+1504,0);PPC_STORE_U8(mode+1505,0);
        PPC_STORE_U32(mode+2052,0);PPC_STORE_U32(mode+1392,0x7200);PPC_STORE_U32(mode+1372,0x7204);
        WriteFloat(0x7200,stiffness,base);WriteFloat(0x7204,stiffness,base);WriteFloat(mode+1396,stiffness,base);
        WriteFloat(mode+1144,0,base);WriteFloat(mode+1184,kind==1?.5f:kind==2?-.5f:0.f,base);
        WriteFloat(mode+1176,initial_velocity,base);
        for(auto offset:{1356u,1376u,2632u}) {
            WriteFloat(mode+offset,stiffness,base);WriteFloat(mode+offset+4,float(2*std::sqrt(stiffness)),base);
            WriteFloat(mode+offset+8,initial_velocity,base);WriteFloat(mode+offset+12,0,base);
        }
    };
    auto pitch_step=[&](float dt) {
        auto pitch=input(dt,0);pitch.r3.u64=mode;pitch.r5.u64=0;pitch.f2.f64=0;
        __imp__sub_82A517D0(pitch,base);return pitch;
    };
    for(unsigned kind=0;kind<3;++kind)for(float stiffness:{.0025f,.01f,.04f,.16f,.25f}) {
        std::array<float,15> expected{};
        REXCVAR_SET(em2_camera_spring_timing,false);spring_fixture(kind,stiffness);
        for(auto& value:expected){pitch_step(1.f/30);value=ReadFloat(mode+1184,base);}
        for(unsigned schedule=0;schedule<4;++schedule) {
            REXCVAR_SET(em2_camera_spring_timing,true);spring_fixture(kind,stiffness);
            for(unsigned tick=0;tick<expected.size();++tick) {
                const unsigned count=schedule==0?2:schedule==1?4:schedule==2?8:4;
                for(unsigned step=0;step<count;++step) {
                    constexpr float uneven[]={1.f/240,1.f/120,1.f/80,1.f/120};
                    pitch_step(schedule==3?uneven[step]:1.f/(30*count));++corrected_steps;
                }
                const auto error=std::abs(double(ReadFloat(mode+1184,base))-expected[tick]);maximum_spring_error=std::max(maximum_spring_error,error);
                check(error<.0003,"corrected full native forced/positive/negative pitch trajectories match authored30Hz at60/120/240Hz and uneven intervals");
            }
        }
        // Full context, stack/mode memory and FP parity at the reference rate.
        spring_fixture(kind,stiffness);
        for(unsigned frame=0;frame<3;++frame) {
            auto native=input(1.f/30,0);native.r3.u64=mode;native.r5.u64=0;native.f2.f64=0;auto actual=native;
            std::array<uint8_t,65536> before{},expected_memory{};std::memcpy(before.data(),base,before.size());const auto csr=_mm_getcsr();
            REXCVAR_SET(em2_camera_spring_timing,false);__imp__sub_82A517D0(native,base);const auto native_csr=_mm_getcsr();std::memcpy(expected_memory.data(),base,expected_memory.size());
            std::memcpy(base,before.data(),before.size());_mm_setcsr(csr);REXCVAR_SET(em2_camera_spring_timing,true);__imp__sub_82A517D0(actual,base);
            check(!std::memcmp(&native,&actual,sizeof(actual)) && !std::memcmp(expected_memory.data(),base,expected_memory.size()) && _mm_getcsr()==native_csr,
                "reference-rate complete native pitch context, memory and MXCSR remain bit-identical");++reference_checks;
        }
    }
    // Momentum can cross zero and change which native recenter spring is used.
    for(unsigned schedule=0;schedule<3;++schedule) {
        std::array<float,9> expected{};REXCVAR_SET(em2_camera_spring_timing,false);spring_fixture(1,.01f,-.1f);WriteFloat(mode+1184,.01f,base);
        for(auto& value:expected){pitch_step(1.f/30);value=ReadFloat(mode+1184,base);}
        REXCVAR_SET(em2_camera_spring_timing,true);spring_fixture(1,.01f,-.1f);WriteFloat(mode+1184,.01f,base);
        for(unsigned tick=0;tick<expected.size();++tick) {
            const unsigned count=2u<<schedule;for(unsigned step=0;step<count;++step){pitch_step(1.f/(30*count));++corrected_steps;}
            const auto error=std::abs(double(ReadFloat(mode+1184,base))-expected[tick]);maximum_spring_error=std::max(maximum_spring_error,error);
            check(error<.0003,"canonical recenter velocity crosses between positive/negative native branches without rate-dependent momentum");
        }
    }
    for(unsigned kind=0;kind<3;++kind) {
        spring_fixture(kind,.01f);REXCVAR_SET(em2_camera_spring_timing,true);pitch_step(1.f/120);
        const auto position=ReadFloat(mode+1184,base);
        for(unsigned pause=0;pause<3;++pause){pitch_step(0);check(ReadFloat(mode+1184,base)==position,"zero elapsed time cannot advance native pitch springs");}
    }
    for(unsigned kind=0;kind<3;++kind) {
        spring_fixture(kind,.36f);REXCVAR_SET(em2_camera_spring_timing,true);const auto position=ReadFloat(mode+1184,base);
        pitch_step(0);check(ReadFloat(mode+1184,base)==position,"zero elapsed time freezes even stiff springs whose nonzero-interval fractional flow uses native fallback");
    }
    for(float stiffness:{.0025f,.01f,.1f,.2f})for(float damping:{.05f,.2f,.8f}) {
        const auto full=AdvanceSpring(.2,-.03,stiffness,damping,1);
        auto first=AdvanceSpring(.2,-.03,stiffness,damping,.25);
        const auto second=first?AdvanceSpring(.2-first->correction,first->velocity,stiffness,damping,.75):std::nullopt;
        check(full && first && second && std::abs(first->correction+second->correction-full->correction)<2e-7 &&
            std::abs(second->velocity-full->velocity)<2e-7,"fractional spring state obeys elapsed-time composition for real and complex stable poles");
    }
    check(!AdvanceSpring(1,0,.36,1.2,.5) && !AdvanceSpring(1,0,-1,.2,.5) && !AdvanceSpring(NAN,0,.01,.2,.5),
        "unsupported or nonfinite spring dynamics reject correction instead of inventing a real fractional flow");
    unsigned transition_checks=0,guard_checks=0;
    for(unsigned kind=0;kind<3;++kind) {
        std::array<float,15> expected{};REXCVAR_SET(em2_camera_spring_timing,false);spring_fixture(kind,.01f);
        for(auto& value:expected){pitch_step(1.f/30);value=ReadFloat(mode+1184,base);}
        REXCVAR_SET(em2_camera_spring_timing,true);spring_fixture(kind,.01f);
        for(unsigned tick=0;tick<expected.size();++tick) {
            constexpr unsigned counts[]={1,4,2,8,3};const auto count=counts[tick%5];
            for(unsigned step=0;step<count;++step){pitch_step(1.f/(30*count));++corrected_steps;}
            const auto error=std::abs(double(ReadFloat(mode+1184,base))-expected[tick]);maximum_spring_error=std::max(maximum_spring_error,error);
            check(error<.0003,"native pitch keeps its trajectory through30/120/60/240/90Hz rate transitions");++transition_checks;
        }
        spring_fixture(kind,.01f);
        for(unsigned tick=1;tick<expected.size();tick+=2) {
            pitch_step(1.f/15);++corrected_steps;
            const auto error=std::abs(double(ReadFloat(mode+1184,base))-expected[tick]);maximum_spring_error=std::max(maximum_spring_error,error);
            check(error<.0003,"longer native pitch intervals preserve canonical velocity instead of feeding total frame displacement back as velocity");++transition_checks;
        }
    }
    {
        std::array<float,2> expected{};REXCVAR_SET(em2_camera_spring_timing,false);
        for(unsigned slot=0;slot<2;++slot){spring_fixture(slot?2:1,slot?.04f:.01f);for(unsigned i=0;i<3;++i)pitch_step(1.f/30);expected[slot]=ReadFloat(mode+1184,base);}
        REXCVAR_SET(em2_camera_spring_timing,true);spring_fixture(1,.01f);
        constexpr uint32_t peer_mode=0x8000,peer_controller=0xB000;
        std::memcpy(base+peer_mode,base+mode,2768);PPC_STORE_U32(peer_mode+20,peer_controller);PPC_STORE_U32(peer_controller,0x82112778);
        PPC_STORE_U32(peer_controller+16,1);PPC_STORE_U32(peer_controller+24,peer_mode);PPC_STORE_U32(0x5000+348,peer_controller);PPC_STORE_U32(0x5000+340,1);
        WriteFloat(peer_mode+1184,-.5f,base);PPC_STORE_U32(peer_mode+1392,0x7300);PPC_STORE_U32(peer_mode+1372,0x7304);
        WriteFloat(0x7300,.04f,base);WriteFloat(0x7304,.04f,base);WriteFloat(peer_mode+1396,.04f,base);
        for(unsigned frame=0;frame<12;++frame) {
            if(!(frame&1)){pitch_step(1.f/60);++corrected_steps;}
            auto peer=input(1.f/120,0);peer.r3.u64=peer_mode;peer.r5.u64=0;peer.f2.f64=0;__imp__sub_82A517D0(peer,base);++corrected_steps;
        }
        check(std::abs(ReadFloat(mode+1184,base)-expected[0])<.0003 && std::abs(ReadFloat(peer_mode+1184,base)-expected[1])<.0003,
            "interleaved local co-op cameras retain independent spring state at different intervals/stiffness/directions");++transition_checks;
        PPC_STORE_U32(0x5000+348,0);PPC_STORE_U32(0x5000+340,0);
    }
    for(unsigned guard=0;guard<7;++guard) {
        spring_fixture(0,.01f);REXCVAR_SET(em2_camera_spring_timing,guard!=4);
        if(guard==0)WriteFloat(mode+2636,1.2f,base);
        if(guard==1)PPC_STORE_U32(0x3000+28,1);
        if(guard==2)PPC_STORE_U32(mode,0x82126C9C);
        if(guard==5)WriteFloat(mode+2632,-.01f,base);
        if(guard==6)PPC_STORE_U32(0x501C,0);
        auto native=input(1.f/120,1);native.r3.u64=mode+2632;native.r31.u64=mode;native.lr=guard==3?0:0x82A51944;
        native.f2.f64=1.f/120;native.f3.f64=.0001f;auto actual=native;
        std::array<uint8_t,65536> before{},expected{};std::memcpy(before.data(),base,before.size());const auto csr=_mm_getcsr();
        __imp__sub_831C6930(native,base);const auto native_csr=_mm_getcsr();std::memcpy(expected.data(),base,expected.size());
        std::memcpy(base,before.data(),before.size());_mm_setcsr(csr);sub_831C6930(actual,base);
        check(!std::memcmp(&native,&actual,sizeof(actual)) && !std::memcmp(expected.data(),base,expected.size()) && _mm_getcsr()==native_csr,
            "unsupported/scripted/foreign/disabled/unowned spring calls preserve original context, whole memory and MXCSR");++guard_checks;
        PPC_STORE_U32(0x3000+28,0);PPC_STORE_U32(mode,0x82126C98);PPC_STORE_U32(0x501C,0x3000);
    }
    unsigned manual_checks=0;
    REXCVAR_SET(em2_camera_spring_timing,true);spring_fixture(1,.01f);pitch_step(1.f/120);pitch_step(1.f/120);
    const auto two_steps=ReadFloat(mode+1184,base);
    auto manual_step=[&](float value){auto ctx=input(1.f/120,value);ctx.r3.u64=mode;ctx.r5.u64=0;ctx.f2.f64=0;ctx.lr=0x82A63098;NativePitchWithMouse(ctx,base);};
    spring_fixture(1,.01f);pitch_step(1.f/120);manual_step(.02f);
    check(std::abs(ReadFloat(mode+1184,base)-two_steps)<2e-7,"native ignored manual input must preserve canonical recenter momentum");++manual_checks;
    spring_fixture(1,.01f);pitch_step(1.f/120);PPC_STORE_U8(mode+580,0);manual_step(.0005f);PPC_STORE_U8(mode+580,1);pitch_step(1.f/120);
    check(std::abs(ReadFloat(mode+1184,base)-two_steps)<2e-7,"sub-threshold native input does not cancel recenter history");++manual_checks;
    spring_fixture(1,.01f);pitch_step(1.f/120);PPC_STORE_U8(mode+580,0);const auto before_manual=ReadFloat(mode+1184,base);manual_step(.02f);
    check(ReadFloat(mode+1176,base)==0 && spring_history[0][1].mode==0 && std::abs(ReadFloat(mode+1184,base)-(before_manual+.02f))<2e-7,
        "accepted native manual pitch cancels cached momentum at the actual reset branch");++manual_checks;
    spring_fixture(1,.01f);pitch_step(1.f/120);PPC_STORE_U8(mode+580,0);const auto before_mouse=ReadFloat(mode+1184,base);mouse_scope={mode,.0002f};manual_step(0);
    check(ReadFloat(mode+1176,base)==0 && spring_history[0][1].mode==0 && std::abs(ReadFloat(mode+1184,base)-(before_mouse+.0002f))<2e-7,
        "sub-stick-threshold raw mouse pitch uses the mouse variant's zero threshold when cancelling momentum");++manual_checks;mouse_scope={};
    REXCVAR_SET(em2_camera_spring_timing,false);
    REXLOG_INFO("Elapsed-time native pitch springs: {} corrected complete parent steps, {} reference-rate full-context/memory/MXCSR comparisons, {} transition/co-op checks, {} guarded original-code differentials, {} manual/ignored/sub-threshold/mouse handoff checks, maximum aligned trajectory error {:.9f} rad; forced and both recenter directions,60/120/240Hz/uneven, sign handoff, pause,15Hz/variable rates, local co-op and stable real/complex matrix composition",corrected_steps,reference_checks,transition_checks,guard_checks,manual_checks,maximum_spring_error);
    unsigned threshold_steps=0,threshold_checks=0;double maximum_input_error=0;
    auto motion_fixture=[&] {
        spring_fixture(1,.01f);WriteFloat(mode+1184,0,base);WriteFloat(mode+1544,std::numeric_limits<float>::max(),base);
        PPC_STORE_U8(mode+580,0);PPC_STORE_U8(mode+582,0);mouse_scope={};
    };
    auto motion_step=[&](float camera_dt,float value,float spring_dt=0) {
        auto ctx=input(camera_dt,value);ctx.r3.u64=mode;ctx.r30.u64=mode;ctx.r5.u64=0;ctx.f2.f64=0;ctx.lr=0x82A63098;
        if(spring_dt)WriteFloat(0x833FCA54,spring_dt,base);
        NativePitchWithMouse(ctx,base);return ctx;
    };
    const float reference=ReadFloat(0x833C0230,base),authored_threshold=ReadFloat(0x821242AC,base);
    std::array<float,2> original_weak{};
    for(unsigned sample=0;sample<2;++sample) {
        const unsigned hz=sample?120:30;motion_fixture();REXCVAR_SET(em2_camera_pitch_threshold_timing,false);
        const float dt=1.f/hz,increment=float(double(.002f)*dt/reference);
        for(unsigned frame=0;frame<hz;++frame)motion_step(dt,increment);
        original_weak[sample]=ReadFloat(mode+1184,base);
    }
    check(original_weak[0]>.059f && original_weak[1]==0,"original native pitch threshold counterexample rejects equivalent slow stick motion at120Hz");++threshold_checks;
    REXCVAR_SET(em2_camera_pitch_threshold_timing,true);
    for(unsigned hz:{4u,15u,30u,40u,60u,90u,120u,240u,1000u})for(float reference_increment:{-.002f,-.0005f,.0005f,.002f}) {
        motion_fixture();const float dt=1.f/hz,increment=float(double(reference_increment)*dt/reference);
        for(unsigned frame=0;frame<hz;++frame){motion_step(dt,increment);++threshold_steps;}
        const double expected=std::abs(reference_increment)>authored_threshold?double(increment)*hz:0;
        const auto error=std::abs(double(ReadFloat(mode+1184,base))-expected);maximum_input_error=std::max(maximum_input_error,error);
        check(error<2e-6,"complete native pitch accepts/rejects the same angular speed at4..1000Hz");++threshold_checks;
    }
    for(unsigned hz:{15u,30u,40u,60u,90u,120u,240u,1000u})for(bool positive:{false,true})for(int edge:{-1,0,1}) {
        motion_fixture();const float dt=1.f/hz;
        float value=float(double(authored_threshold)*dt/reference);
        if(edge)value=std::nextafter(value,edge>0?INFINITY:0.f);if(!positive)value=-value;
        motion_step(dt,value);const auto angle=ReadFloat(mode+1184,base);
        check(edge>0?std::abs(angle-value)<2e-7:angle==0,"scaled native pitch threshold retains strict boundary semantics for both directions");++threshold_checks;
    }
    // Camera f30 and global spring time are distinct native arguments.
    motion_fixture();motion_step(reference,.0005f,1.f/120);
    check(ReadFloat(mode+1184,base)==0,"manual threshold follows the camera interval rather than global spring time");++threshold_checks;
    motion_fixture();motion_step(1.f/120,.0005f,reference);
    check(std::abs(ReadFloat(mode+1184,base)-.0005f)<2e-7,"manual threshold accepts equivalent speed when only the camera interval is short");++threshold_checks;
    auto flags_fixture=[&](float position,float velocity) {
        spring_fixture(1,.01f,velocity);WriteFloat(mode+1184,position,base);WriteFloat(mode+1544,std::numeric_limits<float>::max(),base);
        PPC_STORE_U8(mode+582,0);PPC_STORE_U8(mode+1505,1);PPC_STORE_U8(mode+2154,1);
    };
    REXCVAR_SET(em2_camera_spring_timing,true);
    flags_fixture(.5f,0);motion_step(1.f/120,.005f);
    check(PPC_LOAD_U8(mode+1505)==0 && PPC_LOAD_U8(mode+2154)==0,"strong-input reset threshold is rate-correct even when direct pitch input is disabled");++threshold_checks;
    flags_fixture(.0005f,-.002f);motion_step(reference,0,1.f/120);
    check(PPC_LOAD_U8(mode+1505)==1,"recenter velocity threshold uses the spring interval independently of the camera interval");++threshold_checks;
    flags_fixture(.0008f,0);motion_step(1.f/120,0);
    check(PPC_LOAD_U8(mode+1505)==0 && ReadFloat(mode+1184,base)>.00025f && ReadFloat(mode+1184,base)<.001f,
        "absolute settled-angle tolerance remains unscaled");++threshold_checks;
    spring_fixture(1,.01f);pitch_step(1.f/120);PPC_STORE_U8(mode+580,0);WriteFloat(mode+1544,std::numeric_limits<float>::max(),base);
    const auto before_small_manual=ReadFloat(mode+1184,base);motion_step(1.f/120,.0005f);
    check(ReadFloat(mode+1176,base)==0 && spring_history[0][1].mode==0 && std::abs(ReadFloat(mode+1184,base)-before_small_manual-.0005f)<2e-7,
        "newly accepted high-rate manual pitch cancels recenter momentum through the same scaled threshold");++threshold_checks;
    REXCVAR_SET(em2_camera_spring_timing,false);
    unsigned pitch_reference_checks=0;
    for(bool ignored:{false,true})for(float value:{-.02f,-.002f,-.0005f,0.f,.0005f,.002f,.02f}) {
        motion_fixture();PPC_STORE_U8(mode+580,ignored);WriteFloat(mode+1184,ignored?.5f:0.f,base);
        auto native=input(reference,value);native.r3.u64=mode;native.r5.u64=0;native.f2.f64=0;native.lr=0x82A63098;auto actual=native;
        std::array<uint8_t,65536> before{},expected{};std::memcpy(before.data(),base,before.size());const auto csr=_mm_getcsr();
        __imp__sub_82A517D0(native,base);const auto native_csr=_mm_getcsr();std::memcpy(expected.data(),base,expected.size());
        std::memcpy(base,before.data(),before.size());_mm_setcsr(csr);NativePitchWithMouse(actual,base);
        check(!std::memcmp(&native,&actual,sizeof(actual)) && !std::memcmp(expected.data(),base,expected.size()) && _mm_getcsr()==native_csr,
            "reference-rate pitch thresholds preserve complete original context, memory and MXCSR");++pitch_reference_checks;
    }
    check(pitch_threshold_scale.native(),"pitch threshold scope restores both clocks after native calls");++threshold_checks;
    unsigned threshold_guards=0;
    for(unsigned guard=0;guard<8;++guard) {
        motion_fixture();REXCVAR_SET(em2_camera_pitch_threshold_timing,guard!=0);
        if(guard==1)PPC_STORE_U32(0x5020,1);
        if(guard==2)PPC_STORE_U32(mode,0x82126C9C);
        if(guard==3)PPC_STORE_U32(0x501C,0);
        auto native=input(1.f/120,.0005f);native.r3.u64=mode;native.r30.u64=guard==4?0:mode;native.r5.u64=0;native.f2.f64=0;native.lr=guard==5?0:0x82A63098;
        if(guard==6)native.f30.f64=NAN;if(guard==7)WriteFloat(0x833FCA54,0,base);auto actual=native;
        std::array<uint8_t,65536> before{},expected{};std::memcpy(before.data(),base,before.size());const auto csr=_mm_getcsr();
        __imp__sub_82A517D0(native,base);const auto native_csr=_mm_getcsr();std::memcpy(expected.data(),base,expected.size());
        std::memcpy(base,before.data(),before.size());_mm_setcsr(csr);NativePitchWithMouse(actual,base);
        check(!std::memcmp(&native,&actual,sizeof(actual)) && !std::memcmp(expected.data(),base,expected.size()) && _mm_getcsr()==native_csr,
            "disabled/scripted/foreign/unowned/wrong-parent/invalid-clock threshold cases preserve complete original context, memory and MXCSR");++threshold_guards;
        PPC_STORE_U32(0x5020,0);PPC_STORE_U32(mode,0x82126C98);PPC_STORE_U32(0x501C,0x3000);
    }
    REXCVAR_SET(em2_camera_pitch_threshold_timing,true);motion_fixture();
    {
        constexpr uint32_t peer_mode=0x8000,peer_controller=0xB000;
        std::memcpy(base+peer_mode,base+mode,2768);PPC_STORE_U32(peer_mode+20,peer_controller);PPC_STORE_U32(peer_controller,0x82112778);
        PPC_STORE_U32(peer_controller+16,1);PPC_STORE_U32(peer_controller+24,peer_mode);PPC_STORE_U32(0x5000+348,peer_controller);PPC_STORE_U32(0x5000+340,1);
        for(unsigned frame=0;frame<120;++frame) {
            if(!(frame&1))motion_step(1.f/60,float(double(.002f)*(1.f/60)/reference));
            auto peer=input(1.f/120,float(-double(.002f)*(1.f/120)/reference));peer.r3.u64=peer_mode;peer.r30.u64=peer_mode;peer.r5.u64=0;peer.f2.f64=0;peer.lr=0x82A63098;
            NativePitchWithMouse(peer,base);
        }
        check(std::abs(ReadFloat(mode+1184,base)-.06f)<2e-6 && std::abs(ReadFloat(peer_mode+1184,base)+.06f)<2e-6 && pitch_threshold_scale.native(),
            "interleaved owned local co-op cameras use separate actual intervals and restore the threshold scope");++threshold_checks;
        PPC_STORE_U32(0x5000+348,0);PPC_STORE_U32(0x5000+340,0);
    }
    {
        PitchThresholdFactors saved=pitch_threshold_scale;pitch_threshold_scale={.5,.25};
        motion_fixture();motion_step(1.f/120,.0005f);
        check(pitch_threshold_scale.input==.5 && pitch_threshold_scale.velocity==.25,"nested pitch threshold scopes restore independent outer clocks");++threshold_checks;
        pitch_threshold_scale=saved;
    }
    REXLOG_INFO("Native pitch input threshold timing: {} complete parent motion updates, {} acceptance/boundary/dual-clock/flag/handoff/scope/co-op checks, {} full reference context/memory/MXCSR comparisons, {} guarded original differentials; original1s weak motion30/120Hz {:.9f}/{:.9f}, corrected maximum angular accumulation error {:.9f}rad at4..1000Hz. Absolute angle tolerance unchanged",threshold_steps,threshold_checks,pitch_reference_checks,threshold_guards,original_weak[0],original_weak[1],maximum_input_error);
    REXCVAR_SET(em2_camera_pitch_threshold_timing,false);
    mouse_scope={0x1234,.5f};PPCContext scoped{};scoped.r3.u64=mode;
    CameraMouseScope(scoped,base,+[](PPCContext& ctx,uint8_t*){mouse_scope.pitch=2;ctx.r3.u64=42;});
    check(mouse_scope.mode==0x1234 && mouse_scope.pitch==.5f && scoped.r3.u64==42,
        "camera scope must retire pending mouse pitch without changing native return context");
    mouse_scope={0,0};
    PPC_STORE_U8(mode+309,1);WriteFloat(mode+12,5,base);scoped.r3.u64=mode;
    CameraMouseScope(scoped,base,+[](PPCContext& ctx,uint8_t* base){
        ctx.r3.u64=!PPC_LOAD_U8(0x1000+309) && ReadFloat(0x1000+12,base)==0 ? 42:0;
    },true);
    check(scoped.r3.u32==42 && PPC_LOAD_U8(mode+309)==1 && ReadFloat(mode+12,base)==0,
        "mouse activity must reset idle time for one update without latching the native idle flag");
    scoped.r3.u64=mode;
    CameraMouseScope(scoped,base,+[](PPCContext&,uint8_t* base){PPC_STORE_U32(0x501C,0);},true);
    check(PPC_LOAD_U8(mode+309)==0,"retired camera ownership must prevent stale idle restoration");
    PPC_STORE_U32(0x501C,0x3000);
    {
        struct RestoreDiagnostics {bool trace=REXCVAR_GET(em2_trace_camera),native=NativeTimingDiagnosticsEnabled();unsigned csr=_mm_getcsr();
            ~RestoreDiagnostics(){REXCVAR_SET(em2_trace_camera,trace);SetNativeTimingPhysicsDiagnostics(native);_mm_setcsr(csr);}}restore_diagnostics;
        REXCVAR_SET(em2_trace_camera,false);SetNativeTimingPhysicsDiagnostics(false);
        PPCContext observed{};observed.r3.u64=0xFFFF0000;observed.f1.f64=1./90;
        // Disabled tracing must not even dereference the supplied controller.
        ObserveNativeCameraUpdate(observed,base);
        observed.r3.u64=0x3000;observed.r5.u64=0x11223344;observed.r6.u64=0x55667788;
        const auto saved_context=observed;std::array<uint8_t,65536> saved_memory;
        std::memcpy(saved_memory.data(),base,saved_memory.size());
        for(bool automatic:{true,false}) {
            SetNativeTimingPhysicsDiagnostics(automatic);REXCVAR_SET(em2_trace_camera,!automatic);
            const auto csr=_mm_getcsr();ObserveNativeCameraUpdate(observed,base);
            check(std::memcmp(&observed,&saved_context,sizeof(observed))==0 &&
                std::memcmp(saved_memory.data(),base,saved_memory.size())==0 && _mm_getcsr()==csr,
                "native camera diagnostics must preserve context, guest memory and MXCSR under automatic/explicit enablement");
        }
    }
    if(ok)REXLOG_INFO("Native camera diagnostics passed: bounded interval/argument accounting, pause/invalid inputs, disabled no-read guard and automatic/explicit observer context/memory/MXCSR transparency; no extra camera update invoked");
    if(ok)REXLOG_INFO("Native mouse camera passed {} real yaw/pitch steps at five update rates: no stick-history contamination, once-only pitch and original pitch limits; OS capture and live camera feel unverified",mouse_steps);
    if(ok)REXLOG_INFO("Native camera timing tests passed: original 30-Hz helper differential, elapsed-time acceleration/smoothing response, changed-interval release and pause; {} faster/direct native response updates with press/reversal/release, both axes, {} full-context scripted/disabled/foreign guards and online preference guards",response_steps,guarded_responses);
    return ok;
}
