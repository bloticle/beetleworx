#include "online_cutscene_skip.h"
#include "online_cutscene_input.h"
#include "online_input_clock.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <rex/logging.h>
PPC_EXTERN_FUNC(__imp__sub_830A8200);
PPC_EXTERN_FUNC(__imp__sub_830ACA80);
PPC_EXTERN_FUNC(__imp__sub_827C3278);
PPC_EXTERN_FUNC(__imp__sub_827C5F30);
PPC_EXTERN_FUNC(__imp__sub_830A7FD0);
PPC_EXTERN_FUNC(__imp__sub_830A8030);
PPC_EXTERN_FUNC(__imp__sub_830A8090);
PPC_EXTERN_FUNC(__imp__sub_830A7968);
namespace em2::net {
namespace {
struct Environment {unsigned fp=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(fp);SetLastError(error);}};
bool Readable(uint8_t* base,uint32_t p,size_t n){return base && p>=4096 && uint64_t(p)+n<=0x100000000ull && GuestReadBatch::Readable(base+p,n);}
using Record=std::array<uint8_t,436>;
void Normalize(Record& a){
    std::fill(a.begin()+4,a.begin()+80,uint8_t(0));std::fill(a.begin()+84,a.begin()+88,uint8_t(0));
    a[105]=0;std::fill(a.begin()+112,a.begin()+120,uint8_t(0));std::fill(a.begin()+120,a.begin()+408,uint8_t(0));
}
constexpr std::array<const char*,3> Names{"DisplaySkipMovieButton","MovieSkipStart","MovieSkipStop"};
constexpr std::array<uint32_t,3> Kinds{2,1,3};
void FixtureControlMode(PPCContext& ctx,uint8_t*){ctx.r3.u64=3;}
struct Binding {
    uint32_t registry=0,manager=0,input=0,device=0,buttons=0,threshold=0;unsigned user=0;
    std::array<uint32_t,3> actions{};std::array<Record,3> shapes{};
    bool operator==(const Binding&)const=default;
};
bool Discover(uint32_t registry,unsigned user,PPCContext& ctx,uint8_t* base,Binding& out){
    if(user>3)return false;
    NativeCutsceneUiSnapshot ui;if(!InspectNativeCutsceneUi(ctx,base,registry,ui) || !Readable(base,ui.manager,176) || !Readable(base,0x833B65F8,4))return false;
    Binding b;b.registry=registry;b.manager=ui.manager;b.user=user;b.threshold=PPC_LOAD_U32(0x833B65F8);
    b.input=PPC_LOAD_U32(b.manager+72);if(!b.threshold || b.threshold>255 || !Readable(base,b.input,88))return false;
    b.device=PPC_LOAD_U32(b.input+72+user*4);
    if(!Readable(base,b.device,96) || PPC_LOAD_U32(b.device)!=0x821DE6E8 || PPC_LOAD_U32(b.device+48)!=16 ||
       PPC_LOAD_U32(0x821DE6E8+60)!=0x830A7FD0 || PPC_LOAD_U32(0x821DE6E8+64)!=0x830A8030 ||
       PPC_LOAD_U32(0x821DE6E8+68)!=0x830A8090 || PPC_LOAD_U32(0x821DE6E8+112)!=0x830A7968)return false;
    b.buttons=PPC_LOAD_U32(b.device+52);if(!Readable(base,b.buttons,320))return false;
    unsigned contexts=0;
    for(const auto& c:ui.contexts)if(c.name=="CutScene"){
        ++contexts;
        for(const auto& a:c.actions)for(unsigned i=0;i<Names.size();++i)if(a.name==Names[i]){
            if(b.actions[i] || a.kind!=Kinds[i] || a.condition || a.curve || a.bindings.size()!=1 ||
               a.bindings[0][0]!=7 || a.bindings[0][1]!=0x01400009 || a.bindings[0][2] || a.bindings[0][3] ||
               PPC_LOAD_U8(a.action+104) || a.bindings[0][5])return false;
            b.actions[i]=a.action;std::memcpy(b.shapes[i].data(),base+a.action,436);Normalize(b.shapes[i]);
        }
    }
    if(contexts!=1 || std::any_of(b.actions.begin(),b.actions.end(),[](auto a){return !a;}))return false;
    out=std::move(b);return true;
}
struct Candidate {
    std::array<uint8_t,96> device{};std::array<uint8_t,320> buttons{};std::array<Record,3> actions{};
    bool down=false,changed=false;NativeCutsceneSkipSample result;
};
Candidate Fresh(const Binding& b,uint8_t* base){
    Candidate c;std::memcpy(c.device.data(),base+b.device,96);c.actions=b.shapes;
    for(unsigned at:{20u,40u,44u})std::fill_n(c.device.begin()+at,4,uint8_t(0));return c;
}
struct Evaluation {const Binding& binding;Candidate& candidate;bool validate=true;};
bool Evaluate(PPCContext& ctx,uint8_t* base,NativeInputClockScratch& scratch,void* opaque){
    auto& e=*static_cast<Evaluation*>(opaque);const auto& b=e.binding;auto& c=e.candidate;
    const auto manager=scratch.data(),input=manager+176,device=input+96,buttons=device+96,action=buttons+320,output=action+440;
    if(scratch.capacity()<output+8-manager || !Readable(base,b.manager,176))return false;
    std::memcpy(base+manager,base+b.manager,176);std::memset(base+input,0,96);
    std::memcpy(base+device,c.device.data(),96);std::memcpy(base+buttons,c.buttons.data(),320);
    PPC_STORE_U32(manager+72,input);PPC_STORE_U32(input+72+b.user*4,device);
    PPC_STORE_U32(device+52,buttons);PPC_STORE_U32(device+56,0);PPC_STORE_U32(device+60,0);
    if(!scratch.OwnButtonHistory(buttons,16))return false;
    ctx.r3.u64=device;__imp__sub_830A8200(ctx,base);
    if(c.changed){ctx.r3.u64=device;ctx.r4.u64=c.down?0x1000:0;__imp__sub_830ACA80(ctx,base);}
    for(unsigned i=0;i<3;++i){
        std::memcpy(base+action,c.actions[i].data(),436);
        if(Kinds[i]==2 && !scratch.OwnActionRecord(action))return false;
        ctx.r3.u64=manager;ctx.r4.u64=action;__imp__sub_827C3278(ctx,base);const auto enabled=ctx.r3.u32;if(enabled>1)return false;
        PPC_STORE_U32(output,0);PPC_STORE_U32(output+4,0);
        ctx.r3.u64=manager;ctx.r4.u64=action;ctx.r5.u64=enabled;ctx.r6.u64=output;ctx.r7.u64=output+4;
        __imp__sub_827C5F30(ctx,base);
        if(ctx.r3.u32>1 || !std::isfinite(std::bit_cast<float>(PPC_LOAD_U32(output))) || !std::isfinite(std::bit_cast<float>(PPC_LOAD_U32(output+4))))return false;
        c.result.dispatch[i]=ctx.r3.u32!=0;std::memcpy(c.actions[i].data(),base+action,436);
    }
    std::memcpy(c.device.data(),base+device,96);std::memcpy(c.buttons.data(),base+buttons,320);
    if(e.validate){Binding current;if(!Discover(b.registry,b.user,ctx,base,current) || current!=b)return false;}
    return true;
}
}
struct NativeCutsceneSkipSource::Impl {
    DWORD thread=0;uint8_t* base=nullptr;Binding binding;Candidate committed;NativeInputClockState clock;
    uint64_t epoch=0,next=0,last=0;bool armed=false,cached=false,exhausted=false,down=false;
};
NativeCutsceneSkipSource::NativeCutsceneSkipSource():impl_(std::make_unique<Impl>()){}
NativeCutsceneSkipSource::~NativeCutsceneSkipSource()=default;
bool NativeCutsceneSkipSource::Arm(uint32_t registry,unsigned user,uint64_t epoch,uint64_t first,PPCContext& ctx,uint8_t* base){
    Environment env;auto& s=*impl_;Binding binding;
    if(s.armed || !epoch || (epoch&1) || !Discover(registry,user,ctx,base,binding) || !InitializeNativeInputClock(s.clock,epoch,first,ctx,base))return false;
    s.committed=Fresh(binding,base);s.binding=std::move(binding);s.epoch=epoch;s.next=first;s.thread=GetCurrentThreadId();s.base=base;s.armed=true;return true;
}
bool NativeCutsceneSkipSource::Produce(const Pad& pad,unsigned user,uint64_t epoch,uint64_t tick,PPCContext& ctx,uint8_t* base,NativeCutsceneSkipSample& out){
    Environment env;out={};auto& s=*impl_;Binding current;
    if(!s.armed || s.thread!=GetCurrentThreadId() || base!=s.base || user!=s.binding.user || epoch!=s.epoch ||
       !Discover(s.binding.registry,user,ctx,base,current) || current!=s.binding)return false;
    const bool down=bool(pad.buttons&0x1000);
    if(s.cached && s.last==tick){if(down!=s.down)return false;out=s.committed.result;return true;}
    if(s.exhausted || tick!=s.next)return false;
    auto next=s.committed;next.down=down;next.changed=!s.cached || down!=s.down;next.result={epoch,tick,{}};
    Evaluation evaluation{s.binding,next};if(!RunNativeInputClock(s.clock,epoch,tick,ctx,base,Evaluate,&evaluation))return false;
    s.committed=std::move(next);s.last=tick;s.down=down;s.cached=true;s.exhausted=tick==UINT64_MAX;if(!s.exhausted)s.next=tick+1;
    out=s.committed.result;return true;
}

bool TestNativeCutsceneSkipMath(){
    Environment environment;
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
    struct Release {uint8_t* base;~Release(){if(base)VirtualFree(base,0,MEM_RELEASE);}}release{base};
    const auto path=std::filesystem::path(__FILE__).parent_path().parent_path()/"private/image.bin";std::error_code error;const auto size=std::filesystem::file_size(path,error);
    if(!base || error || !VirtualAlloc(base,0x80000,MEM_COMMIT,PAGE_READWRITE) || !VirtualAlloc(base+PPC_IMAGE_BASE,size,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+PPC_IMAGE_BASE+PPC_IMAGE_SIZE,4096,MEM_COMMIT,PAGE_READWRITE))return false;
    std::ifstream image(path,std::ios::binary);if(!image.read(reinterpret_cast<char*>(base+PPC_IMAGE_BASE),size))return false;
    auto map=[&](uint32_t at,PPCFunc* fn){auto* pointer=&PPC_LOOKUP_FUNC(base,at);if(!VirtualAlloc(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pointer)&~uintptr_t(4095)),4096,MEM_COMMIT,PAGE_READWRITE))return false;*pointer=fn;return true;};
    if(!map(0x830A7FD0,__imp__sub_830A7FD0) || !map(0x830A8030,__imp__sub_830A8030) || !map(0x830A8090,__imp__sub_830A8090) || !map(0x830A7968,__imp__sub_830A7968))return false;
    PPCContext ctx{};ctx.fpscr.InitHost();ctx.r1.u64=0x7F000;ctx.lr=0x12345678;
    constexpr uint32_t manager=0x10000,device=0x20000,records=0x28000;
    PPC_STORE_U8(0x83396930,0);PPC_STORE_U64(0x833D03C0,100000000);PPC_STORE_U64(0x833D03C8,1000003);PPC_STORE_U64(0x833D04D8,1000003);
    PPC_STORE_U32(0x833D04E0,std::bit_cast<uint32_t>(float(1./1000003.)));PPC_STORE_U32(0x833B65F8,32);
    // No live context dispatch is installed. The original action lookup sees
    // no default subscriptions; wildcard user7 must find only our private slot.
    PPC_STORE_U32(device,0x821DE6E8);PPC_STORE_U32(device+48,16);
    Binding b;b.manager=manager;b.device=device;
    for(unsigned i=0;i<3;++i){const auto p=records+i*512,name=records+2048+i*96;
        std::memcpy(base+name,Names[i],std::strlen(Names[i])+1);PPC_STORE_U32(p,name);PPC_STORE_U32(p+80,Kinds[i]);PPC_STORE_U32(p+88,7);PPC_STORE_U32(p+92,0x01400009);PPC_STORE_U32(p+408,1);
        std::memcpy(b.shapes[i].data(),base+p,436);b.actions[i]=p;}
    unsigned checks=0;auto check=[&](bool ok,const char* why){++checks;if(!ok)REXLOG_ERROR("Native cutscene skip math: {}",why);return ok;};
    std::array<uint8_t,sizeof(PPCContext)> context;std::memcpy(context.data(),&ctx,sizeof(ctx));
    std::array<uint8_t,8192> stack;std::memcpy(stack.data(),base+ctx.r1.u32-8192,8192);
    for(unsigned user=0;user<4;++user){
        b.user=user;auto c=Fresh(b,base);NativeInputClockState clock;
        if(!InitializeNativeInputClock(clock,2+user*2,0,ctx,base))return false;
        unsigned display=0,start=0,stop=0;int held_start=-1;
        for(unsigned tick=0;tick<28;++tick){auto next=c;next.down=tick>=2 && tick<20;next.changed=tick==0 || next.down!=c.down;next.result={2+user*2,tick,{}};
            Evaluation e{b,next,false};
            if(!check(RunNativeInputClock(clock,2+user*2,tick,ctx,base,Evaluate,&e),"original interpreter completes wildcard binding with private device history"))return false;
            display+=next.result.dispatch[0];start+=next.result.dispatch[1];stop+=next.result.dispatch[2];if(next.result.dispatch[1])held_start=int(tick);
            if(!check(next.result.dispatch[0]==(tick==2) && next.result.dispatch[2]==(tick==20),"prompt press and release retain exact native edges"))return false;
            c=std::move(next);
        }
        if(!check(display==1 && start==1 && stop==1 && held_start>2 && held_start<20,"native held action has its own delay/latch instead of a fabricated per-tick callback"))return false;
    }
    if(!check(!std::memcmp(context.data(),&ctx,sizeof(ctx)) && !std::memcmp(stack.data(),base+ctx.r1.u32-8192,8192),"original interpreter preserves caller and entire borrowed stack"))return false;
    for(unsigned i=0;i<3;++i)if(!check(!std::memcmp(base+b.actions[i],b.shapes[i].data(),436),"live action records remain untouched"))return false;
    // Complete public source lifecycle against an isolated, bounded native UI
    // context. Only the control-state getter is a fixture; skip/device/action
    // interpreters and action selection are original recompiled functions.
    constexpr uint32_t registry=0x2000,profile=0x3000,table=0x4000,event=0x11000,name=0x12000,list=0x13000,input=0x23000;
    if(!map(uint32_t(PPC_CODE_BASE),FixtureControlMode))return false;
    PPC_STORE_U32(0x833DA7E4,registry);PPC_STORE_U32(0x833DAD98,profile);PPC_STORE_U32(0x833E36CC,0);
    PPC_STORE_U32(profile+12,table);PPC_STORE_U32(table+20,uint32_t(PPC_CODE_BASE));
    PPC_STORE_U32(0x833DBAD8,manager);PPC_STORE_U32(manager+4,list);PPC_STORE_U32(manager+8,1);PPC_STORE_U32(list,event);
    std::memcpy(base+name,"CutScene",9);std::memcpy(base+name+96,"UI_Global",10);
    PPC_STORE_U32(manager+16,name);PPC_STORE_U32(manager+24,name+96);PPC_STORE_U32(event,name);
    PPC_STORE_U32(event+16,list+96);PPC_STORE_U32(event+20,3);for(unsigned i=0;i<3;++i)PPC_STORE_U32(list+96+i*4,b.actions[i]);
    PPC_STORE_U32(manager+72,input);PPC_STORE_U32(input+80,device);PPC_STORE_U32(device+52,device+256);
    NativeCutsceneSkipSource source;NativeCutsceneSkipSample sample;Pad pad;
    if(!check(!source.Arm(registry,2,1,0,ctx,base) && source.Arm(registry,2,2,100,ctx,base) && !source.Arm(registry,2,2,100,ctx,base),"source arms once with an explicit even input epoch and current native UI binding"))return false;
    if(!check(!source.Produce(pad,1,2,100,ctx,base,sample) && !source.Produce(pad,2,4,100,ctx,base,sample) && !source.Produce(pad,2,2,101,ctx,base,sample),"wrong device, epoch and skipped source ticks cannot advance history"))return false;
    bool foreign=true;std::thread other([&]{auto local=ctx;NativeCutsceneSkipSample result;foreign=source.Produce({},2,2,100,local,base,result);});other.join();
    if(!check(!foreign,"foreign thread cannot evaluate the source"))return false;
    pad.buttons=0x1000;
    if(!check(source.Produce(pad,2,2,100,ctx,base,sample) && sample.dispatch==std::array<bool,3>{true,false,false},"first original press emits prompt only"))return false;
    const auto cached=sample;
    if(!check(source.Produce(pad,2,2,100,ctx,base,sample) && sample==cached && !source.Produce({},2,2,100,ctx,base,sample),"same-tick retry is cached while a conflicting press state is rejected"))return false;
    if(!source.Produce(pad,2,2,101,ctx,base,sample))return false;
    if(!check(source.Produce({},2,2,102,ctx,base,sample) && sample.dispatch==std::array<bool,3>{false,false,true},"short tap releases without fabricating a hold action"))return false;
    // Replacing a selected action may not partially commit its clock/history.
    const auto action=PPC_LOAD_U32(list+96);PPC_STORE_U32(list+96,list+512);
    const bool stale=!source.Produce({},2,2,103,ctx,base,sample);PPC_STORE_U32(list+96,action);
    if(!check(stale && source.Produce({},2,2,103,ctx,base,sample),"stale definition rejects transactionally before source time commits"))return false;
    pad.buttons=0xFFFF;pad.left_trigger=255;pad.left_x=20000;
    if(!check(source.Produce(pad,2,2,104,ctx,base,sample) && sample.dispatch==std::array<bool,3>{true,false,false},"other buttons and axes cannot introduce pause, quick-exit, join or gameplay decisions"))return false;
    REXLOG_INFO("Native cutscene skip math passed {} original-interpreter and source-lifecycle checks across four wildcard slots; no UI callback or online skip claim",checks);return true;
}
}
