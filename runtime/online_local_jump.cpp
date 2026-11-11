#include "online_local_jump.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>
#include <vector>
PPC_EXTERN_FUNC(__imp__sub_827C2D98);
PPC_EXTERN_FUNC(__imp__sub_827C5F30);
PPC_EXTERN_FUNC(__imp__sub_830A8200);
PPC_EXTERN_FUNC(__imp__sub_830ACA80);
PPC_EXTERN_FUNC(__imp__sub_830A7FD0);
PPC_EXTERN_FUNC(__imp__sub_830A8030);
PPC_EXTERN_FUNC(__imp__sub_830A7968);
PPC_EXTERN_FUNC(__imp__sub_830A8090);
PPC_EXTERN_FUNC(__imp__sub_827B58D0);
PPC_EXTERN_FUNC(__imp__sub_82A69948);
namespace {
using namespace em2::net;
struct Environment {unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(csr);SetLastError(error);}};
bool Readable(uint32_t p,size_t n,uint8_t* base){return base && p>=4096 && uint64_t(p)+n<=0x100000000ull && em2::GuestReadBatch::Readable(base+p,n);}
struct Scratch {
    PPCContext& ctx;std::array<uint8_t,sizeof(PPCContext)> saved{};Environment environment;
    std::array<uint8_t,8192> bytes{};uint8_t* memory=nullptr;uint32_t stack=0;
    Scratch(PPCContext& c,uint8_t* base):ctx(c){
        std::memcpy(saved.data(),&c,sizeof(c));const auto top=c.r1.u32&~15u;
        if(top>=12288 && Readable(top-8192,8192,base)){
            memory=base+top-8192;std::memcpy(bytes.data(),memory,bytes.size());stack=top-4096;ctx.r1.u64=stack;
        }
    }
    ~Scratch(){if(memory)std::memcpy(memory,bytes.data(),bytes.size());std::memcpy(&ctx,saved.data(),sizeof(ctx));}
};
// Audited ORIGINAL830ACA80 SDK-held-level -> native button index permutation.
// These are levels only. ORIGINAL5F30/4F48/5158/getters detect the actual edges.
constexpr std::array<uint16_t,16> masks{1,2,4,8,0x100,0,0x200,0,0x8000,0x1000,0x4000,0x2000,0x40,0x80,0x10,0x20};
struct Stream {
    bool active=false,held=false,exhausted=false,cached=false;
    DWORD thread=0;uint64_t epoch=0,next=0,last=0,sequence=0;
    NativeLocalJumpBinding binding{};Pad pad{};NativeLocalJumpEvents output{};
    std::array<uint8_t,640> buttons{};
};
thread_local Stream stream;
thread_local bool evaluating=false;
bool Parent(const NativeLocalMovementBinding& expected,PPCContext& ctx,uint8_t* base){
    NativeLocalMovementBinding current;
    return NativeLocalMovementBindingFor(ctx,base,current) && current==expected;
}
bool Member(uint32_t list,uint32_t count,uint32_t target,unsigned limit,uint8_t* base){
    if(!count || count>limit || !Readable(list,size_t(count)*4,base))return false;
    unsigned hits=0;for(unsigned i=0;i<count;++i)hits+=PPC_LOAD_U32(list+i*4)==target;return hits==1;
}
bool InspectAction(NativeLocalJumpAction& out,unsigned edge,const NativeLocalMovementBinding& m,PPCContext& ctx,uint8_t* base){
    if(!Readable(out.action,436,base) || !Readable(out.event,24,base))return false;
    const auto a=out.action;out.uid=PPC_LOAD_U32(a);out.kind=PPC_LOAD_U32(a+80);out.key=PPC_LOAD_U32(a+92);
    // Single gamepad digital binding only. Analog thresholds, modifiers,
    // chords, timed-repeat and multiple-device routes need separate proofs.
    if(out.uid!=PPC_LOAD_U32(edge?0x833DAEC0:0x833DAEBC) ||
       PPC_LOAD_U32(a+408)!=1 || PPC_LOAD_U32(a+412) || PPC_LOAD_U32(a+88)!=m.logical_user ||
       (out.key&0xFFFF0000)!=0x01400000 || PPC_LOAD_U32(a+108)!=0 ||
       out.kind!=(edge?3u:2u) || PPC_LOAD_U8(a+104) || PPC_LOAD_U32(a+432)!=0)return false;
    const auto actions=PPC_LOAD_U32(out.event+16),count=PPC_LOAD_U32(out.event+20);
    if(!Member(PPC_LOAD_U32(out.event+4),PPC_LOAD_U32(out.event+8),m.listener,128,base) ||
       !Member(actions,count,a,256,base) || out.index>=count || PPC_LOAD_U32(actions+out.index*4)!=a)return false;
    out.shape={out.uid,out.kind,PPC_LOAD_U32(a+408),PPC_LOAD_U32(a+412),PPC_LOAD_U32(a+88),out.key,
        PPC_LOAD_U32(a+96),PPC_LOAD_U8(a+104),PPC_LOAD_U32(a+108),PPC_LOAD_U32(a+432)};
    Scratch scratch(ctx,base);if(!scratch.memory)return false;
    ctx.r4.u64=out.key;__imp__sub_827C2D98(ctx,base);
    return ctx.r3.u32<masks.size() && masks[ctx.r3.u32]!=0;
}
// Every mutation below is to private guest scratch. The live native input
// device, histories, action timers and global singleton pointers remain intact.
bool Evaluate(const NativeLocalJumpBinding& b,const Pad& pad,const std::array<uint8_t,640>& previous,
    PPCContext& ctx,uint8_t* base,std::array<bool,2>& edges,std::array<uint8_t,640>& next){
    if(evaluating)return false;
    struct Guard {Guard(){evaluating=true;}~Guard(){evaluating=false;}} guard;
    Scratch scratch(ctx,base);if(!scratch.memory)return false;
    const auto manager=scratch.stack+512,input=manager+96,device=input+96,buttons=device+128;
    const auto actions=buttons+640,output=actions+880;
    std::memcpy(base+manager,base+b.movement.manager,96);std::memcpy(base+input,base+b.movement.input,88);
    std::memcpy(base+device,base+b.movement.device,96);std::memcpy(base+buttons,previous.data(),b.button_count*20);
    PPC_STORE_U32(manager+72,input);PPC_STORE_U32(input+72+b.movement.user*4,device);
    PPC_STORE_U32(device+52,buttons);
    // Only digital records are needed; native history advance otherwise also
    // advances axes through device+60, which must never point at live memory.
    PPC_STORE_U32(device+56,0);
    ctx.r3.u64=device;__imp__sub_830A8200(ctx,base);
    for(unsigned i=0;i<b.button_count;++i){
        if(i>=masks.size() || !masks[i])continue;
        const bool held=(pad.buttons&masks[i])!=0;
        PPC_STORE_U8(buttons+i*20,held?255:0);PPC_STORE_U8(buttons+i*20+1,held?1:0);
    }
    for(unsigned i=0;i<2;++i){
        const auto a=actions+i*440;std::memcpy(base+a,base+b.actions[i].action,436);
        PPC_STORE_U32(output,0);PPC_STORE_U32(output+4,0);
        ctx.r3.u64=manager;ctx.r4.u64=a;ctx.r5.u64=1;ctx.r6.u64=output;ctx.r7.u64=output+4;
        __imp__sub_827C5F30(ctx,base);
        if(ctx.r3.u32>1)return false;edges[i]=ctx.r3.u32!=0;
    }
    next=previous;std::memcpy(next.data(),base+buttons,b.button_count*20);return true;
}
bool Advance(Stream& state,const NativeLocalJumpBinding& b,const Pad& pad,uint64_t epoch,uint64_t tick,
    PPCContext& ctx,uint8_t* base,NativeLocalJumpEvents& output){
    output={};if(!state.active || epoch!=state.epoch || b!=state.binding)return false;
    if(state.cached && tick==state.last){if(pad!=state.pad)return false;output=state.output;return true;}
    if(state.exhausted || tick!=state.next)return false;
    std::array<bool,2> edges{};std::array<uint8_t,640> next;
    if(!Evaluate(b,pad,state.buttons,ctx,base,edges,next))return false;
    bool held=state.held;uint64_t sequence=state.sequence;NativeLocalJumpEvents result;result.button_mask=b.button_mask;
    // Exact native dispatch order is default event followed by active event,
    // then action index. Do not reorder two actual edges by their names.
    std::array<unsigned,2> order{0,1};
    auto ordinal=[&](unsigned i){return std::pair{b.actions[i].event==b.movement.manager+24?0u:1u,b.actions[i].index};};
    if(ordinal(1)<ordinal(0))std::swap(order[0],order[1]);
    for(auto i:order)if(edges[i]){
        if(sequence==UINT64_MAX || held==(i==0))return false;
        held=i==0;result.events[result.count++]={++sequence,i?RoomJumpEdge::Released:RoomJumpEdge::Pressed};
    }
    state.buttons=next;state.held=held;state.sequence=sequence;state.pad=pad;state.last=tick;
    state.cached=true;state.exhausted=tick==UINT64_MAX;if(!state.exhausted)state.next=tick+1;
    state.output=result;output=result;return true;
}
}
bool em2::net::DiscoverNativeLocalJumpBinding(const NativeLocalMovementBinding& m,PPCContext& ctx,uint8_t* base,NativeLocalJumpBinding& output){
    Environment preserve;output={};em2::GuestReadBatch reads;
    if(evaluating || GameplayInputGate().active() || !Parent(m,ctx,base) || !Readable(0x833DAEBC,8,base) || !PPC_LOAD_U32(0x833DAEBC) || !PPC_LOAD_U32(0x833DAEC0) ||
       !Readable(m.manager,96,base) || !Readable(m.input,88,base) || !Readable(m.device,96,base))return false;
    NativeLocalJumpBinding b;b.movement=m;std::array<unsigned,2> found{};
    for(const auto event:{m.manager+24,m.active_event}){
        if(!Readable(event,24,base))return false;
        const auto list=PPC_LOAD_U32(event+16),count=PPC_LOAD_U32(event+20);
        if(count>256 || (count && !Readable(list,size_t(count)*4,base)))return false;
        for(unsigned index=0;index<count;++index){const auto action=PPC_LOAD_U32(list+index*4);
            if(!Readable(action,4,base))return false;
            const auto uid=PPC_LOAD_U32(action);
            for(unsigned i=0;i<2;++i)if(uid==PPC_LOAD_U32(i?0x833DAEC0:0x833DAEBC)){
                if(++found[i]!=1)return false;auto& a=b.actions[i];a.event=event;a.action=action;a.index=index;
                if(!InspectAction(a,i,m,ctx,base))return false;
            }
        }
    }
    if(found[0]!=1 || found[1]!=1 || b.actions[0].key!=b.actions[1].key)return false;
    b.button_count=PPC_LOAD_U32(m.device+48);b.buttons=PPC_LOAD_U32(m.device+52);
    if(!b.button_count || b.button_count>32 || !Readable(b.buttons,b.button_count*20,base) ||
       PPC_LOAD_U32(m.device)!=0x821DE6E8 || PPC_LOAD_U32(0x821DE6E8+60)!=0x830A7FD0 ||
       PPC_LOAD_U32(0x821DE6E8+64)!=0x830A8030)return false;
    {
        Scratch scratch(ctx,base);if(!scratch.memory)return false;
        ctx.r4.u64=b.actions[0].key;__imp__sub_827C2D98(ctx,base);
        if(ctx.r3.u32>=b.button_count || ctx.r3.u32>=masks.size())return false;b.button_mask=masks[ctx.r3.u32];
    }
    if(!b.button_mask || !Parent(m,ctx,base))return false;output=b;return true;
}
bool em2::net::ArmNativeLocalJump(const NativeLocalJumpBinding& b,uint64_t epoch,uint64_t first,PPCContext& ctx,uint8_t* base){
    Environment preserve;NativeLocalJumpBinding current;
    if(evaluating || stream.active || !epoch || !DiscoverNativeLocalJumpBinding(b.movement,ctx,base,current) || current!=b)return false;
    Scratch scratch(ctx,base);if(!scratch.memory)return false;
    ctx.r4.u64=b.actions[0].key;__imp__sub_827C2D98(ctx,base);const auto button=b.buttons+ctx.r3.u32*20;
    // No manufactured Pressed/Released baseline when attaching mid-hold.
    if(PPC_LOAD_U8(button+1) || PPC_LOAD_U8(button+3))return false;
    stream={};stream.active=true;stream.thread=GetCurrentThreadId();stream.epoch=epoch;stream.next=first;stream.binding=b;
    std::memcpy(stream.buttons.data(),base+b.buttons,b.button_count*20);return true;
}
void em2::net::DisarmNativeLocalJump(){stream={};}
bool em2::net::ProduceNativeLocalJump(const NativeLocalJumpBinding& b,const Pad& pad,uint64_t epoch,uint64_t tick,
    PPCContext& ctx,uint8_t* base,NativeLocalJumpEvents& output,unsigned user){
    Environment preserve;output={};NativeLocalJumpBinding current;
    if(evaluating || !stream.active || stream.thread!=GetCurrentThreadId() || user!=b.movement.user)return false;
    if(!DiscoverNativeLocalJumpBinding(b.movement,ctx,base,current) || current!=b){stream.active=false;return false;}
    auto next=stream;NativeLocalJumpEvents result;
    if(!Advance(next,b,pad,epoch,tick,ctx,base,result))return false;
    if(!DiscoverNativeLocalJumpBinding(b.movement,ctx,base,current) || current!=b){stream.active=false;return false;}
    stream=next;output=result;return true;
}
namespace {
void JumpFixturePhysicalUser(PPCContext& ctx,uint8_t* base){ctx.r3.u64=PPC_LOAD_U32(ctx.r3.u32+4);}
}
bool em2::net::TestNativeLocalJump(){
    Environment preserve;const auto prior=stream;
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
    struct Restore {uint8_t* base;Stream prior;~Restore(){DisarmNativeLocalJump();DisarmNativeLocalMovement();stream=prior;if(base)VirtualFree(base,0,MEM_RELEASE);}} restore{base,prior};
    const auto path=std::filesystem::path(__FILE__).parent_path().parent_path()/"private/image.bin";
    std::error_code error;const auto size=std::filesystem::file_size(path,error);
    if(error || !base || !VirtualAlloc(base,0x50000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+PPC_IMAGE_BASE,size,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+PPC_IMAGE_BASE+PPC_IMAGE_SIZE,4096,MEM_COMMIT,PAGE_READWRITE))return false;
    std::ifstream image(path,std::ios::binary);if(!image.read(reinterpret_cast<char*>(base+PPC_IMAGE_BASE),size))return false;
    auto map=[&](uint32_t address,PPCFunc* function){
        const auto slot=reinterpret_cast<uintptr_t>(&PPC_LOOKUP_FUNC(base,address));
        if(!VirtualAlloc(reinterpret_cast<void*>(slot&~uintptr_t(4095)),4096,MEM_COMMIT,PAGE_READWRITE))return false;
        PPC_LOOKUP_FUNC(base,address)=function;return true;
    };
    if(!map(uint32_t(PPC_CODE_BASE),__imp__sub_827B58D0) || !map(uint32_t(PPC_CODE_BASE)+4,__imp__sub_82A69948) ||
       !map(uint32_t(PPC_CODE_BASE)+8,JumpFixturePhysicalUser) || !map(0x830A7FD0,__imp__sub_830A7FD0) ||
       !map(0x830A8030,__imp__sub_830A8030) || !map(0x830A8090,__imp__sub_830A8090) || !map(0x830A7968,__imp__sub_830A7968))return false;
    constexpr uint32_t registry=0x2000,players=0x3000,manager=0x4000,movement=0x5000,event=0x6000,input=0x7000,device=0x8000;
    constexpr uint32_t axes=0x9000,buttons=0xA000,entity=0x10000,controller=0x11000,types=0x12000,list=0x13000;
    constexpr uint32_t buckets=0x14000,node=0x15000,handle=0x1234,jump=0x24000,release=0x25000;
    auto context=[] {PPCContext c{};c.fpscr.InitHost();c.r1.u64=0x4F000;c.r13.u64=0x13572468;c.lr=0x24681357;return c;};
    auto put=[&](uint32_t p,float f){PPC_STORE_U32(p,std::bit_cast<uint32_t>(f));};
    PPC_STORE_U32(0x833DA7E4,registry);PPC_STORE_U32(0x833DAB60,players);PPC_STORE_U32(0x833DBAD8,manager);
    PPC_STORE_U32(0x833DAEB8,0x2B000);PPC_STORE_U32(0x833DAEBC,0x2A000);PPC_STORE_U32(0x833DAEC0,0x2A020);
    std::memcpy(base+0x2B000,"Movement",9);std::memcpy(base+0x2A000,"Jump",5);std::memcpy(base+0x2A020,"ReleaseJump",12);
    PPC_STORE_U32(registry+4,uint32_t(PPC_CODE_BASE));PPC_STORE_U32(registry+8,uint32_t(PPC_CODE_BASE)+4);
    PPC_STORE_U32(registry+16,buckets);PPC_STORE_U32(registry+20,4);PPC_STORE_U32(players+20,handle);
    PPC_STORE_U32(0x16000,handle);auto c=context();c.r3.u64=0x16000;__imp__sub_827B58D0(c,base);
    PPC_STORE_U32(buckets+(c.r3.u32%4)*4,node);PPC_STORE_U32(node,handle);PPC_STORE_U32(node+4,entity);
    PPC_STORE_U8(0x833E2188,3);PPC_STORE_U32(entity+88,types);PPC_STORE_U32(entity+36,list);
    std::memset(base+types,255,256);PPC_STORE_U8(types+3,0);PPC_STORE_U32(list,controller+24);
    PPC_STORE_U32(controller+24,0x8211DCE0);PPC_STORE_U32(controller+80,0x8211DCD0);PPC_STORE_U32(controller+64,entity-8);
    PPC_STORE_U32(manager+4,0x1D000);PPC_STORE_U32(manager+8,1);PPC_STORE_U32(0x1D000,event);
    PPC_STORE_U32(manager+16,0x820A4248);PPC_STORE_U32(manager+24,0x8208C4BC);PPC_STORE_U32(event,0x820A4248);
    PPC_STORE_U32(manager+72,input);PPC_STORE_U32(input+72,device);PPC_STORE_U32(device,0x821DE6E8);
    PPC_STORE_U32(device+48,16);PPC_STORE_U32(device+52,buttons);PPC_STORE_U32(device+56,8);PPC_STORE_U32(device+60,axes);
    for(unsigned i=0;i<8;++i){PPC_STORE_U32(axes+i*32+16,uint32_t(-32768));PPC_STORE_U32(axes+i*32+20,32767);
        PPC_STORE_U32(axes+i*32+24,uint32_t(-100));PPC_STORE_U32(axes+i*32+28,100);}
    PPC_STORE_U32(0x833DC87C,0x19000);PPC_STORE_U32(0x19000+12,0x1A000);PPC_STORE_U32(0x1A000,0x1B000);
    PPC_STORE_U32(0x1B000,0x1C000);PPC_STORE_U32(0x1C000+8,uint32_t(PPC_CODE_BASE)+8);
    PPC_STORE_U32(event+4,0x17000);PPC_STORE_U32(event+8,1);PPC_STORE_U32(0x17000,controller+80);
    PPC_STORE_U32(event+16,0x18000);PPC_STORE_U32(event+20,3);
    PPC_STORE_U32(0x18000,movement);PPC_STORE_U32(0x18004,jump);PPC_STORE_U32(0x18008,release);
    PPC_STORE_U32(movement,0x2B000);PPC_STORE_U32(movement+80,4);PPC_STORE_U32(movement+408,2);
    PPC_STORE_U32(movement+92,0x02400018);PPC_STORE_U32(movement+124,0x02400017);PPC_STORE_U32(movement+128,3);
    put(movement+416,.2f);put(movement+420,.95f);put(movement+424,1.6f);put(movement+428,1);PPC_STORE_U32(movement+432,2);
    PPC_STORE_U32(jump,0x2A000);PPC_STORE_U32(jump+80,2);PPC_STORE_U32(jump+408,1);
    PPC_STORE_U32(release,0x2A020);PPC_STORE_U32(release+80,3);PPC_STORE_U32(release+408,1);
    unsigned checks=0,cases=0;auto check=[&](bool value,const char* why){++checks;if(!value)REXLOG_ERROR("Native local Jump: {}",why);return value;};
    // Independent exhaustive native SDK mapping oracle. All buttons start held,
    // so ORIGINAL ACA80 takes only release/no-change paths: no lazy timer init.
    for(unsigned raw=0;raw<=65535;++raw){
        for(unsigned i=0;i<16;++i){PPC_STORE_U8(buttons+i*20,255);PPC_STORE_U8(buttons+i*20+1,1);}
        c=context();c.r3.u64=device;c.r4.u64=raw;__imp__sub_830ACA80(c,base);
        for(unsigned i=0;i<16;++i)if(masks[i] && bool(PPC_LOAD_U8(buttons+i*20+1))!=bool(raw&masks[i]))
            return check(false,"held-level mapping differs from ORIGINAL SDK button producer");
    }
    std::memset(base+buttons,0,640);
    if(!check(ArmNativeLocalMovement(9,registry,base),"explicit parent Movement ownership"))return false;
    NativeLocalMovementBinding parent;c=context();
    if(!check(DiscoverNativeLocalMovementBinding(c,base,parent),"pre-tick real parent lease resolves without fabricated callback"))return false;
    for(uint32_t key=0x01400001;key<=0x01400016;++key){
        auto index=context();index.r4.u64=key;__imp__sub_827C2D98(index,base);
        if(index.r3.u32>=masks.size() || !masks[index.r3.u32])continue;
        PPC_STORE_U32(jump+92,key);PPC_STORE_U32(release+92,key);NativeLocalJumpBinding binding;
        if(!check(DiscoverNativeLocalJumpBinding(parent,c,base,binding) && binding.button_mask==masks[index.r3.u32],"binding key maps through ORIGINAL device-code resolver to actual raw Pad mask"))return false;
        if(!check(ArmNativeLocalJump(binding,65,100,c,base),"new explicit semantic edge stream begins neutral"))return false;
        const std::array<bool,7> levels{false,true,true,true,false,false,true};
        uint64_t sequence=0;bool held=false;
        for(unsigned i=0;i<levels.size();++i){
            Pad pad;pad.buttons=levels[i]?binding.button_mask:0;NativeLocalJumpEvents events;
            std::vector<uint8_t> memory(base,base+0x50000);std::array<uint8_t,sizeof(PPCContext)> saved{};std::memcpy(saved.data(),&c,sizeof(c));
            _mm_setcsr(0x1F80);SetLastError(0x4321ABCD);
            const bool produced=ProduceNativeLocalJump(binding,pad,65,100+i,c,base,events);
            const auto last_error=GetLastError();const auto csr=_mm_getcsr();
            const bool edge=levels[i]!=held;
            if(!check(produced && events.button_mask==binding.button_mask && events.count==unsigned(edge) &&
                (!edge || (events.events[0].sequence==++sequence && events.events[0].edge==(levels[i]?RoomJumpEdge::Pressed:RoomJumpEdge::Released))),
                "ORIGINAL digital action yields ordered press/release with no held repeats"))return false;
            if(!check(std::memcmp(saved.data(),&c,sizeof(c))==0 && std::memcmp(memory.data(),base,memory.size())==0 && csr==0x1F80 && last_error==0x4321ABCD,
                "native digital production preserves full caller, guest input/history/actions/scratch, FP and LastError"))return false;
            NativeLocalJumpEvents again;
            if(!check(ProduceNativeLocalJump(binding,pad,65,100+i,c,base,again) && again==events,"same source tick repeats cached semantic events exactly"))return false;
            auto different=pad;different.buttons^=binding.button_mask;
            if(!check(!ProduceNativeLocalJump(binding,different,65,100+i,c,base,again),"different sample cannot overwrite committed source tick"))return false;
            held=levels[i];++cases;
        }
        NativeLocalJumpEvents unused;
        if(!check(!ProduceNativeLocalJump(binding,{},65,108,c,base,unused) && !ProduceNativeLocalJump(binding,{},66,107,c,base,unused) &&
            !ProduceNativeLocalJump(binding,{},65,107,c,base,unused,1),"gap, wrong authoritative epoch and wrong physical source reject without consuming tick"))return false;
        PPC_STORE_U32(jump+108,1);
        if(!check(!ProduceNativeLocalJump(binding,{},65,107,c,base,unused),"changed repeat policy invalidates native binding"))return false;
        PPC_STORE_U32(jump+108,0);DisarmNativeLocalJump();
    }
    if(!check(cases>7,"digital native fixtures cover multiple real mapped buttons rather than assuming A"))return false;
    // Exact captured retail key and pair: Jump kind2, JumpRelease kind3,
    // both curve0. Unused second records contain arbitrary content in retail.
    PPC_STORE_U32(jump+92,0x01400009);PPC_STORE_U32(release+92,0x01400009);
    std::memset(base+jump+120,0xAB,288);std::memset(base+release+120,0xCD,288);
    NativeLocalJumpBinding binding;
    if(!check(DiscoverNativeLocalJumpBinding(parent,c,base,binding) && binding.button_mask==0x1000,
        "captured native key derives1000 mask and ignores unused garbage records"))return false;
    auto key=context();key.r4.u64=binding.actions[0].key;__imp__sub_827C2D98(key,base);const auto button=buttons+key.r3.u32*20;
    PPC_STORE_U8(button+1,1);
    if(!check(!ArmNativeLocalJump(binding,65,0,c,base),"initial native hold cannot manufacture a Pressed baseline"))return false;
    PPC_STORE_U8(button+1,0);PPC_STORE_U8(button+3,1);
    if(!check(!ArmNativeLocalJump(binding,65,0,c,base),"initial native release history must settle before stream ownership"))return false;
    PPC_STORE_U8(button+3,0);
    if(!check(ArmNativeLocalJump(binding,65,0,c,base),"settled exact retail Jump pair arms"))return false;
    NativeLocalJumpEvents events;
    PPC_STORE_U32(jump+80,1);
    if(!check(!ProduceNativeLocalJump(binding,{},65,0,c,base,events),"replaced Jump interpretation invalidates producer"))return false;
    PPC_STORE_U32(jump+80,2);
    if(!check(!ProduceNativeLocalJump(binding,{},65,0,c,base,events),"restored action bytes cannot resurrect retired semantic stream"))return false;
    DisarmNativeLocalJump();
    PPC_STORE_U32(release+432,3);
    if(!check(!DiscoverNativeLocalJumpBinding(parent,c,base,binding),"curve3 dispatcher route is not mistaken for proved curve0 digital interpreter"))return false;
    PPC_STORE_U32(release+432,0);
    if(!check(DiscoverNativeLocalJumpBinding(parent,c,base,binding) && ArmNativeLocalJump(binding,65,UINT64_MAX,c,base),
        "final representable source tick can be owned explicitly"))return false;
    if(!check(ProduceNativeLocalJump(binding,{},65,UINT64_MAX,c,base,events) &&
        ProduceNativeLocalJump(binding,{},65,UINT64_MAX,c,base,events) && !ProduceNativeLocalJump(binding,{},65,0,c,base,events),
        "source tick exhaustion preserves exact repeats without wrapping"))return false;
    DisarmNativeLocalJump();DisarmNativeLocalMovement();PPC_STORE_U32(0x1B000+4,3);PPC_STORE_U32(input+84,device);
    if(!check(ArmNativeLocalMovement(10,registry,base,3) && DiscoverNativeLocalMovementBinding(c,base,parent) &&
        DiscoverNativeLocalJumpBinding(parent,c,base,binding) && ArmNativeLocalJump(binding,66,300,c,base),
        "logical user0 can own the actual selected native physical device3"))return false;
    if(!check(!ProduceNativeLocalJump(binding,{},66,300,c,base,events,0) && ProduceNativeLocalJump(binding,{},66,300,c,base,events,3),
        "Jump sample physical source is explicit and cannot be confused with logical user0"))return false;
    PPC_STORE_U32(0x1B000+4,2);
    if(!check(!ProduceNativeLocalJump(binding,{},66,301,c,base,events,3),"profile/device remap invalidates parent and Jump producer"))return false;
    PPC_STORE_U32(0x1B000+4,3);
    if(!check(!ProduceNativeLocalJump(binding,{},66,301,c,base,events,3),"restoring remapped profile cannot resurrect retired stream"))return false;
    if(checks)REXLOG_INFO("Native local Jump passed {} checks/{} source ticks: exhaustive ORIGINAL SDK mapping, ORIGINAL digital interpreter, parent leases, exact cache/sequence and no live input writes",checks,cases);
    return true;
}
