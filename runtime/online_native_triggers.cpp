#include "online_native_triggers.h"
#include "online_input_clock.h"
#include "online_local_source.h"
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
#include <rex/cvar.h>
REXCVAR_DECLARE(bool,em2_private_profile);
PPC_EXTERN_FUNC(__imp__sub_830A8200);
PPC_EXTERN_FUNC(__imp__sub_830A89B8);
PPC_EXTERN_FUNC(__imp__sub_830ACA80);
PPC_EXTERN_FUNC(__imp__sub_827C3278);
PPC_EXTERN_FUNC(__imp__sub_827C5F30);
PPC_EXTERN_FUNC(__imp__sub_827B58D0);
PPC_EXTERN_FUNC(__imp__sub_82A69948);
PPC_EXTERN_FUNC(__imp__sub_830A7FD0);
PPC_EXTERN_FUNC(__imp__sub_830A8030);
PPC_EXTERN_FUNC(__imp__sub_830A8090);
PPC_EXTERN_FUNC(__imp__sub_830A7968);
PPC_EXTERN_FUNC(__imp__sub_828F0360);
PPC_EXTERN_FUNC(__imp__sub_827C2D98);
namespace em2::net {
namespace {
struct Environment {unsigned fp=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(fp);SetLastError(error);}};
bool Readable(uint32_t p,size_t n,uint8_t* base){return base && p>=4096 && uint64_t(p)+n<=0x100000000ull && em2::GuestReadBatch::Readable(base+p,n);}
bool Parent(const NativeLocalMovementBinding& expected,PPCContext& ctx,uint8_t* base){NativeLocalMovementBinding current;
    return NativeLocalMovementBindingFor(ctx,base,current) && current==expected;}
bool ButtonKey(uint32_t key,PPCContext& ctx,uint8_t* base){
    if((key&0xffff0000)!=0x01400000)return false;
    // Original leaf decoder; preserve the caller instead of inventing another
    // mapping for shoulder/face/D-pad/stick buttons and analog trigger slots.
    auto local=ctx;local.r4.u64=key;__imp__sub_827C2D98(local,base);return local.r3.u32<16;
}
bool Member(uint32_t list,unsigned count,uint32_t target,unsigned limit,uint8_t* base){
    if(!count || count>limit || !Readable(list,count*4,base))return false;unsigned hits=0;
    for(unsigned i=0;i<count;++i)hits+=PPC_LOAD_U32(list+i*4)==target;return hits==1;
}
void Normalize(std::array<uint8_t,436>& shape,unsigned count){
    // Actual native 5F30 mutations, not immutable binding configuration.
    for(unsigned i=0;i<count;++i){shape[88+i*32+17]=0;std::fill_n(shape.begin()+88+i*32+24,8,uint8_t(0));}
    // Unused bindings/header bytes may contain unrelated allocator residue.
    std::fill(shape.begin()+4,shape.begin()+80,uint8_t(0));std::fill(shape.begin()+84,shape.begin()+88,uint8_t(0));
    std::fill(shape.begin()+88+count*32,shape.begin()+408,uint8_t(0));
}
bool Name(uint32_t p,uint8_t* base,std::string& out){if(!Readable(p,96,base))return false;
    const auto* start=reinterpret_cast<const char*>(base+p);const auto* end=static_cast<const char*>(std::memchr(start,0,96));
    if(!end || end==start)return false;out.assign(start,end);return true;}
struct Candidate {
    std::array<uint8_t,96> device{};
    std::array<uint8_t,640> buttons{};
    std::vector<std::array<uint8_t,436>> actions;
    NativeTriggerBatch batch;
    Pad pad{};bool changed=true;
};
struct Evaluation {const NativeTriggerBinding& binding;Candidate& next;bool post_validate=true;};
bool Evaluate(PPCContext& ctx,uint8_t* base,NativeInputClockScratch& scratch,void* opaque){
    auto& e=*static_cast<Evaluation*>(opaque);const auto& b=e.binding;auto& next=e.next;
    const auto manager=scratch.data(),input=manager+176,device=input+96,buttons=device+128,action=buttons+640,output=action+440;
    if(scratch.capacity()<output+8-manager || !Readable(b.parent.manager,176,base) || !Readable(b.parent.input,88,base))return false;
    // 3278 condition0 returns true before reading per-user condition state.
    // Reserve the entire observed +96-based eight-user state array anyway;
    // native copies remain bounded through manager+175.
    std::memcpy(base+manager,base+b.parent.manager,176);std::memcpy(base+input,base+b.parent.input,88);
    std::memcpy(base+device,next.device.data(),96);std::memcpy(base+buttons,next.buttons.data(),b.button_count*20);
    PPC_STORE_U32(manager+72,input);PPC_STORE_U32(input+72+b.parent.user*4,device);PPC_STORE_U32(device+52,buttons);
    // This slice has no axis/aim claim. Even history maintenance cannot follow
    // a copied pointer into a live axis array.
    PPC_STORE_U32(device+56,0);PPC_STORE_U32(device+60,0);
    if(!scratch.OwnButtonHistory(buttons,b.button_count))return false;
    ctx.r3.u64=device;__imp__sub_830A8200(ctx,base);
    // Mirrors 830AD1A8: maintenance every sample, level updates only a changed
    // SDK packet. Full Pad equality includes stick axes, not just triggers.
    if(next.changed){
        ctx.r3.u64=device;ctx.r4.u64=5;ctx.r5.u64=next.pad.left_trigger;__imp__sub_830A89B8(ctx,base);
        ctx.r3.u64=device;ctx.r4.u64=7;ctx.r5.u64=next.pad.right_trigger;__imp__sub_830A89B8(ctx,base);
        ctx.r3.u64=device;ctx.r4.u64=next.pad.buttons;__imp__sub_830ACA80(ctx,base);
    }
    next.batch.count=unsigned(b.actions.size());
    for(unsigned i=0;i<b.actions.size();++i){
        std::memcpy(base+action,next.actions[i].data(),436);
        if(b.actions[i].kind==2 && !scratch.OwnActionRecord(action))return false;
        ctx.r3.u64=manager;ctx.r4.u64=action;__imp__sub_827C3278(ctx,base);const auto enabled=ctx.r3.u32;
        if(enabled>1)return false;
        PPC_STORE_U32(output,0);PPC_STORE_U32(output+4,0);
        ctx.r3.u64=manager;ctx.r4.u64=action;ctx.r5.u64=enabled;ctx.r6.u64=output;ctx.r7.u64=output+4;
        __imp__sub_827C5F30(ctx,base);
        if(ctx.r3.u32>1)return false;
        const auto x=std::bit_cast<float>(PPC_LOAD_U32(output)),y=std::bit_cast<float>(PPC_LOAD_U32(output+4));
        if(!std::isfinite(x) || !std::isfinite(y))return false;
        next.batch.actions[i]={uint16_t(i),ctx.r3.u32!=0,x,y};std::memcpy(next.actions[i].data(),base+action,436);
    }
    std::memcpy(next.device.data(),base+device,96);std::memcpy(next.buttons.data(),base+buttons,b.button_count*20);
    // All of this is still staged. A stale parent/event/action/threshold must
    // make the clock callback fail BEFORE source time commits.
    if(e.post_validate){NativeTriggerBinding current;if(!DiscoverNativeTriggerBinding(b.parent,ctx,base,current,b.scope) || current!=b)return false;}
    return true;
}
}
struct NativeTriggerSource::Impl {
    DWORD thread=0;uint8_t* base=nullptr;uint64_t epoch=0,next=0,last=0;
    bool armed=false,cached=false,exhausted=false;
    NativeTriggerBinding binding;NativeInputClockState clock;
    Candidate committed;Pad last_pad{};
};
struct NativeTriggerAccess {
    static auto& Get(NativeTriggerSource& source){return *source.impl_;}
    static void Clear(NativeTriggerSource& source){source.impl_=std::make_unique<NativeTriggerSource::Impl>();}
};
NativeTriggerSource::NativeTriggerSource():impl_(std::make_unique<Impl>()){}
NativeTriggerSource::~NativeTriggerSource()=default;
bool DiscoverNativeTriggerBinding(const NativeLocalMovementBinding& parent,PPCContext& ctx,uint8_t* base,NativeTriggerBinding& out,NativeButtonScope scope){
    Environment environment;em2::GuestReadBatch reads;out={};
    if(scope!=NativeButtonScope::TriggersOnly && scope!=NativeButtonScope::DigitalAndTriggers)return false;
    if(!Parent(parent,ctx,base) || parent.user>3 || parent.logical_user>1 || !Readable(parent.manager,176,base) ||
       !Readable(parent.device,96,base) || PPC_LOAD_U32(parent.device)!=0x821DE6E8 ||
       !Readable(0x833B65F8,4,base) || PPC_LOAD_U32(0x821DE6E8+112)!=0x830A7968 ||
       PPC_LOAD_U32(0x821DE6E8+60)!=0x830A7FD0 || PPC_LOAD_U32(0x821DE6E8+64)!=0x830A8030 ||
       PPC_LOAD_U32(0x821DE6E8+68)!=0x830A8090)return false;
    NativeTriggerBinding b;b.parent=parent;b.scope=scope;b.threshold=PPC_LOAD_U32(0x833B65F8);b.button_count=PPC_LOAD_U32(parent.device+48);b.buttons=PPC_LOAD_U32(parent.device+52);
    if(!Name(parent.active_event_uid,base,b.active_context_name))return false;
    if(!b.threshold || b.threshold>255 || b.button_count!=16 || !Readable(b.buttons,b.button_count*20,base))return false;
    std::array<uint32_t,2> contexts{parent.manager+24,parent.active_event};
    for(unsigned context=0;context<contexts.size();++context){const auto event=contexts[context];if(context && event==contexts[0])continue;
        if(!Readable(event,24,base))return false;const auto list=PPC_LOAD_U32(event+16),count=PPC_LOAD_U32(event+20);
        if(count>256 || (count && !Readable(list,count*4,base)))return false;
        NativeTriggerContext snapshot;snapshot.event=event;
        for(unsigned i=0;i<6;++i)snapshot.header[i]=PPC_LOAD_U32(event+i*4);
        const auto listener_list=snapshot.header[1],listener_count=snapshot.header[2];
        if(listener_count>128 || (listener_count && !Readable(listener_list,listener_count*4,base)))return false;
        for(unsigned i=0;i<listener_count;++i)snapshot.listeners.push_back(PPC_LOAD_U32(listener_list+i*4));
        snapshot.retail_listener=std::count(snapshot.listeners.begin(),snapshot.listeners.end(),parent.listener)==1;
        // Default UI listeners are not retail controllers. Their ordered math
        // may be inspected, but this does not admit their UI/pause callbacks.
        if(context && !snapshot.retail_listener)return false;
        for(unsigned i=0;i<count;++i)snapshot.action_order.push_back(PPC_LOAD_U32(list+i*4));
        b.contexts.push_back(snapshot);
        for(unsigned index=0;index<count;++index){const auto record=PPC_LOAD_U32(list+index*4);
            if(!Readable(record,436,base))return false;const auto bindings=PPC_LOAD_U32(record+408);
            if(bindings>10)return false;bool selected=false;
            for(unsigned j=0;j<bindings;++j){const auto p=record+88+j*32,key=PPC_LOAD_U32(p+4);
                selected|=PPC_LOAD_U32(p)==parent.logical_user && (scope==NativeButtonScope::DigitalAndTriggers?
                    (key&0xffff0000)==0x01400000:(key==0x01400006 || key==0x01400008));}
            if(!selected)continue;
            if(!Member(list,count,record,256,base))return false;
            // A shared native record would share its latch/timer across both
            // passes. This isolated per-descriptor state cannot split it.
            if(std::any_of(b.actions.begin(),b.actions.end(),[&](const auto& a){return a.action==record;}) ||
               std::any_of(b.excluded.begin(),b.excluded.end(),[&](const auto& a){return a.action==record;}))return false;
            const auto kind=PPC_LOAD_U32(record+80),condition=PPC_LOAD_U32(record+412),curve=PPC_LOAD_U32(record+432);
            const float repeat=std::bit_cast<float>(PPC_LOAD_U32(record+108));
            std::string unsupported;
            if(bindings!=1)unsupported="multiple/mixed bindings outside single-button slice";
            else if(!ButtonKey(PPC_LOAD_U32(record+92),ctx,base))unsupported="unrecognized native button key";
            else if(kind>3)unsupported="native kind outside digital0/1/2/3 slice";
            else if(condition || curve)unsupported="native condition/curve outside zero-selector slice";
            else if(PPC_LOAD_U32(record+96) || PPC_LOAD_U32(record+100) || PPC_LOAD_U8(record+104))unsupported="unsupported native destination/flags/inversion";
            else if(!std::isfinite(repeat) || repeat<0 || repeat>60)unsupported="unsupported repeat interval";
            if(!unsupported.empty()){
                if(b.excluded.size()==kNativeTriggerActions)return false;b.excluded.push_back({event,record,index,kind,condition,curve,std::move(unsupported)});continue;}
            if(b.actions.size()==kNativeTriggerActions)return false;
            NativeTriggerAction a;a.event=event;a.action=record;a.ordinal=index;a.context=context;a.uid=PPC_LOAD_U32(record);a.kind=kind;a.key=PPC_LOAD_U32(record+92);
            if(!Name(a.uid,base,a.name))return false;std::memcpy(a.shape.data(),base+record,436);Normalize(a.shape,1);b.actions.push_back(std::move(a));
        }
    }
    for(const auto& snapshot:b.contexts){
        for(unsigned i=0;i<6;++i)if(PPC_LOAD_U32(snapshot.event+i*4)!=snapshot.header[i])return false;
        for(unsigned i=0;i<snapshot.listeners.size();++i)if(PPC_LOAD_U32(snapshot.header[1]+i*4)!=snapshot.listeners[i])return false;
        for(unsigned i=0;i<snapshot.action_order.size();++i)if(PPC_LOAD_U32(snapshot.header[4]+i*4)!=snapshot.action_order[i])return false;
    }
    if(b.actions.empty() || !Parent(parent,ctx,base) || PPC_LOAD_U32(0x833B65F8)!=b.threshold)return false;
    // Only this retail receiver's subscription belongs to binding identity.
    // Authored cameras legitimately add/remove other native observers.
    for(auto& snapshot:b.contexts){
        const auto own=std::count(snapshot.listeners.begin(),snapshot.listeners.end(),parent.listener);
        if(own>1)return false;
        snapshot.listeners.clear();if(own)snapshot.listeners.push_back(parent.listener);
        snapshot.header[1]=0;snapshot.header[2]=uint32_t(own);snapshot.header[3]=0;
    }
    out=std::move(b);return true;
}
bool ArmNativeTriggers(NativeTriggerSource& source,const NativeTriggerBinding& binding,uint64_t epoch,uint64_t first,PPCContext& ctx,uint8_t* base){
    Environment environment;auto& s=NativeTriggerAccess::Get(source);NativeTriggerBinding current;
    if(s.armed || !epoch || !DiscoverNativeTriggerBinding(binding.parent,ctx,base,current,binding.scope) || current!=binding)return false;
    for(unsigned index=0;index<binding.button_count;++index){
        if(binding.scope==NativeButtonScope::TriggersOnly && index!=5 && index!=7)continue;
        if(PPC_LOAD_U8(binding.buttons+index*20+1) || PPC_LOAD_U8(binding.buttons+index*20+3))return false;
    }
    Candidate candidate;auto retained=binding;
    std::memcpy(candidate.device.data(),base+binding.parent.device,96);
    // Explicit fresh neutral private baseline. Do not import wall-clock press
    // stamps or repeat timers from the live device/action allocations.
    for(unsigned offset:{20u,40u,44u})std::fill_n(candidate.device.begin()+offset,4,uint8_t(0));
    candidate.actions.reserve(binding.actions.size());for(const auto& action:binding.actions)candidate.actions.push_back(action.shape);
    if(!InitializeNativeInputClock(s.clock,epoch,first,ctx,base))return false;
    s.thread=GetCurrentThreadId();s.base=base;s.epoch=epoch;s.next=first;s.binding=std::move(retained);s.committed=std::move(candidate);
    s.armed=true;return true;
}
bool ResetNativeTriggers(NativeTriggerSource& source){Environment environment;auto& s=NativeTriggerAccess::Get(source);
    if(s.armed && s.thread!=GetCurrentThreadId())return false;if(!ResetNativeInputClock(s.clock))return false;
    NativeTriggerAccess::Clear(source);return true;
}
bool ValidateNativeButtonDefinitionSnapshot(const NativeTriggerBinding& b,uint8_t* base){
    Environment environment;em2::GuestReadBatch reads;
    if(!Readable(0x833DBAD8,4,base) || PPC_LOAD_U32(0x833DBAD8)!=b.parent.manager ||
       !Readable(b.parent.manager,176,base) || PPC_LOAD_U32(b.parent.manager+16)!=b.parent.active_event_uid ||
       !Readable(0x833B65F8,4,base) || PPC_LOAD_U32(0x833B65F8)!=b.threshold)return false;
    std::string name;if(!Name(b.parent.active_event_uid,base,name) || name!=b.active_context_name)return false;
    const std::array<uint32_t,2> events{b.parent.manager+24,b.parent.active_event};
    if(b.contexts.size()!=(events[0]==events[1]?1u:2u))return false;
    for(unsigned c=0;c<b.contexts.size();++c){const auto& snapshot=b.contexts[c];
        const unsigned expected=snapshot.retail_listener?1u:0u;
        if(snapshot.event!=events[c] || !Readable(snapshot.event,24,base) || snapshot.listeners.size()!=expected ||
           snapshot.header[1] || snapshot.header[2]!=expected || snapshot.header[3] || snapshot.action_order.size()>256 ||
           (expected && snapshot.listeners[0]!=b.parent.listener))return false;
        for(unsigned i:{0u,4u,5u})if(PPC_LOAD_U32(snapshot.event+i*4)!=snapshot.header[i]){
            if(REXCVAR_GET(em2_private_profile))REXLOG_ERROR("Native button context header changed: context {} word {} expected {:08X} actual {:08X}",c,i,snapshot.header[i],PPC_LOAD_U32(snapshot.event+i*4));
            return false;
        }
        const auto listeners=PPC_LOAD_U32(snapshot.event+4),count=PPC_LOAD_U32(snapshot.event+8);
        if(count>128 || (count && !Readable(listeners,size_t(count)*4,base)))return false;
        unsigned own=0;for(unsigned i=0;i<count;++i){const auto listener=PPC_LOAD_U32(listeners+i*4);if(!Readable(listener,4,base))return false;own+=listener==b.parent.listener;}
        if(own!=expected)return false;
        if(snapshot.header[5]!=snapshot.action_order.size() ||
           (!snapshot.action_order.empty() && !Readable(snapshot.header[4],snapshot.action_order.size()*4,base)))return false;
        for(unsigned i=0;i<snapshot.action_order.size();++i)if(PPC_LOAD_U32(snapshot.header[4]+i*4)!=snapshot.action_order[i]){
            if(REXCVAR_GET(em2_private_profile))REXLOG_ERROR("Native button action order changed: context {} index {}",c,i);
            return false;
        }
    }
    for(const auto& a:b.actions){
        if(a.context>=b.contexts.size() || a.event!=events[a.context] || a.ordinal>=b.contexts[a.context].action_order.size() ||
           b.contexts[a.context].action_order[a.ordinal]!=a.action || !Readable(a.action,436,base))return false;
        std::array<uint8_t,436> shape;std::memcpy(shape.data(),base+a.action,shape.size());
        if(PPC_LOAD_U32(a.action+408)!=1 || PPC_LOAD_U32(a.action)!=a.uid || !Name(a.uid,base,name) || name!=a.name)return false;
        Normalize(shape,1);if(shape!=a.shape){
            if(REXCVAR_GET(em2_private_profile))for(unsigned i=0;i<shape.size();++i)if(shape[i]!=a.shape[i]){
                REXLOG_ERROR("Native button action shape changed: '{}' offset {} expected {:02X} actual {:02X}",a.name,i,a.shape[i],shape[i]);break;
            }
            return false;
        }
    }
    return true;
}
bool ProduceNativeTriggers(NativeTriggerSource& source,const NativeTriggerBinding& binding,const Pad& pad,unsigned user,uint64_t epoch,uint64_t tick,
    PPCContext& ctx,uint8_t* base,NativeTriggerBatch& output){
    Environment environment;output={};auto& s=NativeTriggerAccess::Get(source);NativeTriggerBinding current;
    if(!s.armed || s.thread!=GetCurrentThreadId() || s.base!=base || epoch!=s.epoch || binding!=s.binding || user!=binding.parent.user ||
       !DiscoverNativeTriggerBinding(binding.parent,ctx,base,current,binding.scope) || current!=binding)return false;
    if(s.cached && tick==s.last){if(pad!=s.last_pad)return false;output=s.committed.batch;return true;}
    if(s.exhausted || tick!=s.next)return false;
    Candidate candidate=s.committed;candidate.pad=pad;candidate.changed=!s.cached || pad!=s.last_pad;candidate.batch={};
    candidate.batch.source_epoch=epoch;candidate.batch.source_tick=tick;
    Evaluation evaluation{binding,candidate};
    if(!RunNativeInputClock(s.clock,epoch,tick,ctx,base,Evaluate,&evaluation))return false;
    s.committed=std::move(candidate);s.last_pad=pad;s.last=tick;s.cached=true;s.exhausted=tick==UINT64_MAX;if(!s.exhausted)s.next=tick+1;
    output=s.committed.batch;return true;
}
bool ProduceNativeTriggers(const Room& room,NativeTriggerSource& source,const NativeTriggerBinding& binding,const NativeRoutedPadSample& sample,
    uint64_t epoch,PPCContext& ctx,uint8_t* base,NativeTriggerBatch& output){
    Environment environment;output={};
    return ValidateNativeLocalSource(room,sample,binding.parent,ctx,base) &&
        ProduceNativeTriggers(source,binding,sample.pad(),sample.native_user(),epoch,sample.source_tick(),ctx,base,output);
}
bool TestNativeTriggers(){
    Environment environment;
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
    struct Release {uint8_t* base;~Release(){DisarmNativeLocalMovement();if(base)VirtualFree(base,0,MEM_RELEASE);}}release{base};
    const auto path=std::filesystem::path(__FILE__).parent_path().parent_path()/"private/image.bin";
    std::error_code error;const auto size=std::filesystem::file_size(path,error);
    if(!base || error || !VirtualAlloc(base,0x80000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+PPC_IMAGE_BASE,size,MEM_COMMIT,PAGE_READWRITE) || !VirtualAlloc(base+PPC_IMAGE_BASE+PPC_IMAGE_SIZE,4096,MEM_COMMIT,PAGE_READWRITE))return false;
    std::ifstream image(path,std::ios::binary);if(!image.read(reinterpret_cast<char*>(base+PPC_IMAGE_BASE),size))return false;
    auto map=[&](uint32_t address,PPCFunc* function){auto* pointer=&PPC_LOOKUP_FUNC(base,address);
        if(!VirtualAlloc(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pointer)&~uintptr_t(4095)),4096,MEM_COMMIT,PAGE_READWRITE))return false;*pointer=function;return true;};
    if(!map(uint32_t(PPC_CODE_BASE),__imp__sub_827B58D0) || !map(uint32_t(PPC_CODE_BASE)+4,__imp__sub_82A69948) ||
       !map(0x828F0360,__imp__sub_828F0360) || !map(0x830A7FD0,__imp__sub_830A7FD0) || !map(0x830A8030,__imp__sub_830A8030) ||
       !map(0x830A8090,__imp__sub_830A8090) || !map(0x830A7968,__imp__sub_830A7968))return false;
    constexpr uint32_t registry=0x2000,players=0x3000,buckets=0x5000,entity=0x6000,controller=0x7000,types=0x8000,components=0x9000,node=0xA000;
    constexpr uint32_t manager=0x10000,event=0x11000,event_name=0x12000,event_list=0x13000,movement=0x14000;
    constexpr uint32_t listeners=0x17000,actions=0x18000,device=0x20000,buttons=0x21000,axes=0x22000,input=0x23000;
    constexpr uint32_t profile=0x24000,user_object=0x25000,user_table=0x26000,users=0x27000,trigger_actions=0x28000,names=0x2B000;
    auto context=[](){PPCContext ctx{};ctx.fpscr.InitHost();ctx.r1.u64=0x7F000;ctx.lr=0x12345678;ctx.r13.u64=0x24681357;return ctx;};
    auto ctx=context();auto put=[&](uint32_t at,float value){PPC_STORE_U32(at,std::bit_cast<uint32_t>(value));};
    PPC_STORE_U32(0x833DA7E4,registry);PPC_STORE_U32(0x833DAB60,players);PPC_STORE_U32(0x833DBAD8,manager);PPC_STORE_U32(0x833DC87C,profile);
    PPC_STORE_U32(registry+4,uint32_t(PPC_CODE_BASE));PPC_STORE_U32(registry+8,uint32_t(PPC_CODE_BASE)+4);PPC_STORE_U32(registry+16,buckets);PPC_STORE_U32(registry+20,4);
    PPC_STORE_U32(0xB000,123);ctx.r3.u64=0xB000;__imp__sub_827B58D0(ctx,base);
    PPC_STORE_U32(buckets+(ctx.r3.u32%4)*4,node);PPC_STORE_U32(node,123);PPC_STORE_U32(node+4,entity);PPC_STORE_U32(players+20,123);
    PPC_STORE_U32(entity+88,types);PPC_STORE_U32(entity+36,components);std::memset(base+types,255,256);
    PPC_STORE_U8(0x833E2188,3);PPC_STORE_U8(types+3,0);PPC_STORE_U32(components,controller+24);
    PPC_STORE_U32(controller+24,0x8211DCE0);PPC_STORE_U32(controller+80,0x8211DCD0);PPC_STORE_U32(controller+64,entity-8);
    std::memcpy(base+event_name,"game",5);PPC_STORE_U32(manager+4,event_list);PPC_STORE_U32(manager+8,1);PPC_STORE_U32(event_list,event);
    PPC_STORE_U32(manager+16,event_name);PPC_STORE_U32(event,event_name);PPC_STORE_U32(manager+24,0x8208C4BC);
    PPC_STORE_U32(event+4,listeners);PPC_STORE_U32(event+8,1);PPC_STORE_U32(listeners,controller+80);PPC_STORE_U32(event+16,actions);PPC_STORE_U32(event+20,10);
    PPC_STORE_U32(actions,movement);PPC_STORE_U32(movement,names);std::memcpy(base+names,"Movement",9);PPC_STORE_U32(0x833DAEB8,names);
    PPC_STORE_U32(movement+80,4);PPC_STORE_U32(movement+408,2);PPC_STORE_U32(movement+92,0x02400018);PPC_STORE_U32(movement+124,0x02400017);PPC_STORE_U32(movement+128,3);
    put(movement+416,.2f);put(movement+420,.95f);put(movement+424,1.6f);put(movement+428,1);PPC_STORE_U32(movement+432,2);
    PPC_STORE_U32(manager+72,input);PPC_STORE_U32(input+76,device);PPC_STORE_U32(device,0x821DE6E8);
    PPC_STORE_U32(device+48,16);PPC_STORE_U32(device+52,buttons);PPC_STORE_U32(device+56,8);PPC_STORE_U32(device+60,axes);
    for(unsigned i=0;i<8;++i){PPC_STORE_U32(axes+i*32+16,uint32_t(-32768));PPC_STORE_U32(axes+i*32+20,32767);
        PPC_STORE_U32(axes+i*32+24,uint32_t(-100));PPC_STORE_U32(axes+i*32+28,100);}
    PPC_STORE_U32(profile+12,users);PPC_STORE_U32(users,user_object);PPC_STORE_U32(user_object,user_table);PPC_STORE_U32(user_object+4,1);PPC_STORE_U32(user_table+8,0x828F0360);
    PPC_STORE_U8(0x83396930,0);PPC_STORE_U64(0x833D03C0,100000000);PPC_STORE_U64(0x833D03C8,1000003);PPC_STORE_U64(0x833D04D8,1000003);put(0x833D04E0,float(1./1000003.));PPC_STORE_U32(0x833B65F8,32);
    constexpr std::array<unsigned,8> kinds{0,3,2,1,0,3,2,1};
    constexpr std::array<const char*,8> labels{"BlastL","EndL","PressL","StartL","BlastR","EndR","PressR","StartR"};
    for(unsigned i=0;i<9;++i){const auto record=trigger_actions+i*512,name=names+64+i*64;
        const auto label=i<8?labels[i]:"VehicleL";std::memcpy(base+name,label,std::strlen(label)+1);
        PPC_STORE_U32(actions+(i+1)*4,record);PPC_STORE_U32(record,name);PPC_STORE_U32(record+80,i<8?kinds[i]:4);
        PPC_STORE_U32(record+408,1);PPC_STORE_U32(record+92,i<4 || i==8?0x01400006:0x01400008);put(record+420,1);put(record+424,1);put(record+428,1);
    }
    unsigned checks=0;auto check=[&](bool value,const char* why){++checks;if(!value)REXLOG_ERROR("Native trigger math: {}",why);return value;};
    DisarmNativeLocalMovement();ctx=context();
    if(!check(ArmNativeLocalMovement(97,registry,base,1,0),"explicit current retail/source profile ownership"))return false;
    NativeLocalMovementBinding parent;if(!check(DiscoverNativeLocalMovementBinding(ctx,base,parent) && parent.user==1 && parent.logical_user==0,
        "original profile resolves logical0 to actual native device1"))return false;
    NativeTriggerBinding binding;if(!check(DiscoverNativeTriggerBinding(parent,ctx,base,binding) && binding.actions.size()==8 && binding.excluded.size()==1,
        "all eligible trigger alternatives retained in authored order, vehicle kind4 explicitly excluded"))return false;
    {
        NativeTriggerSource subscriptions;
        if(!check(ArmNativeTriggers(subscriptions,binding,0x73,0,ctx,base),"observer-lifetime source arms with exact actor subscription"))return false;
        const std::array<uint32_t,3> saved{PPC_LOAD_U32(event+4),PPC_LOAD_U32(event+8),PPC_LOAD_U32(event+12)};
        const auto observer_list=listeners+256,observer=listeners+384;
        PPC_STORE_U32(observer,0x82126C98);PPC_STORE_U32(observer_list,controller+80);PPC_STORE_U32(observer_list+4,observer);
        PPC_STORE_U32(event+4,observer_list);PPC_STORE_U32(event+8,2);PPC_STORE_U32(event+12,2);
        NativeTriggerBinding changed;NativeTriggerBatch sample;
        if(!check(ValidateNativeButtonDefinitionSnapshot(binding,base) && DiscoverNativeTriggerBinding(parent,ctx,base,changed) && changed==binding &&
           ProduceNativeTriggers(subscriptions,binding,{},1,0x73,0,ctx,base,sample),"unrelated observer insertion/list reallocation preserves source, schema and original native button math"))return false;
        PPC_STORE_U32(event+8,1);
        if(!check(ValidateNativeButtonDefinitionSnapshot(binding,base) && ProduceNativeTriggers(subscriptions,binding,{},1,0x73,1,ctx,base,sample),"unrelated camera observer removal preserves native source history"))return false;
        PPC_STORE_U32(observer_list,observer);
        if(!check(!ValidateNativeButtonDefinitionSnapshot(binding,base),"removing intended actor listener still invalidates receiver"))return false;
        PPC_STORE_U32(observer_list,controller+80);PPC_STORE_U32(observer_list+4,controller+80);PPC_STORE_U32(event+8,2);
        if(!check(!ValidateNativeButtonDefinitionSnapshot(binding,base),"duplicate actor subscription cannot pass as an unrelated observer change"))return false;
        PPC_STORE_U32(event+4,0xDEAD0000);
        if(!check(!ValidateNativeButtonDefinitionSnapshot(binding,base),"unreadable listener storage rejects before dispatch"))return false;
        PPC_STORE_U32(event+4,saved[0]);PPC_STORE_U32(event+8,saved[1]);PPC_STORE_U32(event+12,saved[2]);
    }
    std::array<uint8_t,96> live_device;std::memcpy(live_device.data(),base+device,96);
    std::array<uint8_t,320> live_buttons;std::memcpy(live_buttons.data(),base+buttons,320);
    std::array<uint8_t,8192> live_stack;std::memcpy(live_stack.data(),base+ctx.r1.u32-8192,8192);
    std::array<uint8_t,sizeof(PPCContext)> caller;std::memcpy(caller.data(),&ctx,sizeof(ctx));
    const auto fp=_mm_getcsr();const auto last=GetLastError();
    auto dispatched=[&](const NativeTriggerBatch& batch,const char* name){
        for(unsigned i=0;i<binding.actions.size();++i)if(binding.actions[i].name==name)return batch.actions[i].dispatch;return false;};
    NativeTriggerSource levels;NativeTriggerBatch batch;Pad pad;
    if(!check(!ProduceNativeTriggers(levels,binding,pad,1,9,0,ctx,base,batch) && ArmNativeTriggers(levels,binding,9,0,ctx,base),"producer defaults unarmed and pins source epoch"))return false;
    if(!check(!ProduceNativeTriggers(levels,binding,pad,0,9,0,ctx,base,batch),"foreign sampled native user cannot impersonate selected device1"))return false;
    constexpr std::array<unsigned,5> raw{0,31,32,33,255};
    for(unsigned i=0;i<raw.size();++i){pad.left_trigger=uint8_t(raw[i]);
        if(!check(ProduceNativeTriggers(levels,binding,pad,1,9,i,ctx,base,batch),"original private trigger/device/getter interpreter evaluates native thresholds"))return false;
        if(!check(dispatched(batch,"PressL")==(i==2) && !dispatched(batch,"StartL"),"0/31/32/33/255 preserve original32 threshold and one press edge"))return false;
    }
    if(!check(!std::memcmp(live_device.data(),base+device,96) && !std::memcmp(live_buttons.data(),base+buttons,320) &&
        !std::memcmp(live_stack.data(),base+ctx.r1.u32-8192,8192) && !std::memcmp(caller.data(),&ctx,sizeof(ctx)) && fp==_mm_getcsr() && last==GetLastError(),
        "all original computations preserve live device, histories, context, scratch, FP and LastError"))return false;
    NativeTriggerSource short_tap;pad={};pad.left_trigger=255;
    if(!check(ArmNativeTriggers(short_tap,binding,10,0,ctx,base) && ProduceNativeTriggers(short_tap,binding,pad,1,10,0,ctx,base,batch),"short-tap source begins from explicit neutral private history"))return false;
    const auto first=batch;const auto state_before=NativeTriggerAccess::Get(short_tap).committed.buttons;
    if(!check(ProduceNativeTriggers(short_tap,binding,pad,1,10,0,ctx,base,batch) && batch==first &&
        NativeTriggerAccess::Get(short_tap).committed.buttons==state_before,"same source tick returns exact cached output without advancing native history"))return false;
    auto altered=pad;altered.left_trigger=254;
    if(!check(!ProduceNativeTriggers(short_tap,binding,altered,1,10,0,ctx,base,batch) && !ProduceNativeTriggers(short_tap,binding,pad,1,11,1,ctx,base,batch) &&
        !ProduceNativeTriggers(short_tap,binding,pad,1,10,2,ctx,base,batch),"same-tick mutation, foreign epoch and skipped tick reject transactionally"))return false;
    if(!ProduceNativeTriggers(short_tap,binding,pad,1,10,1,ctx,base,batch))return false;
    pad.left_trigger=0;
    if(!check(ProduceNativeTriggers(short_tap,binding,pad,1,10,2,ctx,base,batch) && dispatched(batch,"BlastL") && dispatched(batch,"EndL") &&
        !dispatched(batch,"StartL") && batch.actions[0].descriptor==0 && batch.actions[1].descriptor==1,
        "original short tap emits Blast then End in authored order, never long-hold Start"))return false;
    NativeTriggerSource held;pad={};pad.left_trigger=pad.right_trigger=255;
    if(!ArmNativeTriggers(held,binding,12,0,ctx,base))return false;
    for(unsigned tick=0;tick<=9;++tick){
        if(!check(ProduceNativeTriggers(held,binding,pad,1,12,tick,ctx,base,batch),"simultaneous trigger histories retain native source-tick hold duration"))return false;
        if(!check(dispatched(batch,"StartL")==(tick==8) && dispatched(batch,"StartR")==(tick==8) &&
            dispatched(batch,"PressL")==!tick && dispatched(batch,"PressR")==!tick,"original quarter-second latch crosses at8, no held repeat and independent left/right press"))return false;
    }
    pad={};
    if(!check(ProduceNativeTriggers(held,binding,pad,1,12,10,ctx,base,batch) && !dispatched(batch,"BlastL") && !dispatched(batch,"BlastR") &&
        dispatched(batch,"EndL") && dispatched(batch,"EndR"),"long trigger holds release both original End actions without short-tap Blast"))return false;
    const auto history_before=NativeTriggerAccess::Get(held).committed.buttons;
    Candidate abandoned=NativeTriggerAccess::Get(held).committed;abandoned.pad=pad;abandoned.changed=false;
    Evaluation cancelled{binding,abandoned};
    auto cancel=[](PPCContext& c,uint8_t* b,NativeInputClockScratch& scratch,void* e){return Evaluate(c,b,scratch,e) && false;};
    if(!check(!RunNativeInputClock(NativeTriggerAccess::Get(held).clock,12,11,ctx,base,cancel,&cancelled) &&
        NativeTriggerAccess::Get(held).last==10 && NativeTriggerAccess::Get(held).committed.buttons==history_before,
        "cancel after original action math does not commit source counter or private histories"))return false;
    if(!check(ProduceNativeTriggers(held,binding,pad,1,12,11,ctx,base,batch),"same source tick remains usable after cancelled evaluation"))return false;
    const auto stable_history=NativeTriggerAccess::Get(held).committed.buttons;
    if(!check(std::equal(history_before.begin()+5*20+16,history_before.begin()+5*20+20,stable_history.begin()+5*20+16) &&
        std::equal(history_before.begin()+7*20+16,history_before.begin()+7*20+20,stable_history.begin()+7*20+16),
        "unchanged full Pad preserves original up/up press timestamps while maintenance advances previous state"))return false;
    pad.right_x=1; // A stick-only SDK packet change still updates trigger records.
    if(!check(ProduceNativeTriggers(held,binding,pad,1,12,12,ctx,base,batch) && NativeTriggerAccess::Get(held).committed.buttons!=stable_history,
        "full Pad packet comparison recognizes axis-only change and refreshes native up/up trigger stamp"))return false;
    auto stale=binding;stale.threshold++;
    if(!check(!ProduceNativeTriggers(held,stale,pad,1,12,13,ctx,base,batch) && NativeTriggerAccess::Get(held).last==12,"changed binding/threshold cannot advance source history"))return false;
    PPC_STORE_U32(event+16,actions+4);const auto last_history=NativeTriggerAccess::Get(held).committed.buttons;
    const bool rejected=!ProduceNativeTriggers(held,binding,pad,1,12,13,ctx,base,batch);PPC_STORE_U32(event+16,actions);
    if(!check(rejected && NativeTriggerAccess::Get(held).last==12 && NativeTriggerAccess::Get(held).committed.buttons==last_history,
        "replaced event/action ownership leaves source clock and histories uncommitted"))return false;
    bool foreign=true;std::thread other([&]{auto c=context();NativeTriggerBatch result;foreign=ProduceNativeTriggers(held,binding,pad,1,12,13,c,base,result) || ResetNativeTriggers(held);});other.join();
    if(!check(!foreign,"foreign thread cannot evaluate or reset trigger source"))return false;
    // The stale-event case intentionally retires the parent's retained lease.
    // Restoring guest bytes cannot resurrect it: explicitly rediscover before
    // testing a NEW source lifetime's tick exhaustion contract.
    NativeLocalMovementBinding rediscovered;
    if(!check(!NativeLocalMovementBindingFor(ctx,base,rediscovered) &&
        DiscoverNativeLocalMovementBinding(ctx,base,rediscovered) && rediscovered==parent,
        "restored action membership requires explicit parent rediscovery after lease retirement"))return false;
    NativeTriggerSource final;
    if(!check(ArmNativeTriggers(final,binding,13,UINT64_MAX,ctx,base),"new source may begin exactly at maximum source tick"))return false;
    if(!check(ProduceNativeTriggers(final,binding,pad,1,13,UINT64_MAX,ctx,base,batch),"maximum first source tick executes original math once"))return false;
    const auto maximum_batch=batch;const auto maximum_history=NativeTriggerAccess::Get(final).committed.buttons;
    if(!check(ProduceNativeTriggers(final,binding,pad,1,13,UINT64_MAX,ctx,base,batch) && batch==maximum_batch &&
        NativeTriggerAccess::Get(final).committed.buttons==maximum_history,"repeated maximum source tick returns identical cached batch and history"))return false;
    if(!check(!ProduceNativeTriggers(final,binding,pad,1,13,0,ctx,base,batch),"exhausted source never wraps to tick zero"))return false;
    if(!check(ResetNativeTriggers(held),"explicit owning lifecycle reset releases source clock"))return false;
    // Exercise original kind2 repeat timer state as well as one-shot presses.
    put(trigger_actions+2*512+108,.1f);
    NativeTriggerBinding repeated;if(!DiscoverNativeTriggerBinding(parent,ctx,base,repeated))return false;
    NativeTriggerSource repeat_source;if(!ArmNativeTriggers(repeat_source,repeated,14,0,ctx,base))return false;
    pad={};pad.left_trigger=255;unsigned repeats=0;
    for(unsigned tick=0;tick<=8;++tick){
        if(!ProduceNativeTriggers(repeat_source,repeated,pad,1,14,tick,ctx,base,batch))return false;
        const bool fired=batch.actions[2].dispatch;repeats+=fired;
        if(!check((tick || fired) && (tick>=3 || !tick || !fired),"original repeat timer fires initial press and respects native interval before repetition"))return false;
    }
    if(!check(repeats>=2,"original elapsed/restart calls retain private repeat state across source ticks"))return false;
    pad={};if(!ProduceNativeTriggers(repeat_source,repeated,pad,1,14,9,ctx,base,batch))return false;
    const auto& repeated_shape=NativeTriggerAccess::Get(repeat_source).committed.actions[2];
    if(!check(std::all_of(repeated_shape.begin()+112,repeated_shape.begin()+120,[](uint8_t v){return v==0;}),
        "original inactive kind2 interpreter clears its private repeat timer"))return false;
    // Expand the same original interpreter to face/shoulder/D-pad/stick keys.
    // This is not a second hand-written edge detector: SDK button updates and
    // timed tap/hold/repeat decisions still run through the native routines.
    constexpr std::array<const char*,4> digital_names{"TapB","HoldB","PressB","ReleaseB"};
    constexpr std::array<unsigned,4> digital_kinds{0,1,2,3};
    for(unsigned i=0;i<4;++i){const auto record=trigger_actions+(10+i)*512,name=names+1024+i*64;
        std::memcpy(base+name,digital_names[i],std::strlen(digital_names[i])+1);
        PPC_STORE_U32(actions+(10+i)*4,record);PPC_STORE_U32(record,name);PPC_STORE_U32(record+80,digital_kinds[i]);
        PPC_STORE_U32(record+408,1);PPC_STORE_U32(record+92,0x01400010);put(record+420,1);put(record+424,1);put(record+428,1);
    }
    PPC_STORE_U32(event+20,14);put(trigger_actions+2*512+108,0);
    NativeLocalMovementBinding refreshed;
    if(!check(!NativeLocalMovementBindingFor(ctx,base,refreshed) && DiscoverNativeLocalMovementBinding(ctx,base,parent),
        "adding digital action definitions retires the previous immutable event snapshot before explicit rediscovery"))return false;
    NativeTriggerBinding digital,trigger_only;
    if(!check(DiscoverNativeTriggerBinding(parent,ctx,base,digital,NativeButtonScope::DigitalAndTriggers) && digital.actions.size()==12 &&
        DiscoverNativeTriggerBinding(parent,ctx,base,trigger_only) && trigger_only.actions.size()==8 && digital!=trigger_only,
        "digital scope retains all original button definitions in order without changing the trigger-only contract"))return false;
    auto fired=[&](const NativeTriggerBatch& result,const char* label){for(unsigned i=0;i<digital.actions.size();++i)
        if(digital.actions[i].name==label)return result.actions[i].dispatch;return false;};
    NativeTriggerSource digital_hold;pad={};pad.buttons=0x2000;
    if(!ArmNativeTriggers(digital_hold,digital,15,0,ctx,base))return false;
    for(unsigned tick=0;tick<=9;++tick){
        if(!check(ProduceNativeTriggers(digital_hold,digital,pad,1,15,tick,ctx,base,batch) && fired(batch,"PressB")==!tick &&
            fired(batch,"HoldB")== (tick==8) && !fired(batch,"TapB") && !fired(batch,"ReleaseB"),
            "original digital B press/quarter-second hold decisions share exact source time with analog triggers"))return false;
    }
    pad={};if(!check(ProduceNativeTriggers(digital_hold,digital,pad,1,15,10,ctx,base,batch) && fired(batch,"ReleaseB") && !fired(batch,"TapB"),
        "releasing a long digital B hold does not turn into a tap"))return false;
    NativeTriggerSource mixed;pad.buttons=0x2000;pad.left_trigger=255;
    if(!check(ArmNativeTriggers(mixed,digital,16,0,ctx,base) && ProduceNativeTriggers(mixed,digital,pad,1,16,0,ctx,base,batch) &&
        fired(batch,"PressB") && fired(batch,"PressL"),"one actual SDK Pad independently updates simultaneous B and LT histories"))return false;
    pad={};if(!check(ProduceNativeTriggers(mixed,digital,pad,1,16,1,ctx,base,batch) && fired(batch,"TapB") && fired(batch,"ReleaseB") &&
        fired(batch,"BlastL") && fired(batch,"EndL") && batch.actions[8].descriptor==8,
        "simultaneous digital/analog short releases retain native authored action order"))return false;
    PPC_STORE_U8(buttons+11*20+1,1);NativeTriggerSource held_baseline;
    if(!check(!ArmNativeTriggers(held_baseline,digital,17,0,ctx,base),"digital scope cannot import a physically held B as a fresh neutral baseline"))return false;
    PPC_STORE_U8(buttons+11*20+1,0);
    auto wrong_scope=digital;wrong_scope.scope=NativeButtonScope::TriggersOnly;
    if(!check(!ProduceNativeTriggers(mixed,wrong_scope,pad,1,16,2,ctx,base,batch) &&
        NativeTriggerAccess::Get(mixed).last==1,"changing the button scope cannot silently rebind a running history"))return false;
    if(!check(!std::memcmp(caller.data(),&ctx,sizeof(ctx)) && !std::memcmp(live_stack.data(),base+ctx.r1.u32-8192,8192) &&
        !std::memcmp(live_device.data(),base+device,96) && !std::memcmp(live_buttons.data(),base+buttons,320) && _mm_getcsr()==fp && GetLastError()==last,
        "digital and mixed original interpreters preserve live caller, device, history, scratch and host state"))return false;
    REXLOG_INFO("Native trigger math: {} checks passed (private original history/interpreter only; no action dispatch, aim, wire or gameplay admission)",checks);return true;
}
}
