#include "mouse_look.h"
#include "em1_preview_runtime.h"
#include "em1_level_entry.h"
#include "debug_level_browser.h"
#include "ppc_context.h"
#include "simulation.h"
#include "camera_targets.h"
#include "camera_cadence.h"
#include "camera_projection.h"
#include "camera_look.h"
#include "pc_settings.h"
#include "shadow_resize.h"
#include "render_options.h"
#include "native_sampler.h"
#include "native_random.h"
#include "online_behavior_trace.h"
#include "online_input_clock.h"
#include "skinning.h"
#include "physics_timing.h"
#include "players.h"
#include "gameplay_mods.h"
#include "native_engine.h"
#include "online.h"
#include "online_runtime.h"
#include "online_room_bootstrap.h"
#include "online_room_runtime.h"
#include "online_room_gameplay.h"
#include "world_observation.h"
#include "native_entity_probe.h"
#include "native_character_physics.h"
#include <rex/cvar.h>
REXCVAR_DEFINE_BOOL(em2_trace_online_random_owners,false,"Debug","Capture bounded owner registers and parent stacks for online CRT diagnostics; requires observation tracing");
REXCVAR_DEFINE_BOOL(em2_trace_online_lua_owners,false,"Debug","Private bounded Lua random-source and early-wait capture without per-tick clock logging; requires observation tracing");
REXCVAR_DEFINE_UINT32(em2_trace_online_random_start_tick,0,"Debug","First of two agreed ticks to capture bounded CRT events for online observation tracing");
#include <rex/logging.h>
#include <rex/kernel/kernel_state.h>
#include <rex/kernel/xmemory.h>
#include <immintrin.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <mutex>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>
#include <windows.h>

PPC_EXTERN_FUNC(__imp__sub_829EC348);
PPC_EXTERN_FUNC(__imp__sub_829ED310);
PPC_EXTERN_FUNC(__imp__sub_829D95B8);
PPC_EXTERN_FUNC(__imp__sub_829D8378);
PPC_EXTERN_FUNC(__imp__sub_829D8228);
PPC_EXTERN_FUNC(__imp__sub_829A4220);
PPC_EXTERN_FUNC(__imp__sub_829CDE48);
PPC_EXTERN_FUNC(__imp__sub_83241358);
PPC_EXTERN_FUNC(__imp__sub_82654850);
PPC_EXTERN_FUNC(__imp__sub_826548B8);
PPC_EXTERN_FUNC(__imp__sub_83074F78);
PPC_EXTERN_FUNC(__imp__sub_83074F90);
PPC_EXTERN_FUNC(__imp__sub_830749A0);
PPC_EXTERN_FUNC(__imp__sub_83074938);
PPC_EXTERN_FUNC(__imp__sub_83074508);
PPC_EXTERN_FUNC(__imp__sub_83074548);
PPC_EXTERN_FUNC(__imp__sub_830750B8);
PPC_EXTERN_FUNC(__imp__sub_825BE2E0);
PPC_EXTERN_FUNC(__imp__sub_82547580);
PPC_EXTERN_FUNC(__imp__sub_83075690);
PPC_EXTERN_FUNC(__imp__sub_825BE1A0);
PPC_EXTERN_FUNC(__imp__sub_8256A528);
PPC_EXTERN_FUNC(__imp__sub_8256A248);
PPC_EXTERN_FUNC(__imp__sub_830B50B8);
PPC_EXTERN_FUNC(__imp__sub_83246488);
PPC_EXTERN_FUNC(__imp__sub_831D0DA8);
PPC_EXTERN_FUNC(__imp__sub_830B5240);
PPC_EXTERN_FUNC(__imp__sub_830751E0);
PPC_EXTERN_FUNC(__imp__sub_826C9F38);
PPC_EXTERN_FUNC(__imp__sub_831BC328);
PPC_EXTERN_FUNC(__imp__sub_831C1360);
PPC_EXTERN_FUNC(__imp__sub_831C13F0);
PPC_EXTERN_FUNC(__imp__sub_8273D880);
PPC_EXTERN_FUNC(__imp__sub_82581840);
PPC_EXTERN_FUNC(__imp__sub_82568C20);
PPC_EXTERN_FUNC(__imp__sub_825532B0);
PPC_EXTERN_FUNC(sub_82568720);
PPC_EXTERN_FUNC(sub_825517C8);
PPC_EXTERN_FUNC(__imp__sub_82599800);
PPC_EXTERN_FUNC(__imp__sub_82909D00);
PPC_EXTERN_FUNC(__imp__sub_8290A238);
PPC_EXTERN_FUNC(__imp__sub_82909528);
PPC_EXTERN_FUNC(__imp__sub_830A0C40);
PPC_EXTERN_FUNC(__imp__sub_82732A18);
PPC_EXTERN_FUNC(__imp__RtlEnterCriticalSection);
PPC_EXTERN_FUNC(__imp__RtlLeaveCriticalSection);
REXCVAR_DEFINE_BOOL(em2_fixed_simulation,false,"Debug","Experimental fixed 30 Hz gameplay with interpolated rendering");
REXCVAR_DEFINE_BOOL(em2_native_variable_simulation,false,"Debug","Experimental offline native variable-delta updates; physics/campaign acceptance pending");
REXCVAR_DEFINE_BOOL(em2_interpolation,true,"Debug","Interpolate scene transforms between simulation ticks");
REXCVAR_DEFINE_BOOL(em2_cache_skin_snapshots,true,"CPU","Reuse immutable skeletal pose snapshots between renders of one simulation update");
REXCVAR_DEFINE_BOOL(em2_benchmark_skin_snapshots,false,"Debug","Benchmark render pose snapshot reuse without launching the game");
REXCVAR_DEFINE_BOOL(em2_cache_scene_render_entries,true,"CPU","Reuse stable moving-history entries and one interpolation alpha per render");
REXCVAR_DEFINE_BOOL(em2_benchmark_scene_render_entries,false,"Debug","Benchmark complete scene apply/restore paths without launching gameplay");
REXCVAR_DEFINE_BOOL(em2_cache_scene_interpolation,true,"CPU","Reuse scene-transform endpoint analysis between renders of one completed simulation tick");
REXCVAR_DEFINE_BOOL(em2_benchmark_scene_interpolation,false,"Debug","Benchmark scene-transform endpoint reuse without launching the game");
REXCVAR_DEFINE_BOOL(em2_cache_pose_interpolation,true,"CPU","Reuse skeletal interpolation endpoint analysis within a captured pose");
REXCVAR_DEFINE_BOOL(em2_benchmark_pose_interpolation,false,"Debug","Benchmark skeletal interpolation preparation without launching the game");
REXCVAR_DEFINE_BOOL(em2_skin_interpolation,true,"Debug","Interpolate skeletal poses through native skinning jobs on fixed-step render frames");
REXCVAR_DEFINE_BOOL(em2_trace_camera,false,"Debug","Measure camera motion and delivered look input per simulation tick");
REXCVAR_DEFINE_BOOL(em2_trace_motion,false,"Debug","Record skinned mesh world and root-bone positions per simulation tick");
REXCVAR_DEFINE_BOOL(em2_trace_online_observations,false,"Debug","Record native player fingerprint inputs once per simulated second for online validation");
REXCVAR_DEFINE_BOOL(em2_trace_render,false,"Debug","Trace update, rendering and skinning call stacks");
REXCVAR_DEFINE_BOOL(em2_profile_render,false,"Debug","Measure native and interpolated rendering separately, with nested helper wall/self timing");

namespace em2 {
std::atomic<uint64_t> simulation_steps{0};
std::atomic<uint64_t> interpolated_nodes{0};
std::atomic<uint64_t> maximum_simulation_us{0};
std::atomic<uint64_t> maximum_render_us{0};
}

namespace {
std::atomic<int> simulation_speed_percent{100};
std::atomic<unsigned> requested_simulation_steps{0};
const auto speed_registration=rex::cvar::FlagRegistrar({
    "em2_simulation_speed_percent",rex::cvar::FlagType::Int32,"Gameplay",
    "Offline simulation speed: 0 pauses, 100 normal, up to 800; physics keeps its original step",
    [](std::string_view text){int value;const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);return error==std::errc{} && end==text.data()+text.size() && em2::SetSimulationSpeedPercent(value);},
    []{return std::to_string(em2::SimulationSpeedPercent());},rex::cvar::Lifecycle::kHotReload,{},"100",false});
}

int em2::SimulationSpeedPercent(){return simulation_speed_percent.load(std::memory_order_relaxed);}
int em2::EffectiveSimulationSpeedPercent(){return em2::net::SynchronizedSessionActive()?100:SimulationSpeedPercent();}
bool em2::SetSimulationSpeedPercent(int percent){
    if(percent<0 || percent>800 || (percent!=100 && em2::net::SynchronizedSessionActive()))return false;
    simulation_speed_percent.store(percent,std::memory_order_relaxed);return true;
}
bool em2::RequestSimulationSteps(unsigned count){
    if(!count || count>8 || SimulationSpeedPercent()!=0 || em2::net::SynchronizedSessionActive())return false;
    auto prior=requested_simulation_steps.load();
    do {if(prior+count>8)return false;}while(!requested_simulation_steps.compare_exchange_weak(prior,prior+count));return true;
}

namespace {
using Matrix = std::array<float,16>;
constexpr std::array<unsigned,13> transform_components={0,1,2,4,5,6,8,9,10,12,13,14,15};
bool RigidRotation(const Matrix& m) {
    for(unsigned row=0;row<3;++row) {
        float length=0;
        for(unsigned col=0;col<3;++col) length+=m[row*4+col]*m[row*4+col];
        if(std::abs(length-1)>1e-3f) return false;
        for(unsigned other=0;other<row;++other) {
            float dot=0;
            for(unsigned col=0;col<3;++col) dot+=m[row*4+col]*m[other*4+col];
            if(std::abs(dot)>1e-3f) return false;
        }
    }
    const float determinant=m[0]*(m[5]*m[10]-m[6]*m[9])-m[1]*(m[4]*m[10]-m[6]*m[8])+m[2]*(m[4]*m[9]-m[5]*m[8]);
    return determinant>0;
}
using Clock = std::chrono::steady_clock;
struct RenderCallProfile {
    uint64_t calls=0,total_ns=0,max_ns=0,self_ns=0,aborted=0;
    bool operator==(const RenderCallProfile&)const=default;
};
constexpr std::array<const char*,23> render_helper_names={
    "829A4220","829CDE48","83241358","82654850","826548B8",
    "83074F78 app+40","83074F90 app+72","830749A0 app+84",
    "83074938 app+60","83074508 app+68","83074548 app+76","830750B8 app+80",
    "825BE2E0 active scene","82547580 active setup","83075690 active callback",
    "825BE1A0 scene begin","8256A528 scene finish","8256A248 renderer finish",
    "830B50B8 stage common","83246488 stage second","831D0DA8 stage third",
    "830B5240 stage views","830751E0 stage before"};
struct RenderFrameProfile {
    uint64_t calls=0,aborted=0,root_helper_ns=0;
    std::array<uint64_t,4> total{},maximum{};
    std::array<RenderCallProfile,render_helper_names.size()> helpers{};
    bool operator==(const RenderFrameProfile&)const=default;
};
struct RenderProfileWindow {
    bool open=false;uint64_t started=0;
    std::array<RenderFrameProfile,2> modes{}; // native, interpolated
    bool operator==(const RenderProfileWindow&)const=default;
};
struct RenderProfileFpGuard {unsigned csr=_mm_getcsr();~RenderProfileFpGuard(){_mm_setcsr(csr);}};
class RenderHelperScope;
constinit thread_local bool native_render_profiling=false;
constinit thread_local RenderProfileWindow render_profile_window;
constinit thread_local RenderFrameProfile* active_render_profile=nullptr;
constinit thread_local RenderHelperScope* active_render_helper=nullptr;
// Injectable only for deterministic in-process diagnostics; no gameplay input.
constinit thread_local uint64_t (*render_profile_test_clock)() noexcept=nullptr;
constinit thread_local void (*render_profile_test_report)(const RenderProfileWindow&,uint64_t)=nullptr;
uint64_t RenderProfileNow() noexcept {
    RenderProfileFpGuard fp;
    return render_profile_test_clock?render_profile_test_clock():uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}
uint64_t ProfileElapsed(uint64_t end,uint64_t start){return end>=start?end-start:0;}
void ReportRenderProfile(uint64_t now) {
    if(!render_profile_window.open || ProfileElapsed(now,render_profile_window.started)<5000000000ull)return;
    RenderProfileFpGuard fp;
    const auto snapshot=render_profile_window;
    render_profile_window={}; // Reentrant sinks start a fresh window.
    const auto elapsed=ProfileElapsed(now,snapshot.started);
    if(render_profile_test_report){render_profile_test_report(snapshot,elapsed);return;}
    for(unsigned mode=0;mode<snapshot.modes.size();++mode) {
        const auto& frame=snapshot.modes[mode];if(!frame.calls)continue;
        const auto name=mode?"interpolated":"native";
        uint64_t total=0;for(auto span:frame.total)total+=span;
        const auto uninstrumented=total>frame.root_helper_ns?total-frame.root_helper_ns:0;
        REXLOG_INFO("Render profile {}: {} calls ({} aborted) / {:.3f}s; skin {:.3f}/{:.3f}, prepare {:.3f}/{:.3f}, submission {:.3f}/{:.3f}, restore {:.3f}/{:.3f} ms mean/max; outside measured helpers {:.3f} ms/call (thread-local wall time includes waits and profiling overhead; not GPU time)",
            name,frame.calls,frame.aborted,elapsed/1e9,frame.total[0]/double(frame.calls)/1e6,frame.maximum[0]/1e6,
            frame.total[1]/double(frame.calls)/1e6,frame.maximum[1]/1e6,frame.total[2]/double(frame.calls)/1e6,frame.maximum[2]/1e6,
            frame.total[3]/double(frame.calls)/1e6,frame.maximum[3]/1e6,uninstrumented/double(frame.calls)/1e6);
        std::string helpers;
        for(unsigned i=0;i<frame.helpers.size();++i) {
            const auto& h=frame.helpers[i];if(!h.calls)continue;
            helpers+=fmt::format("\nRender helper {}: {} calls ({} aborted), {:.3f} ms/render call inclusive, {:.3f} ms/render call self excluding measured children, {:.3f}/{:.3f} ms/helper call mean/max",
                render_helper_names[i],h.calls,h.aborted,h.total_ns/double(frame.calls)/1e6,h.self_ns/double(frame.calls)/1e6,h.total_ns/double(h.calls)/1e6,h.max_ns/1e6);
        }
        if(!helpers.empty())REXLOG_INFO("Render profile {} helper breakdown:{}",name,helpers);
    }
}
class RenderFrameScope {
public:
    RenderFrameScope(bool enabled,bool interpolated):mode_(interpolated?1:0) {
        if(!enabled || native_render_profiling)return;
        root_=true;last_=RenderProfileNow();uncaught_=std::uncaught_exceptions();
        if(!render_profile_window.open){render_profile_window.open=true;render_profile_window.started=last_;}
        active_render_profile=&render_profile_window.modes[mode_];native_render_profiling=true;
    }
    ~RenderFrameScope(){if(root_ && !finished_)Finalize(true);}
    void Mark(unsigned phase) {
        if(!root_)return;
        const auto now=RenderProfileNow();spans_[phase]+=ProfileElapsed(now,last_);last_=now;
    }
    void End() {
        if(!root_ || finished_)return;
        const auto now=Finalize(std::uncaught_exceptions()>uncaught_);
        // Reporting runs only after native rendering and after clearing TLS;
        // unwinding restores state without logging from a destructor.
        ReportRenderProfile(now);
    }
private:
    uint64_t Finalize(bool aborted) {
        const auto now=RenderProfileNow();spans_[mode_?3:2]+=ProfileElapsed(now,last_);
        auto& frame=*active_render_profile;++frame.calls;frame.aborted+=aborted;
        for(unsigned i=0;i<spans_.size();++i){frame.total[i]+=spans_[i];frame.maximum[i]=std::max(frame.maximum[i],spans_[i]);}
        active_render_profile=nullptr;native_render_profiling=false;finished_=true;return now;
    }
    bool root_=false,finished_=false;unsigned mode_=0;int uncaught_=0;uint64_t last_=0;
    std::array<uint64_t,4> spans_{};
};
class RenderHelperScope {
public:
    explicit RenderHelperScope(unsigned index):index_(index),parent_(active_render_helper),frame_(active_render_profile),started_(RenderProfileNow()),uncaught_(std::uncaught_exceptions()){active_render_helper=this;}
    ~RenderHelperScope() {
        const auto elapsed=ProfileElapsed(RenderProfileNow(),started_);
        active_render_helper=parent_;
        auto& h=frame_->helpers[index_];++h.calls;h.total_ns+=elapsed;h.max_ns=std::max(h.max_ns,elapsed);
        h.self_ns+=elapsed>children_?elapsed-children_:0;h.aborted+=std::uncaught_exceptions()>uncaught_;
        if(parent_)parent_->children_+=elapsed;else frame_->root_helper_ns+=elapsed;
    }
private:
    unsigned index_;RenderHelperScope* parent_;RenderFrameProfile* frame_;uint64_t started_,children_=0;int uncaught_;
};
template<typename Call>
__declspec(noinline) void ProfileRenderHelperMeasured(PPCContext& ctx,uint8_t* base,unsigned index,Call original) {
    RenderHelperScope scope(index);original(ctx,base);
}
template<typename Call>
void ProfileRenderHelper(PPCContext& ctx,uint8_t* base,unsigned index,Call original) {
    if(!native_render_profiling) { original(ctx,base); return; }
    ProfileRenderHelperMeasured(ctx,base,index,original);
}
struct RenderProfileProbe {uint64_t now=0,reads=0,reports=0,elapsed=0;RenderProfileWindow report;};
thread_local RenderProfileProbe render_profile_probe;
bool TestRenderProfiling() {
    struct Restore {
        bool enabled=native_render_profiling;RenderProfileWindow window=render_profile_window;
        RenderFrameProfile* frame=active_render_profile;RenderHelperScope* helper=active_render_helper;
        decltype(render_profile_test_clock) clock=render_profile_test_clock;
        decltype(render_profile_test_report) report=render_profile_test_report;
        RenderProfileProbe probe=render_profile_probe;unsigned csr=_mm_getcsr();
        ~Restore(){native_render_profiling=enabled;render_profile_window=window;active_render_profile=frame;active_render_helper=helper;
            render_profile_test_clock=clock;render_profile_test_report=report;render_profile_probe=probe;_mm_setcsr(csr);}
    } restore;
    auto check=[](bool ok,const char* message){if(!ok)REXLOG_ERROR("Render profiling test failed: {}",message);return ok;};
    native_render_profiling=false;active_render_profile=nullptr;active_render_helper=nullptr;render_profile_window={};render_profile_probe={};
    render_profile_test_clock=+[]() noexcept {
        ++render_profile_probe.reads;_mm_setcsr(_mm_getcsr()^0x2000);return render_profile_probe.now;
    };
    render_profile_test_report=+[](const RenderProfileWindow& window,uint64_t elapsed) {
        ++render_profile_probe.reports;render_profile_probe.report=window;render_profile_probe.elapsed=elapsed;
        _mm_setcsr(_mm_getcsr()^0x4020);
    };
    auto action=[](PPCContext& c,uint8_t* memory){c.r3.u64+=7;c.r6.u64^=0x123456789ABCDEF0ull;memory[3]^=0x5A;_mm_setcsr((_mm_getcsr()&~0x6000u)|0x6000u|0x20u);};
    auto parent=[&](PPCContext& c,uint8_t* memory) {
        render_profile_probe.now=20;
        ProfileRenderHelper(c,memory,1,[&](PPCContext& child,uint8_t* bytes){action(child,bytes);render_profile_probe.now=50;});
        action(c,memory);render_profile_probe.now=80;
    };
    PPCContext expected{};std::array<uint8_t,64> expected_memory{};
    const auto initial_csr=_mm_getcsr();parent(expected,expected_memory.data());const auto expected_csr=_mm_getcsr();
    PPCContext actual{};std::array<uint8_t,64> memory{};_mm_setcsr(initial_csr);
    render_profile_probe.now=0;
    {RenderFrameScope frame(true,false);render_profile_probe.now=10;ProfileRenderHelper(actual,memory.data(),0,parent);render_profile_probe.now=100;frame.End();}
    const auto& native=render_profile_window.modes[0];
    if(!check(!std::memcmp(&actual,&expected,sizeof(actual)) && memory==expected_memory && _mm_getcsr()==expected_csr,"instrumentation preserves complete context, output memory and original callback MXCSR") ||
       !check(native.calls==1 && native.total==std::array<uint64_t,4>{0,0,100,0} && native.maximum==native.total && native.root_helper_ns==70 &&
              native.helpers[0].calls==1 && native.helpers[0].total_ns==70 && native.helpers[0].self_ns==40 && native.helpers[1].total_ns==30,"native frame nesting and independent inclusive/self durations"))return false;
    render_profile_probe.now=200;
    {
        RenderFrameScope frame(true,true);render_profile_probe.now=210;frame.Mark(0);render_profile_probe.now=225;frame.Mark(1);
        render_profile_probe.now=230;
        ProfileRenderHelper(actual,memory.data(),2,[&](PPCContext& c,uint8_t* bytes) {
            render_profile_probe.now=240;ProfileRenderHelper(c,bytes,3,[&](PPCContext& c,uint8_t* b){action(c,b);render_profile_probe.now=260;});render_profile_probe.now=290;
        });
        render_profile_probe.now=300;frame.Mark(2);render_profile_probe.now=340;frame.End();
    }
    const auto& interpolated=render_profile_window.modes[1];
    if(!check(interpolated.calls==1 && interpolated.total==std::array<uint64_t,4>{10,15,75,40} && interpolated.root_helper_ns==60 &&
       interpolated.helpers[2].self_ns==40 && interpolated.helpers[3].self_ns==20 && native.helpers[2].calls==0,"interpolated phases and helpers are separate from native frames"))return false;
    render_profile_probe.now=500;
    {
        RenderFrameScope frame(true,false);render_profile_probe.now=510;
        ProfileRenderHelper(actual,memory.data(),0,[&](PPCContext& c,uint8_t* bytes) {
            RenderFrameScope nested(true,true);render_profile_probe.now=530;
            ProfileRenderHelper(c,bytes,1,[&](PPCContext& c,uint8_t* b){action(c,b);render_profile_probe.now=550;});
            nested.End();render_profile_probe.now=580;
        });
        render_profile_probe.now=600;frame.End();
    }
    if(!check(native.calls==2 && interpolated.calls==1 && native.helpers[0].self_ns==90 && native.helpers[1].self_ns==50 && !native_render_profiling && !active_render_helper,"nested renders reuse the outer frame without double-counting or losing parent timing"))return false;
    const auto before_disabled=render_profile_window;const auto reads=render_profile_probe.reads;
    {RenderFrameScope disabled(false,true);ProfileRenderHelper(actual,memory.data(),0,action);disabled.End();}
    if(!check(render_profile_window==before_disabled && render_profile_probe.reads==reads,"disabled profiling invokes no profiling clock and records no work"))return false;
    struct ProbeException {};
    render_profile_probe.now=1000;
    try {
        RenderFrameScope frame(true,false);render_profile_probe.now=1010;
        ProfileRenderHelper(actual,memory.data(),5,[&](PPCContext& c,uint8_t* bytes){action(c,bytes);render_profile_probe.now=1050;throw ProbeException{};});
        frame.End();return false;
    } catch(const ProbeException&) {}
    if(!check(native.calls==3 && native.aborted==1 && native.total[2]==250 && native.helpers[5].aborted==1 && native.helpers[5].total_ns==40 &&
       !native_render_profiling && !active_render_profile && !active_render_helper && !render_profile_probe.reports,"unwinding restores frame/helper TLS and records an aborted call without destructor logging"))return false;
    render_profile_probe.now=5000001000ull;const auto before_report_csr=_mm_getcsr();
    {RenderFrameScope frame(true,false);render_profile_probe.now+=50;frame.End();}
    if(!check(render_profile_probe.reports==1 && render_profile_probe.elapsed==5000001050ull && render_profile_probe.report.modes[0].calls==4 &&
       render_profile_probe.report.modes[1].calls==1 && render_profile_probe.report.modes[0].total[2]==300 && render_profile_window==RenderProfileWindow{} &&
       _mm_getcsr()==before_report_csr,"five-second reporting keeps both modes, resets the entire window and preserves callback FP state even with an FP-changing sink"))return false;
    render_profile_probe.now=6000000000ull;
    bool isolated=false;
    {
        RenderFrameScope frame(true,false);const auto main_window=render_profile_window;
        const auto clock=render_profile_test_clock;
        std::thread worker([&] {
            render_profile_test_clock=clock;render_profile_probe.now=10;PPCContext c{};std::array<uint8_t,64> b{};
            {RenderFrameScope own(true,false);render_profile_probe.now=20;ProfileRenderHelper(c,b.data(),7,[&](PPCContext& c,uint8_t* bytes){action(c,bytes);render_profile_probe.now=40;});render_profile_probe.now=60;own.End();}
            isolated=render_profile_window.modes[0].calls==1 && render_profile_window.modes[0].helpers[7].total_ns==20 && !native_render_profiling;
        });worker.join();
        if(!check(isolated && render_profile_window==main_window && native_render_profiling,"helper and frame windows are thread-local while another thread has an active render"))return false;
        render_profile_probe.now+=50;frame.End();
    }
    REXLOG_INFO("Render profiling tests passed: native/interpolated phase routing, nested inclusive/self accounting, disabled clock bypass, context/memory/MXCSR preservation, report reset, unwinding and thread isolation (synthetic callbacks; no gameplay capture)");
    return true;
}
struct Quaternion { float w=1,x=0,y=0,z=0; };

Quaternion Rotation(const Matrix& m) {
    Quaternion q;
    const float trace=m[0]+m[5]+m[10];
    if(trace>0) {
        const float s=std::sqrt(trace+1)*2;
        q={s*.25f,(m[9]-m[6])/s,(m[2]-m[8])/s,(m[4]-m[1])/s};
    } else if(m[0]>m[5] && m[0]>m[10]) {
        const float s=std::sqrt(std::max(0.f,1+m[0]-m[5]-m[10]))*2;
        if(s<1e-6f) return {};
        q={(m[9]-m[6])/s,s*.25f,(m[1]+m[4])/s,(m[2]+m[8])/s};
    } else if(m[5]>m[10]) {
        const float s=std::sqrt(std::max(0.f,1+m[5]-m[0]-m[10]))*2;
        if(s<1e-6f) return {};
        q={(m[2]-m[8])/s,(m[1]+m[4])/s,s*.25f,(m[6]+m[9])/s};
    } else {
        const float s=std::sqrt(std::max(0.f,1+m[10]-m[0]-m[5]))*2;
        if(s<1e-6f) return {};
        q={(m[4]-m[1])/s,(m[2]+m[8])/s,(m[6]+m[9])/s,s*.25f};
    }
    return q;
}

Matrix Interpolate(const Matrix& a,const Matrix& b,float alpha) {
    if(alpha<=0) return a;
    if(alpha>=1) return b;
    // Preserve non-transform/invalid data rather than propagating NaNs.
    for(auto i:transform_components) if(!std::isfinite(a[i]) || !std::isfinite(b[i])) return b;
    Matrix out=b;
    for(unsigned i=12;i<16;++i) out[i]=a[i]+alpha*(b[i]-a[i]);
    if(!RigidRotation(a) || !RigidRotation(b)) {
        // Preserve authored nonuniform scale, shear and reflections instead of
        // normalizing them away as if every bone matrix were a pure rotation.
        for(unsigned row=0;row<3;++row) for(unsigned col=0;col<3;++col) {
            const auto i=row*4+col;
            out[i]=a[i]+alpha*(b[i]-a[i]);
        }
        return out;
    }
    auto p=Rotation(a),q=Rotation(b);
    float dot=p.w*q.w+p.x*q.x+p.y*q.y+p.z*q.z;
    if(dot<0) { q={-q.w,-q.x,-q.y,-q.z}; dot=-dot; }
    float u=1-alpha,v=alpha;
    if(dot<.9995f) {
        const float angle=std::acos(std::clamp(dot,-1.f,1.f));
        const float inverse=1/std::sin(angle);
        u=std::sin((1-alpha)*angle)*inverse;
        v=std::sin(alpha*angle)*inverse;
    }
    Quaternion r{u*p.w+v*q.w,u*p.x+v*q.x,u*p.y+v*q.y,u*p.z+v*q.z};
    const float length=std::sqrt(r.w*r.w+r.x*r.x+r.y*r.y+r.z*r.z);
    if(length<1e-6f || !std::isfinite(length)) return b;
    r.w/=length; r.x/=length; r.y/=length; r.z/=length;
    out[0]=1-2*(r.y*r.y+r.z*r.z); out[1]=2*(r.x*r.y-r.z*r.w); out[2]=2*(r.x*r.z+r.y*r.w);
    out[4]=2*(r.x*r.y+r.z*r.w); out[5]=1-2*(r.x*r.x+r.z*r.z); out[6]=2*(r.y*r.z-r.x*r.w);
    out[8]=2*(r.x*r.z-r.y*r.w); out[9]=2*(r.y*r.z+r.x*r.w); out[10]=1-2*(r.x*r.x+r.y*r.y);
    return out;
}

// Endpoint analysis is invariant across render samples of one completed tick.
// Keep the original interpolation above as the reference and fallback. No
// approximate trigonometry or changed animation/physics cadence is involved.
struct PreparedInterpolation {
    enum Kind {Invalid,Affine,Rigid} kind=Invalid;
    Quaternion p{},q{};
    bool spherical=false;
    float angle=0,inverse=0;
};
PreparedInterpolation PrepareInterpolation(const Matrix& a,const Matrix& b) {
    PreparedInterpolation result;
    for(auto i:transform_components)if(!std::isfinite(a[i]) || !std::isfinite(b[i]))return result;
    result.kind=PreparedInterpolation::Affine;
    if(!RigidRotation(a) || !RigidRotation(b))return result;
    result.kind=PreparedInterpolation::Rigid;
    result.p=Rotation(a);result.q=Rotation(b);
    auto& p=result.p;auto& q=result.q;
    float dot=p.w*q.w+p.x*q.x+p.y*q.y+p.z*q.z;
    if(dot<0){q={-q.w,-q.x,-q.y,-q.z};dot=-dot;}
    if(dot<.9995f){result.spherical=true;result.angle=std::acos(std::clamp(dot,-1.f,1.f));result.inverse=1/std::sin(result.angle);}
    return result;
}
Matrix InterpolatePrepared(const Matrix& a,const Matrix& b,float alpha,const PreparedInterpolation& prepared) {
    if(alpha<=0)return a;
    if(alpha>=1)return b;
    if(prepared.kind==PreparedInterpolation::Invalid)return b;
    Matrix out=b;
    for(unsigned i=12;i<16;++i)out[i]=a[i]+alpha*(b[i]-a[i]);
    if(prepared.kind==PreparedInterpolation::Affine){
        for(unsigned row=0;row<3;++row)for(unsigned col=0;col<3;++col){const auto i=row*4+col;out[i]=a[i]+alpha*(b[i]-a[i]);}
        return out;
    }
    const auto& p=prepared.p;const auto& q=prepared.q;
    float u=1-alpha,v=alpha;
    if(prepared.spherical){u=std::sin((1-alpha)*prepared.angle)*prepared.inverse;v=std::sin(alpha*prepared.angle)*prepared.inverse;}
    Quaternion r{u*p.w+v*q.w,u*p.x+v*q.x,u*p.y+v*q.y,u*p.z+v*q.z};
    const float length=std::sqrt(r.w*r.w+r.x*r.x+r.y*r.y+r.z*r.z);
    if(length<1e-6f || !std::isfinite(length))return b;
    r.w/=length;r.x/=length;r.y/=length;r.z/=length;
    out[0]=1-2*(r.y*r.y+r.z*r.z);out[1]=2*(r.x*r.y-r.z*r.w);out[2]=2*(r.x*r.z+r.y*r.w);
    out[4]=2*(r.x*r.y+r.z*r.w);out[5]=1-2*(r.x*r.x+r.z*r.z);out[6]=2*(r.y*r.z-r.x*r.w);
    out[8]=2*(r.x*r.z-r.y*r.w);out[9]=2*(r.y*r.z+r.x*r.w);out[10]=1-2*(r.x*r.x+r.y*r.y);
    return out;
}

struct PreparedPalette {
    unsigned control=0,flags=0;
    std::vector<PreparedInterpolation> bones;
};
struct SkinInterpolationCache {
    std::mutex mutex;
    std::shared_ptr<const PreparedPalette> palette;
};

Matrix ReadTransform(uint32_t object,uint8_t* base) {
    Matrix result;
    for(unsigned i=0;i<16;++i) result[i]=std::bit_cast<float>(PPC_LOAD_U32(object+144+i*4));
    return result;
}
void WriteTransform(uint32_t object,const Matrix& transform,uint8_t* base) {
    for(unsigned i=0;i<16;++i) PPC_STORE_U32(object+144+i*4,std::bit_cast<uint32_t>(transform[i]));
}

// NiRefObject's counter is a big-endian uint32 at +4. Holding one engine
// reference prevents a transform-history pointer from surviving its object.
uint32_t ChangeReference(uint32_t object,int change,uint8_t* base) {
    auto* counter=reinterpret_cast<uint32_t*>(PPC_RAW_ADDR(object+4));
    std::atomic_ref<uint32_t> atomic(*counter);
    auto current=atomic.load(std::memory_order_relaxed);
    for(;;) {
        const auto value=std::byteswap(current)+change;
        if(atomic.compare_exchange_weak(current,std::byteswap(value),std::memory_order_acq_rel)) return value;
    }
}

struct TransformHistory {
    Matrix previous,current,saved;
    uint64_t updated=0;
    uint32_t parent=0;
    bool camera=false,applied=false,moving=false;
    PreparedInterpolation prepared{};
    unsigned prepared_control=0,prepared_flags=0;
    bool prepared_ready=false;
    uint8_t scene_samples=0,prepare_streak=0;
};
void BeginSceneInterpolationTick(TransformHistory& h) {
    h.prepared_ready=false;
    h.prepare_streak=h.scene_samples>1?std::min<unsigned>(h.prepare_streak+1,2):0;
    h.scene_samples=0;
}
Matrix InterpolateScene(TransformHistory& h,float alpha) {
    if(!REXCVAR_GET(em2_cache_scene_interpolation) || !(alpha>0 && alpha<1))return Interpolate(h.previous,h.current,alpha);
    if(h.scene_samples<2)++h.scene_samples;
    // Do not pay preparation overhead for a single interpolation per tick.
    // Observe sustained reuse instead of guessing from the selected FPS cap.
    if(h.prepare_streak<2)return Interpolate(h.previous,h.current,alpha);
    const auto csr=_mm_getcsr();
    if((csr&0x1F80)!=0x1F80)return Interpolate(h.previous,h.current,alpha);
    const auto control=csr&~0x3Fu;
    if(h.prepared_ready && h.prepared_control!=control)return Interpolate(h.previous,h.current,alpha);
    if(!h.prepared_ready) {
        struct Restore {unsigned csr;~Restore(){_mm_setcsr(csr|(_mm_getcsr()&0x3F));}} restore{csr};
        _mm_setcsr(control);
        h.prepared=PrepareInterpolation(h.previous,h.current);
        h.prepared_control=control;h.prepared_flags=_mm_getcsr()&0x3F;h.prepared_ready=true;
    }
    const auto now=_mm_getcsr();if((now|h.prepared_flags)!=now)_mm_setcsr(now|h.prepared_flags);
    return InterpolatePrepared(h.previous,h.current,alpha,h.prepared);
}
void BenchmarkSceneInterpolation() {
    if(!REXCVAR_GET(em2_benchmark_scene_interpolation))return;
    struct Restore {unsigned csr=_mm_getcsr();bool enabled=REXCVAR_GET(em2_cache_scene_interpolation);
        DWORD_PTR affinity=SetThreadAffinityMask(GetCurrentThread(),DWORD_PTR(1)<<GetCurrentProcessorNumber());
        ~Restore(){REXCVAR_SET(em2_cache_scene_interpolation,enabled);if(affinity)SetThreadAffinityMask(GetCurrentThread(),affinity);_mm_setcsr(csr);}}restore;
    _mm_setcsr(0x1F80);
    for(unsigned count:{64u,256u})for(bool spherical:{false,true})for(unsigned samples:{1u,2u,4u,8u}) {
        std::vector<TransformHistory> nodes(count);
        for(unsigned i=0;i<count;++i) {
            auto& h=nodes[i];h.previous={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};h.current=h.previous;
            const auto angle=float(i)*.043f,next=angle+(spherical?.31f:.01f);
            h.previous[0]=h.previous[5]=std::cos(angle);h.previous[1]=-std::sin(angle);h.previous[4]=std::sin(angle);
            h.current[0]=h.current[5]=std::cos(next);h.current[1]=-std::sin(next);h.current[4]=std::sin(next);
        }
        auto run=[&](bool cached,unsigned ticks) {
            REXCVAR_SET(em2_cache_scene_interpolation,cached);uint64_t checksum=0;
            for(auto& node:nodes){node.prepare_streak=0;node.scene_samples=0;node.prepared_ready=false;}
            const auto started=Clock::now();
            for(unsigned tick=0;tick<ticks;++tick) {
                for(unsigned i=0;i<count;++i){BeginSceneInterpolationTick(nodes[i]);nodes[i].current[12]=float(tick+i)*.0001f;}
                for(unsigned sample=0;sample<samples;++sample)for(auto& node:nodes) {
                    const auto matrix=InterpolateScene(node,(sample+.5f)/samples);
                    for(auto value:matrix)checksum+=std::bit_cast<uint32_t>(value);
                }
            }
            return std::pair(std::chrono::duration<double,std::micro>(Clock::now()-started).count()/(ticks*samples),checksum);
        };
        const auto warm_until=Clock::now()+std::chrono::milliseconds(15);
        do{run(false,8);run(true,8);}while(Clock::now()<warm_until);
        std::array<std::array<double,7>,2> timings{};
        for(unsigned round=0;round<7;++round) {
            const auto first=run(round&1,64),second=run(!(round&1),64);
            if(first.second!=second.second)throw std::runtime_error("Scene interpolation benchmark output mismatch");
            timings[round&1][round]=first.first;timings[!(round&1)][round]=second.first;
        }
        for(auto& mode:timings)std::sort(mode.begin(),mode.end());
        REXLOG_INFO("Scene interpolation benchmark: {} nodes, spherical {}, {} samples/tick, reference {:.3f}, cached {:.3f} us/render, {:.3f}x (seven alternating rounds x64 ticks, adaptive warmup/preparation/flags/checksum included; interpolation calculation only, not gameplay FPS)",count,spherical,samples,timings[0][3],timings[1][3],timings[0][3]/timings[1][3]);
    }
}
std::mutex history_mutex;
std::unordered_map<uint32_t,TransformHistory> history;
struct MovingTransform {uint32_t object;TransformHistory* state;};
// unordered_map rehash preserves node references. Expiry/reset clear this list
// before reuse; native detach removes its entry before erasing the history node.
std::vector<MovingTransform> moving_objects;
void ForgetTransform(uint32_t object) {
    const auto found=history.find(object);if(found==history.end())return;
    if(found->second.moving)std::erase_if(moving_objects,[&](const auto& entry){return entry.object==object;});
    history.erase(found);
}
// Animation tasks may execute on engine worker threads. Only the core clock
// needs a thread-local tick scope; transform recording follows the shared epoch.
std::atomic<uint64_t> recording_epoch{0};
thread_local bool in_tick=false;
thread_local bool record_random=false;
thread_local em2::net::RandomObservation random_observation;
struct RandomTraceEvent {bool reseed;uint32_t caller,state,result;};
struct ScriptRandomOwner {
    size_t event=0;
    uint32_t defined_line=0;
    std::array<char,192> source{};
    std::array<uint32_t,12> parents{};
};
struct ScriptWaitTrace {
    bool agreed;
    uint64_t tick;
    uint32_t slot,count,target,remaining,flags,scaled,unscaled;
    uint32_t after_count=0,after_target=0,after_remaining=0,after_flags=0;
};
thread_local std::array<ScriptWaitTrace,384> script_wait_trace{};
thread_local size_t script_wait_trace_size=0;
void LogScriptWaitTrace(){
    for(size_t i=0;i<script_wait_trace_size;++i){
        const auto& wait=script_wait_trace[i];
        REXLOG_INFO("Native online Lua wait: agreed {}, tick {}, slot {}/{}, target {:08X}, remaining {:08X}, flags {:08X}, scaled {:08X}, unscaled {:08X}, after count {}, target {:08X}, remaining {:08X}, flags {:08X}",
            wait.agreed,wait.tick,wait.slot,wait.count,wait.target,wait.remaining,wait.flags,wait.scaled,wait.unscaled,
            wait.after_count,wait.after_target,wait.after_remaining,wait.after_flags);
    }
    script_wait_trace_size=0;
}
thread_local std::array<RandomTraceEvent,4096> online_random_trace{};
thread_local size_t online_random_trace_size=0;
// Lazily allocate on the tracing simulation thread only. Preserve recent basic
// CRT events until failure, rather than logging every successful frame.
struct RandomFailureHistory {
    struct Frame {
        uint64_t tick=0;
        size_t size=0,script_count=0;
        bool valid=false,published=false;
        std::array<RandomTraceEvent,4096> events{};
        std::array<ScriptRandomOwner,32> scripts{};
        bool character_coverage=false;
        std::vector<em2::net::GameplayObservation::CharacterPhysics> characters;
    };
    std::array<Frame,4> frames{};size_t next=0;Frame* current=nullptr;
    void Begin(uint64_t tick){auto& f=frames[next++%frames.size()];f.tick=tick;f.size=0;f.script_count=0;f.valid=true;f.published=false;f.character_coverage=false;f.characters.clear();current=&f;}
    void Record(RandomTraceEvent event){if(current && current->size<current->events.size())current->events[current->size++]=event;}
    template<class Emit>void Drain(Emit emit){
        std::array<Frame*,4> sorted{};for(size_t i=0;i<frames.size();++i)sorted[i]=&frames[i];
        std::sort(sorted.begin(),sorted.end(),[](const auto* a,const auto* b){return a->tick<b->tick;});
        for(auto* f:sorted)if(f->valid && !f->published){emit(*f);f->published=true;}
    }
};
thread_local std::unique_ptr<RandomFailureHistory> random_failure_history;
bool TestRandomFailureHistory(){
    auto h=std::make_unique<RandomFailureHistory>();
    for(uint64_t tick=0;tick<6;++tick){h->Begin(tick);h->Record({false,uint32_t(tick),7,9});}
    std::vector<uint64_t> ticks;
    h->frames[2].published=true; // tick 2 was already emitted by the explicit window.
    bool valid=true;
    h->Drain([&](const auto& f){ticks.push_back(f.tick);valid&=f.size==1 && f.events[0].caller==f.tick;});
    if(!valid || ticks!=std::vector<uint64_t>{3,4,5})return false;
    h->Drain([&](const auto&){valid=false;});if(!valid)return false;
    h->Begin(6);for(size_t i=0;i<4097;++i)h->Record({false,uint32_t(i),0,0});
    size_t count=0;h->Drain([&](const auto& f){++count;valid&=f.tick==6 && f.size==4096 && f.events.back().caller==4095;});
    return valid && count==1;
}
thread_local bool capture_online_random=false;
thread_local bool capture_online_random_owners=false;
thread_local bool capture_script_random_owners=false;
struct RandomOwnerTrace {size_t event;std::array<uint32_t,3> registers{},heads{};std::array<uint32_t,6> returns{};std::array<uint32_t,5> schedule{};std::array<uint32_t,2> identity{};bool scheduled=false;};
thread_local std::array<RandomOwnerTrace,32> online_random_owners{};
thread_local size_t online_random_owner_size=0;
uint32_t RandomTraceWord(uint8_t* base,uint32_t address){
    if(!base || address<4096 || address>UINT32_MAX-4)return 0;
    uint32_t word=0;SIZE_T copied=0;
    if(!ReadProcessMemory(GetCurrentProcess(),PPC_RAW_ADDR(address),&word,sizeof(word),&copied) || copied!=sizeof(word))return 0;
    return _byteswap_ulong(word);
}

ScriptRandomOwner TraceScriptRandomOwner(uint8_t* base,const PPCContext& ctx,size_t event) {
    ScriptRandomOwner owner;owner.event=event;
    auto stack=ctx.r1.u32;
    for(auto& address:owner.parents){
        const auto parent=RandomTraceWord(base,stack);
        if(parent<=stack || parent-stack>65536)break;
        address=RandomTraceWord(base,parent-8);stack=parent;
    }
    const auto state=ctx.r31.u32;
    // The retail debug API reads the CallInfo stack at L+12/+20. A saved
    // frame base points past its function slot. Tag 9 is a Lua closure.
    const auto first=RandomTraceWord(base,state+12),current=RandomTraceWord(base,state+20);
    if(!first || current<first || (current-first)%16 || current-first>16*1024*1024)return owner;
    for(auto frame=current;frame>first && (current-frame)/16<8;frame-=16){
        const auto lua_stack=RandomTraceWord(base,frame);
        if(lua_stack<4096 || (RandomTraceWord(base,lua_stack-4)&15)!=9)continue;
        const auto closure=RandomTraceWord(base,lua_stack-8);
        const auto proto=RandomTraceWord(base,closure+8);
        const auto location=RandomTraceWord(base,proto+40);
        const auto source=RandomTraceWord(base,location+24);
        if(!closure || !proto || !location || !source)continue;
        const auto length=RandomTraceWord(base,source+4)&0x3fffffff;
        const auto count=std::min<size_t>(length,owner.source.size()-1);
        SIZE_T copied=0;
        if(!count || uint64_t(source)+12+count>0x100000000ull ||
           !ReadProcessMemory(GetCurrentProcess(),PPC_RAW_ADDR(source+12),owner.source.data(),count,&copied) ||
           copied!=count) {
            owner.source.fill(0);
            continue;
        }
        owner.defined_line=RandomTraceWord(base,location);
        for(size_t i=0;i<count;++i)if(static_cast<unsigned char>(owner.source[i])<32)owner.source[i]='?';
        break;
    }
    return owner;
}
bool TestScriptRandomOwner(){
    struct HostState {DWORD error=GetLastError();unsigned csr=_mm_getcsr();~HostState(){SetLastError(error);_mm_setcsr(csr);}}host;
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,65536,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!base)return false;
    struct Release{uint8_t* base;~Release(){VirtualFree(base,0,MEM_RELEASE);}}release{base};
    PPCContext ctx{};ctx.r31.u64=0x2000;ctx.r1.u64=0x8000;
    PPC_STORE_U32(0x200C,0x3000);PPC_STORE_U32(0x2014,0x3010);PPC_STORE_U32(0x3010,0x4008);
    PPC_STORE_U32(0x4004,9);PPC_STORE_U32(0x4000,0x5000);PPC_STORE_U32(0x5008,0x5100);
    PPC_STORE_U32(0x5128,0x5200);PPC_STORE_U32(0x5218,0x5300);PPC_STORE_U32(0x5200,449);
    constexpr char source[]="@fixture/script.lua";PPC_STORE_U32(0x5304,sizeof(source)-1);std::memcpy(base+0x530C,source,sizeof(source));
    PPC_STORE_U32(0x8000,0x8100);PPC_STORE_U32(0x80F8,0x8273DA80);
    const auto saved=ctx;const auto bytes=std::vector<uint8_t>(base,base+65536);
    const auto owner=TraceScriptRandomOwner(base,ctx,24);
    if(owner.event!=24 || owner.defined_line!=449 || std::strcmp(owner.source.data(),source) || owner.parents[0]!=0x8273DA80 ||
       std::memcmp(&ctx,&saved,sizeof(ctx)) || std::memcmp(base,bytes.data(),bytes.size()))return false;
    PPC_STORE_U32(0x2014,0x3011);
    if(TraceScriptRandomOwner(base,ctx,25).source[0])return false;
    PPC_STORE_U32(0x2014,0x3010);PPC_STORE_U32(0x5304,0x3FFFFFFF);
    const auto bounded=TraceScriptRandomOwner(base,ctx,26);
    if(std::string_view(bounded.source.data()).size()>191 || bounded.source.back())return false;
    PPC_STORE_U32(0x5218,0xFFFFFFF0);
    if(TraceScriptRandomOwner(base,ctx,27).source[0])return false;
    REXLOG_INFO("Native Lua owner capture passed heap-string/call-info/stack, malformed alignment, bounded length, unreadable source and read-only context/data checks");return true;
}
void LogScriptRandomOwners(const RandomFailureHistory::Frame& frame) {
    for(size_t i=0;i<frame.script_count;++i){
        const auto& owner=frame.scripts[i];
        REXLOG_INFO("Native online Lua RNG: tick {}, index {}, source '{}', function line {}",
            frame.tick,owner.event,owner.source[0]?owner.source.data():"<unavailable>",owner.defined_line);
        REXLOG_INFO("Native online Lua stack: tick {}, index {}, returns {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
            frame.tick,owner.event,owner.parents[0],owner.parents[1],owner.parents[2],owner.parents[3],
            owner.parents[4],owner.parents[5],owner.parents[6],owner.parents[7],owner.parents[8],
            owner.parents[9],owner.parents[10],owner.parents[11]);
    }
}

struct TraceTargetCache{bool ready=false;uint64_t tick=0;std::unordered_map<uint32_t,uint32_t> handles;};
thread_local TraceTargetCache trace_target_cache;
std::array<uint32_t,2> TraceTargetIdentity(uint8_t* base,uint32_t target,uint64_t tick){
    auto& cache=trace_target_cache;
    if(!cache.ready || cache.tick!=tick){
        cache.ready=true;cache.tick=tick;cache.handles.clear();
        const auto registry=RandomTraceWord(base,0x833DA7E4);
        const auto count=RandomTraceWord(base,registry+20),buckets=RandomTraceWord(base,registry+16);
        if(registry && count && count<=65536 && RandomTraceWord(base,registry+4)==0x827B58D0 && RandomTraceWord(base,registry+8)==0x82A69948){
            size_t visited=0;
            for(uint32_t bucket=0;bucket<count && visited<16384;++bucket){
                auto node=RandomTraceWord(base,buckets+bucket*4);
                for(unsigned depth=0;node && depth<4096 && visited<16384;++depth,++visited){
                    const auto handle=RandomTraceWord(base,node),entity=RandomTraceWord(base,node+4),next=RandomTraceWord(base,node+8);
                    if(handle && entity)cache.handles.emplace(entity,handle);
                    if(next==node)break;node=next;
                }
            }
        }
    }
    const auto attached=RandomTraceWord(base,target+60);
    for(const uint32_t entity:{target,target+8,attached,attached?attached+8:0}){
        if(const auto i=cache.handles.find(entity);i!=cache.handles.end())return {entity,i->second};
    }
    return {};
}


thread_local bool in_clock_update=false;
thread_local uint64_t tick_number=0;
thread_local uint64_t tick_counter=0;
thread_local uint64_t tick_frequency=50000000;
thread_local uint64_t counter_remainder=0;
thread_local bool agreed_clock_tick=false;
thread_local uint64_t clock_input_tick=0;
thread_local bool frame_active=false;
thread_local bool frame_halted=false;
thread_local unsigned frame_steps=0;
thread_local bool frame_finish_pending=false;
thread_local uint32_t application_delta=0;
thread_local em2::SimulationScheduler scheduler;
thread_local em2::SimulationDebt network_debt;
double RenderAlpha() {
    const auto& gate=em2::net::GameplayInputGate();
    return frame_halted || em2::net::NativeRoomGameplayWaiting() || (gate.active() && gate.waiting()) || (!gate.active() && !em2::EffectiveSimulationSpeedPercent())?1.0:scheduler.Alpha();
}
thread_local std::vector<uint32_t> ui_containers;

// MeshSkinningModifier owns its attached NiMesh at +96. Its NiSP input
// stream covers +104..+8319: a header, 128 64-byte bone transforms, morph data.
// Reuse the native jobs and completion/upload path; never patch vertex buffers.
struct SkinPose {
    uint32_t mesh=0,source=0,morph=0;
    uint64_t updated=0;
    std::vector<Matrix> previous,current;
    std::shared_ptr<SkinInterpolationCache> interpolation_cache;
};
struct SkinHistory:SkinPose { std::shared_ptr<const SkinPose> snapshot; };
std::shared_ptr<const SkinPose> SnapshotSkin(SkinHistory& h) {
    if(!h.snapshot){
        auto pose=static_cast<const SkinPose&>(h);
        // At a 30-FPS cap, or after a multi-tick catch-up, there is no expected
        // reuse of this capture. Keep those frames on the allocation-free
        // reference path instead of paying to prepare a single sample.
        if(REXCVAR_GET(em2_cache_pose_interpolation) &&
           (!frame_active || (em2::FrameLimit()!=30 && frame_steps<=1)))
            pose.interpolation_cache=std::make_shared<SkinInterpolationCache>();
        h.snapshot=std::make_shared<const SkinPose>(std::move(pose));
    }
    return h.snapshot;
}
std::shared_ptr<const PreparedPalette> PreparedSkin(const SkinPose& h,float alpha) {
    if(!REXCVAR_GET(em2_cache_pose_interpolation) || !h.interpolation_cache || !(alpha>0 && alpha<1))return {};
    const auto csr=_mm_getcsr();
    // Preserve unusual floating-point control states through the reference
    // path, including unmasked exceptions. Cache only the first control mode
    // seen for this immutable snapshot; other modes fall back, never reuse it.
    if((csr&0x1F80)!=0x1F80)return {};
    const auto control=csr&~0x3Fu;
    std::lock_guard lock(h.interpolation_cache->mutex);
    auto& cached=h.interpolation_cache->palette;
    if(cached && cached->control!=control)return {};
    if(!cached){
        auto palette=std::make_shared<PreparedPalette>();
        palette->control=control;palette->bones.resize(h.current.size());
        struct Restore {unsigned csr;~Restore(){_mm_setcsr(csr|(_mm_getcsr()&0x3F));}} restore{csr};
        _mm_setcsr(control);
        for(unsigned bone=0;bone<h.current.size();++bone)
            palette->bones[bone]=PrepareInterpolation(h.previous[bone],h.current[bone]);
        palette->flags=_mm_getcsr()&0x3F;
        cached=std::move(palette);
    }
    // Replaying cached arithmetic must still raise the sticky status bits that
    // the original endpoint analysis would have raised before the native job.
    const auto now=_mm_getcsr();
    if((now|cached->flags)!=now)_mm_setcsr(now|cached->flags);
    return cached;
}
bool TestPreparedSkinInterpolation() {
    struct Restore {unsigned csr=_mm_getcsr();bool enabled=REXCVAR_GET(em2_cache_pose_interpolation),scene=REXCVAR_GET(em2_cache_scene_interpolation);
        ~Restore(){REXCVAR_SET(em2_cache_pose_interpolation,enabled);REXCVAR_SET(em2_cache_scene_interpolation,scene);_mm_setcsr(csr);}} restore;
    REXCVAR_SET(em2_cache_pose_interpolation,true);REXCVAR_SET(em2_cache_scene_interpolation,true);
    _mm_setcsr(0x1F80);
    SkinPose pose;
    Matrix identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    for(unsigned index=0;index<96;++index){
        auto a=identity,b=identity;
        const auto angle=float(index)*.137f;
        const auto axis=index%3;
        const auto x=(axis+1)%3,y=(axis+2)%3;
        a[x*4+x]=a[y*4+y]=std::cos(angle);a[x*4+y]=-std::sin(angle);a[y*4+x]=std::sin(angle);
        b[x*4+x]=b[y*4+y]=std::cos(angle+.033f);b[x*4+y]=-std::sin(angle+.033f);b[y*4+x]=std::sin(angle+.033f);
        if(index%4==0)b=identity; // Spherical path and all quaternion branches.
        if(index%11==0){a[0]=2;b[5]=.5f;} // Nonuniform scale.
        if(index%13==0){a[0]=-1;b[0]=-1;} // Reflection.
        if(index%17==0){a[1]=.25f;b[6]=.4f;} // Shear.
        if(index%19==0)a[14]=std::bit_cast<float>(uint32_t(0x7FC01234));
        if(index%23==0)b[12]=INFINITY;
        a[12]=float(index)*.001f;b[13]=float(index)*-.013f;
        if(index%7==0){a[15]=std::bit_cast<float>(uint32_t(1));b[15]=std::bit_cast<float>(uint32_t(17));}
        a[3]=std::bit_cast<float>(uint32_t(0x7FA01234));b[7]=std::bit_cast<float>(uint32_t(0x7FC05678));
        pose.previous.push_back(a);pose.current.push_back(b);
    }
    unsigned comparisons=0;
    for(unsigned rounding=0;rounding<4;++rounding)for(unsigned flush=0;flush<4;++flush){
        const unsigned control=0x1F80|(rounding<<13)|((flush&1)?0x8000:0)|((flush&2)?0x40:0);
        pose.interpolation_cache=std::make_shared<SkinInterpolationCache>();
        std::vector<TransformHistory> scenes(pose.current.size());
        for(size_t i=0;i<scenes.size();++i){scenes[i].previous=pose.previous[i];scenes[i].current=pose.current[i];scenes[i].prepare_streak=2;}
        for(unsigned pass=0;pass<2;++pass)for(unsigned sticky:{0u,0x3Fu})for(float alpha:{-.1f,0.f,.125f,.5f,.875f,1.f,1.1f}){
            std::vector<Matrix> expected;
            _mm_setcsr(control|sticky);
            for(unsigned bone=0;bone<pose.current.size();++bone)expected.push_back(Interpolate(pose.previous[bone],pose.current[bone],alpha));
            const auto expected_csr=_mm_getcsr();
            _mm_setcsr(control|sticky);
            const auto prepared=PreparedSkin(pose,alpha);
            for(unsigned bone=0;bone<pose.current.size();++bone){
                const auto actual=prepared?InterpolatePrepared(pose.previous[bone],pose.current[bone],alpha,prepared->bones[bone]):Interpolate(pose.previous[bone],pose.current[bone],alpha);
                if(std::memcmp(actual.data(),expected[bone].data(),sizeof(Matrix))){REXLOG_ERROR("Prepared interpolation mismatch: round {}, flush {}, pass {}, sticky {}, alpha {}, bone {}",rounding,flush,pass,sticky,alpha,bone);return false;}
                ++comparisons;
            }
            if(_mm_getcsr()!=expected_csr){REXLOG_ERROR("Prepared interpolation FP status mismatch: round {}, flush {}, pass {}, sticky {}, alpha {}, expected {:X}, got {:X}",rounding,flush,pass,sticky,alpha,expected_csr,_mm_getcsr());return false;}
            _mm_setcsr(control|sticky);
            for(size_t i=0;i<scenes.size();++i) {
                const auto actual=InterpolateScene(scenes[i],alpha);
                if(std::memcmp(actual.data(),expected[i].data(),sizeof(Matrix))){REXLOG_ERROR("Scene preparation mismatch: round {}, flush {}, pass {}, alpha {}, node {}",rounding,flush,pass,alpha,i);return false;}
            }
            if(_mm_getcsr()!=expected_csr){REXLOG_ERROR("Scene preparation FP flags mismatch");return false;}
        }
        _mm_setcsr(control^0x2000);
        if(PreparedSkin(pose,.5f)){REXLOG_ERROR("Prepared interpolation reused a different rounding mode");return false;}
        _mm_setcsr(control&~0x80u);
        if(PreparedSkin(pose,.5f)){REXLOG_ERROR("Prepared interpolation accepted unmasked FP exceptions");return false;}
        for(const auto altered:{control^0x2000u,control&~0x80u}) {
            _mm_setcsr(altered);const auto expected=Interpolate(scenes[1].previous,scenes[1].current,.37f);const auto flags=_mm_getcsr();
            _mm_setcsr(altered);const auto actual=InterpolateScene(scenes[1],.37f);
            if(std::memcmp(actual.data(),expected.data(),sizeof(Matrix)) || _mm_getcsr()!=flags){REXLOG_ERROR("Scene preparation control-state fallback mismatch");return false;}
        }
    }
    REXLOG_INFO("Prepared skin and scene interpolation matched {} complete matrices and MXCSR: cold/warm caches, endpoints, spherical/linear/affine/invalid data, padding, denormals, four rounding modes, FTZ/DAZ and sticky flags",comparisons);
    return true;
}
void BenchmarkSkinSnapshots() {
    if(!REXCVAR_GET(em2_benchmark_skin_snapshots))return;
    struct Scope {
        unsigned csr=_mm_getcsr();
        DWORD_PTR affinity=SetThreadAffinityMask(GetCurrentThread(),DWORD_PTR(1)<<GetCurrentProcessorNumber());
        ~Scope(){if(affinity)SetThreadAffinityMask(GetCurrentThread(),affinity);_mm_setcsr(csr);}
    } scope;
    for(unsigned modifiers:{1u,8u,32u})for(unsigned bones:{32u,128u})for(unsigned samples:{1u,2u,4u,8u}) {
        std::vector<SkinHistory> histories(modifiers);
        for(auto& h:histories){h.previous.resize(bones);h.current.resize(bones);for(unsigned b=0;b<bones;++b){h.previous[b][12]=float(b);h.current[b][12]=float(b+1);}}
        auto run=[&](bool cached,unsigned ticks) {
            uint64_t checksum=0;
            std::vector<std::pair<uint32_t,std::shared_ptr<const SkinPose>>> active;active.reserve(modifiers);
            const auto start=Clock::now();
            for(unsigned tick=0;tick<ticks;++tick) {
                for(auto& h:histories){h.snapshot.reset();h.current[0][12]=float(tick+1);}
                for(unsigned frame=0;frame<samples;++frame) {
                    if(cached) {
                        active.clear();for(unsigned i=0;i<modifiers;++i)active.emplace_back(i,SnapshotSkin(histories[i]));
                        for(const auto& [i,h]:active)checksum+=std::bit_cast<uint32_t>(h->current[(tick+frame+i)%bones][12]);
                    }else {
                        std::vector<std::pair<uint32_t,SkinPose>> old;
                        for(unsigned i=0;i<modifiers;++i)old.emplace_back(i,static_cast<const SkinPose&>(histories[i]));
                        for(const auto& [i,h]:old)checksum+=std::bit_cast<uint32_t>(h.current[(tick+frame+i)%bones][12]);
                    }
                }
            }
            return std::pair{std::chrono::duration<double,std::micro>(Clock::now()-start).count()/(ticks*samples),checksum};
        };
        run(false,8);run(true,8);
        std::array<std::array<double,7>,2> times{};
        for(unsigned round=0;round<7;++round) {
            const auto first=run(round%2!=0,128),second=run(round%2==0,128);
            if(first.second!=second.second)throw std::runtime_error("Skin snapshot benchmark output mismatch");
            times[round%2][round]=first.first;times[1-round%2][round]=second.first;
        }
        for(auto& row:times)std::sort(row.begin(),row.end());
        REXLOG_INFO("Skin snapshot benchmark: {} modifiers x {} bones, {} renders/tick: legacy {:.3f}, cached {:.3f} us/render, {:.3f}x (seven alternating rounds x128 ticks, pinned {}; snapshot collection only, not skinning jobs or gameplay FPS)",modifiers,bones,samples,times[0][3],times[1][3],times[0][3]/times[1][3],scope.affinity!=0);
    }
}
std::mutex skin_mutex;
std::unordered_map<uint32_t,SkinHistory> skin_history;
using GuestCall=void(*)(PPCContext&,uint8_t*);
// Diagnostic only: prove native vertex output changes on render-only frames.
std::atomic<uint64_t> skin_replay_epoch{0},skin_replay_serial{0};
std::atomic<uint32_t> skin_replay_input{0};
struct SkinOutputSample { uint64_t epoch=0,serial=0,hash=0; };
std::mutex skin_output_mutex;
std::unordered_map<uint64_t,SkinOutputSample> skin_output_samples;
std::atomic<uint64_t> skin_replay_workloads{0},skin_render_output_changes{0};

void SampleSkinOutput(uint32_t address,uint32_t blocks,uint32_t stride,
                      uint64_t epoch,uint64_t serial,uint8_t* base) {
    if(!address || !blocks || stride!=12 || !epoch || !serial) return;
    uint64_t hash=14695981039346656037ull;
    const auto bytes=std::min(blocks,8u)*stride;
    for(unsigned i=0;i<bytes;++i) { hash^=PPC_LOAD_U8(address+i); hash*=1099511628211ull; }
    const uint64_t key=(uint64_t(address)<<32)|blocks;
    std::lock_guard lock(skin_output_mutex);
    if(skin_output_samples.size()>=256 && !skin_output_samples.contains(key)) return;
    auto& sample=skin_output_samples[key];
    if(sample.epoch==epoch && sample.serial && sample.serial!=serial && sample.hash!=hash)
        skin_render_output_changes.fetch_add(1,std::memory_order_relaxed);
    sample={epoch,serial,hash};
}

void CaptureSkinPose(uint32_t modifier,uint32_t mesh,uint32_t source,uint32_t sync_args,
                     unsigned count,uint64_t epoch,uint8_t* base) {
    std::lock_guard lock(skin_mutex);
    if(recording_epoch.load(std::memory_order_acquire)!=epoch)return;
    auto [it,added]=skin_history.try_emplace(modifier);
    auto& h=it->second;
    h.snapshot.reset(); // Existing render snapshots stay immutable for their readers.
    if(added)ChangeReference(modifier,1,base);
    const bool reset=added || h.mesh!=mesh || h.source!=source || h.current.size()!=count ||
        epoch<h.updated || epoch-h.updated>1;
    // Multiple updates within one tick must keep the previous completed pose.
    // An offscreen gap or new palette cannot reuse an unrelated old baseline.
    if(!reset && h.updated!=epoch)h.previous=h.current;
    h.mesh=mesh;h.source=source;h.morph=sync_args;h.updated=epoch;
    h.current.resize(count);
    for(unsigned bone=0;bone<count;++bone)
        for(unsigned i=0;i<16;++i)h.current[bone][i]=std::bit_cast<float>(PPC_LOAD_U32(source+bone*64+i*4));
    if(reset)h.previous=h.current;
}

// Observe the actual submitted animation palette in native-delta mode. That
// mode intentionally has no fixed-tick history, so its normal motion trace
// otherwise says nothing about whether animation advances on every frame.
// Never retain guest objects or write a palette from this diagnostic.
void ObserveNativeSkinCadence(uint32_t modifier,uint32_t mesh,uint32_t source,
                              unsigned count,uint32_t frame,uint8_t* base) noexcept {
    struct RestoreMath {unsigned csr=_mm_getcsr();~RestoreMath(){_mm_setcsr(csr);}} restore;
    try {
        struct Sample {
            uint32_t modifier=0,mesh=0,source=0,frame=0;unsigned bones=0;
            uint64_t palette=0,root=0,world=0,frames=0,comparisons=0;
            uint64_t palette_changes=0,root_changes=0,world_changes=0,skipped=0;
            unsigned repeat=0,longest_repeat=0;
            double dt_sum=0,dt_min=.25,dt_max=0;uint64_t invalid_dt=0;
            Clock::time_point started=Clock::now();bool initialized=false;
        };
        // Fixed-size storage bounds both retained metadata and report volume.
        static thread_local std::array<Sample,32> samples;
        auto slot=std::find_if(samples.begin(),samples.end(),[&](const auto& h){return h.modifier==modifier;});
        if(slot==samples.end())slot=std::find_if(samples.begin(),samples.end(),[](const auto& h){return !h.modifier;});
        if(slot==samples.end())return;
        auto& h=*slot;
        if(h.mesh!=mesh || h.source!=source || h.bones!=count) {
            h={};h.modifier=modifier;h.mesh=mesh;h.source=source;h.bones=count;
        }
        // Native skin jobs can be requested by several passes in one frame.
        // Count engine frames, not those repeated requests, as pose samples.
        if(h.initialized && h.frame==frame)return;
        auto hash=[&](uint32_t address,unsigned matrices) {
            uint64_t value=1469598103934665603ull;
            for(unsigned bone=0;bone<matrices;++bone)for(unsigned i:transform_components) {
                value^=PPC_LOAD_U32(address+bone*64+i*4);value*=1099511628211ull;
            }
            return value;
        };
        const auto palette=hash(source,count),root=hash(source,1),world=hash(mesh+144,1);
        if(h.initialized) {
            const uint32_t gap=frame-h.frame;
            // A backward/reset frame counter starts a new observation window.
            if(gap>1000000u) {
                const auto identity=h.modifier;h={};h.modifier=identity;h.mesh=mesh;h.source=source;h.bones=count;
            }else {
                ++h.comparisons;h.skipped+=gap-1;
                h.palette_changes+=palette!=h.palette;h.root_changes+=root!=h.root;h.world_changes+=world!=h.world;
                h.repeat=palette==h.palette?h.repeat+1:0;
                h.longest_repeat=std::max(h.longest_repeat,h.repeat);
            }
        }
        h.initialized=true;h.frame=frame;h.palette=palette;h.root=root;h.world=world;++h.frames;
        const double dt=std::bit_cast<float>(PPC_LOAD_U32(0x833FCA54));
        if(std::isfinite(dt) && dt>=0 && dt<=.25) {
            h.dt_sum+=dt;h.dt_min=std::min(h.dt_min,dt);h.dt_max=std::max(h.dt_max,dt);
        }else ++h.invalid_dt;
        const auto now=Clock::now();const double elapsed=std::chrono::duration<double>(now-h.started).count();
        if(elapsed<5)return;
        REXLOG_INFO("Native pose cadence mesh {:08X} modifier {:08X}: {} bones, {} engine-frame samples / {:.3f}s, {} comparisons, palette/root/world changes {}/{}/{}, longest repeated palette {} frames, skipped engine frames {}, game dt sum {:.6f}, dt {:.6f}..{:.6f}, invalid {} (submitted CPU palettes; no GPU presentation or gameplay-input claim)",
            mesh,modifier,count,h.frames,elapsed,h.comparisons,h.palette_changes,h.root_changes,h.world_changes,
            h.longest_repeat,h.skipped,h.dt_sum,h.dt_min,h.dt_max,h.invalid_dt);
        // Keep the last endpoint/frame for continuity across report windows.
        h.frames=h.comparisons=h.palette_changes=h.root_changes=h.world_changes=h.skipped=h.invalid_dt=0;
        h.repeat=h.longest_repeat=0;h.dt_sum=0;h.dt_min=.25;h.dt_max=0;h.started=now;
    }catch(...){} // Trace failures must never replace a native skin update.
}

void RecordSkinSource(PPCContext& ctx,uint8_t* base) {
    if(!REXCVAR_GET(em2_skin_interpolation) && !REXCVAR_GET(em2_trace_motion)) return;
    const auto epoch=recording_epoch.load(std::memory_order_acquire);
    const bool native_trace=!epoch && REXCVAR_GET(em2_trace_motion) && em2::NativeTimingDiagnosticsEnabled();
    if(!epoch && !native_trace) return;
    const auto modifier=ctx.r3.u32,mesh=ctx.r4.u32;
    const auto count=PPC_LOAD_U32(modifier+84),source=PPC_LOAD_U32(modifier+56);
    if(!source || !count || count>128 || mesh!=PPC_LOAD_U32(modifier+96)) return;
    if(PPC_LOAD_U32(0x833FA68C)) return;
    const auto clock=PPC_LOAD_U32(0x833EFA38);
    if(!clock || (PPC_LOAD_U32(modifier+8436)==PPC_LOAD_U32(clock+80) && !PPC_LOAD_U8(modifier+8440))) return;
    if(native_trace)ObserveNativeSkinCadence(modifier,mesh,source,count,PPC_LOAD_U32(clock+80),base);
    else CaptureSkinPose(modifier,mesh,source,ctx.r5.u32,count,epoch,base);
}

void ReplaySkin(PPCContext& ctx,uint8_t* base,uint32_t modifier,const SkinPose& h,
                uint32_t scratch,float alpha,GuestCall update,GuestCall complete) {
    const auto saved_context=ctx;
    struct RestoreContext {
        PPCContext& context;const PPCContext& saved;unsigned csr=_mm_getcsr();
        ~RestoreContext(){context=saved;_mm_setcsr(csr);}
    } restore_context{ctx,saved_context};
    // Drain the native simulation job before replacing its input stream.
    ctx.r3.u64=modifier; complete(ctx,base);
    std::array<uint8_t,8216> saved_input;
    std::memcpy(saved_input.data(),PPC_RAW_ADDR(modifier+104),saved_input.size());
    // Native update also writes its four cached morph selections/weights at
    // +8444..+8459. Those are simulation bookkeeping, not rendered vertices.
    std::array<uint8_t,16> saved_morph_cache;
    std::memcpy(saved_morph_cache.data(),PPC_RAW_ADDR(modifier+8444),saved_morph_cache.size());
    const auto source=PPC_LOAD_U32(modifier+56),stamp=PPC_LOAD_U32(modifier+8436);
    const auto repeat=PPC_LOAD_U8(modifier+8440);
    const auto prepared=PreparedSkin(h,alpha);
    for(unsigned bone=0;bone<h.current.size();++bone) {
        const auto pose=prepared?InterpolatePrepared(h.previous[bone],h.current[bone],alpha,prepared->bones[bone]):Interpolate(h.previous[bone],h.current[bone],alpha);
        for(unsigned i=0;i<16;++i) PPC_STORE_U32(scratch+bone*64+i*4,std::bit_cast<uint32_t>(pose[i]));
    }
    struct RestoreSource {
        uint8_t* base;uint32_t modifier,source;bool armed=true;
        void Restore(){if(armed){PPC_STORE_U32(modifier+56,source);armed=false;}}
        ~RestoreSource(){Restore();}
    } restore_source{base,modifier,source};
    PPC_STORE_U32(modifier+56,scratch);
    PPC_STORE_U8(modifier+8440,1);
    ctx=saved_context;
    ctx.r3.u64=modifier; ctx.r4.u64=h.mesh; ctx.r5.u64=h.morph;
    const bool trace=REXCVAR_GET(em2_trace_render);
    struct RestoreTrace {
        bool enabled;uint64_t epoch;uint32_t input;
        ~RestoreTrace(){if(enabled){skin_replay_input.store(input,std::memory_order_relaxed);skin_replay_epoch.store(epoch,std::memory_order_release);}}
    } restore_trace{trace,trace?skin_replay_epoch.load(std::memory_order_acquire):0,trace?skin_replay_input.load(std::memory_order_relaxed):0};
    if(trace) {
        skin_replay_input.store(modifier+104,std::memory_order_relaxed);
        skin_replay_serial.fetch_add(1,std::memory_order_relaxed);
        skin_replay_epoch.store(tick_number,std::memory_order_release);
    }
    auto restore_input=[&] {
        std::memcpy(PPC_RAW_ADDR(modifier+104),saved_input.data(),saved_input.size());
        std::memcpy(PPC_RAW_ADDR(modifier+8444),saved_morph_cache.data(),saved_morph_cache.size());
        PPC_STORE_U32(modifier+8436,stamp);PPC_STORE_U8(modifier+8440,repeat);
    };
    bool completion_started=false;
    try {
        update(ctx,base);
        restore_source.Restore();
        ctx.r3.u64=modifier;completion_started=true;complete(ctx,base);
        restore_input();
    }catch(...) {
        if(!completion_started) {
            // Submission can fail after queuing work. Drain once before
            // rewinding its input; a failed drain must retain that live input.
            restore_source.Restore();
            ctx=saved_context;ctx.r3.u64=modifier;
            try{complete(ctx,base);restore_input();}catch(...){}
        }
        // Do not mask the original submission/completion failure. Context,
        // source pointer and trace scopes still unwind if draining failed.
        throw;
    }
}

void BenchmarkPoseInterpolation() {
    if(!REXCVAR_GET(em2_benchmark_pose_interpolation))return;
    using Clock=std::chrono::steady_clock;
    struct Restore {
        unsigned csr=_mm_getcsr();bool enabled=REXCVAR_GET(em2_cache_pose_interpolation);
        DWORD_PTR affinity=SetThreadAffinityMask(GetCurrentThread(),DWORD_PTR(1)<<GetCurrentProcessorNumber());
        ~Restore(){REXCVAR_SET(em2_cache_pose_interpolation,enabled);if(affinity)SetThreadAffinityMask(GetCurrentThread(),affinity);_mm_setcsr(csr);}
    } restore;
    alignas(64) std::array<uint8_t,65536> memory{};auto* base=memory.data();
    constexpr uint32_t modifier=256,scratch=24576;
    auto complete=+[](PPCContext&,uint8_t*){};
    auto update=+[](PPCContext& ctx,uint8_t* base){
        const auto modifier=ctx.r3.u32,source=PPC_LOAD_U32(modifier+56),count=PPC_LOAD_U32(modifier+84);
        std::memcpy(PPC_RAW_ADDR(modifier+108),PPC_RAW_ADDR(source),count*64);
    };
    for(unsigned count:{32u,128u})for(bool spherical:{false,true})for(unsigned samples:{1u,2u,4u,8u}){
        SkinHistory history;
        history.mesh=40000;history.source=16384;history.morph=41000;
        for(unsigned bone=0;bone<count;++bone){
            Matrix a{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},b=a;
            const float angle=float(bone)*.043f,next=angle+(spherical?.31f:.01f);
            a[0]=a[5]=std::cos(angle);a[1]=-std::sin(angle);a[4]=std::sin(angle);
            b[0]=b[5]=std::cos(next);b[1]=-std::sin(next);b[4]=std::sin(next);
            history.previous.push_back(a);history.current.push_back(b);
        }
        PPC_STORE_U32(modifier+56,history.source);PPC_STORE_U32(modifier+84,count);
        auto run=[&](bool cached,unsigned ticks){
            REXCVAR_SET(em2_cache_pose_interpolation,cached);
            PPCContext ctx{};ctx.fpscr.InitHost();uint64_t checksum=0;
            const auto started=Clock::now();
            for(unsigned tick=0;tick<ticks;++tick){
                history.snapshot.reset();history.current[0][12]=float(tick);
                const auto snapshot=SnapshotSkin(history);
                for(unsigned sample=0;sample<samples;++sample){
                    ReplaySkin(ctx,base,modifier,*snapshot,scratch,(sample+.5f)/samples,update,complete);
                    checksum+=PPC_LOAD_U32(scratch+48);
                }
            }
            return std::pair(std::chrono::duration<double,std::micro>(Clock::now()-started).count()/(ticks*samples),checksum);
        };
        const auto deadline=Clock::now()+std::chrono::milliseconds(15);
        do{run(false,8);run(true,8);}while(Clock::now()<deadline);
        std::array<std::array<double,7>,2> times{};
        for(unsigned round=0;round<7;++round){
            const auto first=run(round&1,128),second=run(!(round&1),128);
            if(first.second!=second.second)throw std::runtime_error("Prepared skin benchmark output mismatch");
            times[round&1][round]=first.first;times[!(round&1)][round]=second.first;
        }
        for(auto& mode:times)std::sort(mode.begin(),mode.end());
        REXLOG_INFO("Prepared skin benchmark: {} bones, spherical {}, {} renders/tick: reference {:.3f}, cached {:.3f} us/render, {:.3f}x (seven alternating rounds x128 ticks, snapshot/preparation/replay included; copy-job stubs, not native skinning or gameplay FPS)",count,spherical,samples,times[0][3],times[1][3],times[0][3]/times[1][3]);
    }
}

void ApplySkinPoses(PPCContext& ctx,uint8_t* base) {
    if(!REXCVAR_GET(em2_skin_interpolation)) return;
    // World updates and scene submission run on the main thread. Copy the
    // histories before native calls, which can wait on engine worker threads.
    const bool cached=REXCVAR_GET(em2_cache_skin_snapshots);
    thread_local std::vector<std::pair<uint32_t,std::shared_ptr<const SkinPose>>> active;
    active.clear(); // Retain the entry capacity, never a stale frame's contents.
    struct ReleaseSnapshots {decltype(active)& entries;~ReleaseSnapshots(){entries.clear();}} release{active};
    std::vector<std::pair<uint32_t,SkinPose>> legacy;
    {
        std::lock_guard lock(skin_mutex);
        for(auto& [modifier,h]:skin_history)
            if(h.updated==tick_number && h.previous!=h.current &&
               PPC_LOAD_U32(modifier+96)==h.mesh && PPC_LOAD_U32(modifier+56)==h.source) {
                if(cached)active.emplace_back(modifier,SnapshotSkin(h));
                else legacy.emplace_back(modifier,static_cast<const SkinPose&>(h));
            }
    }
    if(active.empty() && legacy.empty()) return;
    static uint32_t scratch=0;
    if(!scratch) scratch=rex::kernel::kernel_memory()->SystemHeapAlloc(128*64,128);
    if(!scratch) return;
    const auto started=Clock::now();
    for(const auto& [modifier,h]:active)
        ReplaySkin(ctx,base,modifier,*h,scratch,RenderAlpha(),__imp__sub_82909D00,__imp__sub_8290A238);
    for(const auto& [modifier,h]:legacy)
        ReplaySkin(ctx,base,modifier,h,scratch,RenderAlpha(),__imp__sub_82909D00,__imp__sub_8290A238);
    if(REXCVAR_GET(em2_trace_render)) {
        static auto last=Clock::now()-std::chrono::seconds(5);
        static uint64_t calls=0,total_us=0,max_us=0;
        const auto now=Clock::now();
        const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(now-started).count();
        ++calls; total_us+=elapsed; max_us=std::max(max_us,uint64_t(elapsed));
        if(now-last>=std::chrono::seconds(5)) {
            last=now;
            REXLOG_INFO("Replayed {} native skin modifiers at alpha {}; CPU mean/max {} / {} us, workloads {}, render-only output changes {}",
                active.size()+legacy.size(),RenderAlpha(),total_us/calls,max_us,
                skin_replay_workloads.exchange(0),skin_render_output_changes.exchange(0));
            calls=total_us=max_us=0;
        }
    }
}

std::atomic<uint64_t> camera_look_input{0},camera_button_input{0};

bool ReadEntityPosition(uint32_t entity,uint8_t slot,uint8_t* base,
                        uint32_t& component,std::array<float,3>& position) {
    if(!entity || slot==255) return false;
    const auto components=PPC_LOAD_U32(entity+36);
    if(!components) return false;
    const auto entry=PPC_LOAD_U32(components+unsigned(slot)*4);
    if(entry<8) return false;
    component=entry-8;
    for(unsigned i=0;i<3;++i) {
        position[i]=std::bit_cast<float>(PPC_LOAD_U32(component+172+i*4));
        if(!std::isfinite(position[i])) return false;
    }
    return true;
}

void ProbePlayerPosition(PPCContext& ctx,uint8_t* base) {
    if(!REXCVAR_GET(em2_trace_motion)) return;
    const auto manager=PPC_LOAD_U32(0x833DAB60),registry=PPC_LOAD_U32(0x833DA7E4);
    if(!manager || !registry) return;
    // PlayerManager stores entity handles at +20 in its 320-byte records.
    // Resolve through the game's registry; do not retain a raw entity pointer
    // across ticks or treat the animated NiMesh root as a physical position.
    const auto saved=ctx;
    const auto saved_mxcsr=_mm_getcsr();
    for(unsigned player=0;player<2;++player) {
        const auto handle_address=manager+player*320+20;
        const auto handle=PPC_LOAD_U32(handle_address);
        if(!handle) continue;
        ctx=saved; ctx.r3.u64=registry; ctx.r4.u64=handle_address;
        __imp__sub_82732A18(ctx,base);
        const auto entity=ctx.r3.u32;
        if(!entity) continue;
        const auto types=PPC_LOAD_U32(entity+88);
        if(!types) continue;
        const auto slot=PPC_LOAD_U8(types+PPC_LOAD_U8(0x833DBC3C));
        uint32_t component=0;
        std::array<float,3> position{};
        if(!ReadEntityPosition(entity,slot,base,component,position)) continue;
        const auto input=camera_button_input.load(std::memory_order_acquire);
        const auto buttons=uint32_t(input>>32)==uint32_t(tick_number)?uint16_t(input):uint16_t(0);
        REXLOG_INFO("Player entity motion P{} {:08X} component {:08X} handle {:08X}: epoch {}, world ({:.6f}, {:.6f}, {:.6f}), P1 buttons {:04X}",
            player,entity,component,handle,tick_number,position[0],position[1],position[2],buttons);
    }
    ctx=saved;
    _mm_setcsr(saved_mxcsr);
}

struct CameraMotion {
    uint64_t seen=0;
    unsigned ticks=0,input_ticks=0,dpad_ticks=0,invalid=0;
    double rotation=0,distance=0;
};

void CompleteNetworkObservation(PPCContext& ctx,uint8_t* base) {
    auto& gate=em2::net::GameplayInputGate();
    auto* room=em2::net::CurrentRoomBootstrapGate();
    auto* gameplay=em2::net::CurrentNativeRoomGameplay();
    const bool gameplay_report=gameplay && gameplay->active() && gameplay->completion_pending();
    const bool room_report=room && room->active() && room->completion_pending();
    if(gameplay_report && room_report){gameplay->ObservationFailed();room->ObservationFailed();return;}
    if(room && room->active() && gate.active()){
        room->ObservationFailed();gate.ObservationFailed();return;
    }
    const bool legacy_report=gate.active() && gate.completion_pending();
    const bool report=room_report || gameplay_report || legacy_report;
    const bool trace=REXCVAR_GET(em2_trace_online_observations) && tick_number%30==0;
    if(!report && !trace) return;
    const auto saved=ctx;const auto mxcsr=_mm_getcsr();
    struct Restore {
        PPCContext& ctx;std::array<uint8_t,sizeof(PPCContext)> saved;unsigned mxcsr;DWORD error=GetLastError();
        Restore(PPCContext& value,unsigned csr):ctx(value),mxcsr(csr){std::memcpy(saved.data(),&ctx,sizeof(ctx));}
        ~Restore(){std::memcpy(&ctx,saved.data(),sizeof(ctx));_mm_setcsr(mxcsr);SetLastError(error);}
    } restore{ctx,mxcsr};
    em2::net::GameplayObservation state;
    auto& observations=state.players;
    // Probe the real admitted actor/producer contract before freezing the
    // observation and CRT counters for this same completed native update.
    const bool room_ready=room_report?room->CheckGameplayReadiness(ctx,base):false;
    ctx=saved;_mm_setcsr(mxcsr);
    bool valid=em2::ReadNativeGameplayObservation(ctx,base,state);
    if(valid && gameplay_report){
        valid=em2::ReadNativeCharacterPhysics(base,state.characters) && state.characters.size()>=2;
        state.includes_character_physics=valid;
    }
    state.random=random_observation;
    const auto observed_tick=gameplay_report?gameplay->committed_tick():(room_report?room->committed_tick():(legacy_report?gate.committed_tick():tick_number));
    if(report && valid && state.players[0].mode==0 && state.players[1].mode==0)em2::TraceNativeEntitySnapshot(base,observed_tick);
    const auto digest=valid?em2::net::HashGameplayObservation(observed_tick,state):std::nullopt;
    if(report && valid && random_failure_history && random_failure_history->current && random_failure_history->current->tick==observed_tick){
        auto& frame=*random_failure_history->current;frame.character_coverage=state.includes_character_physics;
        if(frame.character_coverage)frame.characters=state.characters;
    }
    ctx=saved;_mm_setcsr(mxcsr);
    if((trace || (report && REXCVAR_GET(em2_trace_online_observations))) && digest) {
        std::string hex;for(auto byte:*digest)hex+=fmt::format("{:02x}",byte);
        const auto& a=observations[0];const auto& b=observations[1];
        if(report && REXCVAR_GET(em2_trace_online_random_owners))REXLOG_INFO("Native online scene clock: tick {}, seconds {:08X}, accumulated {:016X}, counter {}, previous {}",observed_tick,PPC_LOAD_U32(0x833FCA4C),PPC_LOAD_U64(0x833FCA38),PPC_LOAD_U64(0x833FCA60),PPC_LOAD_U64(0x833FCA68));
        REXLOG_INFO("Native gameplay observation v{}: level '{}' state '{}' interior '{}' epoch {}, tick {}, modes {}/{}, bits {:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}, life {}/{}/{}/{}, ammo {:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}, CRT calls/seeds {}/{}, last state {:08X}, stream {:016X}, digest {}, agreed {}, character proxies {}",
            state.includes_character_physics?5:4,state.world.level.data(),state.world.state.data(),state.world.interior.data(),tick_number,observed_tick,a.mode,b.mode,std::bit_cast<uint32_t>(a.position[0]),std::bit_cast<uint32_t>(a.position[1]),std::bit_cast<uint32_t>(a.position[2]),
            std::bit_cast<uint32_t>(b.position[0]),std::bit_cast<uint32_t>(b.position[1]),std::bit_cast<uint32_t>(b.position[2]),
            state.life[0],state.life[1],state.life[2],state.life[3],std::bit_cast<uint32_t>(state.ammo[0]),std::bit_cast<uint32_t>(state.ammo[1]),
            std::bit_cast<uint32_t>(state.ammo[2]),std::bit_cast<uint32_t>(state.ammo[3]),std::bit_cast<uint32_t>(state.ammo[4]),std::bit_cast<uint32_t>(state.ammo[5]),
            state.random.calls,state.random.seeds,state.random.last_state,state.random.sequence,hex,report,state.characters.size());
    }
    if(report && capture_online_random){
        for(size_t i=0;i<online_random_trace_size;++i){const auto& event=online_random_trace[i];
            REXLOG_INFO("Native online CRT event: tick {}, index {}, seed {}, caller {:08X}, state {:08X}, result {:08X}",observed_tick,i,event.reseed,event.caller,event.state,event.result);
        }
        for(size_t i=0;i<online_random_owner_size;++i){const auto& owner=online_random_owners[i];
            REXLOG_INFO("Native online CRT owner: tick {}, index {}, r29 {:08X} head {:08X}, r30 {:08X} head {:08X}, r31 {:08X} head {:08X}",observed_tick,owner.event,owner.registers[0],owner.heads[0],owner.registers[1],owner.heads[1],owner.registers[2],owner.heads[2]);
            REXLOG_INFO("Native online CRT stack: tick {}, index {}, returns {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",observed_tick,owner.event,owner.returns[0],owner.returns[1],owner.returns[2],owner.returns[3],owner.returns[4],owner.returns[5]);
            if(owner.scheduled)REXLOG_INFO("Native online CRT schedule: tick {}, index {}, scene {:08X}, last {:08X}, interval {:08X}, budget {:08X}, gate {:08X}",observed_tick,owner.event,owner.schedule[0],owner.schedule[1],owner.schedule[2],owner.schedule[3],owner.schedule[4]);
            if(owner.scheduled)REXLOG_INFO("Native online CRT target: tick {}, index {}, key {:08X}, entity {:08X}, handle {:08X}",observed_tick,owner.event,owner.heads[1],owner.identity[0],owner.identity[1]);
        }
        if(online_random_trace_size==online_random_trace.size())REXLOG_INFO("Native online CRT trace reached its {} event limit",online_random_trace.size());
        if(random_failure_history && random_failure_history->current && random_failure_history->current->tick==observed_tick){
            LogScriptRandomOwners(*random_failure_history->current);
            random_failure_history->current->published=true;
        }
    }
    if(report) {
        if(observed_tick==12 && script_wait_trace_size)LogScriptWaitTrace();
        if(gameplay_report){
            if(!digest || !gameplay->Complete(*digest,ctx,base))gameplay->ObservationFailed();
        }else if(room_report){
            if(!digest)room->ObservationFailed();
            else if(room->Complete(room_ready,*digest) && !room->SetJoining(!em2::NativeCoopReady(base)))room->ObservationFailed();
        }else if(digest) gate.Complete(*digest);else gate.ObservationFailed();
    }
}
thread_local std::unordered_map<uint32_t,CameraMotion> camera_motion;

double CameraRotationDegrees(const Matrix& a,const Matrix& b) {
    if(!RigidRotation(a) || !RigidRotation(b)) return NAN;
    const auto qa=Rotation(a),qb=Rotation(b);
    const double dot=double(qa.w)*qb.w+double(qa.x)*qb.x+double(qa.y)*qb.y+double(qa.z)*qb.z;
    const double an=double(qa.w)*qa.w+double(qa.x)*qa.x+double(qa.y)*qa.y+double(qa.z)*qa.z;
    const double bn=double(qb.w)*qb.w+double(qb.x)*qb.x+double(qb.y)*qb.y+double(qb.z)*qb.z;
    return 2*std::acos(std::clamp(std::abs(dot)/std::sqrt(an*bn),0.,1.))*180/3.141592653589793;
}
void ProbeCameraMotion(uint32_t camera,const TransformHistory& h) {
    if(!REXCVAR_GET(em2_trace_camera) || h.updated!=tick_number) return;
    auto& probe=camera_motion[camera];
    if(probe.seen+1!=tick_number) probe={};
    probe.seen=tick_number; ++probe.ticks;
    const auto angle=CameraRotationDegrees(h.previous,h.current);
    if(std::isfinite(angle)) probe.rotation+=angle; else ++probe.invalid;
    double distance=0;
    for(unsigned i=12;i<15;++i) { const double delta=double(h.current[i])-h.previous[i]; distance+=delta*delta; }
    if(std::isfinite(distance)) probe.distance+=std::sqrt(distance); else ++probe.invalid;
    const auto input=camera_look_input.load(std::memory_order_acquire);
    if(uint32_t(input>>32)==uint32_t(tick_number) && uint32_t(input)) ++probe.input_ticks;
    const auto buttons=camera_button_input.load(std::memory_order_acquire);
    if(uint32_t(buttons>>32)==uint32_t(tick_number) && (uint16_t(buttons)&15)) ++probe.dpad_ticks;
    if(probe.ticks>=30) {
        if(probe.input_ticks || probe.dpad_ticks || probe.rotation>.01 || probe.distance>.1 || probe.invalid)
            REXLOG_INFO("Camera motion {:08X}: {} ticks, rotation {:.4f} deg, travel {:.4f}, P1 look ticks {}, D-pad ticks {}, invalid poses {}, epoch {}",
                camera,probe.ticks,probe.rotation,probe.distance,probe.input_ticks,probe.dpad_ticks,probe.invalid,tick_number);
        probe={}; probe.seen=tick_number;
    }
}

void PrepareUiRender(uint32_t container,uint8_t* base) {
    // NiGFxMovie::Advance sets +18, and Display consumes that flag. On a
    // render-only frame retain the last movie state and allow it to draw again.
    // Both normal and texture-backed movies live in these two native arrays.
    for(unsigned offset:{348u,360u}) {
        const auto array=PPC_LOAD_U32(container+offset);
        const auto count=PPC_LOAD_U32(container+offset+4);
        for(uint32_t i=0;i<count;++i) {
            const auto movie=PPC_LOAD_U32(array+i*4);
            if(movie && PPC_LOAD_U8(movie+16)) PPC_STORE_U8(movie+18,1);
        }
    }
}

void MeasureMaximum(std::atomic<uint64_t>& maximum,Clock::time_point started) {
    const auto elapsed=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-started).count());
    auto value=maximum.load(std::memory_order_relaxed);
    while(elapsed>value && !maximum.compare_exchange_weak(value,elapsed,std::memory_order_relaxed)) {}
}

void RecordTransform(uint32_t object,uint8_t* base,bool camera=false) {
    const auto epoch=recording_epoch.load(std::memory_order_acquire);
    if(!epoch || !REXCVAR_GET(em2_interpolation)) return;
    const auto matrix=ReadTransform(object,base);
    const auto parent=PPC_LOAD_U32(object+56);
    std::lock_guard lock(history_mutex);
    if(recording_epoch.load(std::memory_order_acquire)!=epoch) return;
    auto found=history.find(object);
    if(found==history.end()) {
        ChangeReference(object,1,base);
        history.emplace(object,TransformHistory{matrix,matrix,{},epoch,parent,camera});
        return;
    } else {
        auto& h=found->second;
        if(h.parent!=parent) h.previous=matrix;
        h.current=matrix; h.parent=parent; h.updated=epoch; h.camera|=camera;h.prepared_ready=false;
    }
    return;
}

void StartTick(uint8_t* base,bool agreed=false,uint64_t input_tick=0) {
    agreed_clock_tick=agreed;clock_input_tick=input_tick;
    if(agreed)em2::net::BeginNativeBehaviorTraceTick(input_tick);
    random_observation={};
    online_random_trace_size=0;online_random_owner_size=0;
    const uint64_t random_start=REXCVAR_GET(em2_trace_online_random_start_tick);
    capture_online_random=agreed && input_tick>=random_start && input_tick-random_start<2 && REXCVAR_GET(em2_trace_online_observations);
    capture_online_random_owners=capture_online_random && REXCVAR_GET(em2_trace_online_random_owners);
    capture_script_random_owners=agreed && REXCVAR_GET(em2_trace_online_observations) &&
        (input_tick<16 || REXCVAR_GET(em2_trace_online_random_owners) || REXCVAR_GET(em2_trace_online_lua_owners));
    if(agreed && REXCVAR_GET(em2_trace_online_observations)){
        if(!random_failure_history)random_failure_history=std::make_unique<RandomFailureHistory>();
        random_failure_history->Begin(input_tick);
    }else if(random_failure_history)random_failure_history->current=nullptr;
    record_random=em2::net::GameplayInputGate().active() || em2::net::RoomBootstrapActive() || REXCVAR_GET(em2_trace_online_observations);
    ui_containers.clear();
    ++tick_number;
    if(REXCVAR_GET(em2_trace_render)) { std::lock_guard lock(skin_output_mutex); skin_output_samples.clear(); }
    // Read the game's initialized QPC frequency, rather than assuming host TSC Hz.
    const auto frequency=PPC_LOAD_U64(0x833D04D8);
    if(frequency) tick_frequency=frequency;
    std::lock_guard lock(history_mutex);
    for(auto& [object,h]:history){h.previous=h.current;BeginSceneInterpolationTick(h);}
    recording_epoch.store(tick_number,std::memory_order_release);
    in_tick=true;
}

// A failed native input transaction must not leave a phantom agreed update
// visible to later render/UI calls. This NEVER runs post-update or completion.
struct NativeTickAdmissionScope {
    em2::net::NativeRoomGameplay* gameplay=nullptr;
    em2::net::RoomBootstrapGate* bootstrap=nullptr;
    bool legacy=false,completed=false;
    ~NativeTickAdmissionScope()noexcept{
        if(completed)return;
        const auto error=GetLastError();const auto csr=_mm_getcsr();
        recording_epoch.store(0,std::memory_order_release);
        in_tick=false;agreed_clock_tick=false;record_random=false;
        capture_online_random=capture_online_random_owners=capture_script_random_owners=false;
        try{
            if(gameplay)gameplay->ObservationFailed();
            if(bootstrap)bootstrap->ObservationFailed();
            if(legacy)em2::net::GameplayInputGate().ObservationFailed();
        }catch(...){/* Keep the original native failure authoritative. */}
        _mm_setcsr(csr);SetLastError(error);
    }
};

void ResetSimulationHistory(PPCContext& ctx,uint8_t* base) {
    em2::ResetCameraPresentation();
    em2::ResetGameplayViewCameras();
    recording_epoch.store(0,std::memory_order_release);
    em2::interpolated_nodes.store(0,std::memory_order_relaxed);
    in_tick=false;
    agreed_clock_tick=false;
    std::vector<uint32_t> retained;
    {
        std::lock_guard lock(history_mutex);
        for(const auto& [object,h]:history) retained.push_back(object);
        history.clear();
        moving_objects.clear();
    }
    {
        std::lock_guard lock(skin_mutex);
        for(const auto& [modifier,h]:skin_history) retained.push_back(modifier);
        skin_history.clear();
    }
    ui_containers.clear();
    camera_motion.clear();
    counter_remainder=0;
    const auto saved=ctx;
    // Release outside the locks: guest deletion can call our detach hooks.
    for(auto object:retained) if(ChangeReference(object,-1,base)==0) {
        ctx.r3.u64=object;
        sub_825517C8(ctx,base);
    }
    ctx=saved;
}

void FinishTick(PPCContext& ctx,uint8_t* base) {
    CompleteNetworkObservation(ctx,base);
    recording_epoch.store(0,std::memory_order_release);
    in_tick=false;
    agreed_clock_tick=false;
    ProbePlayerPosition(ctx,base);
    std::vector<uint32_t> expired;
    {
        std::lock_guard lock(history_mutex);
        moving_objects.clear();
        for(auto it=history.begin();it!=history.end();) {
            auto& [object,h]=*it;
            if(tick_number-h.updated>3) { expired.push_back(object); it=history.erase(it); }
            else {
                h.current=ReadTransform(object,base);h.prepared_ready=false;
                if(h.camera) ProbeCameraMotion(object,h);
                h.moving=std::any_of(transform_components.begin(),transform_components.end(),[&](unsigned i) {
                    return std::bit_cast<uint32_t>(h.previous[i])!=std::bit_cast<uint32_t>(h.current[i]);
                });
                if(h.moving) moving_objects.push_back({object,&h});
                ++it;
            }
        }
    }
    {
        std::lock_guard lock(skin_mutex);
        for(auto it=skin_history.begin();it!=skin_history.end();) {
            if(tick_number-it->second.updated>3) { expired.push_back(it->first); it=skin_history.erase(it); }
            else {
                const auto& h=it->second;
                if(REXCVAR_GET(em2_trace_motion) && h.updated==tick_number && !h.current.empty() &&
                   PPC_LOAD_U32(it->first+96)==h.mesh) {
                    const auto world=ReadTransform(h.mesh,base);
                    const auto input=camera_button_input.load(std::memory_order_acquire);
                    const auto buttons=uint32_t(input>>32)==uint32_t(tick_number)?uint16_t(input):uint16_t(0);
                    REXLOG_INFO("Skinned mesh motion {:08X}: epoch {}, world ({:.6f}, {:.6f}, {:.6f}), root bone ({:.6f}, {:.6f}, {:.6f}), P1 buttons {:04X}",
                        h.mesh,tick_number,world[12],world[13],world[14],
                        h.current[0][12],h.current[0][13],h.current[0][14],buttons);
                    // The visible mesh may include animation offsets. Observe
                    // its scene ancestry independently before calling a mesh
                    // displacement a change in the character's physics.
                    std::array<uint32_t,7> ancestry{h.mesh};
                    auto node=PPC_LOAD_U32(h.mesh+56);
                    for(unsigned depth=1;node && depth<ancestry.size();++depth) {
                        if(std::find(ancestry.begin(),ancestry.begin()+depth,node)!=ancestry.begin()+depth) break;
                        ancestry[depth]=node;
                        const auto parent_world=ReadTransform(node,base);
                        REXLOG_INFO("Skinned mesh ancestor {:08X} depth {} node {:08X}: epoch {}, world ({:.6f}, {:.6f}, {:.6f}), P1 buttons {:04X}",
                            h.mesh,depth,node,tick_number,parent_world[12],parent_world[13],parent_world[14],buttons);
                        node=PPC_LOAD_U32(node+56);
                    }
                }
                ++it;
            }
        }
    }
    if(REXCVAR_GET(em2_trace_camera)) {
        for(auto it=camera_motion.begin();it!=camera_motion.end();)
            if(tick_number-it->second.seen>90) it=camera_motion.erase(it); else ++it;
    }
    // Engine deletion can recurse into NiAVObject; never hold our lock across it.
    const auto saved=ctx;
    for(auto object:expired) if(ChangeReference(object,-1,base)==0) {
        ctx.r3.u64=object;
        sub_825517C8(ctx,base);
    }
    ctx=saved;
    em2::simulation_steps.fetch_add(1,std::memory_order_relaxed);
    em2::CaptureCameraPresentationTarget(ctx,base,tick_number);
}

void ApplyTransforms(PPCContext& ctx,uint8_t* base,bool restore) {
    thread_local std::vector<uint32_t> cameras;
    cameras.clear();
    uint64_t applied=0;
    const bool cached=REXCVAR_GET(em2_cache_scene_render_entries);
    // Admission/wait state belongs to this thread and cannot change in the
    // locked node loop. Snapshot alpha once, preserving the reference formula.
    float alpha=0;
    {
        std::lock_guard lock(history_mutex);
        if(cached && !restore && !moving_objects.empty())alpha=float(RenderAlpha());
        for(const auto& entry:moving_objects) {
            const auto object=entry.object;
            auto* state=entry.state;
            if(!cached){const auto found=history.find(object);if(found==history.end())continue;state=&found->second;}
            auto& h=*state;
            if(restore) {
                if(!h.applied) continue;
                WriteTransform(object,h.saved,base); h.applied=false;
            } else {
                if(!h.moving) continue;
                // Shadow/reflection views have native projector constants and
                // resource updates on the simulation clock. Blending only
                // their NiCamera pose makes the depth map disagree with its
                // receiver matrix. Interpolate owned display cameras only.
                if(h.camera && !em2::IsGameplayViewCameraNode(object,base))continue;
                if(em2::PresentationOwnsCameraNode(object))continue;
                h.saved=ReadTransform(object,base);
                WriteTransform(object,InterpolateScene(h,cached?alpha:float(RenderAlpha())),base);
                h.applied=true;
            }
            ++applied;
            if(h.camera) cameras.push_back(object);
        }
        if(!restore) em2::interpolated_nodes.store(applied,std::memory_order_relaxed);
    }
    struct RestoreContext {PPCContext& ctx;PPCContext saved;~RestoreContext(){ctx=saved;}} context{ctx,ctx};
    for(auto object:cameras) { ctx.r3.u64=object; sub_82568720(ctx,base); }
}

thread_local unsigned render_transform_depth=0;
template<class Render>void WithRenderTransforms(PPCContext& ctx,uint8_t* base,Render&& render) {
    struct Depth {bool outer=render_transform_depth++==0;~Depth(){--render_transform_depth;}} depth;
    // A nested render must use the existing interpolated world. Saving it as
    // another authoritative pose would lose the outer simulation snapshot.
    if(!depth.outer){render();return;}
    try {
        ApplyTransforms(ctx,base,false);
        render();
    }catch(...) {
        // Preserve the original failure. Core world transforms are restored
        // before rebuilding camera projections, which can itself fail.
        try{ApplyTransforms(ctx,base,true);}catch(...){}
        throw;
    }
    ApplyTransforms(ctx,base,true);
}
void BenchmarkSceneRenderEntries() {
    if(!REXCVAR_GET(em2_benchmark_scene_render_entries))return;
    if(!history.empty() || !moving_objects.empty())throw std::runtime_error("Scene render benchmark requires empty history");
    auto* bytes=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x1000000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!bytes)throw std::runtime_error("Scene render benchmark allocation failed");
    auto* base=bytes;
    struct Restore {
        uint8_t* bytes;unsigned csr=_mm_getcsr();bool cached=REXCVAR_GET(em2_cache_scene_render_entries);
        em2::SimulationScheduler prior=scheduler;uint64_t nodes=em2::interpolated_nodes.load();
        DWORD_PTR affinity=SetThreadAffinityMask(GetCurrentThread(),DWORD_PTR(1)<<GetCurrentProcessorNumber());
        ~Restore(){moving_objects.clear();history.clear();scheduler=prior;em2::interpolated_nodes.store(nodes);
            REXCVAR_SET(em2_cache_scene_render_entries,cached);if(affinity)SetThreadAffinityMask(GetCurrentThread(),affinity);_mm_setcsr(csr);VirtualFree(bytes,0,MEM_RELEASE);}
    } restore{bytes};
    scheduler.Reset();scheduler.Advance(0);scheduler.Advance(16666667);
    for(unsigned count:{64u,512u,4096u})for(bool rotate:{false,true}) {
        moving_objects.clear();history.clear();history.reserve(count);
        for(unsigned i=0;i<count;++i){
            const uint32_t object=4096+i*256;Matrix previous={1,0,0,0,0,1,0,0,0,0,1,0,float(i),0,0,1},current=previous;
            current[12]+=1;current[13]=float(i%8)*.01f;
            if(rotate){const float angle=.15f;current[0]=current[5]=std::cos(angle);current[1]=-std::sin(angle);current[4]=std::sin(angle);}
            auto [entry,inserted]=history.emplace(object,TransformHistory{previous,current,{},1,0,false,false,true});
            moving_objects.push_back({object,&entry->second});WriteTransform(object,current,bytes);
        }
        auto run=[&](bool cached,unsigned frames){
            REXCVAR_SET(em2_cache_scene_render_entries,cached);_mm_setcsr(0x1F80);
            for(auto& [object,h]:history){h.prepared_ready=false;h.prepare_streak=2;h.scene_samples=0;}
            PPCContext ctx{};uint64_t checksum=0;const auto start=Clock::now();
            for(unsigned frame=0;frame<frames;++frame){ApplyTransforms(ctx,bytes,false);
                checksum+=PPC_LOAD_U32(4096+(frame%count)*256+144+48);
                ApplyTransforms(ctx,bytes,true);}
            const double us=std::chrono::duration<double,std::micro>(Clock::now()-start).count()/frames;
            for(const auto& [object,h]:history){const auto actual=ReadTransform(object,bytes);if(std::memcmp(actual.data(),h.current.data(),64))throw std::runtime_error("Scene render benchmark did not restore simulation pose");}
            return std::pair(us,checksum);
        };
        const auto until=Clock::now()+std::chrono::milliseconds(20);do{run(false,8);run(true,8);}while(Clock::now()<until);
        const unsigned frames=count<512?128:count<4096?64:16;std::array<std::array<double,7>,2> times{};
        for(unsigned round=0;round<7;++round){const auto first=run(round&1,frames),second=run(!(round&1),frames);
            if(first.second!=second.second)throw std::runtime_error("Scene render benchmark intermediate pose mismatch");
            times[round&1][round]=first.first;times[!(round&1)][round]=second.first;}
        for(auto& row:times)std::sort(row.begin(),row.end());
        REXLOG_INFO("Scene render entry benchmark: {} nodes, rotation {}, reference {:.3f}, cached {:.3f} us/apply+restore, {:.3f}x (seven alternating rounds, pinned {}; synthetic scene CPU work, not gameplay FPS)",count,rotate,times[0][3],times[1][3],times[0][3]/times[1][3],restore.affinity!=0);
    }
}

}

namespace em2 {
void ResetPlayerMotionHistory(PPCContext& ctx,uint8_t* base) {
    ResetSimulationHistory(ctx,base);
    scheduler.Reset();
}
void RecordCameraInput(unsigned user,int16_t x,int16_t y,uint16_t buttons) {
    if(user || (!REXCVAR_GET(em2_trace_camera) && !REXCVAR_GET(em2_trace_motion))) return;
    const auto epoch=recording_epoch.load(std::memory_order_acquire);
    if(!epoch) return;
    const uint32_t axes=uint16_t(x)|(uint32_t(uint16_t(y))<<16);
    const auto prior=camera_look_input.exchange((uint64_t(uint32_t(epoch))<<32)|axes,std::memory_order_acq_rel);
    const auto prior_buttons=camera_button_input.exchange((uint64_t(uint32_t(epoch))<<32)|buttons,std::memory_order_acq_rel);
    if(REXCVAR_GET(em2_trace_camera) && (uint32_t(prior)!=axes || uint16_t(prior_buttons)!=buttons))
        REXLOG_INFO("Camera look input P1: epoch {}, x {}, y {}, buttons {:04X}",epoch,x,y,buttons);
}
}

bool em2::InAgreedSimulationTick(){return in_tick && agreed_clock_tick;}
bool em2::CurrentAgreedSimulationTick(uint64_t& tick){
    if(!InAgreedSimulationTick())return false;
    tick=clock_input_tick;return true;
}

PPC_FUNC_IMPL(sub_8273D880) {
    // The coroutine scheduler subtracts one of the native clock deltas from
    // each pending wait before it resumes Lua. Capture the inputs before the
    // original scheduler mutates or removes entries.
    const auto trace_begin=script_wait_trace_size;
    uint32_t trace_manager=0;
    if((REXCVAR_GET(em2_trace_online_random_owners) || REXCVAR_GET(em2_trace_online_lua_owners) ||
        (REXCVAR_GET(em2_trace_online_observations) && em2::InAgreedSimulationTick() && clock_input_tick<12)) && em2::net::SynchronizedSessionActive() &&
       (!em2::InAgreedSimulationTick() || clock_input_tick<12)){
        struct HostState {DWORD error=GetLastError();unsigned csr=_mm_getcsr();~HostState(){SetLastError(error);_mm_setcsr(csr);}} host_state;
        const auto manager=ctx.r3.u32;
        trace_manager=manager;
        const auto list=RandomTraceWord(base,manager+72),count=RandomTraceWord(base,manager+76);
        const auto state=RandomTraceWord(base,manager+92);
        if(count && list && uint64_t(list)+uint64_t(count)*132<=0x100000000ull){
            const auto mode=RandomTraceWord(base,state+24);
            const auto scaled=RandomTraceWord(base,mode?0x833FCA70:0x833FCA54);
            const auto unscaled=RandomTraceWord(base,0x833FCA74);
            for(uint32_t i=0;i<std::min(count,24u) && script_wait_trace_size<script_wait_trace.size();++i){
                const auto entry=list+i*132;
                const auto wait=RandomTraceWord(base,entry+96),flags=RandomTraceWord(base,entry+100);
                const auto target=RandomTraceWord(base,entry+112);
                if(!(flags&0x00FF0000))continue;
                script_wait_trace[script_wait_trace_size++]={
                    em2::InAgreedSimulationTick(),clock_input_tick,i,count,target,wait,flags,scaled,unscaled};
            }
        }
    }
    const auto trace_end=script_wait_trace_size;
    __imp__sub_8273D880(ctx,base);
    if(trace_manager && trace_end>trace_begin){
        // A target's pause flags can suppress the subtraction even with equal
        // clock inputs. Observe the original scheduler's output without
        // resolving targets or invoking another guest callback. Slots may be
        // removed or reused, so retain their post-call target and count too.
        struct HostState {DWORD error=GetLastError();unsigned csr=_mm_getcsr();~HostState(){SetLastError(error);_mm_setcsr(csr);}} host_state;
        const auto list=RandomTraceWord(base,trace_manager+72),count=RandomTraceWord(base,trace_manager+76);
        for(auto i=trace_begin;i<trace_end;++i){
            auto& wait=script_wait_trace[i];wait.after_count=count;
            if(!list || wait.slot>=count || uint64_t(list)+uint64_t(wait.slot+1)*132>0x100000000ull)continue;
            const auto entry=list+wait.slot*132;
            wait.after_target=RandomTraceWord(base,entry+112);
            wait.after_remaining=RandomTraceWord(base,entry+96);
            wait.after_flags=RandomTraceWord(base,entry+100);
        }
    }
}
void em2::ResetOnlineRandomTraceHistory(){random_failure_history.reset();trace_target_cache={};script_wait_trace_size=0;em2::net::ResetNativeBehaviorTrace();}
void em2::DumpOnlineRandomTraceHistory(){
    em2::net::DumpNativeBehaviorTrace();
    if(!random_failure_history)return;
    struct HostState{DWORD error=GetLastError();unsigned csr=_mm_getcsr();~HostState(){SetLastError(error);_mm_setcsr(csr);}}host_state;
    LogScriptWaitTrace();
    random_failure_history->Drain([](const auto& f){
        if(f.character_coverage){
            REXLOG_INFO("Native character proof history: tick {}, count {}",f.tick,f.characters.size());
            for(const auto& c:f.characters)REXLOG_INFO("Native character proof: tick {}, GUID {:08X} {:08X} {:08X} {:08X}, active {}, state {}, position {:08X} {:08X} {:08X}, velocity {:08X} {:08X} {:08X}",
                f.tick,c.id[0],c.id[1],c.id[2],c.id[3],c.active,c.state,c.position[0],c.position[1],c.position[2],c.velocity[0],c.velocity[1],c.velocity[2]);
        }
        REXLOG_INFO("Native online CRT failure history: tick {}, retained {} events (maximum {})",f.tick,f.size,f.events.size());
        for(size_t i=0;i<f.size;++i){const auto& e=f.events[i];
            REXLOG_INFO("Native online CRT event: tick {}, index {}, seed {}, caller {:08X}, state {:08X}, result {:08X}",f.tick,i,e.reseed,e.caller,e.state,e.result);
        }
        LogScriptRandomOwners(f);
    });
}


void em2::RecordNativeRandom(bool reseed,uint32_t caller,uint32_t state,uint32_t result,const PPCContext* context,uint8_t* base) {
    if(in_tick && record_random){
        random_observation.Record(reseed,caller,state,result);
        if(random_failure_history){
            auto* frame=random_failure_history->current;
            if(capture_script_random_owners && context && (caller==0x826F5CB4 || caller==0x826F5ED8) &&
               frame && frame->size<frame->events.size() && frame->script_count<frame->scripts.size()){
                struct HostState {DWORD error=GetLastError();unsigned csr=_mm_getcsr();~HostState(){SetLastError(error);_mm_setcsr(csr);}} host_state;
                frame->scripts[frame->script_count++]=TraceScriptRandomOwner(base,*context,frame->size);
            }
            random_failure_history->Record({reseed,caller,state,result});
        }
        if(capture_online_random && online_random_trace_size<online_random_trace.size()){
            const auto index=online_random_trace_size;
            online_random_trace[online_random_trace_size++]={reseed,caller,state,result};
            if(capture_online_random_owners && context && (caller==0x829506C0 || caller==0x82917C84 || caller==0x830CEC58) && online_random_owner_size<online_random_owners.size()){
                struct HostState {DWORD error=GetLastError();unsigned csr=_mm_getcsr();~HostState(){SetLastError(error);_mm_setcsr(csr);}}host_state;
                auto& owner=online_random_owners[online_random_owner_size++];owner={};owner.event=index;
                owner.registers={context->r29.u32,context->r30.u32,context->r31.u32};
                if(caller==0x829506C0){
                    owner.scheduled=true;owner.schedule={RandomTraceWord(base,0x833FCA4C),RandomTraceWord(base,context->r30.u32+8),RandomTraceWord(base,context->r29.u32+44),RandomTraceWord(base,context->r29.u32+48),RandomTraceWord(base,context->r30.u32+12)};
                }
                for(size_t i=0;i<3;++i)owner.heads[i]=RandomTraceWord(base,owner.registers[i]);
                if(owner.scheduled)owner.identity=TraceTargetIdentity(base,owner.heads[1],clock_input_tick);
                auto stack=context->r1.u32;
                for(auto& address:owner.returns){
                    const auto parent=RandomTraceWord(base,stack);
                    if(parent<=stack || parent-stack>65536)break;
                    address=RandomTraceWord(base,parent-8);stack=parent;
                }
            }
        }
    }
}

PPC_FUNC_IMPL(sub_826C9F38) {
    if(em2::net::TryNativeInputClockQuery(ctx,base))return;
    if(!in_clock_update) { __imp__sub_826C9F38(ctx,base); return; }
    PPC_STORE_U64(ctx.r3.u32,tick_counter);
    ctx.r3.u64=1;
}

namespace {
uint64_t FixedClockInterval(uint64_t frequency,uint64_t input_tick) {
    const auto remainder=frequency%30;
    const auto phase=((input_tick%30)*remainder)%30;
    return frequency/30+(phase+remainder>=30);
}
void UpdateFixedClock(PPCContext& ctx,uint8_t* base,uint64_t counter) {
    // Scope the synthetic interval to the core game clock only. Movie playback,
    // profiling, streaming and kernel timers must continue reading real time.
    // Anchor every update to real QPC so a loading stall cannot leave one clock
    // permanently seconds behind other systems.
    tick_counter=counter;
    uint64_t interval;
    if(agreed_clock_tick) {
        // The same committed input tick gets the same fractional phase on
        // both peers, independent of pre-world ticks, stalls or render cadence.
        interval=FixedClockInterval(tick_frequency,clock_input_tick);
    } else {
        counter_remainder+=tick_frequency%30;
        interval=tick_frequency/30+counter_remainder/30;
        counter_remainder%=30;
    }
    PPC_STORE_U64(0x833FCA68,tick_counter-interval);
    struct Scope {bool previous=in_clock_update;Scope(){in_clock_update=true;}~Scope(){in_clock_update=previous;}} scope;
    __imp__sub_831BC328(ctx,base);
}
}
PPC_FUNC_IMPL(sub_831BC328) {
    if(!in_tick) { __imp__sub_831BC328(ctx,base); return; }
    UpdateFixedClock(ctx,base,PPC_QUERY_TIMEBASE());
}

namespace {
bool FixedTimingPolicy(int cap,bool native_delta,bool force_fixed,bool network,bool retained) {
    return force_fixed || network || retained || ((cap==0 || cap>30) && !native_delta);
}
}
PPC_FUNC_IMPL(sub_829EC348) {
    // Reserve the capped frame before input polling, the native frame gate,
    // and our elapsed-time measurement. Sleeping at Present kept a completed
    // scene waiting and made the controls older by the remaining cap interval.
    em2::GuestFramePacingScope pacing;
    em2::ApplyLiveLightingSettings(base);
    em2::net::GameplayInputGate().Poll();
    em2::UpdateCharacterSwap(ctx,base);
    em2::UpdateGameplayModTargets(ctx,base);
    em2::SetControlUIContext(PPC_LOAD_U8(0x833FCA86)!=0);
    em2::TickMouseCursor(ctx,base);
    em2::TickDebugLevelLoads(ctx,base);
    em2::RunNativeEngineCommands(ctx,base);
    em2::MaybeQueueNativeStartLevel();
    em2::em1::MaybeProbeRegisteredLevelEntry();
    em2::em1::TickArchitecturePreview(ctx,base);
    if(em2::net::NativeRoomActive()){
        // Also reachable while this loaded copy is parked for slower peers.
        // Advancing startup here is safe only after the preceding native finish.
        em2::net::UpdateNativeOnlineSession(base,!in_tick && !frame_finish_pending);
    }else if(!em2::net::RoomBootstrapActive() && !em2::net::UpdateNativeOnlineSession(base))em2::net::UpdateOnlineQASession(base);
    static thread_local auto previous=Clock::now();
    static thread_local bool was_enabled=false;
    static thread_local bool was_network=false;
    const auto now=Clock::now();
    const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(now-previous).count();
    previous=now;
    const bool network=em2::net::GameplayInputGate().active() || em2::net::RoomBootstrapActive();
    const auto retained=em2::net::NativeOnlineTiming();
    frame_halted=retained.halted;
    const auto selected_cap=em2::FrameLimit();
    const bool native_delta=REXCVAR_GET(em2_native_variable_simulation) || em2::PCSetting(15)==1;
    const unsigned speed_percent=em2::EffectiveSimulationSpeedPercent();
    const auto manual_steps=requested_simulation_steps.exchange(0);
    const bool enabled=speed_percent!=100 || FixedTimingPolicy(selected_cap,native_delta,
        REXCVAR_GET(em2_fixed_simulation),network,retained.fixed_step);
    em2::SetNativeTimingPhysicsDiagnostics(native_delta && !enabled);
    if(enabled!=was_enabled || network!=was_network) {scheduler.Reset();network_debt.Reset();was_network=network;}
    if(enabled!=was_enabled) {
        // A native-rate interval is not missed fixed-rate gameplay. Discard
        // its accumulator and poses before entering either mode again.
        scheduler.Reset();
        ResetSimulationHistory(ctx,base);
        was_enabled=enabled;
    }
    em2::BeginCameraPresentationFrame(enabled,double(elapsed)*1e-9);
    if(!enabled) {__imp__sub_829EC348(ctx,base);return;}
    frame_steps=em2::ScheduleSimulationFrame(scheduler,network_debt,elapsed,network,frame_halted,speed_percent);
    if(!network && !speed_percent)frame_steps+=manual_steps;
    frame_active=true;
    __imp__sub_829EC348(ctx,base);
    frame_active=false;
}

PPC_FUNC_IMPL(sub_829ED310) {
    if(!frame_active) {
        const auto started=Clock::now();
        __imp__sub_829ED310(ctx,base);
        em2::em1::TickArchitectureScripts(ctx,base,std::bit_cast<float>(PPC_LOAD_U32(0x833FCA54)));
        MeasureMaximum(em2::maximum_simulation_us,started);
        em2::simulation_steps.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    const auto arguments=ctx;
    application_delta=PPC_LOAD_U32(arguments.r3.u32+76);
    em2::net::Frame network_frame{};
    em2::net::RoomBootstrapFrame room_frame{};
    em2::net::RoomFrame gameplay_frame{};
    auto* room=em2::net::CurrentRoomBootstrapGate();
    auto* gameplay=em2::net::CurrentNativeRoomGameplay();
    const bool room_budget=em2::net::RoomBootstrapActive();
    const bool network_budget=em2::net::GameplayInputGate().active() || room_budget;
    bool network_tick=false,room_tick=false,gameplay_tick=false;
    const auto batch=em2::RunSimulationBatch(frame_steps,[&] {
        network_tick=room_tick=gameplay_tick=false;
        if(em2::net::RoomBootstrapActive()){
            // A worker/current-owner mismatch, terminal state or gameplay
            // cutover cannot accidentally fall through to an offline update.
            if(!room || !room->active())return false;
            if(em2::net::GameplayInputGate().active()){
                room->ObservationFailed();em2::net::GameplayInputGate().ObservationFailed();return false;
            }
            const auto admission=room->Prepare(room_frame);
            room_tick=admission==em2::net::BootstrapAdmission::Ready;
            if(admission==em2::net::BootstrapAdmission::Gameplay && gameplay && gameplay->active())
                gameplay_tick=gameplay->Prepare(ctx,base,gameplay_frame)==em2::net::RoomGameplayAdmission::Ready;
            return room_tick || gameplay_tick;
        }
        if(room_budget)return false;
        const auto admission=em2::net::GameplayInputGate().Prepare(network_frame);
        network_tick=admission==em2::net::Admission::Ready;
        return network_tick || admission==em2::net::Admission::Inactive;
    },[&] {
        ctx.r3=arguments.r3;
        __imp__sub_829D95B8(ctx,base);
        FinishTick(ctx,base);
    },[&] {
        // Finish the preceding tick with its original controller frame before
        // publishing the next agreed frame for the native update.
        if(room_tick && !room->Commit(room_frame))return false;
        if(gameplay_tick && !gameplay->Commit(gameplay_frame,ctx,base)){gameplay->ObservationFailed();return false;}
        if(network_tick && !em2::net::GameplayInputGate().Commit(network_frame)) return false;
        em2::NativeJoinTick join;
        if(room_tick){
            join.joining=room->joining();
            if(!room->PublishedRetailFrame(join.owner_generation,join.raw_retail)){
                room->ObservationFailed();return false;
            }
        }
        ctx=arguments;
        PPC_STORE_U32(arguments.r3.u32+76,std::bit_cast<uint32_t>(1.f/30.f));
        StartTick(base,network_tick || room_tick || gameplay_tick,gameplay_tick?gameplay_frame.tick:(room_tick?room_frame.tick:network_frame.tick));
        NativeTickAdmissionScope admission_scope{gameplay_tick?gameplay:nullptr,room_tick?room:nullptr,network_tick};
        if(gameplay_tick && !gameplay->Deliver(ctx,base)){gameplay->ObservationFailed();return false;}
        if(room_tick)em2::RunNativeSynchronizedJoin(ctx,base,join);
        else if(network_tick)em2::RunNativeSynchronizedJoin(ctx,base,network_frame.players[1].buttons);
        const auto started=Clock::now();
        __imp__sub_829ED310(ctx,base);
        em2::em1::TickArchitectureScripts(ctx,base,std::bit_cast<float>(PPC_LOAD_U32(0x833FCA54)));
        admission_scope.completed=true;
        MeasureMaximum(em2::maximum_simulation_us,started);
        return true;
    },[&] {
        if(!em2::net::NativeOnlineTiming().fixed_step)return true;
        // A slow presentation frame may contain several native updates. Join
        // activation/readiness belongs between each completed pair, not once
        // per presentation. Keep checking after attachment: a new level can
        // restore the single-player companion. Never carry a pre-world budget
        // across attachment.
        ctx=arguments;
        em2::UpdateNativeJoinedPlayerControls(ctx,base);
        if(!em2::net::RoomBootstrapActive())em2::net::UpdateNativeOnlineSession(base,true);
        return (network_budget || (!em2::net::GameplayInputGate().active() && !em2::net::RoomBootstrapActive())) &&
            !em2::net::NativeOnlineTiming().halted && !em2::net::RoomBootstrapHalted();
    },gameplay && gameplay->active());
    frame_steps=batch.steps;
    if(network_budget && !network_debt.Consume(batch.steps)) {
        REXLOG_ERROR("Online simulation consumed more ticks than its bounded schedule");network_debt.Reset();
    }
    frame_finish_pending=batch.finish_pending;
    if(!frame_steps) {
        for(auto container:ui_containers) {
            const bool locked=PPC_LOAD_U8(container+308)!=0;
            if(locked) { ctx=arguments; ctx.r3.u64=container+312; __imp__RtlEnterCriticalSection(ctx,base); }
            PrepareUiRender(container,base);
            if(locked) { ctx=arguments; ctx.r3.u64=container+312; __imp__RtlLeaveCriticalSection(ctx,base); }
        }
        ctx=arguments;
    }
}

PPC_FUNC_IMPL(sub_829D95B8) {
    if(!frame_active) { __imp__sub_829D95B8(ctx,base); return; }
    const auto application=ctx.r3.u32;
    if(frame_finish_pending) { __imp__sub_829D95B8(ctx,base); FinishTick(ctx,base);frame_finish_pending=false; }
    PPC_STORE_U32(application+76,application_delta);
}

PPC_FUNC_IMPL(sub_829D8228) {
    em2::VisualRandomScope visual_random;
    const bool interpolate=frame_active && tick_number && REXCVAR_GET(em2_interpolation);
    const auto started=Clock::now();
    RenderFrameScope profile(REXCVAR_GET(em2_profile_render),interpolate);
    if(!interpolate) {
        {em2::NativeRenderSampleScope sample;__imp__sub_829D8228(ctx,base);}
        MeasureMaximum(em2::maximum_render_us,started);profile.End();return;
    }
    ApplySkinPoses(ctx,base);profile.Mark(0);
    WithRenderTransforms(ctx,base,[&] {
        em2::RenderPresentationCamera(ctx,base,RenderAlpha());
        profile.Mark(1);
        {em2::NativeRenderSampleScope sample;__imp__sub_829D8228(ctx,base);}
        profile.Mark(2);
    });
    MeasureMaximum(em2::maximum_render_us,started);profile.End();
}

PPC_FUNC_IMPL(sub_829A4220) { ProfileRenderHelper(ctx,base,0,__imp__sub_829A4220); }
PPC_FUNC_IMPL(sub_829CDE48) { ProfileRenderHelper(ctx,base,1,__imp__sub_829CDE48); }
PPC_FUNC_IMPL(sub_83241358) { ProfileRenderHelper(ctx,base,2,__imp__sub_83241358); }
PPC_FUNC_IMPL(sub_82654850) { ProfileRenderHelper(ctx,base,3,__imp__sub_82654850); }
PPC_FUNC_IMPL(sub_826548B8) { ProfileRenderHelper(ctx,base,4,__imp__sub_826548B8); }
PPC_FUNC_IMPL(sub_83074F78) { ProfileRenderHelper(ctx,base,5,__imp__sub_83074F78); }
PPC_FUNC_IMPL(sub_83074F90) { ProfileRenderHelper(ctx,base,6,__imp__sub_83074F90); }
PPC_FUNC_IMPL(sub_830749A0) { ProfileRenderHelper(ctx,base,7,__imp__sub_830749A0); }
PPC_FUNC_IMPL(sub_83074938) { ProfileRenderHelper(ctx,base,8,__imp__sub_83074938); }
PPC_FUNC_IMPL(sub_83074508) { ProfileRenderHelper(ctx,base,9,__imp__sub_83074508); }
PPC_FUNC_IMPL(sub_83074548) { ProfileRenderHelper(ctx,base,10,__imp__sub_83074548); }
PPC_FUNC_IMPL(sub_830750B8) { ProfileRenderHelper(ctx,base,11,__imp__sub_830750B8); }
PPC_FUNC_IMPL(sub_825BE2E0) {
    if(native_render_profiling) {
        RenderProfileFpGuard fp;
        static uint64_t probes=0;
        static std::array<uint64_t,64> seen{};
        static unsigned seen_count=0;
        // Bounded discovery, outside the timed scope. Never invoke callbacks
        // or change the native list: the original routine still owns traversal.
        if(++probes%120==1 && seen_count<seen.size()) {
            auto node=PPC_LOAD_U32(ctx.r3.u32+12);
            for(unsigned count=0;node && count<64;++count) {
                const auto stage=PPC_LOAD_U32(node+8);
                if(stage) {
                    const auto table=PPC_LOAD_U32(stage),render=PPC_LOAD_U32(table+40);
                    const auto key=(uint64_t(table)<<32)|render;
                    if(std::find(seen.begin(),seen.begin()+seen_count,key)==seen.begin()+seen_count && seen_count<seen.size()) {
                        seen[seen_count++]=key;
                        REXLOG_INFO("Render stage table {:08X}, render {:08X}, before {:08X}, after {:08X}, enabled {}",
                            table,render,PPC_LOAD_U32(stage+12),PPC_LOAD_U32(stage+16),PPC_LOAD_U8(stage+32));
                    }
                }
                node=PPC_LOAD_U32(node);
            }
        }
    }
    ProfileRenderHelper(ctx,base,12,__imp__sub_825BE2E0);
}
PPC_FUNC_IMPL(sub_82547580) { ProfileRenderHelper(ctx,base,13,__imp__sub_82547580); }
PPC_FUNC_IMPL(sub_83075690) { ProfileRenderHelper(ctx,base,14,__imp__sub_83075690); }
PPC_FUNC_IMPL(sub_825BE1A0) { ProfileRenderHelper(ctx,base,15,__imp__sub_825BE1A0); }
PPC_FUNC_IMPL(sub_8256A528) { ProfileRenderHelper(ctx,base,16,__imp__sub_8256A528); }
PPC_FUNC_IMPL(sub_8256A248) {
    const auto renderer=ctx.r3.u32;
    ProfileRenderHelper(ctx,base,17,__imp__sub_8256A248);
    em2::ApplyLiveShadowResize(ctx,base,renderer);
}
PPC_FUNC_IMPL(sub_830B50B8) { ProfileRenderHelper(ctx,base,18,__imp__sub_830B50B8); }
PPC_FUNC_IMPL(sub_83246488) { ProfileRenderHelper(ctx,base,19,__imp__sub_83246488); }
PPC_FUNC_IMPL(sub_831D0DA8) { ProfileRenderHelper(ctx,base,20,__imp__sub_831D0DA8); }
PPC_FUNC_IMPL(sub_830B5240) { ProfileRenderHelper(ctx,base,21,__imp__sub_830B5240); }
PPC_FUNC_IMPL(sub_830751E0) { ProfileRenderHelper(ctx,base,22,__imp__sub_830751E0); }

PPC_FUNC_IMPL(sub_82581840) {
    const auto object=ctx.r3.u32;
    __imp__sub_82581840(ctx,base);
    RecordTransform(object,base);
}
PPC_FUNC_IMPL(sub_82568C20) {
    const auto camera=ctx.r3.u32;
    __imp__sub_82568C20(ctx,base);
    RecordTransform(camera,base,true);
}
PPC_FUNC_IMPL(sub_825532B0) {
    { std::lock_guard lock(history_mutex); ForgetTransform(ctx.r3.u32); }
    __imp__sub_825532B0(ctx,base);
}

namespace {
bool TraceRenderCall(const char* name,PPCContext& ctx,uint8_t* base,uint64_t& last_report) {
    if(!REXCVAR_GET(em2_trace_render)) return false;
    const auto now=GetTickCount64();
    if(last_report && now-last_report<5000) return false;
    last_report=now;
    std::string callers;
    auto sp=ctx.r1.u32;
    for(int i=0;i<16 && sp;++i) {
        MEMORY_BASIC_INFORMATION memory{};
        if(!VirtualQuery(PPC_RAW_ADDR(sp),&memory,sizeof(memory)) || memory.State!=MEM_COMMIT || (memory.Protect&(PAGE_NOACCESS|PAGE_GUARD))) break;
        const auto next=PPC_LOAD_U32(sp);
        if(next<=sp || next-sp>4*1024*1024) break;
        if(!VirtualQuery(PPC_RAW_ADDR(next-8),&memory,sizeof(memory)) || memory.State!=MEM_COMMIT || (memory.Protect&(PAGE_NOACCESS|PAGE_GUARD))) break;
        callers+=fmt::format(" {:08X}",PPC_LOAD_U32(next-8));
        sp=next;
    }
    REXLOG_INFO("{} at LR {:08X}, object {:08X}, argument {:08X}, tick {} / frame steps {}:{}",name,uint32_t(ctx.lr),ctx.r3.u32,ctx.r4.u32,in_tick,frame_steps,callers);
    return true;
}
}
PPC_EXTERN_FUNC(__imp__sub_82909D00);
PPC_EXTERN_FUNC(__imp__sub_8290A238);
PPC_FUNC_IMPL(sub_82909D00) {
    static thread_local uint64_t last_report=0;
    if(TraceRenderCall("Game skin update",ctx,base,last_report)) {
        const auto modifier=ctx.r3.u32;
        REXLOG_INFO("Skin modifier {:08X}: args {:08X}/{:08X}, attached {:08X}, source {:08X}, count {}, stamp {}, repeat {}",
            modifier,ctx.r4.u32,ctx.r5.u32,PPC_LOAD_U32(modifier+96),PPC_LOAD_U32(modifier+56),
            PPC_LOAD_U32(modifier+84),PPC_LOAD_U32(modifier+8436),PPC_LOAD_U8(modifier+8440));
    }
    RecordSkinSource(ctx,base);
    __imp__sub_82909D00(ctx,base);
}
PPC_FUNC_IMPL(sub_82909528) {
    const auto modifier=ctx.r3.u32;
    bool retained=false;
    { std::lock_guard lock(skin_mutex); retained=skin_history.erase(modifier)!=0; }
    __imp__sub_82909528(ctx,base);
    if(retained) {
        const auto saved=ctx;
        if(ChangeReference(modifier,-1,base)==0) { ctx.r3.u64=modifier; sub_825517C8(ctx,base); }
        ctx=saved;
    }
}
PPC_FUNC_IMPL(sub_8290A238) {
    static thread_local uint64_t last_report=0;
    TraceRenderCall("Game skin completion",ctx,base,last_report);
    __imp__sub_8290A238(ctx,base);
}
PPC_EXTERN_FUNC(__imp__sub_82968430);
PPC_FUNC_IMPL(sub_82968430) {
    auto replay_epoch=REXCVAR_GET(em2_trace_render)?skin_replay_epoch.load(std::memory_order_acquire):0;
    const auto replay_serial=replay_epoch?skin_replay_serial.load(std::memory_order_relaxed):0;
    uint32_t output=0,blocks=0,stride=0;
    if(replay_epoch) {
        const auto workload=ctx.r4.u32;
        const auto streams=PPC_LOAD_U32(workload);
        const auto inputs=PPC_LOAD_U16(workload+8);
        if(PPC_LOAD_U32(streams)+PPC_LOAD_U32(streams+4)!=skin_replay_input.load(std::memory_order_relaxed)) replay_epoch=0;
        if(replay_epoch && PPC_LOAD_U16(workload+10)) {
            const auto descriptor=streams+inputs*16;
            output=PPC_LOAD_U32(descriptor)+PPC_LOAD_U32(descriptor+4);
            blocks=PPC_LOAD_U32(workload+12); stride=PPC_LOAD_U32(descriptor+12);
        }
    }
    static thread_local uint64_t last_report=0;
    const bool report=TraceRenderCall("Game skinning kernel",ctx,base,last_report);
    if(report) {
        const auto workload=ctx.r4.u32;
        REXLOG_INFO("Game skinning workload {:08X}, inputs {}, outputs {}, blocks {}, active epoch {}",
            workload,PPC_LOAD_U16(workload+8),PPC_LOAD_U16(workload+10),PPC_LOAD_U32(workload+12),
            recording_epoch.load(std::memory_order_acquire));
        const auto streams=PPC_LOAD_U32(workload);
        const auto inputs=PPC_LOAD_U16(workload+8),outputs=PPC_LOAD_U16(workload+10);
        for(unsigned i=0;i<std::min<unsigned>(outputs,3);++i) {
            const auto descriptor=streams+(inputs+i)*16;
            REXLOG_INFO("Skin output {}: base {:08X}, offset {:08X}, metadata {:08X}/{:08X}",i,
                PPC_LOAD_U32(descriptor),PPC_LOAD_U32(descriptor+4),PPC_LOAD_U32(descriptor+8),PPC_LOAD_U32(descriptor+12));
        }
    }
    const bool native=em2::TryNativeSkinning(ctx,base);
    if(!native) __imp__sub_82968430(ctx,base);
    if(report) REXLOG_INFO("SIMD skinning selected: {}",native);
    if(replay_epoch) {
        skin_replay_workloads.fetch_add(1,std::memory_order_relaxed);
        SampleSkinOutput(output,blocks,stride,replay_epoch,replay_serial,base);
    }
}
PPC_EXTERN_FUNC(__imp__sub_825C33A0);
PPC_FUNC_IMPL(sub_825C33A0) {
    static thread_local uint64_t last_report=0;
    TraceRenderCall("Legacy skin bounds",ctx,base,last_report);
    __imp__sub_825C33A0(ctx,base);
}
PPC_EXTERN_FUNC(__imp__sub_82609128);
PPC_EXTERN_FUNC(__imp__sub_82606A10);
namespace {
void TraceSkinningCall(const char* name,PPCContext& ctx,uint8_t* base,std::array<uint64_t,4>& reports) {
    if(!REXCVAR_GET(em2_trace_render) || !ctx.r5.u32) return;
    const auto stage=PPC_LOAD_U16(ctx.r5.u32+4);
    const auto slot=stage==0x8020?0u:stage==0x8030?1u:stage==0x8040?2u:3u;
    if(TraceRenderCall(name,ctx,base,reports[slot])) {
        REXLOG_INFO("Skinning sync {:04X}/{:04X}, context {:08X}, last update bits {:08X}",
            stage,PPC_LOAD_U16(ctx.r5.u32+6),PPC_LOAD_U32(ctx.r5.u32+8),PPC_LOAD_U32(ctx.r3.u32+1192));
    }
}
}
PPC_FUNC_IMPL(sub_82609128) {
    static thread_local std::array<uint64_t,4> reports{};
    TraceSkinningCall("Skinning submit",ctx,base,reports);
    __imp__sub_82609128(ctx,base);
}
PPC_FUNC_IMPL(sub_82606A10) {
    static thread_local std::array<uint64_t,4> reports{};
    TraceSkinningCall("Complete skinning",ctx,base,reports);
    __imp__sub_82606A10(ctx,base);
}
PPC_EXTERN_FUNC(__imp__sub_830AAC98);
PPC_EXTERN_FUNC(__imp__sub_830AAE98);
PPC_FUNC_IMPL(sub_830AAC98) {
    static thread_local uint64_t last_report=0;
    TraceRenderCall("Controller update AAC98",ctx,base,last_report);
    __imp__sub_830AAC98(ctx,base);
}
PPC_FUNC_IMPL(sub_830AAE98) {
    static thread_local uint64_t last_report=0;
    TraceRenderCall("Controller update AAE98",ctx,base,last_report);
    __imp__sub_830AAE98(ctx,base);
}
PPC_FUNC_IMPL(sub_82599800) {
    static thread_local uint64_t last_report=0;
    if(TraceRenderCall("NiViewRenderClick",ctx,base,last_report)) {
        auto processor=PPC_LOAD_U32(ctx.r3.u32+104);
        if(!processor) processor=PPC_LOAD_U32(0x833CE930);
        if(processor) {
            const auto table=PPC_LOAD_U32(processor);
            REXLOG_INFO("Live render processor {:08X}, vtable {:08X}, process {:08X}, flush {:08X}",
                processor,table,PPC_LOAD_U32(table+20),PPC_LOAD_U32(table+28));
        }
    }
    __imp__sub_82599800(ctx,base);
}
PPC_FUNC_IMPL(sub_830A0C40) {
    if(frame_active && in_tick && std::find(ui_containers.begin(),ui_containers.end(),ctx.r3.u32)==ui_containers.end())
        ui_containers.push_back(ctx.r3.u32);
    static thread_local uint64_t last_report=0;
    TraceRenderCall("GFx movie advance",ctx,base,last_report);
    __imp__sub_830A0C40(ctx,base);
}

namespace em2 {
static bool TestNativeClockIntervals() {
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));
    if(!base)return false;
    struct Restore {
        uint8_t* base;uint64_t counter=tick_counter,frequency=tick_frequency,remainder=counter_remainder,input_tick=clock_input_tick;
        bool active=in_clock_update,agreed=agreed_clock_tick;unsigned csr=_mm_getcsr();
        ~Restore(){tick_counter=counter;tick_frequency=frequency;counter_remainder=remainder;clock_input_tick=input_tick;
            in_clock_update=active;agreed_clock_tick=agreed;_mm_setcsr(csr);VirtualFree(base,0,MEM_RELEASE);}
    } restore{base};
    for(uint32_t address:{0u,0x82000000u,0x833C0000u,0x833D0000u,0x833F0000u})
        if(!VirtualAlloc(base+address,0x10000,MEM_COMMIT,PAGE_READWRITE))return false;
    PPC_STORE_U32(0x82002060,std::bit_cast<uint32_t>(0.f));
    PPC_STORE_U32(0x82003F48,std::bit_cast<uint32_t>(1.f));
    PPC_STORE_U64(0x833D04D8,1000000);
    PPC_STORE_U32(0x833D04E0,std::bit_cast<uint32_t>(1e-6f));
    PPC_STORE_U32(0x833C01C0,std::bit_cast<uint32_t>(1.f));
    for(uint32_t interval:{1000u,4167u,8333u,16667u,33333u,1000000u}) for(bool paused:{false,true}) {
        std::memset(base+0x833FCA20,0,128);
        PPC_STORE_U32(0x833FCA80,30);PPC_STORE_U8(0x833FCA86,paused);
        PPC_STORE_U64(0x833FCA68,1000000);
        tick_counter=1000000+interval;in_clock_update=true;
        PPCContext context{};context.r1.u64=0xF000;context.fpscr.InitHost();
        // Execute the original native clock, mocking only the counter read.
        // This is the path selected by native variable simulation, without
        // our fixed-tick clock wrapper or a host reimplementation of its math.
        __imp__sub_831BC328(context,base);
        const auto delta=std::bit_cast<float>(PPC_LOAD_U32(0x833FCA54));
        const auto expected=paused?0.f:float(std::min(interval,33333u))*1e-6f;
        if(std::abs(delta-expected)>1e-7f || PPC_LOAD_U64(0x833FCA68)!=tick_counter) {
            REXLOG_ERROR("Original variable clock mismatch: interval {}, pause {}, delta {}, expected {}",interval,paused,delta,expected);return false;
        }
    }
    REXLOG_INFO("Original native clock accepts sub-30-Hz intervals, clamps stalls and preserves pause in 12 interval checks");
    using Snapshot=std::array<uint8_t,128>;
    bool scope_ok=true;
    auto snapshot=[&](uint64_t frequency,uint64_t phase,bool agreed,uint64_t input_tick,uint64_t counter,bool paused,float scale) {
        std::memset(base+0x833FCA20,0,128);
        PPC_STORE_U32(0x833FCA80,30);PPC_STORE_U8(0x833FCA86,paused);
        PPC_STORE_U64(0x833D04D8,frequency);
        PPC_STORE_U32(0x833D04E0,std::bit_cast<uint32_t>(1.f/float(frequency)));
        PPC_STORE_U32(0x833C01C0,std::bit_cast<uint32_t>(scale));
        tick_frequency=frequency;counter_remainder=phase;agreed_clock_tick=agreed;clock_input_tick=input_tick;
        in_clock_update=false;
        PPCContext context{};context.r1.u64=0xF000;context.fpscr.InitHost();
        UpdateFixedClock(context,base,counter);
        scope_ok&=!in_clock_update && PPC_LOAD_U64(0x833FCA68)==counter;
        Snapshot result;std::memcpy(result.data(),base+0x833FCA20,result.size());
        // Only the absolute last-QPC anchor is intentionally local. Compare
        // every other byte, including scaled/unscaled deltas and accumulators.
        std::fill_n(result.begin()+(0x833FCA68-0x833FCA20),8,0);
        return result;
    };
    const auto floor_step=snapshot(50000000,0,false,0,5000000000,false,1.f);
    const auto ceil_step=snapshot(50000000,20,false,0,6000000000,false,1.f);
    if(floor_step==ceil_step ||
       std::memcmp(floor_step.data()+0x54-0x20,ceil_step.data()+0x54-0x20,4)!=0 ||
       std::memcmp(floor_step.data()+0x74-0x20,ceil_step.data()+0x74-0x20,4)==0) {
        REXLOG_ERROR("Native clock control did not expose the local fractional-phase difference");return false;
    }
    size_t comparisons=0;
    for(uint64_t frequency:{1000000ull,50000000ull,50000001ull}) for(bool paused:{false,true}) for(float scale:{.5f,1.f,2.f}) {
        for(unsigned index=0;index<62;++index) {
            const uint64_t input_tick=index<60?uint64_t(index):UINT64_MAX-(61-index);
            const auto canonical_phase=((input_tick%30)*(frequency%30))%30;
            const auto expected=snapshot(frequency,canonical_phase,false,input_tick,5000000000,paused,scale);
            for(uint64_t phase=0;phase<30;++phase) {
                const auto actual=snapshot(frequency,phase,true,input_tick,6000000000+phase*1234567,paused,scale);
                if(actual!=expected) {
                    REXLOG_ERROR("Agreed native clock mismatch: frequency {}, pause {}, scale {}, tick {}, local phase {}",
                        frequency,paused,scale,input_tick,phase);return false;
                }
                ++comparisons;
            }
        }
    }
    for(uint64_t frequency:std::array<uint64_t,8>{0,1,29,30,31,1000000,50000000,UINT64_MAX}) {
        uint64_t sum=0;
        for(uint64_t index=0;index<30;++index) {
            sum+=FixedClockInterval(frequency,index);
            if(FixedClockInterval(frequency,index)!=FixedClockInterval(frequency,index+30))return false;
        }
        if(sum!=frequency || FixedClockInterval(frequency,UINT64_MAX)!=FixedClockInterval(frequency,UINT64_MAX%30))return false;
    }
    if(!scope_ok)return false;
    // Execute the actual native NPC timer helpers against consecutive original
    // core-clock updates. Changing the sampled host counter must not change
    // the cached scaled game counter used by 831C13F0. This distinguishes that
    // counter from the intentionally local raw counter at 833FCA68.
    for(const uint64_t frequency:{1000000ull,50000001ull}) {
        std::vector<uint32_t> reference;
        for(unsigned peer=0;peer<2;++peer) {
            std::memset(base+0x833FCA20,0,128);
            const uint64_t epoch=peer?9000000000ull:1000000000ull;
            PPC_STORE_U64(0x833FCA60,epoch);
            PPC_STORE_U64(0x833D04D8,frequency);
            PPC_STORE_U32(0x833D04E0,std::bit_cast<uint32_t>(1.f/float(frequency)));
            PPC_STORE_U32(0x833FCA80,30);
            PPC_STORE_U32(0x833C01C0,std::bit_cast<uint32_t>(1.f));
            tick_frequency=frequency;
            PPCContext context{};context.r1.u64=0xF000;context.fpscr.InitHost();
            context.r3.u64=0x2000;context.f1.f64=0;
            __imp__sub_831C1360(context,base);
            for(unsigned tick=0;tick<1080;++tick) {
                // Different host stalls/epochs, identical authored simulation.
                agreed_clock_tick=true;clock_input_tick=tick;
                PPC_STORE_U8(0x833FCA86,tick>=20 && tick<23);
                UpdateFixedClock(context,base,epoch+uint64_t(tick+1)*(peer?987654321ull:1000ull));
                context.r3.u64=0x2000;
                __imp__sub_831C13F0(context,base);
                const auto elapsed=std::bit_cast<uint32_t>(float(context.f1.f64));
                if(!peer)reference.push_back(elapsed);
                else if(reference[tick]!=elapsed) {
                    REXLOG_ERROR("Native NPC timer differs across host pacing: frequency {}, tick {}",frequency,tick);return false;
                }
                // Native single-precision accumulation may land slightly below
                // 35 seconds; require the authored tick boundary, while the
                // comparison above requires bit-exact elapsed time on peers.
                if(tick==1052 && std::abs(context.f1.f64-35.)>1./30.) {
                    REXLOG_ERROR("Native NPC timer lost authored 35-second boundary: {}",context.f1.f64);return false;
                }
                if(tick==1053) {
                    context.r3.u64=0x2000;context.f1.f64=.25;
                    __imp__sub_831C1360(context,base);
                }
            }
        }
    }
    REXLOG_INFO("Original native NPC timers: 2160 exact peer comparisons across host stalls, local epochs, fractional frequency, pause, 35-second boundary and delayed reset");
    REXLOG_INFO("Agreed native clock: {} byte-exact output comparisons across local phases, counters, pauses, scales and tick exhaustion; local-phase control differs only as expected",comparisons);
    return true;
}
bool TestSimulation() {
    if(!TestCameraTargetHistory())return false;
    if(!TestCameraPresentation())return false;
    if(!TestNativeCameraLookAction())return false;
    if(!TestRandomFailureHistory())return false;
    if(!TestScriptRandomOwner())return false;
    if(!TestRenderProfiling() || !TestNativeClockIntervals() || !TestPreparedSkinInterpolation())return false;
    bool ok=true;
    auto check=[&](bool condition,const char* message) {
        if(!condition) { REXLOG_ERROR("Simulation test: {}",message); ok=false; }
    };
    {
        struct Restore {
            bool tick=in_tick,agreed=agreed_clock_tick,random=record_random,trace=capture_online_random,
                owners=capture_online_random_owners,scripts=capture_script_random_owners;
            uint64_t epoch=recording_epoch.load();
            ~Restore(){in_tick=tick;agreed_clock_tick=agreed;record_random=random;capture_online_random=trace;
                capture_online_random_owners=owners;capture_script_random_owners=scripts;recording_epoch.store(epoch);}
        } restore;
        for(bool throwing:{false,true}){
            unsigned native_updates=0,post_updates=0;bool caught=false;
            const auto csr=_mm_getcsr();const auto error=GetLastError();
            try{
                const auto batch=RunSimulationBatch(1,[]{return true;},[&]{++post_updates;},[&]{
                    in_tick=agreed_clock_tick=record_random=capture_online_random=capture_online_random_owners=capture_script_random_owners=true;
                    recording_epoch.store(912);
                    NativeTickAdmissionScope failed;
                    if(throwing)throw 7;
                    return false; // Native input failed before the world update.
                });
                check(batch.steps==0 && !batch.finish_pending,"failed delivery cannot invent a completed native world update");
            }catch(int){caught=true;}
            check(caught==throwing && native_updates==0 && post_updates==0 && !in_tick && !agreed_clock_tick &&
                !record_random && !capture_online_random && !capture_online_random_owners && !capture_script_random_owners &&
                recording_epoch.load()==0 && _mm_getcsr()==csr && GetLastError()==error,
                "returned/thrown delivery failure clears actual native observation clock without running world/post-update or changing host state");
        }
    }
    for(unsigned speed:{0u,25u,50u,100u,200u,800u}){
        SimulationScheduler clock;clock.Advance(0,speed);unsigned steps=0;
        for(int i=0;i<120;++i)steps+=clock.Advance(25000000,speed);
        check(steps==90*speed/100,"time scaling changes tick admission and preserves the fixed physics step");
        const auto alpha=clock.Alpha();check(!clock.Advance(250000000,0) && clock.Alpha()==alpha,"pausing retains the simulation clock without hidden catch-up");
    }
    {SimulationScheduler clock;SimulationDebt debt;ScheduleSimulationFrame(clock,debt,0,true,false,0);check(ScheduleSimulationFrame(clock,debt,100000000,true,false,0)==4,"offline time scaling cannot alter agreed online tick admission");}
    for(int cap:{30,40,50,60,90,120,144,165,240,0}) {
        check(!FixedTimingPolicy(cap,true,false,false,false),"native offline timing must bypass interpolation at every cap");
        check(FixedTimingPolicy(cap,false,false,false,false)==(cap!=30),"compatibility timing must retain the original 30-FPS native path");
        check(FixedTimingPolicy(cap,true,false,true,false) && FixedTimingPolicy(cap,true,false,false,true) &&
              FixedTimingPolicy(cap,true,true,false,false),"online ownership and explicit fixed override must win over native menu selection");
    }
    // A real Room owner cannot prepare a frame while its prior native update
    // still awaits post-update/Complete. Finalize it before the next admission
    // so low presentation rates can run multiple authored ticks per frame.
    for(int fps:{15,20,30,60,120}) {
        SimulationScheduler clock;SimulationDebt debt;
        unsigned updates=0,finishes=0;bool pending=false,ordered=true;
        int64_t previous=0;
        for(int frame=0;frame<=fps*10;++frame) {
            const auto now=int64_t(frame)*1000000000/fps;
            const auto due=ScheduleSimulationFrame(clock,debt,now-previous,true,false);previous=now;
            const auto batch=RunSimulationBatch(due,[&]{return !pending;},[&]{
                ordered&=pending;pending=false;++finishes;
            },[&]{ordered&=!pending;pending=true;++updates;return true;},[]{return true;},true);
            if(batch.finish_pending){ordered&=pending;pending=false;++finishes;}
            check(debt.Consume(batch.steps),"completed-first Room batch consumes only executed simulation debt");
        }
        check(ordered && !pending && updates==301 && finishes==301,
            "completion-dependent admission sustains thirty native ticks at 15/20/30/60/120 presentation Hz with exactly one post-update each");
    }
    {
        unsigned updates=0,finishes=0;bool pending=false;
        const auto batch=RunSimulationBatch(3,[&]{return updates<1;},[&]{pending=false;++finishes;},
            [&]{pending=true;++updates;return true;},[]{return true;},true);
        check(updates==1 && finishes==1 && !pending && batch.steps==1 && !batch.finish_pending,
            "blocked next admission cannot leave an already completed Room tick for outer finalization");
    }
    for(int fps:{30,60,90,120,240}) {
        SimulationScheduler clock;SimulationDebt debt;unsigned executed=0,finished=0;
        debt.Add(clock.Advance(0));
        int64_t previous=0;
        for(int frame=1;frame<=fps*10+1;++frame) {
            const auto now=int64_t(std::min(frame,fps*10))*1000000000/fps;
            const auto planned=debt.Add(clock.Advance(now-previous));previous=now;
            const auto batch=RunSimulationBatch(planned,[&]{return frame%5!=0;},[&]{++finished;},[]{return true;});
            executed+=batch.steps;finished+=batch.finish_pending;
            check(debt.Consume(batch.steps),"network batch consumes only scheduled debt");
        }
        check(executed==301 && finished==301,"intermittent network waits retain thirty-Hz ticks at every presentation rate");
        check(debt.Add(UINT32_MAX)==8 && !debt.Consume(9) && debt.Consume(8),"long network stalls retain a bounded catch-up budget without overflow");
    }
    for(int fps:{30,60,120,240})for(bool stall:{false,true}) {
        SimulationScheduler startup_clock;unsigned updates=0,finishes=0,boundaries=0;
        bool attached=false,pending=false,ordered=true;
        int64_t previous=0;
        for(unsigned frame=0;frame<100 && !attached;++frame){
            const auto now=int64_t(frame)*1000000000/fps+(stall && frame?250000000:0);
            const auto planned=startup_clock.Advance(now-previous);previous=now;
            const auto batch=RunSimulationBatch(planned,[]{return true;},[&]{
                ordered&=pending;pending=false;++finishes;
            },[&]{ordered&=!pending && !attached;pending=true;++updates;return true;},[&]{
                ordered&=!pending;++boundaries;
                // Fixture readiness becomes visible after three native updates.
                // The production boundary runs after native finalization too.
                if(updates==3){attached=true;return false;}return true;
            });
            if(batch.finish_pending){ordered&=pending;pending=false;++finishes;}
        }
        check(attached && ordered && !pending && updates==3 && finishes==3 && boundaries==4,
            "startup attachment happens at the same completed tick across presentation rates and multi-tick stalls, without carrying pre-world ticks across attachment");
    }
    for(const auto terminal:{net::BootstrapAdmission::Gameplay,net::BootstrapAdmission::Stopped}){
        unsigned updates=0,post_updates=0,completed=0,readiness_probes=0,observations=0;
        bool pending=false,ordered=true;net::BootstrapAdmission admission=net::BootstrapAdmission::Ready;
        auto finish=[&]{
            ordered &= pending && post_updates==completed;
            ++post_updates; // Original native post-update precedes readiness.
            ordered &= post_updates==updates;++readiness_probes;
            ordered &= readiness_probes==post_updates;++observations;
            ordered &= observations==readiness_probes;++completed;pending=false;
            admission=completed==3?terminal:net::BootstrapAdmission::Ready;
        };
        for(unsigned presentation=0;presentation<8;++presentation){
            // One full native update owns its raw pad until outer finalization.
            // Waiting for its completion is NOT permission for offline fallback.
            const auto batch=RunSimulationBatch(8,[&]{
                if(pending)return false;
                return admission==net::BootstrapAdmission::Ready;
            },finish,[&]{ordered &= !pending;pending=true;++updates;return true;});
            if(batch.finish_pending)finish(); // Actual outer829D95B8 responsibility.
            else ordered &= !pending;
        }
        check(ordered && !pending && updates==3 && post_updates==3 && readiness_probes==3 && observations==3 && completed==3,
            "raw Room completion waits finalize native post-update/readiness/observation exactly once; gameplay/stopped admission never falls through offline");
    }
    for(unsigned planned=0;planned<=8;++planned) for(unsigned available=0;available<=planned;++available) {
        unsigned started=0,finished=0,admissions=0;
        bool pending=false;bool ordered=true;
        const auto batch=RunSimulationBatch(planned,[&]{++admissions;return started<available;},[&] {
            ordered&=pending;pending=false;++finished;
        },[&] {ordered&=!pending;pending=true;++started;return true;});
        if(batch.finish_pending) {ordered&=pending;pending=false;++finished;}
        check(ordered && !pending && batch.steps==available && started==available && finished==available &&
            admissions==std::min(planned,available+1),"partial network catch-up finishes each admitted tick exactly once");
    }
    {
        unsigned started=0,finished=0;
        const auto batch=RunSimulationBatch(3,[]{return true;},[&]{++finished;},[&]{return ++started<2;});
        check(batch.steps==1 && finished==1 && !batch.finish_pending,
            "failed next-frame commit cannot finalize the previous tick twice");
    }
    for(int fps:{30,40,50,60,90,120,144,165,240,1000}) {
        SimulationScheduler clock;
        unsigned steps=clock.Advance(0);
        int64_t previous=0;
        for(int frame=1;frame<=fps*10;++frame) {
            const int64_t now=int64_t(frame)*1000000000/fps;
            steps+=clock.Advance(now-previous); previous=now;
            check(clock.Alpha()>=0 && clock.Alpha()<1,"interpolation alpha bounds");
        }
        check(steps==301,"identical 30 Hz simulation over ten seconds at every presentation rate");
    }
    SimulationScheduler stalled; stalled.Advance(0);
    check(stalled.Advance(10000000000)==7,"bounded catchup after suspension");
    check(stalled.Advance(-100)==0,"negative clock delta");
    stalled.Reset();
    check(stalled.Alpha()==0 && stalled.Advance(10000000000)==1,
        "re-enabling fixed simulation starts one update without disabled-time catchup");
    check(stalled.Advance(16666667)==0 && stalled.Advance(16666667)==1,
        "fresh accumulator resumes normal 30 Hz cadence after a mode change");
    Matrix a{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    Matrix b{-1,0,0,0,0,-1,0,0,0,0,1,0,10,20,30,3};
    const auto halfway=Interpolate(a,b,.5f);
    check(halfway[12]==5 && halfway[13]==10 && halfway[14]==15 && halfway[15]==2,"translation and scale midpoint");
    check(std::abs(halfway[0])<1e-5f && std::abs(halfway[1]+1)<1e-5f && std::abs(halfway[4]-1)<1e-5f,"rotation follows an arc without shrinking bones");
    check(Interpolate(a,b,0)==a && Interpolate(a,b,1)==b,"exact transform endpoints");
    auto scaled=a; scaled[0]=2; scaled[5]=.5f; scaled[1]=.25f;
    auto scaled_next=scaled; scaled_next[0]=3; scaled_next[12]=10;
    const auto affine=Interpolate(scaled,scaled_next,.5f);
    check(affine[0]==2.5f && affine[5]==.5f && affine[1]==.25f && affine[12]==5,"authored bone scale and shear survive interpolation");
    auto mirrored=a; mirrored[0]=-1;
    check(Interpolate(mirrored,mirrored,.5f)==mirrored,"reflected transforms retain handedness");
    auto padded=a; padded[3]=NAN;
    auto padded_next=padded; padded_next[12]=10;
    const auto padded_half=Interpolate(padded,padded_next,.5f);
    check(padded_half[12]==5 && std::isnan(padded_half[3]),"unused matrix padding cannot disable valid interpolation");
    b[12]=NAN;
    check(std::isnan(Interpolate(a,b,.5f)[12]),"invalid input does not contaminate another transform");
    alignas(16) std::array<uint8_t,1024> memory{};
    auto* base=memory.data();
    PPC_STORE_U32(256+4,1);
    PPC_STORE_U32(256+36,512);
    PPC_STORE_U32(512+5*4,648);
    PPC_STORE_U32(640+172,std::bit_cast<uint32_t>(34.f));
    PPC_STORE_U32(640+176,std::bit_cast<uint32_t>(11.f));
    PPC_STORE_U32(640+180,std::bit_cast<uint32_t>(194.f));
    uint32_t position_component=0;
    std::array<float,3> entity_position{};
    check(ReadEntityPosition(256,5,base,position_component,entity_position) &&
          position_component==640 && entity_position==std::array<float,3>{34,11,194},
          "entity position follows native component indirection and guest byte order");
    check(!ReadEntityPosition(256,255,base,position_component,entity_position),
          "missing entity transform slot is not reported as a zero position");
    PPC_STORE_U32(640+176,std::bit_cast<uint32_t>(NAN));
    check(!ReadEntityPosition(256,5,base,position_component,entity_position),
          "invalid entity position is excluded from motion evidence");
    PPC_STORE_U32(256+36,0);
    check(!ReadEntityPosition(256,5,base,position_component,entity_position),
          "missing entity component array is excluded from motion evidence");
    check(ChangeReference(256,1,base)==2 && PPC_LOAD_U32(256+4)==2,"engine reference increment preserves guest byte order");
    check(ChangeReference(256,-1,base)==1 && PPC_LOAD_U32(256+4)==1,"engine reference decrement preserves guest byte order");
    WriteTransform(256,a,base);
    check(ReadTransform(256,base)==a,"64-byte NiTransform memory roundtrip");
    PPC_STORE_U32(512+348,64); PPC_STORE_U32(512+352,1);
    PPC_STORE_U32(512+360,96); PPC_STORE_U32(512+364,1);
    PPC_STORE_U32(64,128); PPC_STORE_U32(96,256);
    PPC_STORE_U8(128+16,1); PPC_STORE_U8(128+18,0);
    PPC_STORE_U8(256+16,0); PPC_STORE_U8(256+18,0);
    PPC_STORE_U32(128+40,1234);
    PrepareUiRender(512,base);
    check(PPC_LOAD_U8(128+18)==1 && PPC_LOAD_U8(256+18)==0,"render-only UI redraw preserves hidden movies");
    check(PPC_LOAD_U32(128+40)==1234,"UI redraw does not advance its animation clock");
    // Exercise the actual render-history path, not just interpolation math.
    // One external reference keeps this fake guest object alive after expiry,
    // so no guest destructor needs to execute in this headless check.
    const auto saved_scheduler=scheduler;
    const auto saved_tick=tick_number;
    const auto saved_steps=simulation_steps.load();
    const auto saved_interpolated=interpolated_nodes.load();
    check(history.empty(),"history tests start without live game objects");
    {
        const auto prior_scheduler=scheduler;const auto prior_flags=_mm_getcsr();
        scheduler.Reset();scheduler.Advance(0);scheduler.Advance(11111111);_mm_setcsr(0x1F80);
        PPCContext empty_context{};ApplyTransforms(empty_context,base,false);
        check(_mm_getcsr()==0x1F80,"empty moving history performs no interpolation math or FP-state changes");
        scheduler=prior_scheduler;_mm_setcsr(prior_flags);
    }
    Matrix next=a; next[12]=10;
    const auto saved_epoch=recording_epoch.load();
    const auto saved_interpolation=REXCVAR_GET(em2_interpolation);
    const auto saved_motion_trace=REXCVAR_GET(em2_trace_motion);
    REXCVAR_SET(em2_trace_motion,false); // Fake guest memory has no player registry.
    REXCVAR_SET(em2_interpolation,true);
    WriteTransform(256,a,base); PPC_STORE_U32(256+56,0);
    tick_number=10;
    recording_epoch.store(tick_number);
    std::thread worker([&] { RecordTransform(256,base); });
    worker.join();
    check(history.contains(256) && PPC_LOAD_U32(256+4)==2,"worker-thread transforms join the active simulation epoch");
    recording_epoch.store(0);
    WriteTransform(256,next,base);
    scheduler=SimulationScheduler{};
    scheduler.Advance(0); scheduler.Advance(16666667);
    PPCContext context{}; context.r3.u64=77;
    FinishTick(context,base);
    check(moving_objects.size()==1,"completed tick caches the moving object");
    history.at(256).prepare_streak=2;
    ApplyTransforms(context,base,false);
    check(ReadTransform(256,base)[12]==5 && context.r3.u64==77,"render sees interpolated pose without changing guest registers");
    ApplyTransforms(context,base,true);
    check(ReadTransform(256,base)==next,"render restores the exact simulation pose");
    unsigned nested_draws=0;
    WithRenderTransforms(context,base,[&] {
        check(ReadTransform(256,base)[12]==5,"outer render enters interpolated pose");
        WithRenderTransforms(context,base,[&] {
            ++nested_draws;context.r3.u64=99;
            check(ReadTransform(256,base)[12]==5,"nested render sees the same interpolated pose");
        });
        check(ReadTransform(256,base)[12]==5,"nested render cannot restore the outer pose early");
    });
    check(nested_draws==1 && ReadTransform(256,base)==next && !history.at(256).applied &&
          context.r3.u64==99 && render_transform_depth==0,"nested render restores authoritative pose and preserves native return registers");
    bool render_failed=false;
    try {
        WithRenderTransforms(context,base,[&] {
            WithRenderTransforms(context,base,[&] {
                check(ReadTransform(256,base)[12]==5,"fault fixture executes with interpolated pose");
                context.r3.u64=123;throw std::runtime_error("render pose unwind fixture");
            });
        });
    }catch(const std::runtime_error& error){render_failed=std::strcmp(error.what(),"render pose unwind fixture")==0;}
    check(render_failed && ReadTransform(256,base)==next && !history.at(256).applied &&
          context.r3.u64==123 && render_transform_depth==0,"render exception restores simulation pose, depth and original exception without clobbering registers");
    WithRenderTransforms(context,base,[&]{check(ReadTransform(256,base)[12]==5,"interpolation recovers after a failed render");});
    check(ReadTransform(256,base)==next && render_transform_depth==0,"post-failure render restores authoritative pose");
    context.r3.u64=77; // Restore the surrounding mode-transition fixture.
    history.at(256).previous=next;
    auto rotated=next;rotated[0]=rotated[5]=0;rotated[1]=-1;rotated[4]=1;rotated[12]=20;
    WriteTransform(256,rotated,base);FinishTick(context,base);
    const auto expected_changed=Interpolate(next,rotated,.5f);
    ApplyTransforms(context,base,false);
    check(ReadTransform(256,base)==expected_changed,"completed simulation tick invalidates cached scene endpoints");
    ApplyTransforms(context,base,true);
    check(ReadTransform(256,base)==rotated,"cached scene interpolation restores new authoritative pose");
    auto recorded=next;recorded[12]=24;
    WriteTransform(256,recorded,base);recording_epoch.store(tick_number);RecordTransform(256,base);recording_epoch.store(0);
    ApplyTransforms(context,base,false);
    check(ReadTransform(256,base)==Interpolate(next,recorded,.5f),"newly recorded transforms invalidate scene preparation within the epoch");
    ApplyTransforms(context,base,true);
    check(ReadTransform(256,base)==recorded,"recorded-pose interpolation restores authoritative transform");
    history.rehash(history.bucket_count()*4+1);
    ApplyTransforms(context,base,false);
    check(ReadTransform(256,base)==Interpolate(next,recorded,.5f),"cached moving pointers survive history rehash");
    ApplyTransforms(context,base,true);
    Matrix parity;unsigned parity_flags=0;const unsigned before_parity_flags=_mm_getcsr();
    const bool saved_render_cache=REXCVAR_GET(em2_cache_scene_render_entries);
    for(bool cached:{false,true}) {
        REXCVAR_SET(em2_cache_scene_render_entries,cached);_mm_setcsr(0x1F80);
        ApplyTransforms(context,base,false);
        if(!cached){parity=ReadTransform(256,base);parity_flags=_mm_getcsr();}
        else {const auto actual=ReadTransform(256,base);check(std::memcmp(actual.data(),parity.data(),64)==0 && _mm_getcsr()==parity_flags,"cached render entries preserve full transform and FP flags");}
        ApplyTransforms(context,base,true);
        check(ReadTransform(256,base)==recorded,"reference and cached render entries both restore simulation pose");
    }
    REXCVAR_SET(em2_cache_scene_render_entries,saved_render_cache);_mm_setcsr(before_parity_flags);
    history.at(256).camera=true;
    ApplyTransforms(context,base,false);
    check(ReadTransform(256,base)==recorded && !history.at(256).applied,
        "offscreen shadow/reflection camera poses stay native instead of disagreeing with cached projector constants");
    ApplyTransforms(context,base,true);history.at(256).camera=false;
    {std::lock_guard lock(history_mutex);auto [entry,inserted]=history.emplace(1024,TransformHistory{});entry->second.moving=true;moving_objects.push_back({1024,&entry->second});ForgetTransform(1024);}
    check(!history.contains(1024) && moving_objects.size()==1 && moving_objects.front().object==256,"native detach removes cached pointer before history destruction");
    history.at(256).previous=next;
    next[3]=NAN; WriteTransform(256,next,base);
    FinishTick(context,base);
    check(moving_objects.empty(),"padding-only changes do not enqueue stationary objects");
    tick_number+=4;
    FinishTick(context,base);
    check(history.empty() && moving_objects.empty() && PPC_LOAD_U32(256+4)==1,"expired history releases its engine reference and cached render entry");
    recording_epoch.store(tick_number);
    RecordTransform(256,base);
    check(PPC_LOAD_U32(256+4)==2,"mode-reset fixture retains the scene object");
    PPC_STORE_U32(768+4,1);
    ChangeReference(768,1,base);
    skin_history.emplace(768,SkinHistory{});
    ResetSimulationHistory(context,base);
    check(history.empty() && moving_objects.empty() && skin_history.empty() &&
        !recording_epoch.load() && !in_tick && !interpolated_nodes.load() && PPC_LOAD_U32(256+4)==1 &&
        PPC_LOAD_U32(768+4)==1 && context.r3.u64==77,
        "mode transition discards stale poses and releases references without changing guest registers");
    scheduler=saved_scheduler; tick_number=saved_tick;
    recording_epoch.store(saved_epoch);
    REXCVAR_SET(em2_interpolation,saved_interpolation);
    REXCVAR_SET(em2_trace_motion,saved_motion_trace);
    simulation_steps.store(saved_steps); interpolated_nodes.store(saved_interpolated);
    Matrix camera_a{}; camera_a[0]=camera_a[5]=camera_a[10]=camera_a[15]=1;
    auto camera_b=camera_a;
    camera_b[0]=camera_b[10]=0; camera_b[2]=1; camera_b[8]=-1;
    check(CameraRotationDegrees(camera_a,camera_a)<1e-4 &&
          std::abs(CameraRotationDegrees(camera_a,camera_b)-90)<1e-3,
          "camera diagnostic measures stationary and quarter-turn rotations");
    camera_b[0]=2;
    check(std::isnan(CameraRotationDegrees(camera_a,camera_b)),"camera diagnostic rejects non-rigid rotation matrices");
    // Exercise the native skinning transaction against a fake guest backend.
    // The callbacks verify what the engine actually receives and consumes.
    alignas(64) std::array<uint8_t,65536> skin_memory{};
    auto* skin_base=skin_memory.data();
    constexpr uint32_t skin_modifier=256,skin_source=16384,skin_scratch=24576;
    SkinHistory skin_pose;
    skin_pose.mesh=40000; skin_pose.source=skin_source; skin_pose.morph=41000;
    Matrix bone{}; bone[0]=bone[5]=bone[10]=bone[15]=1; bone[12]=2;
    skin_pose.previous.push_back(bone);
    bone[12]=10; skin_pose.current.push_back(bone);
    auto* saved_base=base; base=skin_base;
    PPC_STORE_U32(skin_modifier+56,skin_source);
    PPC_STORE_U32(skin_modifier+8436,123);
    PPC_STORE_U8(skin_modifier+8440,0);
    for(unsigned i=0;i<4;++i)PPC_STORE_U32(skin_modifier+8444+i*4,0x12340000+i);
    for(unsigned i=0;i<16;++i) PPC_STORE_U32(skin_source+i*4,std::bit_cast<uint32_t>(bone[i]));
    std::memset(base+skin_modifier+104,0x5A,8216);
    const auto original_skin_memory=skin_memory;
    static unsigned skin_completions=0;
    static bool skin_arguments=false;
    skin_completions=0; skin_arguments=false;
    auto fake_complete=+[](PPCContext& ctx,uint8_t* base) {
        ++skin_completions;
        if(skin_completions==2) {
            // A completed job produces visible geometry from its copied input.
            PPC_STORE_U32(50000,PPC_LOAD_U32(ctx.r3.u32+108+48));
        }
        ctx.r31.u64=0xBAD;
    };
    auto fake_update=+[](PPCContext& ctx,uint8_t* base) {
        const auto modifier=ctx.r3.u32;
        skin_arguments=ctx.r4.u32==40000 && ctx.r5.u32==41000 && PPC_LOAD_U8(modifier+8440)==1;
        std::memcpy(base+modifier+108,base+PPC_LOAD_U32(modifier+56),64);
        PPC_STORE_U32(modifier+8436,999);
        for(unsigned i=0;i<4;++i)PPC_STORE_U32(modifier+8444+i*4,0x56780000+i);
        ctx.r4.u64=0xBAD;
        _mm_setcsr((_mm_getcsr()&~_MM_ROUND_MASK)|_MM_ROUND_DOWN);
    };
    PPCContext skin_context{};
    skin_context.r3.u64=77; skin_context.r4.u64=88; skin_context.r31.u64=99;
    const auto original_mxcsr=_mm_getcsr();
    ReplaySkin(skin_context,base,skin_modifier,skin_pose,skin_scratch,.25f,fake_update,fake_complete);
    check(skin_completions==2 && skin_arguments && std::bit_cast<float>(PPC_LOAD_U32(50000))==4,
          "native skinning receives interpolated bones and completes the render output");
    check(std::memcmp(base+skin_source,original_skin_memory.data()+skin_source,64)==0 &&
          std::memcmp(base+skin_modifier+104,original_skin_memory.data()+skin_modifier+104,8216)==0 &&
          std::memcmp(base+skin_modifier+8444,original_skin_memory.data()+skin_modifier+8444,16)==0 &&
          PPC_LOAD_U32(skin_modifier+56)==skin_source && PPC_LOAD_U32(skin_modifier+8436)==123 &&
          PPC_LOAD_U8(skin_modifier+8440)==0,
          "skinning replay restores the source pointer, native input stream and update stamp");
    check(skin_context.r3.u64==77 && skin_context.r4.u64==88 && skin_context.r31.u64==99 &&
          _mm_getcsr()==original_mxcsr,"skinning replay preserves guest and host floating-point state");
    for(float alpha:{0.f,.25f,.5f,.75f,1.f}) {
        skin_completions=0;skin_arguments=false;
        ReplaySkin(skin_context,base,skin_modifier,skin_pose,skin_scratch,alpha,fake_update,fake_complete);
        check(skin_completions==2 && skin_arguments && std::bit_cast<float>(PPC_LOAD_U32(50000))==2+8*alpha &&
              std::memcmp(base+skin_source,original_skin_memory.data()+skin_source,64)==0 &&
              std::memcmp(base+skin_modifier+104,original_skin_memory.data()+skin_modifier+104,8216)==0 &&
              std::memcmp(base+skin_modifier+8444,original_skin_memory.data()+skin_modifier+8444,16)==0,
              "multiple render samples of one tick produce distinct deformed positions while preserving simulation input");
    }
    const auto saved_pose_cache=REXCVAR_GET(em2_cache_pose_interpolation);
    REXCVAR_SET(em2_cache_pose_interpolation,true);
    const auto replay_snapshot=SnapshotSkin(skin_pose);
    for(float alpha:{0.f,.125f,.5f,.875f,1.f}) {
        skin_memory=original_skin_memory;skin_completions=0;skin_arguments=false;
        ReplaySkin(skin_context,base,skin_modifier,skin_pose,skin_scratch,alpha,fake_update,fake_complete);
        const auto reference_memory=skin_memory;const auto reference_context=skin_context;const auto reference_csr=_mm_getcsr();
        skin_memory=original_skin_memory;skin_completions=0;skin_arguments=false;
        ReplaySkin(skin_context,base,skin_modifier,*replay_snapshot,skin_scratch,alpha,fake_update,fake_complete);
        check(skin_memory==reference_memory && !std::memcmp(&skin_context,&reference_context,sizeof(skin_context)) &&
              _mm_getcsr()==reference_csr && skin_completions==2 && skin_arguments,
              "immutable snapshot replay matches full native output/input memory, context, MXCSR and callbacks");
    }
    REXCVAR_SET(em2_cache_pose_interpolation,saved_pose_cache);
    {
        struct RestoreTraceTest {
            bool enabled=REXCVAR_GET(em2_trace_render);uint64_t epoch=skin_replay_epoch.load(),serial=skin_replay_serial.load();uint32_t input=skin_replay_input.load();
            ~RestoreTraceTest(){REXCVAR_SET(em2_trace_render,enabled);skin_replay_input.store(input);skin_replay_epoch.store(epoch);skin_replay_serial.store(serial);}
        }restore_trace_test;
        REXCVAR_SET(em2_trace_render,true);
        static unsigned failure_mode=0;
        auto fault_update=+[](PPCContext& ctx,uint8_t* base) {
            const auto modifier=ctx.r3.u32;
            PPC_STORE_U32(modifier+108,0xD00D1234);PPC_STORE_U32(modifier+8444,0xF00D1234);
            PPC_STORE_U32(modifier+8436,999);PPC_STORE_U8(modifier+8440,1);
            ctx.r31.u64=0xBAD;_mm_setcsr((_mm_getcsr()&~_MM_ROUND_MASK)|_MM_ROUND_DOWN);
            if(failure_mode==1 || failure_mode==3)throw std::runtime_error("skin submit failure");
        };
        auto fault_complete=+[](PPCContext& ctx,uint8_t*) {
            ++skin_completions;ctx.r31.u64=0xBAD;
            _mm_setcsr((_mm_getcsr()&~_MM_ROUND_MASK)|_MM_ROUND_UP);
            if((failure_mode==0 && skin_completions==1) || (failure_mode>=2 && skin_completions==2))throw std::runtime_error("skin drain failure");
        };
        const auto initial_context=skin_context;
        for(failure_mode=0;failure_mode<4;++failure_mode) {
            skin_memory=original_skin_memory;skin_context=initial_context;skin_completions=0;
            skin_replay_epoch.store(0);skin_replay_input.store(333);skin_replay_serial.store(10);
            _mm_setcsr(original_mxcsr);bool caught=false;
            try{ReplaySkin(skin_context,base,skin_modifier,skin_pose,skin_scratch,.25f,fault_update,fault_complete);}
            catch(const std::runtime_error& error){caught=std::string_view(error.what())==((failure_mode==1 || failure_mode==3)?"skin submit failure":"skin drain failure");}
            const bool safe_to_restore=failure_mode<2;
            check(caught && skin_completions==(failure_mode?2:1) &&
                  !std::memcmp(&skin_context,&initial_context,sizeof(skin_context)) && _mm_getcsr()==original_mxcsr &&
                  PPC_LOAD_U32(skin_modifier+56)==skin_source && skin_replay_epoch.load()==0 && skin_replay_input.load()==333 &&
                  skin_replay_serial.load()==(failure_mode?11:10),
                  "failed skin job restores context/source/FP/trace and preserves the primary exception without repeating a failed completion");
            if(safe_to_restore)check(
                !std::memcmp(base+skin_modifier+104,original_skin_memory.data()+skin_modifier+104,8216) &&
                !std::memcmp(base+skin_modifier+8444,original_skin_memory.data()+skin_modifier+8444,16) &&
                PPC_LOAD_U32(skin_modifier+8436)==123 && PPC_LOAD_U8(skin_modifier+8440)==0,
                "initial drain failure leaves input intact; failed submit rewinds bookkeeping only after confirmed drain");
            else check(PPC_LOAD_U32(skin_modifier+108)==0xD00D1234 && PPC_LOAD_U32(skin_modifier+8444)==0xF00D1234 &&
                       PPC_LOAD_U32(skin_modifier+8436)==999 && PPC_LOAD_U8(skin_modifier+8440)==1,
                       "unconfirmed completion retains live job input and bookkeeping instead of racing its worker");
        }
        skin_memory=original_skin_memory;skin_context=initial_context;_mm_setcsr(original_mxcsr);skin_completions=0;
        auto rebind_complete=+[](PPCContext& ctx,uint8_t* base) {
            if(++skin_completions==2)PPC_STORE_U32(ctx.r3.u32+56,skin_source+64);
        };
        ReplaySkin(skin_context,base,skin_modifier,skin_pose,skin_scratch,.25f,fake_update,rebind_complete);
        check(skin_completions==2 && PPC_LOAD_U32(skin_modifier+56)==skin_source+64 &&
              !std::memcmp(&skin_context,&initial_context,sizeof(skin_context)) && _mm_getcsr()==original_mxcsr,
              "source cleanup disarms before native completion and preserves a completion callback's new canonical binding");
        skin_memory=original_skin_memory;skin_context=initial_context;_mm_setcsr(original_mxcsr);
        REXLOG_INFO("Skin replay unwind passed four injected job failures: initial drain, submit with successful drain, final drain, and submit plus failed drain; context/source/FP/trace restored, safe input restoration and primary exception preserved; no native worker fault forced");
    }
    const auto saved_changes=skin_render_output_changes.load();
    { std::lock_guard lock(skin_output_mutex); skin_output_samples.clear(); }
    SampleSkinOutput(50000,1,12,42,1,base);
    PPC_STORE_U32(50000,std::bit_cast<uint32_t>(5.f));
    SampleSkinOutput(50000,1,12,42,2,base);
    check(skin_render_output_changes.load()==saved_changes+1,
          "skin output diagnostic detects geometry changing between renders of one simulation tick");
    SampleSkinOutput(50000,1,12,42,3,base);
    PPC_STORE_U32(50000,std::bit_cast<uint32_t>(6.f));
    SampleSkinOutput(50000,1,12,43,4,base);
    check(skin_render_output_changes.load()==saved_changes+1,
          "skin output diagnostic excludes identical output and changes across simulation ticks");
    { std::lock_guard lock(skin_output_mutex); skin_output_samples.clear(); }
    skin_render_output_changes.store(saved_changes);
    // Exercise actual pose capture across ticks, repeat updates, culling gaps
    // and palette replacement. A stale worker may not overwrite a newer pose.
    const auto capture_epoch=recording_epoch.load();
    PPC_STORE_U32(skin_modifier+4,1);
    auto put_bone=[&](uint32_t source,float x) {
        auto pose=bone;pose[12]=x;
        for(unsigned i=0;i<16;++i)PPC_STORE_U32(source+i*4,std::bit_cast<uint32_t>(pose[i]));
    };
    recording_epoch.store(100);put_bone(skin_source,2);
    CaptureSkinPose(skin_modifier,40000,skin_source,41000,1,100,base);
    const auto first_snapshot=SnapshotSkin(skin_history.at(skin_modifier));
    check(first_snapshot==SnapshotSkin(skin_history.at(skin_modifier)),"unchanged capture reuses the same immutable skin snapshot");
    recording_epoch.store(101);put_bone(skin_source,10);
    CaptureSkinPose(skin_modifier,40000,skin_source,41000,1,101,base);
    const auto second_snapshot=SnapshotSkin(skin_history.at(skin_modifier));
    check(second_snapshot!=first_snapshot && first_snapshot->current[0][12]==2 && second_snapshot->current[0][12]==10,
          "new capture publishes fresh data while an earlier render keeps an immutable pose");
    put_bone(skin_source,12);CaptureSkinPose(skin_modifier,40000,skin_source,41000,1,101,base);
    const auto repeated_snapshot=SnapshotSkin(skin_history.at(skin_modifier));
    check(repeated_snapshot!=second_snapshot && second_snapshot->current[0][12]==10 && repeated_snapshot->current[0][12]==12 && repeated_snapshot->previous[0][12]==2,
          "same-tick recapture invalidates the snapshot without rewriting readers or previous pose");
    check(skin_history.at(skin_modifier).previous[0][12]==2 && skin_history.at(skin_modifier).current[0][12]==12,
          "repeated skin updates preserve the preceding simulation pose for smooth presentation");
    recording_epoch.store(103);put_bone(skin_source,20);
    CaptureSkinPose(skin_modifier,40000,skin_source,41000,1,103,base);
    check(skin_history.at(skin_modifier).previous==skin_history.at(skin_modifier).current,
          "skipped skin updates reset stale interpolation history");
    recording_epoch.store(104);put_bone(skin_scratch,30);
    CaptureSkinPose(skin_modifier,40000,skin_scratch,41000,1,104,base);
    CaptureSkinPose(skin_modifier,40000,skin_source,41000,1,103,base);
    check(skin_history.at(skin_modifier).source==skin_scratch && skin_history.at(skin_modifier).updated==104 &&
          skin_history.at(skin_modifier).previous[0][12]==30 && skin_history.at(skin_modifier).current[0][12]==30,
          "palette changes reset bone history and late workers cannot replace the current pose");
    skin_history.erase(skin_modifier);ChangeReference(skin_modifier,-1,base);recording_epoch.store(capture_epoch);
    base=saved_base;
    if(ok) {BenchmarkSkinSnapshots();BenchmarkPoseInterpolation();BenchmarkSceneInterpolation();BenchmarkSceneRenderEntries();REXLOG_INFO("Fixed simulation scheduler and transform interpolation tests passed");}
    return ok;
}
}
