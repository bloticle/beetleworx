#include "ppc_config.h"
#include "physics_timing.h"
#include "character_timing.h"
#include "pc_settings.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <immintrin.h>
#include <limits>
#include <string>
#include <utility>
#include <windows.h>

PPC_EXTERN_FUNC(__imp__sub_8246AD50);
PPC_EXTERN_FUNC(__imp__sub_82467468);
PPC_EXTERN_FUNC(__imp__sub_82468610);
PPC_EXTERN_FUNC(__imp__sub_823F92A0);
PPC_EXTERN_FUNC(__imp__sub_823F9478);
PPC_EXTERN_FUNC(__imp__sub_82489860);
PPC_EXTERN_FUNC(__imp__sub_827ADCC8);
PPC_EXTERN_FUNC(sub_82489860);
REXCVAR_DEFINE_BOOL(em2_trace_physics,false,"Debug","Summarize actual native world-step intervals every five seconds");
REXCVAR_DEFINE_BOOL(em2_native_physics_60hz,false,"Gameplay","Experimental native 60 Hz physics scheduler for offline Native delta above 30 FPS; requires gameplay validation");
REXCVAR_DEFINE_BOOL(em2_native_physics_variable,true,"Gameplay","Use actual-delta native physics in experimental offline Native delta mode above 30 FPS; preserves authored 30-FPS and online scheduling");
REXCVAR_DECLARE(bool, em2_trace_motion);
namespace {
thread_local void (*test_step)(PPCContext&,uint8_t*)=nullptr;
std::atomic<bool> native_timing_diagnostics{false};
bool TracePhysics() {
    return REXCVAR_GET(em2_trace_physics) || native_timing_diagnostics.load(std::memory_order_relaxed);
}
bool NativePhysics60Policy(bool candidate,bool native_variable,int cap) {
    return candidate && native_variable && (cap==0 || cap>30);
}
uint32_t NativePhysicsStepPolicy(bool variable,bool fixed60,bool native,int cap,uint32_t delta_bits) {
    if(!NativePhysics60Policy(variable || fixed60,native,cap))return 0;
    if(!variable)return 0x3C888889;
    // Positive finite IEEE floats sort in the same order as their bit patterns.
    // Do not change the guest FP environment merely to select a timestep.
    if(!delta_bits || delta_bits>=0x7F800000)return 0;
    return std::min(delta_bits,0x3D088889u); // Keep the authored maximum step.
}
template<class Call>
void WithNativePhysicsStep(PPCContext& ctx,uint8_t* base,uint32_t requested_step,Call&& original) {
    constexpr uint32_t authored_step=0x3D088889;
    const auto manager=ctx.r3.u32;
    if(!requested_step || requested_step==authored_step || !PPC_LOAD_U32(manager+4) || PPC_LOAD_U32(manager+72)!=authored_step) {
        original(ctx,base);return;
    }
    // Configure the real game's accumulator before its complete native
    // integrate/collide/advance sequence. Do not change individual Havok dt
    // arguments after scheduling or interpolate an already-updated skeleton.
    PPC_STORE_U32(manager+72,requested_step);
    struct RestoreStep {
        uint8_t* base;uint32_t manager;
        ~RestoreStep(){PPC_STORE_U32(manager+72,0x3D088889);}
    } restore{base,manager};
    original(ctx,base);
}
template<class Call>
void WithNativePhysics60(PPCContext& ctx,uint8_t* base,bool enabled,Call&& original) {
    WithNativePhysicsStep(ctx,base,enabled?0x3C888889:0,original);
}
void ObserveNativeBodyRoots(uint32_t manager,uint32_t step_bits,uint8_t* base) noexcept {
    struct RestoreMath {unsigned csr=_mm_getcsr();~RestoreMath(){_mm_setcsr(csr);}} restore;
    try {
        using Clock=std::chrono::steady_clock;
        struct Sample {
            uint32_t manager=0,body=0,data=0,root=0,frame=0;
            uint64_t body_hash=0,root_hash=0,samples=0,comparisons=0,body_changes=0,root_changes=0,skipped=0;
            std::array<uint64_t,4> steps{},body_changes_by_step{};
            Clock::time_point started=Clock::now(),seen=started;bool initialized=false;
        };
        static thread_local std::array<Sample,32> histories;
        if(!PPC_LOAD_U32(manager+4))return; // Match the native no-world bypass during attachment/teardown.
        const auto count=PPC_LOAD_U32(manager+28),bodies=PPC_LOAD_U32(manager+24);
        const auto clock=PPC_LOAD_U32(0x833EFA38);
        if(!bodies || !clock || !count || count>1048576)return;
        const auto frame=PPC_LOAD_U32(clock+80);
        const double step=std::bit_cast<float>(step_bits),advanced=std::bit_cast<float>(PPC_LOAD_U32(manager+80));
        const double residual=std::bit_cast<float>(PPC_LOAD_U32(manager+76));
        if(!std::isfinite(step) || step<=0 || step>.25 || !std::isfinite(advanced) || advanced<0 || advanced>1)return;
        const unsigned steps=advanced>=step*2.5?3u:unsigned(advanced/step+.5);
        const auto now=Clock::now();
        auto hash=[&](uint32_t matrix,bool ni_scale) {
            uint64_t value=1469598103934665603ull;
            for(unsigned i:{0u,1u,2u,4u,5u,6u,8u,9u,10u,12u,13u,14u,15u}) {
                if(i==15 && !ni_scale)continue; // HK transform W is padding, Ni transform W is scale.
                value^=PPC_LOAD_U32(matrix+i*4);value*=1099511628211ull;
            }
            return value;
        };
        // The native scheduler already validated/iterated this live array and
        // these body user-data fields. Bound reads to1024 bodies and retain
        // only32 value-only histories. Cached guest addresses are compared,
        // never dereferenced without re-observing them in this live array.
        for(unsigned i=0;i<std::min(count,1024u);++i) {
            const auto body=PPC_LOAD_U32(bodies+i*4);if(!body || PPC_LOAD_U8(body+232)==5)continue;
            const auto data=PPC_LOAD_U32(body+12);if(!data || !PPC_LOAD_U32(data) || !PPC_LOAD_U8(data+12))continue;
            const auto root=PPC_LOAD_U32(data+4);
            auto slot=std::find_if(histories.begin(),histories.end(),[&](const auto& h){return h.manager==manager && h.body==body;});
            if(slot==histories.end())slot=std::find_if(histories.begin(),histories.end(),[](const auto& h){return !h.body;});
            if(slot==histories.end()) {
                slot=std::min_element(histories.begin(),histories.end(),[](const auto& a,const auto& b){return a.seen<b.seen;});
                if(now-slot->seen<std::chrono::seconds(2))continue;
            }
            auto& h=*slot;
            if(h.manager!=manager || h.body!=body || h.data!=data || h.root!=root || now-h.seen>std::chrono::seconds(2) ||
               (h.initialized && uint32_t(frame-h.frame)>1000000u)) {
                h={};h.manager=manager;h.body=body;h.data=data;h.root=root;
            }
            if(h.initialized && h.frame==frame)continue;
            const auto body_hash=hash(body+240,false),root_hash=root?hash(root+144,true):0;
            if(h.initialized) {
                const uint32_t gap=frame-h.frame;
                if(gap && gap<=1000000) {
                    h.skipped+=gap-1;++h.comparisons;
                    h.body_changes+=body_hash!=h.body_hash;h.root_changes+=root_hash!=h.root_hash;
                    h.body_changes_by_step[steps]+=body_hash!=h.body_hash;
                }
            }
            h.initialized=true;h.frame=frame;h.body_hash=body_hash;h.root_hash=root_hash;h.seen=now;++h.samples;++h.steps[steps];
            const double elapsed=std::chrono::duration<double>(now-h.started).count();
            if(elapsed<5)continue;
            REXLOG_INFO("Native body root cadence manager {:08X} body {:08X} entity {:08X} NiAV root {:08X}: {} samples / {:.3f}s, {} comparisons, body/root changes {}/{}, scheduler0/1/2/3+ step frames {}/{}/{}/{}, body changes by0/1/2/3+ steps {}/{}/{}/{}, skipped engine frames {}, step {:.6f}, residual {:.6f}, body list {} (CPU physics/root transforms only; stationary bodies may legitimately repeat; no render interpolation applied)",
                manager,body,PPC_LOAD_U32(data),root,h.samples,elapsed,h.comparisons,h.body_changes,h.root_changes,
                h.steps[0],h.steps[1],h.steps[2],h.steps[3],h.body_changes_by_step[0],h.body_changes_by_step[1],h.body_changes_by_step[2],h.body_changes_by_step[3],
                h.skipped,step,residual,count);
            h.samples=h.comparisons=h.body_changes=h.root_changes=h.skipped=0;h.steps={};h.body_changes_by_step={};h.started=now;
        }
    }catch(...){}
}
struct TimingSample {
    using Clock=std::chrono::steady_clock;
    Clock::time_point started=Clock::now();uint32_t identity=0;
    std::array<uint64_t,5> outcomes{};
    double first_time=std::numeric_limits<double>::infinity(),last_time=-std::numeric_limits<double>::infinity();
    uint64_t calls=0,units=0,invalid=0;double total=0,min=std::numeric_limits<double>::infinity(),max=0;
    double Add(double dt,uint32_t count) {
        ++calls;units+=count;
        if(std::isfinite(dt)){total+=dt;min=std::min(min,dt);max=std::max(max,dt);}else ++invalid;
        return std::chrono::duration<double>(Clock::now()-started).count();
    }
};
void ObserveWorld(uint32_t world,double dt,uint32_t result) {
    const auto csr=_mm_getcsr();
    // Separate worlds; the last slot is explicitly labeled overflow rather
    // than presenting sums from different worlds as one simulated clock.
    static thread_local std::array<TimingSample,9> samples;
    auto slot=std::find_if(samples.begin(),samples.end()-1,[&](const auto& sample){return sample.identity==world;});
    if(slot==samples.end()-1)slot=std::find_if(samples.begin(),samples.end()-1,[](const auto& sample){return !sample.identity;});
    const auto identity=slot==samples.end()-1?UINT32_MAX:world;
    if(slot->identity!=identity){*slot={};slot->identity=identity;}
    auto& sample=*slot;++sample.outcomes[std::min(result,4u)];const auto elapsed=sample.Add(dt,1);
    if(elapsed>=5) {
        REXLOG_INFO("Native physics dispatch world {:08X}: {} calls / {:.3f}s, dt {:.6f}..{:.6f}, mean {:.6f}, invalid {}, results success/integration-retry/collision-retry/advance-retry/other {}/{}/{}/{}/{} (completed calls, not proof every entry advanced the world; FFFFFFFF pools overflow worlds)",
            sample.identity,sample.calls,elapsed,sample.min,sample.max,sample.total/double(sample.calls-sample.invalid?sample.calls-sample.invalid:1),sample.invalid,
            sample.outcomes[0],sample.outcomes[1],sample.outcomes[2],sample.outcomes[3],sample.outcomes[4]);
        const auto identity=sample.identity;sample={};sample.identity=identity;
    }
    _mm_setcsr(csr);
}
void ObserveIntegrationCallers(const PPCContext& ctx,uint8_t* base) noexcept {
    struct RestoreMath {unsigned csr=_mm_getcsr();~RestoreMath(){_mm_setcsr(csr);}} restore;
    try {
        static thread_local auto last=std::chrono::steady_clock::time_point{};
        const auto now=std::chrono::steady_clock::now();
        if(now-last<std::chrono::seconds(5))return;
        last=now;
        std::string callers;auto sp=ctx.r1.u32;
        for(unsigned depth=0;depth<14 && sp;++depth) {
            MEMORY_BASIC_INFORMATION memory{};
            if(!VirtualQuery(PPC_RAW_ADDR(sp),&memory,sizeof(memory)) || memory.State!=MEM_COMMIT || (memory.Protect&(PAGE_NOACCESS|PAGE_GUARD)))break;
            const auto next=PPC_LOAD_U32(sp);
            if(next<=sp || next-sp>4*1024*1024)break;
            if(!VirtualQuery(PPC_RAW_ADDR(next-8),&memory,sizeof(memory)) || memory.State!=MEM_COMMIT || (memory.Protect&(PAGE_NOACCESS|PAGE_GUARD)))break;
            callers+=fmt::format(" {:08X}",PPC_LOAD_U32(next-8));sp=next;
        }
        REXLOG_INFO("Native physics integration callers: LR {:08X}, step header {:08X}, bodies {}:{} (read-only PPC return chain; identifies the actual stepping interface)",
            uint32_t(ctx.lr),ctx.r4.u32,ctx.r7.u32,callers);
    }catch(...){}
}
void ObserveIntegration(PPCContext& ctx,uint8_t* base) {
    if(ctx.r7.s32<=0 || !ctx.r4.u32)return;
    ObserveIntegrationCallers(ctx,base);
    const auto csr=_mm_getcsr();
    static thread_local TimingSample sample;
    const auto dt=std::bit_cast<float>(PPC_LOAD_U32(ctx.r4.u32+8));
    const auto start=std::bit_cast<float>(PPC_LOAD_U32(ctx.r4.u32));
    if(std::isfinite(start)){sample.first_time=std::min(sample.first_time,double(start));sample.last_time=std::max(sample.last_time,double(start));}
    const auto elapsed=sample.Add(dt,ctx.r7.u32);
    if(elapsed>=5) {
        REXLOG_INFO("Native physics integration: {} batches, {} body entries / {:.3f}s, dt {:.6f}..{:.6f}, mean {:.6f}, invalid {}, step start {:.6f}..{:.6f} (thread-local batches, not whole-world tick frequency)",
            sample.calls,sample.units,elapsed,sample.min,sample.max,sample.total/double(sample.calls-sample.invalid?sample.calls-sample.invalid:1),sample.invalid,sample.first_time,sample.last_time);
        sample={};
    }
    _mm_setcsr(csr);
}
}
bool em2::NativeTimingDiagnosticsEnabled(){return native_timing_diagnostics.load(std::memory_order_relaxed);}
void em2::SetNativeTimingPhysicsDiagnostics(bool enabled) {native_timing_diagnostics.store(enabled,std::memory_order_relaxed);}
PPC_FUNC_IMPL(sub_827ADCC8) {
    // The outer simulation owner publishes native-variable mode only when
    // neither agreed online timing nor compatibility/time scaling owns ticks.
    const bool native=em2::NativeTimingDiagnosticsEnabled();
    const bool variable=REXCVAR_GET(em2_native_physics_variable);
    const auto requested_step=NativePhysicsStepPolicy(variable,REXCVAR_GET(em2_native_physics_60hz),native,
        em2::FrameLimit(),variable && native?PPC_LOAD_U32(0x833FCA70):0);
    const bool trace=native && REXCVAR_GET(em2_trace_motion);
    const auto manager=ctx.r3.u32;
    const uint32_t step=trace?PPC_LOAD_U32(manager+72):0;
    WithNativePhysicsStep(ctx,base,requested_step,__imp__sub_827ADCC8);
    if(trace)ObserveNativeBodyRoots(manager,requested_step && step==0x3D088889?requested_step:step,base);
}
PPC_FUNC_IMPL(sub_8246AD50) {
    if(test_step){test_step(ctx,base);return;}
    if(!TracePhysics()){__imp__sub_8246AD50(ctx,base);return;}
    const auto world=ctx.r3.u32;const auto dt=ctx.f1.f64;
    __imp__sub_8246AD50(ctx,base);
    // An entry may resume a failed stage rather than complete a world step.
    // Inspect the returned code, without reading a potentially retired world.
    ObserveWorld(world,dt,ctx.r3.u32);
}
PPC_FUNC_IMPL(sub_82489860) {
    if(TracePhysics())ObserveIntegration(ctx,base);
    __imp__sub_82489860(ctx,base);
}
namespace {
struct IntegrationHeaderCaptured {};
struct NativeSchedulerProbe {
    std::array<unsigned,3> phases{};unsigned sequence=0;float step=0;double integrated_seconds=0;bool valid=true;
};
thread_local NativeSchedulerProbe scheduler_probe;
void NativeSchedulerPhaseFixture(PPCContext& ctx,uint8_t*) {
    const unsigned phase=ctx.lr==0x8246AE98?0:ctx.lr==0x8246AF0C?1:ctx.lr==0x8246AF7C?2:3;
    if(phase>=3){scheduler_probe.valid=false;return;}
    scheduler_probe.valid&=phase==scheduler_probe.sequence++%3;
    ++scheduler_probe.phases[phase];
    if(!phase){scheduler_probe.valid&=ctx.f1.f64==double(scheduler_probe.step);scheduler_probe.integrated_seconds+=ctx.f1.f64;}
    ctx.r3.u64=0;ctx.f1.f64=123; // The scheduler must reload the next timestep.
}
bool TestNativePhysicsScheduler(uint8_t* base) {
    if(!VirtualAlloc(base+0x833D0000,0x30000,MEM_COMMIT,PAGE_READWRITE))return false;
    constexpr uint32_t manager=0x2000,task=0x3000,simulation=0x4000,vtable=0x5000,world=0x6000;
    for(unsigned phase=0;phase<3;++phase)PPC_LOOKUP_FUNC(base,uint32_t(PPC_CODE_BASE)+24+phase*4)=NativeSchedulerPhaseFixture;
    auto put=[&](uint32_t address,float value){PPC_STORE_U32(address,std::bit_cast<uint32_t>(value));};
    auto get=[&](uint32_t address){return std::bit_cast<float>(PPC_LOAD_U32(address));};
    auto reset=[&](float step,float remainder) {
        std::memset(base,0,0x10000);
        PPC_STORE_U32(manager+4,world);put(manager+72,step);put(manager+76,remainder);
        PPC_STORE_U32(world+8,simulation);PPC_STORE_U32(simulation,vtable);
        for(unsigned phase=0;phase<3;++phase)PPC_STORE_U32(vtable+12+phase*4,uint32_t(PPC_CODE_BASE)+24+phase*4);
        PPC_STORE_U32(0x833C47C8,0x9000);PPC_STORE_U32(0x900C,1);
        PPC_STORE_U32(0xB000,0xA000);PPC_STORE_U32(0xA004,0x23DF4554);
        PPC_STORE_U32(0x833DB68C,0xA100); // Native optional post-update switch.
        scheduler_probe={};scheduler_probe.step=step;
    };
    auto arguments=[&] {
        PPCContext c{};c.r1.u64=0xF000;c.r3.u64=manager;c.r4.u64=task;c.r13.u64=0xB000;c.fpscr.InitHost();return c;
    };
    unsigned checks=0;
    for(int cap:{0,30,40,60,90,120,240})for(bool candidate:{false,true})for(bool native:{false,true}) {
        if(NativePhysics60Policy(candidate,native,cap)!=(candidate && native && cap!=30))return false;
        ++checks;
    }
    for(const auto [input,expected]:std::array<std::pair<uint32_t,uint32_t>,9>{{
        {0,0},{0x80000000,0},{0xBF800000,0},{0x7F800000,0},{0xFF800000,0},{0x7FC00000,0},
        {0x3CA3D70A,0x3CA3D70A},{0x3DCCCCCD,0x3D088889},{0x3A83126F,0x3A83126F}}}) {
        if(NativePhysicsStepPolicy(true,true,true,60,input)!=expected ||
           NativePhysicsStepPolicy(true,true,false,60,input)!=0 ||
           NativePhysicsStepPolicy(true,true,true,30,input)!=0 ||
           NativePhysicsStepPolicy(false,true,true,60,input)!=0x3C888889 ||
           NativePhysicsStepPolicy(false,false,true,60,input)!=0) {
            REXLOG_ERROR("Native variable physics guard/precedence mismatch: delta {:08X}",input);return false;
        }
        ++checks;
    }
    std::array<uint8_t,0x10000> initial{},expected{};
    for(bool enabled:{false,true})for(float remainder:{0.f,1.f/240})for(float dt:{0.f,1.f/120,1.f/60,1.f/30,.1f}) {
        const float step=enabled?1.f/60:1.f/30;
        reset(step,remainder);put(0x833FCA70,dt);
        const auto entry=arguments();auto context=entry;
        std::memcpy(initial.data(),base,initial.size());const auto initial_csr=_mm_getcsr();
        __imp__sub_827ADCC8(context,base);
        const auto native_context=context;const auto native_csr=_mm_getcsr();const auto native_probe=scheduler_probe;
        // Expected output differs only in restoring the authored configuration.
        put(manager+72,1.f/30);std::memcpy(expected.data(),base,expected.size());
        std::memcpy(base,initial.data(),initial.size());put(manager+72,1.f/30);
        scheduler_probe={};scheduler_probe.step=step;context=entry;_mm_setcsr(initial_csr);
        WithNativePhysics60(context,base,enabled,__imp__sub_827ADCC8);
        if(std::memcmp(&context,&native_context,sizeof(context)) || std::memcmp(base,expected.data(),expected.size()) ||
           _mm_getcsr()!=native_csr || !scheduler_probe.valid || scheduler_probe.phases!=native_probe.phases ||
           get(manager+76)<0 || get(manager+76)>=step || get(manager+72)!=1.f/30) {
            REXLOG_ERROR("Native scheduler configuration mismatch: enabled {}, remainder {}, incoming delta {}",enabled,remainder,dt);return false;
        }
        ++checks;
    }
    for(unsigned render_hz:{30u,40u,60u,90u,120u,240u})for(unsigned physics_hz:{30u,60u}) {
        const float step=1.f/physics_hz;reset(1.f/30,0);scheduler_probe.step=step;
        put(0x833FCA70,1.f/render_hz);
        for(unsigned frame=0;frame<render_hz*10;++frame) {
            auto context=arguments();WithNativePhysics60(context,base,physics_hz==60,__imp__sub_827ADCC8);
        }
        const auto count=scheduler_probe.phases[0];
        if(!scheduler_probe.valid || scheduler_probe.phases!=std::array<unsigned,3>{count,count,count} ||
           std::abs(int(count)-int(physics_hz*10))>1 || get(manager+76)<0 || get(manager+76)>=step) {
            REXLOG_ERROR("Native scheduler cadence mismatch: {} render Hz, {} physics Hz, steps {}, residual {}",render_hz,physics_hz,count,get(manager+76));return false;
        }
        ++checks;
    }
    // Execute the actual scheduler under an uneven frame clock. The variable
    // candidate must deliver one complete native phase sequence per frame,
    // rather than merely changing the reported FPS or resampling old poses.
    constexpr std::array<float,4> intervals={.7f,1.3f,.85f,1.15f};
    for(unsigned hz:{40u,50u,90u,120u}) {
        const unsigned frames=hz*8;reset(1.f/30,0);double elapsed=0;
        for(unsigned frame=0;frame<frames;++frame) {
            const float dt=intervals[frame%intervals.size()]/hz;
            const auto bits=std::bit_cast<uint32_t>(dt);put(0x833FCA70,dt);elapsed+=dt;scheduler_probe.step=dt;
            const auto request=NativePhysicsStepPolicy(true,true,true,int(hz),bits);
            const auto before=scheduler_probe.phases;auto context=arguments();
            WithNativePhysicsStep(context,base,request,__imp__sub_827ADCC8);
            if(!scheduler_probe.valid || scheduler_probe.phases!=std::array<unsigned,3>{before[0]+1,before[1]+1,before[2]+1} ||
               get(manager+76)!=0 || get(manager+72)!=1.f/30) {
                REXLOG_ERROR("Native variable scheduler uneven-frame delivery mismatch: {} Hz frame {}, dt {}",hz,frame,dt);return false;
            }
        }
        if(std::abs(scheduler_probe.integrated_seconds-elapsed)>1e-8)return false;
        // Same frame intervals, unmodified authored30 scheduler: demonstrate
        // the control really has held frames, so the test detects cadence.
        reset(1.f/30,0);unsigned held=0;
        for(unsigned frame=0;frame<frames;++frame) {
            put(0x833FCA70,intervals[frame%intervals.size()]/hz);
            const auto before=scheduler_probe.phases[0];auto context=arguments();__imp__sub_827ADCC8(context,base);
            held+=before==scheduler_probe.phases[0];
        }
        if(!scheduler_probe.valid || !held || scheduler_probe.phases[0]>=frames) {
            REXLOG_ERROR("Native variable scheduler control did not expose authored30 held frames at {} Hz",hz);return false;
        }
        ++checks;
    }
    // Transition debt remains time, rather than being silently discarded when
    // switching from an existing fixed-step world to actual-delta physics.
    for(unsigned hz:{40u,50u,90u,120u}) {
        const float carry=1.f/40;reset(1.f/30,carry);double elapsed=carry;
        for(unsigned frame=0;frame<80;++frame) {
            const float dt=intervals[frame%intervals.size()]/hz;put(0x833FCA70,dt);elapsed+=dt;scheduler_probe.step=dt;
            auto context=arguments();WithNativePhysicsStep(context,base,std::bit_cast<uint32_t>(dt),__imp__sub_827ADCC8);
            if(!scheduler_probe.valid || get(manager+76)<0 || get(manager+76)>=dt || get(manager+72)!=1.f/30)return false;
        }
        if(std::abs(scheduler_probe.integrated_seconds+get(manager+76)-elapsed)>2e-6)return false;
        ++checks;
    }
    reset(1.f/120,0);put(0x833FCA70,1.f/60);auto custom=arguments();
    WithNativePhysicsStep(custom,base,std::bit_cast<uint32_t>(1.f/90),__imp__sub_827ADCC8);
    if(!scheduler_probe.valid || scheduler_probe.phases!=std::array<unsigned,3>{2,2,2} || get(manager+72)!=1.f/120)return false;
    ++checks;
    reset(1.f/30,0);
    try {
        auto context=arguments();WithNativePhysics60(context,base,true,[](PPCContext&,uint8_t*){throw IntegrationHeaderCaptured{};});
        return false;
    }catch(const IntegrationHeaderCaptured&) {
        if(get(manager+72)!=1.f/30)return false;
    }
    ++checks;
    bool saw_variable=false;
    try {
        auto context=arguments();WithNativePhysicsStep(context,base,std::bit_cast<uint32_t>(1.f/90),[&](PPCContext&,uint8_t* base){
            saw_variable=PPC_LOAD_U32(manager+72)==std::bit_cast<uint32_t>(1.f/90);throw IntegrationHeaderCaptured{};
        });return false;
    }catch(const IntegrationHeaderCaptured&) {
        if(!saw_variable || get(manager+72)!=1.f/30)return false;
    }
    ++checks;
    REXLOG_INFO("Native scheduler passed {} checks: original827ADCC8 + original world lock/phase interfaces, 30/60 Hz cadence across30..240 render Hz, actual-delta irregular40/50/90/120 Hz one-step delivery and exact admitted-time sum, authored30 held-frame controls, retained transition debt, finite/positive/cap/ownership/precedence guards, byte-exact fixed-step context/memory/MXCSR parity, custom-step preservation and configuration restoration on exception. Solver phases are fixtures; contacts/gameplay require live validation",checks);
    return true;
}
PPC_FUNC_IMPL(EM2CaptureIntegrationHeader) {throw IntegrationHeaderCaptured{};}
bool TestNativeIntegrationHeader(uint8_t* base) {
    constexpr uint32_t simulation=0x4000,world=0x6000,solver=0x7000,callback_object=0x8000,vtable=0x9000;
    PPC_LOOKUP_FUNC(base,uint32_t(PPC_CODE_BASE)+20)=EM2CaptureIntegrationHeader;
    auto put=[&](uint32_t address,float value){PPC_STORE_U32(address,std::bit_cast<uint32_t>(value));};
    auto get=[&](uint32_t address){return std::bit_cast<float>(PPC_LOAD_U32(address));};
    unsigned checks=0;double early_error=0,long_error=0;unsigned lost=0;
    for(float start:{0.f,16.f,128.f,1024.f,8192.f,32768.f,262144.f})for(unsigned hz:{30u,40u,60u,90u,120u,240u}) {
        std::memset(base,0,0x10000);
        PPC_STORE_U32(simulation+12,world);put(simulation+24,start);
        PPC_STORE_U32(world+120,solver);PPC_STORE_U32(world+76,callback_object);
        PPC_STORE_U32(callback_object,vtable);PPC_STORE_U32(vtable+12,uint32_t(PPC_CODE_BASE)+20);
        PPC_STORE_U32(0xB000,0xA000);PPC_STORE_U32(0xA00C,0xC000);
        const float dt=1.f/hz;
        PPCContext context{};context.r1.u64=0xF000;context.r3.u64=simulation;context.r13.u64=0xB000;
        context.f1.f64=dt;context.fpscr.InitHost();
        bool captured=false,cleaned=false;
        try {
            struct Cleanup {bool& cleaned;~Cleanup(){cleaned=true;}}cleanup{cleaned};
            __imp__sub_82468610(context,base);
        }catch(const IntegrationHeaderCaptured&){captured=true;}
        // Stop at the native notification callback after both header copies,
        // before solving. No world or contact result is fabricated as success.
        if(!captured || !cleaned || get(world+480)!=start || get(simulation+28)!=dt ||
           std::memcmp(base+world+480,base+solver+80,16)) {
            REXLOG_ERROR("Native integration header copy/capture mismatch: time {}, {}Hz",start,hz);return false;
        }
        const float end=get(world+484),actual=get(world+488),inverse=get(world+492);
        const auto error=std::abs(double(actual)/dt-1);
        if(start<=128)early_error=std::max(early_error,error);else long_error=std::max(long_error,error);
        lost+=actual==0;
        if(end!=float(double(start)+dt) || actual!=float(double(end)-start) ||
           inverse!=(actual==0?0.f:float(1./actual))) {
            REXLOG_ERROR("Native integration header arithmetic mismatch: time {}, {}Hz, dt {}, inverse {}",start,hz,actual,inverse);return false;
        }
        if(hz==120)REXLOG_INFO("Native integration header sample: time {}, requested {:.9f}, actual {:.9f}, relative error {:.6f}%, no contact solve",start,dt,actual,100*error);
        ++checks;
    }
    if(early_error>.002 || long_error<.05 || !lost) {
        REXLOG_ERROR("Native integration header precision controls did not discriminate short and long clocks");return false;
    }
    REXLOG_INFO("Native integration header passed {} original-code captures: <=128s worst relative delta error {:.6f}%, long-clock worst {:.6f}%, {} lost positive steps. No contact solve or live timestamp sampled",checks,100*early_error,100*long_error,lost);
    return true;
}
struct PhaseProbe {std::array<unsigned,3> calls{};uint32_t failure=0;double integration_dt=0;};
thread_local PhaseProbe phase_probe;
void PhysicsPhaseFixture(PPCContext& ctx,uint8_t* base) {
    const unsigned phase=ctx.lr==0x824674A4?1:ctx.lr==0x824674CC?2:ctx.lr==0x824674F4?3:0;
    if(!phase){phase_probe.calls[0]=UINT32_MAX;return;}
    ++phase_probe.calls[phase-1];if(phase==1)phase_probe.integration_dt=ctx.f1.f64;
    PPC_STORE_U32(ctx.r3.u32+40,phase_probe.failure==phase?phase:0);
    ctx.r3.u64=0xBAD;ctx.f1.f64=123; // Caller must reload its simulation pointer.
}
void PhysicsWorldPostFixture(PPCContext& ctx,uint8_t*) {ctx.r3.u64=0;}
bool TestPhysicsStepDispatcher(uint8_t* base) {
    constexpr uint32_t simulation=0x4000,vtable=0x5000,world=0x6000;
    if(!VirtualAlloc(base+PPC_IMAGE_BASE+PPC_IMAGE_SIZE,0x10000,MEM_COMMIT,PAGE_READWRITE))return false;
    for(unsigned phase=1;phase<=3;++phase){
        const auto callback=uint32_t(PPC_CODE_BASE)+(phase-1)*4;
        PPC_LOOKUP_FUNC(base,callback)=PhysicsPhaseFixture;PPC_STORE_U32(vtable+8+phase*4,callback);
    }
    unsigned checks=0;
    for(uint32_t initial=0;initial<=4;++initial)for(uint32_t failure=0;failure<=3;++failure)
      for(double dt:{0.,1./30,1./40,1./60,1./90,1./120,1./240,.024}) {
        PPC_STORE_U32(simulation,vtable);PPC_STORE_U32(simulation+12,world);PPC_STORE_U32(simulation+40,initial);
        PPC_STORE_U32(world+80,0);phase_probe={};phase_probe.failure=failure;
        PPCContext ctx{};ctx.r1.u64=0xF000;ctx.r3.u64=simulation;ctx.f1.f64=dt;ctx.fpscr.InitHost();
        __imp__sub_82467468(ctx,base);
        std::array<unsigned,3> expected{};auto result=initial;
        for(uint32_t phase=1;phase<=3;++phase)if(!result || result==phase){++expected[phase-1];result=failure==phase?phase:0;}
        if(phase_probe.calls!=expected || ctx.r3.u32!=result ||
           (expected[0] && phase_probe.integration_dt!=dt)) {
            REXLOG_ERROR("Native physics stage dispatch mismatch: initial {}, failed stage {}, dt {}",initial,failure,dt);return false;
        }
        ++checks;
    }
    // The outer native world method has its own lock, saved return register
    // and post-step callbacks. Compare the actual production observer around
    // that original method, not only the phase dispatcher in isolation.
    if(!VirtualAlloc(base+0x833C0000,0x10000,MEM_COMMIT,PAGE_READWRITE))return false;
    PPC_LOOKUP_FUNC(base,uint32_t(PPC_CODE_BASE)+12)=PhysicsWorldPostFixture;
    PPC_LOOKUP_FUNC(base,uint32_t(PPC_CODE_BASE)+16)=__imp__sub_82467468;
    struct RestoreTrace {bool trace=REXCVAR_GET(em2_trace_physics);decltype(test_step) step=test_step;
        ~RestoreTrace(){REXCVAR_SET(em2_trace_physics,trace);test_step=step;}}restore_trace;
    REXCVAR_SET(em2_trace_physics,true);test_step=nullptr;
    std::array<uint8_t,0x10000> before{},expected_memory{};unsigned comparisons=0;
    for(uint32_t initial=0;initial<=4;++initial)for(uint32_t failure=0;failure<=3;++failure)
      for(double dt:{1./30,1./40,1./60,1./90,1./120,1./240,.024}) {
        std::memset(base,0,0x10000);
        PPC_STORE_U32(simulation,vtable);PPC_STORE_U32(simulation+12,world);PPC_STORE_U32(simulation+40,initial);
        PPC_STORE_U32(vtable+8,uint32_t(PPC_CODE_BASE)+16);
        for(unsigned phase=1;phase<=3;++phase)PPC_STORE_U32(vtable+8+phase*4,uint32_t(PPC_CODE_BASE)+(phase-1)*4);
        PPC_STORE_U32(world+8,simulation);PPC_STORE_U32(world+88,0x7000);
        PPC_STORE_U32(0x7000,0x8000);PPC_STORE_U32(0x8008,uint32_t(PPC_CODE_BASE)+12);
        // Model an already-owned recursive native global lock. This exercises
        // its real increment/decrement fast path without kernel imports.
        PPC_STORE_U32(0x833C47C8,0x9000);PPC_STORE_U32(0x900C,1);
        PPC_STORE_U32(0xB000,0xA000);PPC_STORE_U32(0xA004,0x23DF4554);
        PPCContext context{};context.r1.u64=0xF000;context.r3.u64=world;context.r13.u64=0xB000;
        context.f1.f64=dt;context.fpscr.InitHost();const auto entry=context;const auto csr=_mm_getcsr();
        std::memcpy(before.data(),base,before.size());phase_probe={};phase_probe.failure=failure;
        __imp__sub_8246AD50(context,base);const auto expected_context=context;const auto expected_csr=_mm_getcsr();
        const auto expected_probe=phase_probe;std::memcpy(expected_memory.data(),base,expected_memory.size());
        std::memcpy(base,before.data(),before.size());context=entry;_mm_setcsr(csr);phase_probe={};phase_probe.failure=failure;
        sub_8246AD50(context,base);
        if(std::memcmp(&context,&expected_context,sizeof(context)) || _mm_getcsr()!=expected_csr ||
           std::memcmp(base,expected_memory.data(),expected_memory.size()) ||
           phase_probe.calls!=expected_probe.calls || phase_probe.integration_dt!=expected_probe.integration_dt) {
            REXLOG_ERROR("Native physics outcome observer changed world dispatch: initial {}, failed stage {}, dt {}",initial,failure,dt);return false;
        }
        ++comparisons;
    }
    REXLOG_INFO("Native physics outcome observer matched original world-step context,64KiB fixture memory,MXCSR and phase calls in {} checks, including failed/unknown results; real lock fast path/post callback, fixture phase bodies",comparisons);
    REXLOG_INFO("Native physics stage dispatcher passed {} original-code checks: integration/collision/advance ordering, exact incoming integration delta, stage1/2/3 retry and unknown results; phase bodies are fixtures, not contact simulation",checks);
    return true;
}
}
bool em2::TestPhysicsTiming() {
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));if(!base)return false;
    struct Restore {
        uint8_t* base;decltype(test_step) step=test_step;unsigned csr=_mm_getcsr();bool trace=REXCVAR_GET(em2_trace_physics);
        ~Restore(){REXCVAR_SET(em2_trace_physics,trace);test_step=step;_mm_setcsr(csr);VirtualFree(base,0,MEM_RELEASE);}
    } restore{base};
    if(!VirtualAlloc(base,0x10000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0x82000000,0x10000,MEM_COMMIT,PAGE_READWRITE))return false;
    PPC_STORE_U32(0x82002060,0);
    if(!TestPhysicsStepDispatcher(base))return false;
    struct Probe {uint32_t world=0,caller=0;double dt=0;unsigned calls=0;};
    static thread_local Probe probe;
    test_step=[](PPCContext& context,uint8_t*) {
        probe.world=context.r3.u32;probe.caller=uint32_t(context.lr);probe.dt=context.f1.f64;++probe.calls;
        context.r3.u64=0;
    };
    constexpr uint32_t scene=0x1000,bindings=0x2000,world=0x3000;
    unsigned comparisons=0;
    for(unsigned adapter=0;adapter<2;++adapter)for(float dt:{0.f,-.01f,1.f/30,1.f/40,1.f/60,1.f/90,1.f/120,1.f/240})
        for(bool missing:{false,true})for(bool suppressed:{false,true}) {
            std::memset(base,0,0x10000);probe={};
            PPC_STORE_U32(scene+8,missing?0:world);
            // Empty binding array: native adapters still own timestep gating
            // and dispatch, but there are no fabricated body/collision results.
            PPC_STORE_U32(bindings+8,0xFFFFFFFFu);
            if(adapter)PPC_STORE_U8(scene+16,suppressed);else PPC_STORE_U8(bindings+12,suppressed);
            PPCContext context{};context.r1.u64=0xF000;context.r3.u64=scene;context.r4.u64=bindings;
            context.f1.f64=double(dt);context.fpscr.InitHost();
            if(adapter)__imp__sub_823F9478(context,base);else __imp__sub_823F92A0(context,base);
            const bool expected=dt>0 && !missing && !suppressed;
            if(probe.calls!=unsigned(expected) || (expected &&
               (probe.world!=world || probe.dt!=double(dt) || probe.caller!=(adapter?0x823F95CCu:0x823F93A8u)))) {
                REXLOG_ERROR("Native physics adapter timestep mismatch: adapter {}, dt {}, missing {}, suppressed {}",adapter,dt,missing,suppressed);return false;
            }
            ++comparisons;
        }
    REXLOG_INFO("Native physics adapters passed {} original-code checks: exact variable delta forwarding, nonpositive delta, absent world and suppression",comparisons);

    // Execute the real unconstrained rigid-motion integration, including its
    // original quaternion/matrix helper. This fixture has no contacts or world.
    if(!VirtualAlloc(base+0x82010000,0x1B0000,MEM_COMMIT,PAGE_READWRITE))return false;
    std::ifstream image(std::filesystem::path(__FILE__).parent_path().parent_path()/"private/image.bin",std::ios::binary);
    if(!image.read(reinterpret_cast<char*>(base+0x82000000),0x1C0000)) {
        REXLOG_ERROR("Native rigid-motion fixture needs the local private/image.bin constants");return false;
    }
    if(!TestNativeIntegrationHeader(base) || !TestNativePhysicsScheduler(base))return false;
    auto put=[&](uint32_t address,float value){PPC_STORE_U32(address,std::bit_cast<uint32_t>(value));};
    auto get=[&](uint32_t address){return std::bit_cast<float>(PPC_LOAD_U32(address));};
    constexpr uint32_t body=0x2000,motion=body+224,step=0x1000,gravity=0x1200,array=0x1400;
    unsigned integrated=0;
    REXCVAR_SET(em2_trace_physics,true);
    std::array<uint8_t,0x10000> initial_memory,expected_memory;
    for(unsigned hz:{30,40,60,90,120,240})for(unsigned type:{1,2,3,4,7})for(bool falling:{false,true})for(bool rotating:{false,true}) {
        std::memset(base,0,0x10000);
        PPC_STORE_U32(array,body);PPC_STORE_U8(motion+8,type);
        PPC_STORE_U8(motion+9,255); // Native deactivation-disabled sentinel.
        PPC_STORE_U16(motion+184,0x3F80); // Motion time factor 1, bfloat16.
        PPC_STORE_U16(motion+286,0x3F80); // Gravity factor 1, bfloat16.
        PPC_STORE_U8(motion+186,255);PPC_STORE_U8(motion+187,255);
        for(unsigned i=0;i<3;++i)put(motion+16+i*20,1);
        put(motion+112+12,1);put(motion+128+12,1);
        put(motion+208,3);put(motion+212,2);put(motion+216,-1);
        if(rotating)put(motion+228,1.25f);
        const float dt=1.f/hz,g=falling?-9.81f:0.f;
        for(unsigned frame=0;frame<hz;++frame) {
            put(step,float(frame)*dt);put(step+4,float(frame+1)*dt);put(step+8,dt);put(step+12,float(hz));
            put(gravity+4,g*dt);
            PPCContext context{};context.r1.u64=0xF000;context.r3.u64=0x3000;
            context.r4.u64=step;context.r5.u64=gravity;context.r6.u64=array;
            context.r7.u64=1;context.r8.u64=224;context.fpscr.InitHost();
            const auto initial_context=context;const auto initial_csr=_mm_getcsr();
            std::memcpy(initial_memory.data(),base,initial_memory.size());
            __imp__sub_82489860(context,base);
            const auto expected_context=context;const auto expected_csr=_mm_getcsr();
            std::memcpy(expected_memory.data(),base,expected_memory.size());
            std::memcpy(base,initial_memory.data(),initial_memory.size());context=initial_context;_mm_setcsr(initial_csr);
            sub_82489860(context,base);
            if(std::memcmp(&context,&expected_context,sizeof(context)) || std::memcmp(base,expected_memory.data(),expected_memory.size()) || _mm_getcsr()!=expected_csr) {
                REXLOG_ERROR("Native integration observer changed context, memory or MXCSR: {} Hz type {}",hz,type);return false;
            }
            ++integrated;
        }
        // Types 1..3 receive gravity; keyframed/character motion doesn't.
        const float acceleration=type<=3?g:0;
        const float expected_y=2+acceleration*.5f*(1+dt);
        const float qy=get(motion+132),qw=get(motion+140);
        const std::array values{get(motion+96),get(motion+100),get(motion+104),get(motion+212),
            get(motion+128),qy,get(motion+136),qw};
        if(!std::all_of(values.begin(),values.end(),[](float value){return std::isfinite(value);}) ||
           std::abs(get(motion+96)-3)>2e-4f || std::abs(get(motion+100)-expected_y)>2e-4f ||
           std::abs(get(motion+104)+1)>2e-4f || std::abs(get(motion+212)-(2+acceleration))>2e-4f ||
           std::abs(get(motion+128))>2e-4f || std::abs(get(motion+136))>2e-4f ||
           std::abs(qy-std::sin(rotating?.625f:0.f))>2e-4f || std::abs(qw-std::cos(rotating?.625f:0.f))>2e-4f ||
           std::abs(qy*qy+qw*qw-1)>5e-5f) {
            REXLOG_ERROR("Native rigid-motion mismatch: {} Hz, type {}, gravity {}, rotation {}, position ({},{},{}), velocity y {}, expected y {}, quaternion y/w {}/{}",
                hz,type,falling,rotating,get(motion+96),get(motion+100),get(motion+104),get(motion+212),expected_y,qy,qw);return false;
        }
    }
    REXLOG_INFO("Native unconstrained rigid-motion integration passed {} real steps and full context/memory/MXCSR observer comparisons at 30..240 Hz, types 1/2/3/4/7 (contacts and full character controllers excluded)",integrated);
    return TestNativeCharacterTiming();
}
