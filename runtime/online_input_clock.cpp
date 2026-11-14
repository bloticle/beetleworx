#include "online_input_clock.h"
#include "online_cutscene_skip.h"
#include "online.h"
#include "controls.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <windows.h>
#include <immintrin.h>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <stdexcept>
#include <rex/logging.h>
PPC_EXTERN_FUNC(__imp__sub_830A8C48);
PPC_EXTERN_FUNC(__imp__sub_827C06A8);
PPC_EXTERN_FUNC(__imp__sub_831C14E0);
PPC_EXTERN_FUNC(__imp__sub_831C1558);
PPC_EXTERN_FUNC(__imp__sub_826572B8);
PPC_EXTERN_FUNC(sub_826C9F38);
namespace em2::net {
namespace {
struct Environment {unsigned fp=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(fp);SetLastError(error);}};
bool Readable(uint32_t p,size_t n,uint8_t* base){return base && p>=4096 && uint64_t(p)+n<=0x100000000ull && em2::GuestReadBatch::Readable(base+p,n);}
struct ClockFields {
    uint64_t origin=0,frequency=0,repeat_frequency=0;uint32_t reciprocal=0;
    bool operator==(const ClockFields&)const=default;
};
bool ReadFields(uint8_t* base,ClockFields& out){
    em2::GuestReadBatch reads;
    if(!Readable(0x83396930,1,base) || !Readable(0x833D03C0,16,base) || !Readable(0x833D04D8,12,base) || PPC_LOAD_U8(0x83396930))return false;
    out={PPC_LOAD_U64(0x833D03C0),PPC_LOAD_U64(0x833D03C8),PPC_LOAD_U64(0x833D04D8),PPC_LOAD_U32(0x833D04E0)};
    const auto reciprocal=std::bit_cast<float>(out.reciprocal);
    return out.origin && out.origin<=INT64_MAX && out.frequency && out.frequency<=INT64_MAX && out.repeat_frequency==out.frequency &&
        std::isfinite(reciprocal) && reciprocal>0;
}
struct Preserve {
    PPCContext& ctx;std::array<uint8_t,sizeof(PPCContext)> saved;Environment environment;
    std::array<uint8_t,8192> bytes{};uint8_t* memory=nullptr;uint32_t low=0,top=0,stack=0;
    Preserve(PPCContext& c,uint8_t* base):ctx(c){std::memcpy(saved.data(),&c,sizeof(c));top=c.r1.u32&~15u;
        if(top>=12288 && Readable(top-8192,8192,base)){low=top-8192;memory=base+low;std::memcpy(bytes.data(),memory,bytes.size());stack=top-4096;ctx.r1.u64=stack;}}
    ~Preserve(){if(memory)std::memcpy(memory,bytes.data(),bytes.size());std::memcpy(&ctx,saved.data(),sizeof(ctx));}
};
struct Span {uint32_t at=0,size=0,count=0,kind=0;};
struct Scope {
    DWORD thread=0;uint8_t* base=nullptr;PPCContext* context=nullptr;uint32_t low=0,top=0,stack=0,data=0,size=0;
    uint64_t counter=0;ClockFields fields;bool failed=false;
    std::array<Span,8> buttons{},actions{};unsigned button_count=0,action_count=0;
    bool Inside(uint32_t p,uint32_t n)const{return p>=data && uint64_t(p)+n<=uint64_t(data)+size;}
    bool Overlap(uint32_t p,uint32_t n)const{
        for(const auto& list:{buttons,actions})for(const auto& span:list)if(span.size && uint64_t(p)<uint64_t(span.at)+span.size && uint64_t(span.at)<uint64_t(p)+n)return true;
        return false;
    }
    bool Button(uint32_t p)const{for(unsigned i=0;i<button_count;++i){const auto& a=buttons[i];if(p>=a.at && (p-a.at)%20==0 && uint64_t(p)+20<=uint64_t(a.at)+a.size)return true;}return false;}
    bool Timer(uint32_t p)const{for(unsigned i=0;i<action_count;++i){const auto& a=actions[i];
        if(PPC_LOAD_U32(a.at+408)!=a.count || PPC_LOAD_U32(a.at+80)!=a.kind)continue;
        for(unsigned binding=0;binding<a.count;++binding)if(p==a.at+88+binding*32+24)return true;}return false;}
};
thread_local Scope* active=nullptr;
bool IsActive(void* opaque){return active && opaque==active && active->thread==GetCurrentThreadId();}
void CanonicalQueryVolatiles(PPCContext& ctx,uint64_t counter){
    // Exact original826C9F38 mftb/rotlwi. effects. Its low-word-zero second
    // read sees the same source counter here; it never consumes another tick.
    ctx.r11.u64=counter;ctx.r10.u64=uint32_t(counter);ctx.cr0.compare<int32_t>(ctx.r10.s32,0,ctx.xer);
}
struct LiveClock {
    PPCContext* context=nullptr;uint8_t* base=nullptr;uint32_t stack=0,caller=0;uint64_t counter=0;bool failed=false;
};
thread_local LiveClock* live_clock=nullptr;
bool RunLiveClockHelper(PPCContext& ctx,uint8_t* base,PPCFunc* original,uint64_t tick){
    // Normalize only the owned input helper's output/stamp. Never temporarily
    // rewrite shared clock globals: streaming/audio workers can read them too.
    constexpr uint64_t origin=100000000,frequency=30000000,step=1000000;
    ClockFields saved;
    if(active || live_clock || tick>(uint64_t(INT64_MAX)-origin)/step || !ReadFields(base,saved))return false;
    const auto timer=ctx.r3.u32;const double delay=ctx.f1.f64;
    if(original==__imp__sub_831C14E0 && (!std::isfinite(delay) || std::abs(delay)>3600))return false;
    LiveClock clock{&ctx,base,ctx.r1.u32,uint32_t(ctx.lr),origin+tick*step};
    struct Restore {LiveClock* previous;~Restore(){live_clock=previous;}}restore{live_clock};
    live_clock=&clock;original(ctx,base);
    if(clock.failed){GameplayInputGate().ObservationFailed();return true;}
    Environment preserve_math;
    if(original==__imp__sub_826572B8)ctx.f1.f64=double(float(double(tick)/30.));
    else if(original==__imp__sub_831C14E0){
        // Preserve the native float multiply/truncation and zero sentinel,
        // expressed in the input-only counter units rather than host QPC units.
        const auto offset=int64_t(float(float(frequency)*float(delay)));
        auto stamp=clock.counter+uint64_t(offset);if(!stamp)stamp=1;PPC_STORE_U64(timer,stamp);
    }else if(original==__imp__sub_831C1558){
        const auto delta=int64_t(clock.counter-PPC_LOAD_U64(timer));
        ctx.f1.f64=double(float(float(delta)*(1.f/float(frequency))));
    }
    return true;
}
}
struct NativeInputClockState::Impl {
    DWORD thread=0;uint8_t* base=nullptr;ClockFields fields;
    uint64_t epoch=0,first=0,last=0,next=0,counter=0;unsigned phase=0;
    bool initialized=false,cached=false,exhausted=false;
};
struct NativeInputClockAccess {
    static auto& Get(NativeInputClockState& state){return *state.impl_;}
    static NativeInputClockScratch Scratch(Scope& scope){return NativeInputClockScratch(&scope);}
};
NativeInputClockState::NativeInputClockState():impl_(std::make_unique<Impl>()){}
NativeInputClockState::~NativeInputClockState()=default;
uint32_t NativeInputClockScratch::data()const{return IsActive(scope_)?active->data:0;}
uint32_t NativeInputClockScratch::capacity()const{return IsActive(scope_)?active->size:0;}
bool NativeInputClockScratch::OwnButtonHistory(uint32_t at,unsigned count){
    if(!IsActive(scope_))return false;auto& s=*active;
    if(!count || count>32 || (at&3) || !s.Inside(at,count*20)){s.failed=true;return false;}
    for(unsigned i=0;i<s.button_count;++i)if(s.buttons[i].at==at && s.buttons[i].count==count)return true;
    if(s.button_count==s.buttons.size() || s.Overlap(at,count*20)){s.failed=true;return false;}
    s.buttons[s.button_count++]={at,count*20,count,0};return true;
}
bool NativeInputClockScratch::OwnActionRecord(uint32_t at){
    if(!IsActive(scope_))return false;auto& s=*active;
    if((at&7) || !s.Inside(at,436)){s.failed=true;return false;}
    auto* base=s.base;const auto count=PPC_LOAD_U32(at+408),kind=PPC_LOAD_U32(at+80);
    if(!count || count>10 || kind!=2){s.failed=true;return false;}
    for(unsigned i=0;i<count;++i){const auto timer=PPC_LOAD_U64(at+88+i*32+24);
        if(timer && (timer<s.fields.origin || timer>s.counter)){s.failed=true;return false;}}
    for(unsigned i=0;i<s.action_count;++i)if(s.actions[i].at==at && s.actions[i].count==count && s.actions[i].kind==kind)return true;
    if(s.action_count==s.actions.size() || s.Overlap(at,436)){s.failed=true;return false;}
    s.actions[s.action_count++]={at,436,count,kind};return true;
}
bool InitializeNativeInputClock(NativeInputClockState& clock,uint64_t epoch,uint64_t first,PPCContext&,uint8_t* base){
    Environment preserve;auto& s=NativeInputClockAccess::Get(clock);ClockFields fields;
    if(active || s.initialized || !epoch || !ReadFields(base,fields))return false;
    s={};s.thread=GetCurrentThreadId();s.base=base;s.fields=fields;s.epoch=epoch;s.first=s.next=first;s.counter=fields.origin;s.initialized=true;return true;
}
bool ResetNativeInputClock(NativeInputClockState& clock){Environment preserve;auto& s=NativeInputClockAccess::Get(clock);
    if(active || (s.initialized && s.thread!=GetCurrentThreadId()))return false;s={};return true;}
bool RunNativeInputClock(NativeInputClockState& clock,uint64_t epoch,uint64_t tick,PPCContext& ctx,uint8_t* base,NativeInputClockCallback callback,void* user){
    Environment environment;auto& original=NativeInputClockAccess::Get(clock);ClockFields fields;
    if(active || !callback || !original.initialized || original.thread!=GetCurrentThreadId() || original.base!=base || original.epoch!=epoch ||
       !ReadFields(base,fields) || fields!=original.fields)return false;
    const bool repeated=original.cached && tick==original.last;
    if(!repeated && (original.exhausted || tick!=original.next))return false;
    auto next=original;
    if(!repeated){
        if(next.cached){const auto whole=next.fields.frequency/30;const auto phase=next.phase+unsigned(next.fields.frequency%30);
            const auto interval=whole+(phase>=30);if(interval>uint64_t(INT64_MAX)-next.counter)return false;
            next.counter+=interval;next.phase=phase%30;}
        next.last=tick;next.cached=true;next.exhausted=tick==UINT64_MAX;if(!next.exhausted)next.next=tick+1;
    }
    Preserve preserve(ctx,base);if(!preserve.memory)return false;
    Scope scope;scope.thread=GetCurrentThreadId();scope.base=base;scope.context=&ctx;scope.low=preserve.low;scope.top=preserve.top;scope.stack=preserve.stack;
    scope.data=preserve.stack+512;scope.size=preserve.top-scope.data;scope.counter=next.counter;scope.fields=fields;
    active=&scope;struct Release {~Release(){active=nullptr;}}release;
    auto scratch=NativeInputClockAccess::Scratch(scope);
    if(!callback(ctx,base,scratch,user) || scope.failed || !ReadFields(base,fields) || fields!=next.fields)return false;
    original=next;return true;
}
bool TryNativeInputClockQuery(PPCContext& ctx,uint8_t* base){
    if(auto* live=live_clock;live && live->context==&ctx && live->base==base){
        const bool owned=ctx.lr==0x826572F4 || ctx.lr==0x831C1504 || ctx.lr==0x831C1574;
        if(!owned)return false;
        if(uint64_t(ctx.r1.u32)+112!=live->stack || !Readable(ctx.r1.u32,112,base) ||
           PPC_LOAD_U32(ctx.r1.u32+104)!=live->caller || ctx.r3.u32!=ctx.r1.u32+80){
            live->failed=true;CanonicalQueryVolatiles(ctx,live->counter);ctx.r3.u64=0;return true;
        }
        PPC_STORE_U64(ctx.r3.u32,live->counter);CanonicalQueryVolatiles(ctx,live->counter);ctx.r3.u64=1;return true;
    }
    if(!active || active->base!=base || active->context!=&ctx || active->thread!=GetCurrentThreadId())return false;
    auto& s=*active;
    const bool seconds=ctx.lr==0x826572F4,start=ctx.lr==0x831C1504,elapsed=ctx.lr==0x831C1574;
    if(!seconds && !start && !elapsed)return false;
    Environment environment;
    // Exact native callee prolog saves incoming LR at currentSP+104.
    // Unrelated native wall-time/profiler calls keep their original QPC path.
    if(ctx.r1.u32<s.low || uint64_t(ctx.r1.u32)+112>s.stack || !Readable(ctx.r1.u32,112,base)){
        s.failed=true;CanonicalQueryVolatiles(ctx,s.counter);ctx.r3.u64=0;return true;
    }
    const auto caller=PPC_LOAD_U32(ctx.r1.u32+104);
    if(seconds && caller!=0x830A8C90 && caller!=0x827C06F8)return false;
    if(start && caller!=0x827C62BC)return false;
    if(elapsed && caller!=0x827C6054 && caller!=0x827C628C)return false;
    // ORIGINAL826572B8 saves r31 with std (64 bits), not stw. Guest pointers
    // occupy the low word of that big-endian save, at SP+100.
    const auto record=seconds?uint32_t(PPC_LOAD_U64(ctx.r1.u32+96)):ctx.r31.u32;
    ClockFields fields;const bool owned=(seconds?s.Button(record):s.Timer(record)) && ReadFields(base,fields) && fields==s.fields;
    const auto output=ctx.r3.u32;const bool output_owned=output>=s.low && uint64_t(output)+8<=s.stack && Readable(output,8,base);
    if(!owned || !output_owned)s.failed=true;
    CanonicalQueryVolatiles(ctx,s.counter);
    // A recognized but invalid input scope never silently substitutes walltime.
    // Only its private native stack output can be written on that error path.
    if(output_owned)PPC_STORE_U64(output,s.counter);
    ctx.r3.u64=owned && output_owned?1:0;return true;
}
bool RunAgreedNativeInputClockHelper(PPCContext& ctx,uint8_t* base,PPCFunc* original){
    if(active || live_clock || !GameplayInputGate().active())return false;
    const auto caller=uint32_t(ctx.lr);
    const bool button=original==__imp__sub_826572B8 && (caller==0x830A8C90 || caller==0x827C06F8);
    const bool start=original==__imp__sub_831C14E0 && caller==0x827C62BC;
    const bool elapsed=original==__imp__sub_831C1558 && (caller==0x827C6054 || caller==0x827C628C);
    const bool cutscene=(original==__imp__sub_831C14E0 || original==__imp__sub_831C1558) &&
        ((original==__imp__sub_831C14E0 && caller==0x82B211B8) || (original==__imp__sub_831C1558 && caller==0x82B22628)) &&
        NativeCutsceneSkipTimerOwned(ctx.r3.u32,caller,ctx,base);
    if(!button && !start && !elapsed && !cutscene)return false;
    uint64_t generation=0;Frame frame;
    if(!em2::ReadPublishedSynchronizedControls(generation,frame) || !generation || !Readable(button?ctx.r31.u32:ctx.r3.u32,button?20:8,base))return false;
    return RunLiveClockHelper(ctx,base,original,frame.tick);
}
namespace {
struct ClockCase {
    std::array<uint8_t,320> buttons{};std::array<uint8_t,436> action{};
    bool held=true,start=false,throw_after=false;
    float duration=0,repeat=0;uint64_t stamp=0;
};
bool NativeClockCase(PPCContext& ctx,uint8_t* base,NativeInputClockScratch& scratch,void* opaque){
    auto& test=*static_cast<ClockCase*>(opaque);const auto buttons=scratch.data(),action=buttons+512,output=buttons+1200;
    if(scratch.capacity()<1400)return false;
    std::memcpy(base+buttons,test.buttons.data(),test.buttons.size());std::memcpy(base+action,test.action.data(),test.action.size());
    PPC_STORE_U32(action+80,2);PPC_STORE_U32(action+408,1);
    if(!scratch.OwnButtonHistory(buttons,16) || !scratch.OwnActionRecord(action))return false;
    ctx.r3.u64=buttons+5*20;ctx.r4.u64=test.held?(1u<<5):0;ctx.r5.u64=test.held;ctx.r6.u64=test.held?255:0;
    __imp__sub_830A8C48(ctx,base);
    ctx.r3.u64=buttons+5*20;ctx.r4.u64=output;__imp__sub_827C06A8(ctx,base);const auto duration=float(ctx.f1.f64);
    // Native timer functions, with the exact caller ABI that ORIGINAL5F30
    // supplies. This clock fixture does not claim the whole digital interpreter.
    const auto timer=action+88+24;
    if(test.start){ctx.r3.u64=timer;ctx.f1.f64=0;ctx.lr=0x827C62BC;__imp__sub_831C14E0(ctx,base);}
    ctx.r3.u64=timer;ctx.lr=0x827C6054;__imp__sub_831C1558(ctx,base);
    if(test.throw_after)throw std::runtime_error("private input-clock fixture unwind");
    test.duration=duration;test.repeat=float(ctx.f1.f64);test.stamp=PPC_LOAD_U64(timer);
    std::memcpy(test.buttons.data(),base+buttons,test.buttons.size());std::memcpy(test.action.data(),base+action,test.action.size());return true;
}
struct NegativeCase {unsigned mode=0;NativeInputClockState* clock=nullptr;bool observed=false;};
bool QueryVolatileCase(PPCContext& ctx,uint8_t* base,NativeInputClockScratch& scratch,void* opaque){
    const auto expected_counter=*static_cast<const uint64_t*>(opaque);const auto record=scratch.data();
    std::memset(base+record,0,20);if(!scratch.OwnButtonHistory(record,1))return false;
    ctx.r1.u64=ctx.r1.u32-112;ctx.r3.u64=ctx.r1.u32+80;ctx.lr=0x826572F4;
    PPC_STORE_U32(ctx.r1.u32+104,0x830A8C90);PPC_STORE_U64(ctx.r1.u32+96,uint64_t(record));
    // Independent instruction-level oracle from ORIGINAL826C9F38; compare the
    // entire context so extra volatile/nonvolatile changes also fail.
    PPCContext expected;std::memcpy(&expected,&ctx,sizeof(expected));expected.r11.u64=expected_counter;
    expected.r10.u64=__builtin_rotateleft32(expected.r11.u32,0);expected.cr0.compare<int32_t>(expected.r10.s32,0,expected.xer);expected.r3.u64=1;
    sub_826C9F38(ctx,base);
    return PPC_LOAD_U64(ctx.r1.u32+80)==expected_counter && !std::memcmp(&ctx,&expected,sizeof(ctx));
}
bool NegativeClockCase(PPCContext& ctx,uint8_t* base,NativeInputClockScratch& scratch,void* opaque){
    auto& test=*static_cast<NegativeCase*>(opaque);const auto data=scratch.data();
    if(test.mode==0)return false;
    if(test.mode==1){test.observed=!RunNativeInputClock(*test.clock,7,0,ctx,base,NegativeClockCase,opaque);return test.observed;}
    if(test.mode==2){test.observed=!scratch.OwnButtonHistory(0x833D03C0,1);return true;}
    std::memset(base+data,0,1024);PPC_STORE_U32(data+512+80,2);PPC_STORE_U32(data+512+408,1);
    if(!scratch.OwnButtonHistory(data,1) || !scratch.OwnActionRecord(data+512))return false;
    // Construct a private native-query frame to exercise wrong-owner and
    // unrelated-call guards. No real history/global memory is targeted.
    PPCContext query;std::memcpy(&query,&ctx,sizeof(query));query.r1.u64=ctx.r1.u32-112;query.r3.u64=query.r1.u32+80;
    query.lr=test.mode==4?0x831C1504:0x826572F4;
    PPC_STORE_U32(query.r1.u32+104,test.mode==4?0x827C62BC:(test.mode==5?0xDEADBEEF:0x830A8C90));
    PPC_STORE_U64(query.r1.u32+96,uint64_t(data+20));query.r31.u64=data+512+88+24+4;
    if(test.mode==6){std::array<uint8_t,sizeof(PPCContext)> before;std::memcpy(before.data(),&query,sizeof(query));
        test.observed=!TryNativeInputClockQuery(query,base) && !std::memcmp(before.data(),&query,sizeof(query));return test.observed;}
    std::memcpy(&ctx,&query,sizeof(ctx));
    if(test.mode==5){std::array<uint8_t,sizeof(PPCContext)> before;std::memcpy(before.data(),&ctx,sizeof(ctx));
        test.observed=!TryNativeInputClockQuery(ctx,base) && !std::memcmp(before.data(),&ctx,sizeof(ctx));return test.observed;}
    test.observed=TryNativeInputClockQuery(ctx,base) && ctx.r3.u32==0;return test.observed;
}
}
bool TestNativeInputClock(){
    Environment environment;
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
    struct Release {uint8_t* base;~Release(){if(base)VirtualFree(base,0,MEM_RELEASE);}}release{base};
    const auto path=std::filesystem::path(__FILE__).parent_path().parent_path()/"private/image.bin";
    std::error_code error;const auto size=std::filesystem::file_size(path,error);
    if(!base || error || !VirtualAlloc(base,0x20000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+PPC_IMAGE_BASE,size,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+PPC_IMAGE_BASE+PPC_IMAGE_SIZE,4096,MEM_COMMIT,PAGE_READWRITE))return false;
    std::ifstream image(path,std::ios::binary);if(!image.read(reinterpret_cast<char*>(base+PPC_IMAGE_BASE),size))return false;
    auto configure=[&](uint64_t origin,uint64_t frequency){Environment preserve;PPC_STORE_U8(0x83396930,0);
        PPC_STORE_U64(0x833D03C0,origin);PPC_STORE_U64(0x833D03C8,frequency);PPC_STORE_U64(0x833D04D8,frequency);
        PPC_STORE_U32(0x833D04E0,std::bit_cast<uint32_t>(float(1./double(frequency))));};
    auto context=[](){PPCContext ctx{};ctx.fpscr.InitHost();ctx.r1.u64=0x1F000;ctx.lr=0x12345678;ctx.r13.u64=0x76543210;return ctx;};
    auto ctx=context();unsigned checks=0;auto check=[&](bool value,const char* why){++checks;if(!value)REXLOG_ERROR("Native input clock: {}",why);return value;};
    for(const uint64_t real_frequency:{1000000ull,50000001ull}){
        configure(real_frequency*123+7,real_frequency);ClockFields expected;ReadFields(base,expected);
        auto probe=+[](PPCContext& c,uint8_t* base){ClockFields during;
            if(!ReadFields(base,during) || during.origin!=c.r5.u64 || during.frequency!=c.r6.u64 || during.repeat_frequency!=c.r6.u64)
                throw std::runtime_error("Input helper exposed temporary clock values to other threads");};
        auto observed=context();observed.r5.u64=expected.origin;observed.r6.u64=expected.frequency;
        if(!check(RunLiveClockHelper(observed,base,probe,8),"shared clocks stay unchanged during the entire live helper invocation"))return false;
        PPC_STORE_U64(0x2000,0);auto call=context();call.r3.u64=0x2000;call.lr=0x827C62BC;call.f1.f64=0;
        if(!check(RunLiveClockHelper(call,base,__imp__sub_831C14E0,0) && PPC_LOAD_U64(0x2000)==100000000,
            "live input timer starts from the common controller epoch"))return false;
        for(const uint64_t tick:{0ull,7ull,8ull,15ull,30ull,3000ull}){
            call=context();call.r3.u64=0x2000;call.lr=0x827C6054;
            if(!check(RunLiveClockHelper(call,base,__imp__sub_831C1558,tick) &&
                std::bit_cast<uint32_t>(float(call.f1.f64))==std::bit_cast<uint32_t>(float(float(tick*1000000)*float(1.f/30000000.f))),
                "original repeat/held elapsed math uses identical input ticks across host frequencies"))return false;
            call=context();call.r31.u64=0x2000;call.lr=0x830A8C90;
            if(!check(RunLiveClockHelper(call,base,__imp__sub_826572B8,tick) &&
                std::bit_cast<uint32_t>(float(call.f1.f64))==std::bit_cast<uint32_t>(float(double(tick)/30.)),
                "original button clock uses controller time rather than wall time"))return false;
            ClockFields after;
            if(!check(ReadFields(base,after) && after==expected && !live_clock,
                "input helper restores every local native clock field before callbacks or unrelated timers"))return false;
        }
    }
    constexpr uint64_t frequency=1000003;
    configure(100000000,frequency);NativeInputClockState first;
    if(!check(InitializeNativeInputClock(first,7,100,ctx,base),"initialized native clock snapshot establishes source epoch"))return false;
    std::array<uint8_t,sizeof(PPCContext)> context_before;std::memcpy(context_before.data(),&ctx,sizeof(ctx));
    std::array<uint8_t,8192> stack_before;std::memcpy(stack_before.data(),base+ctx.r1.u32-8192,8192);
    std::array<uint8_t,32> globals_before;std::memcpy(globals_before.data(),base+0x833D03C0,32);
    const auto fp=_mm_getcsr();const auto last=GetLastError();
    ClockCase one;std::array<uint32_t,31> duration_bits{},repeat_bits{};
    for(unsigned step=0;step<=30;++step){Environment step_environment;one.start=step==0;
        if(!check(RunNativeInputClock(first,7,100+step,ctx,base,NativeClockCase,&one),"original button/elapsed/repeat native call chains execute in sealed scratch"))return false;
        const auto ticks=(frequency*step)/30;
        const float duration=float(double(ticks)/double(frequency));
        const float repeat=float(double(float(ticks))*double(float(1./double(frequency))));
        duration_bits[step]=std::bit_cast<uint32_t>(one.duration);repeat_bits[step]=std::bit_cast<uint32_t>(one.repeat);
        if(!check(duration_bits[step]==std::bit_cast<uint32_t>(duration) && repeat_bits[step]==std::bit_cast<uint32_t>(repeat),
            "native rounding uses exact rational source ticks, not callback wall time"))return false;
    }
    if(!check(std::bit_cast<float>(duration_bits[7])<.25f && std::bit_cast<float>(duration_bits[8])>.25f && one.duration==1.f,
        "original hold clock crosses quarter-second between native source ticks7/8 and reaches one second at30"))return false;
    const auto cached_counter=NativeInputClockAccess::Get(first).counter;
    if(!check(RunNativeInputClock(first,7,130,ctx,base,NativeClockCase,&one) && NativeInputClockAccess::Get(first).counter==cached_counter &&
        std::bit_cast<uint32_t>(one.duration)==duration_bits[30],"repeated source tick retains exact native counter"))return false;
    if(!check(!RunNativeInputClock(first,7,129,ctx,base,NativeClockCase,&one) && !RunNativeInputClock(first,8,131,ctx,base,NativeClockCase,&one) &&
        !RunNativeInputClock(first,7,132,ctx,base,NativeClockCase,&one),"backward, foreign epoch and skipped source ticks reject"))return false;
    {Environment preserve;
        const auto now=float(double((frequency*31)/30)/double(frequency));
        const auto stamp=std::bit_cast<uint32_t>(float(now-.25f));
        for(unsigned i=0;i<4;++i)one.buttons[5*20+16+i]=uint8_t(stamp>>(24-i*8));
        if(!check(RunNativeInputClock(first,7,131,ctx,base,NativeClockCase,&one) && one.duration==.25f,
            "original duration routine preserves exact quarter-second boundary from a private authored history stamp"))return false;
    }
    if(!check(!std::memcmp(context_before.data(),&ctx,sizeof(ctx)) && !std::memcmp(stack_before.data(),base+ctx.r1.u32-8192,8192) &&
        !std::memcmp(globals_before.data(),base+0x833D03C0,32) && _mm_getcsr()==fp && GetLastError()==last,
        "all original calls preserve complete caller/scratch/global clock bytes/FP/LastError"))return false;
    configure(900000000,frequency);NativeInputClockState second;ClockCase two;
    if(!check(InitializeNativeInputClock(second,99,UINT64_MAX-30,ctx,base),"different real origin and source epoch admit independent clock"))return false;
    for(unsigned step=0;step<=30;++step){two.start=step==0;
        if(!check(RunNativeInputClock(second,99,UINT64_MAX-30+step,ctx,base,NativeClockCase,&two) &&
            std::bit_cast<uint32_t>(two.duration)==duration_bits[step] && std::bit_cast<uint32_t>(two.repeat)==repeat_bits[step],
            "different local real counter origins yield bit-identical native source durations"))return false;
    }
    if(!check(!RunNativeInputClock(second,99,0,ctx,base,NativeClockCase,&two) && RunNativeInputClock(second,99,UINT64_MAX,ctx,base,NativeClockCase,&two),
        "source tick exhaustion permits cached maximum but never wraps"))return false;
    configure(100000000,frequency);
    if(!check(!RunNativeInputClock(second,99,UINT64_MAX,ctx,base,NativeClockCase,&two),"replaced initialized clock snapshot cannot reuse source lease"))return false;
    NativeInputClockState failures;if(!InitializeNativeInputClock(failures,7,0,ctx,base))return false;
    for(unsigned mode:{0u,2u,3u,4u}){NegativeCase test{mode,&failures};
        if(!check(!RunNativeInputClock(failures,7,0,ctx,base,NegativeClockCase,&test) && !NativeInputClockAccess::Get(failures).cached,
            "failed callback or invalid private button/timer/global range leaves source progression untouched"))return false;}
    for(unsigned mode:{1u,5u,6u}){NegativeCase test{mode,&failures};
        if(!check(RunNativeInputClock(failures,7,0,ctx,base,NegativeClockCase,&test) && test.observed,
            "nested scope rejects; unrelated or foreign-context wall-clock query falls through unchanged"))return false;}
    ClockCase throwing;throwing.start=true;throwing.throw_after=true;
    bool caught=false;try{RunNativeInputClock(failures,7,1,ctx,base,NativeClockCase,&throwing);}catch(const std::runtime_error&){caught=true;}
    if(!check(caught && NativeInputClockAccess::Get(failures).last==0 && !active && !std::memcmp(context_before.data(),&ctx,sizeof(ctx)) &&
       !std::memcmp(stack_before.data(),base+ctx.r1.u32-8192,8192) && _mm_getcsr()==fp && GetLastError()==last,
       "native callback exception unwinds full private state/context/scratch/environment"))return false;
    bool foreign=true;std::thread other([&]{auto c=context();ClockCase test;foreign=RunNativeInputClock(failures,7,1,c,base,NativeClockCase,&test) || ResetNativeInputClock(failures);});other.join();
    if(!check(!foreign && NativeInputClockAccess::Get(failures).last==0,"foreign thread cannot consume or reset source clock"))return false;
    NativeInputClockState lazy;PPC_STORE_U8(0x83396930,1);
    if(!check(!InitializeNativeInputClock(lazy,1,0,ctx,base) && PPC_LOAD_U8(0x83396930)==1,"lazy initialization is rejected without writing native global clocks"))return false;
    configure(uint64_t(INT64_MAX)-1,frequency);NativeInputClockState exhausted;ClockCase final_case;final_case.start=true;
    if(!check(InitializeNativeInputClock(exhausted,1,0,ctx,base) && RunNativeInputClock(exhausted,1,0,ctx,base,NativeClockCase,&final_case) &&
        !RunNativeInputClock(exhausted,1,1,ctx,base,NativeClockCase,&final_case),"canonical native counter overflow rejects instead of wrapping"))return false;
    PPCContext outside=context();outside.lr=0x826572F4;
    if(!check(!TryNativeInputClockQuery(outside,base),"unarmed code never handles native wall-clock query"))return false;
    for(uint64_t origin:{0x100000000ull,0x180000000ull,0x100000005ull}){configure(origin,frequency);NativeInputClockState native_volatile;
        if(!check(InitializeNativeInputClock(native_volatile,1,0,ctx,base) && RunNativeInputClock(native_volatile,1,0,ctx,base,QueryVolatileCase,&origin),
            "canonical query preserves original volatile registers/CR0 for zero, signed-negative and positive low words"))return false;}
    REXLOG_INFO("Native input clock: {} checks passed (original clock/button/timer routines; no live input admission)",checks);return true;
}
}

PPC_FUNC_IMPL(sub_826572B8){
    if(!em2::net::RunAgreedNativeInputClockHelper(ctx,base,__imp__sub_826572B8))__imp__sub_826572B8(ctx,base);
}
PPC_FUNC_IMPL(sub_831C14E0){
    if(!em2::net::RunAgreedNativeInputClockHelper(ctx,base,__imp__sub_831C14E0))__imp__sub_831C14E0(ctx,base);
}
