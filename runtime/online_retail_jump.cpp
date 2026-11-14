#include "online_retail_jump.h"
#include "online_retail_input.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
PPC_EXTERN_FUNC(sub_82A3C930);
PPC_EXTERN_FUNC(__imp__sub_827C3D78);
PPC_EXTERN_FUNC(__imp__sub_8287EA38);
PPC_EXTERN_FUNC(__imp__sub_8287ECF8);
PPC_EXTERN_FUNC(__imp__sub_8287D048);
PPC_EXTERN_FUNC(__imp__sub_8287ED00);
namespace {
using namespace em2::net;
using Result=ActorInputResult;
struct Environment {unsigned fp=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(fp);SetLastError(error);}};
struct Preserve {
    PPCContext& ctx;std::array<uint8_t,sizeof(PPCContext)> saved{};Environment environment;
    uint8_t* memory=nullptr;std::array<uint8_t,8192> bytes{};uint32_t stack=0;
    Preserve(PPCContext& c,uint8_t* base):ctx(c){std::memcpy(saved.data(),&c,sizeof(c));const auto top=c.r1.u32&~15u;
        if(base && top>=12288 && em2::GuestReadBatch::Readable(base+top-8192,8192)){
            memory=base+top-8192;std::memcpy(bytes.data(),memory,bytes.size());stack=top-4096;ctx.r1.u64=stack;}}
    ~Preserve(){if(memory)std::memcpy(memory,bytes.data(),bytes.size());std::memcpy(&ctx,saved.data(),sizeof(ctx));}
};
bool Readable(uint32_t p,size_t n,uint8_t* base){return base && p>=4096 && uint64_t(p)+n<=0x100000000ull && em2::GuestReadBatch::Readable(base+p,n);}
struct Action {uint32_t event=0,record=0;std::array<uint32_t,10> shape{};bool operator==(const Action&)const=default;};
struct Target {
    RetailActorInput actor{};uint32_t task=0,physics=0,gravity=0,animation=0;
    uint32_t fsm=0,fsm_query=0,fsm_task=0,press=0,release=0,input_manager=0;
    std::array<Action,2> actions{};
    bool operator==(const Target&)const=default;
};
struct Slot {Target target{};RoomJumpStream stream{};bool delivered=false;};
struct RetailJumpState {DWORD thread=0;uint8_t* base=nullptr;std::array<Slot,2> slots{};bool armed=false,faulted=false,active=false,started=false;uint64_t tick=0,next=0;};
thread_local RetailJumpState state;
struct Permit {unsigned slot=0;uint32_t action=0;bool derived=false,base=false,invalid=false;};
thread_local Permit* permit=nullptr;
struct Stimulus {std::array<uint32_t,2> physics{};std::array<unsigned,2> count{},descriptors{};float height=0;};
thread_local Stimulus* stimulus=nullptr;
bool Code(uint32_t p){return p>=uint32_t(PPC_CODE_BASE) && p<0x83300000 && !(p&3);}
uint32_t Typed(const RetailActorInput& a,uint32_t global,size_t size,uint8_t* base){
    if(!Readable(global,1,base) || !Readable(a.entity,96,base))return 0;
    const auto types=PPC_LOAD_U32(a.entity+88),list=PPC_LOAD_U32(a.entity+36);const auto kind=PPC_LOAD_U8(global);
    if(!Readable(types,unsigned(kind)+1,base))return 0;const auto slot=PPC_LOAD_U8(types+kind);
    if(slot==255 || !Readable(list,unsigned(slot)*4+4,base))return 0;const auto face=PPC_LOAD_U32(list+unsigned(slot)*4);
    return face>=8 && Readable(face-8,size,base)?face-8:0;
}
uint32_t Task(const RetailActorInput& a,uint8_t* base){
    const auto collection=a.controller+156,buckets=PPC_LOAD_U32(collection+16),count=PPC_LOAD_U32(collection+20);
    if(!count || count>4096 || !Readable(buckets,size_t(count)*4,base))return 0;
    std::array<uint32_t,256> visited{};unsigned seen=0;uint32_t result=0;
    for(unsigned i=0;i<count;++i)for(auto node=PPC_LOAD_U32(buckets+i*4);node;node=PPC_LOAD_U32(node+8)){
        if(seen==visited.size() || !Readable(node,12,base) || std::find(visited.begin(),visited.begin()+seen,node)!=visited.begin()+seen)return 0;
        visited[seen++]=node;const auto name=PPC_LOAD_U32(node);
        if(Readable(name,5,base) && !std::memcmp(base+name,"Jump",5)){if(result)return 0;result=PPC_LOAD_U32(node+4);}
    }
    const auto list=PPC_LOAD_U32(collection+36),count_list=PPC_LOAD_U32(collection+40);
    if(!result || count_list>256 || !Readable(list,size_t(count_list)*4,base))return 0;
    unsigned matches=0;for(unsigned i=0;i<count_list;++i)matches+=PPC_LOAD_U32(list+i*4)==result;
    return matches==1?result:0;
}
bool Member(uint32_t list,uint32_t count,uint32_t target,unsigned limit,uint8_t* base){
    if(!count || count>limit || !Readable(list,size_t(count)*4,base))return false;
    unsigned hits=0;for(unsigned i=0;i<count;++i)hits+=PPC_LOAD_U32(list+i*4)==target;return hits==1;
}
bool Actions(Target& t,PPCContext& ctx,uint8_t* base){
    if(!Readable(0x833DBAD8,4,base))return false;t.input_manager=PPC_LOAD_U32(0x833DBAD8);
    if(!Readable(t.input_manager,96,base))return false;
    const auto contexts=PPC_LOAD_U32(t.input_manager+4),context_count=PPC_LOAD_U32(t.input_manager+8);
    if(context_count>64 || (context_count && !Readable(contexts,size_t(context_count)*4,base)))return false;
    for(unsigned i=0;i<context_count;++i)if(!Readable(PPC_LOAD_U32(contexts+i*4),24,base))return false;
    uint32_t active=0;{Preserve preserve(ctx,base);if(!preserve.memory)return false;
        ctx.r3.u64=t.input_manager;ctx.r4.u64=t.input_manager+16;__imp__sub_827C3D78(ctx,base);active=ctx.r3.u32;}
    const std::array<uint32_t,2> events{t.input_manager+24,active};std::array<unsigned,2> found{};
    for(unsigned e=0;e<events.size();++e){const auto event=events[e];if(!event || (e && event==events[0]))continue;
        if(!Readable(event,24,base))return false;
        const auto list=PPC_LOAD_U32(event+16),count=PPC_LOAD_U32(event+20);
        if(count>256 || (count && !Readable(list,size_t(count)*4,base)))return false;
        for(unsigned index=0;index<count;++index){const auto record=PPC_LOAD_U32(list+index*4);if(!Readable(record,4,base))return false;
            const auto uid=PPC_LOAD_U32(record);
            for(unsigned edge=0;edge<2;++edge){if(uid!=(edge?t.release:t.press))continue;
                if(++found[edge]!=1 || !Readable(record,436,base) ||
                   !Member(PPC_LOAD_U32(event+4),PPC_LOAD_U32(event+8),t.actor.listener,128,base) ||
                   !Member(list,count,record,256,base))return false;
                const auto kind=PPC_LOAD_U32(record+80),key=PPC_LOAD_U32(record+92);
                if(kind!=(edge?3u:2u) || PPC_LOAD_U32(record+408)!=1 || PPC_LOAD_U32(record+412) ||
                   (key&0xFFFF0000)!=0x01400000 || PPC_LOAD_U32(record+108) || PPC_LOAD_U8(record+104) || PPC_LOAD_U32(record+432))return false;
                t.actions[edge]={event,record,{uid,kind,PPC_LOAD_U32(record+408),PPC_LOAD_U32(record+412),PPC_LOAD_U32(record+88),key,
                    PPC_LOAD_U32(record+96),PPC_LOAD_U8(record+104),PPC_LOAD_U32(record+108),PPC_LOAD_U32(record+432)}};
            }
        }
    }
    return found[0]==1 && found[1]==1 && t.actions[0].shape[4]==t.actions[1].shape[4] && t.actions[0].shape[5]==t.actions[1].shape[5];
}
bool Inspect(unsigned slot,PPCContext& ctx,uint8_t* base,Target& t){
    if(!RetailActorInputFor(slot,ctx,base,t.actor))return false;const auto& a=t.actor;
    if(Typed(a,0x833DAE58,96,base)!=a.controller+16)return false;
    t.physics=Typed(a,0x833DDF94,224,base);t.gravity=Typed(a,0x833DDF1C,96,base);t.animation=Typed(a,0x833DCCF8,64,base);
    const auto kinds=PPC_LOAD_U32(a.entity+88);const auto animation_kind=PPC_LOAD_U8(0x833DCCF8);
    if(!Readable(kinds,unsigned(animation_kind)+1,base))return false;
    const bool has_animation=PPC_LOAD_U8(kinds+animation_kind)!=255;
    if(!t.physics || !t.gravity || PPC_LOAD_U32(t.physics+48)!=a.entity-8 || PPC_LOAD_U32(t.gravity+48)!=a.entity-8 ||
       (has_animation && (!t.animation || PPC_LOAD_U32(t.animation+48)!=a.entity-8)))return false;
    t.task=Task(a,base);
    if(!Readable(t.task,44,base) || PPC_LOAD_U32(t.task)!=0x820BC838 || PPC_LOAD_U32(t.task+4)!=a.entity ||
       !Readable(0x820BC838,44,base) || PPC_LOAD_U32(0x820BC838+40)!=0x8287EA38 ||
       PPC_LOAD_U32(0x820BC838+4)!=0x8287ECF8 || PPC_LOAD_U32(0x820BC838+8)!=0x8287D048)return false;
    // Pin the native inline FSM as part of this exact human controller lease.
    t.fsm=PPC_LOAD_U32(a.controller+104);
    if(t.fsm<0x82000000 || t.fsm>=0x82300000 || !Readable(t.fsm,32,base))return false;
    t.fsm_query=PPC_LOAD_U32(t.fsm+20);t.fsm_task=PPC_LOAD_U32(t.fsm+28);
    if(!Code(t.fsm_query) || !Code(t.fsm_task))return false;
    t.press=PPC_LOAD_U32(0x833DAEBC+12*slot);t.release=PPC_LOAD_U32(0x833DAEC0+12*slot);
    if(!t.press || !t.release || t.press==t.release)return false;
    for(unsigned offset:{280u,284u,288u,292u,296u,300u}){
        const auto uid=PPC_LOAD_U32(a.listener+offset);if(uid==t.press || uid==t.release)return false;}
    return Actions(t,ctx,base);
}
bool Owner(uint8_t* base){return state.armed && state.thread==GetCurrentThreadId() && state.base==base;}
bool Current(unsigned slot,PPCContext& ctx,uint8_t* base){Target t;return Owner(base) && !state.faulted && slot<2 && Inspect(slot,ctx,base,t) && t==state.slots[slot].target;}
Result Validate(unsigned slot,const RoomMovement& command,PPCContext& ctx,uint8_t* base,bool history=true){
    if(!Owner(base))return Result::WrongThread;if(permit)return Result::Nested;
    if(!Current(slot,ctx,base))return Result::Stale;
    if(!ValidRoomMovement(command) || command.actor_epoch!=state.slots[slot].target.actor.actor_epoch)return Result::InvalidInput;
    if(command.source==RoomMovementSource::NativeActionV3)return Result::UnsupportedAction;
    // Camera intent is separately admitted by the Movement/camera adapter.
    if(command.pad.left_trigger || command.pad.right_trigger || (command.pad.buttons&~command.jump_button_mask))return Result::UnsupportedAction;
    auto stream=state.slots[slot].stream;
    if(!history)return (!stream.native || (command.source==RoomMovementSource::NativeActionV2 && stream.button_mask==command.jump_button_mask))?Result::Applied:Result::InvalidInput;
    return AdvanceRoomJumpStream(stream,command)?Result::Applied:Result::InvalidInput;
}
}
namespace em2::net {
bool ArmRetailJump(PPCContext& ctx,uint8_t* base){
    Environment environment;if(permit)return false;
    if(state.armed)return Owner(base) && !state.active && !state.faulted && Current(0,ctx,base) && Current(1,ctx,base);
    RetailJumpState candidate;candidate.thread=GetCurrentThreadId();candidate.base=base;candidate.armed=true;
    for(unsigned slot=0;slot<2;++slot)if(!Inspect(slot,ctx,base,candidate.slots[slot].target) || (PPC_LOAD_U32(candidate.slots[slot].target.task+8)&16))return false;
    if(candidate.slots[0].target.task==candidate.slots[1].target.task || candidate.slots[0].target.physics==candidate.slots[1].target.physics ||
       candidate.slots[0].target.actor.world_epoch!=candidate.slots[1].target.actor.world_epoch)return false;
    if(!SetRetailJumpAdmission(RetailJumpAllows))return false;state=candidate;return true;
}
bool RetailJumpAllows(unsigned slot,const RoomMovement& command,PPCContext& ctx,uint8_t* base){Environment environment;return Validate(slot,command,ctx,base,false)==Result::Applied;}
ActorInputResult ValidateRetailJumpFrame(const RoomFrame& frame,PPCContext& ctx,uint8_t* base){
    Environment environment;if(!Owner(base))return Result::WrongThread;
    if(state.active || (frame.players_mask&3)!=3 || frame.tick==UINT64_MAX || (state.started && frame.tick!=state.next))return Result::InvalidTick;
    for(unsigned slot=0;slot<2;++slot){const auto r=Validate(slot,frame.players[slot],ctx,base);if(r!=Result::Applied)return r;}return Result::Applied;
}
ActorInputResult ReplayRetailJumpBatch(unsigned slot,const RoomMovement& command,uint64_t tick,PPCContext& ctx,uint8_t* base){
    Environment environment;const auto result=Validate(slot,command,ctx,base);if(result!=Result::Applied)return result;
    if(tick==UINT64_MAX || (state.started && tick!=state.next) || (state.active && state.tick!=tick) || state.slots[slot].delivered)return Result::InvalidTick;
    for(unsigned i=0;i<2;++i){RetailActorInput a;bool delivered=false;RoomMovement prepared;
        if(!Current(i,ctx,base) || !RetailInputTickFor(i,tick,ctx,base,a,delivered,&prepared) || !delivered || a!=state.slots[i].target.actor)return Result::InvalidTick;
        if(i==slot && prepared!=command)return Result::InvalidInput;}
    Preserve preserve(ctx,base);if(!preserve.memory)return Result::InvalidContext;
    auto next=state.slots[slot].stream;if(!AdvanceRoomJumpStream(next,command))return Result::InvalidInput;
    state.active=true;state.tick=tick;state.slots[slot].delivered=true;
    try{
        for(unsigned i=0;i<command.jump_count;++i){const auto& target=state.slots[slot].target;
            // The collection may visit other native task listeners before Jump.
            // Supply the actual complete current native action record and event,
            // never a four-byte UID descriptor masquerading as that record.
            const auto& action=target.actions[command.jump_events[i].edge==RoomJumpEdge::Pressed?0:1];
            Permit authorized{slot,action.record};permit=&authorized;struct Reset{~Reset(){permit=nullptr;}}reset;
            ctx.r3.u64=target.actor.listener;ctx.r4.u64=action.event;ctx.r5.u64=action.record;ctx.f1.f64=1;ctx.f2.f64=0;
            sub_82A3C930(ctx,base);
            if(!authorized.derived || authorized.invalid){state.faulted=true;return Result::InvalidContext;}
            if(!Current(slot,ctx,base)){state.faulted=true;return Result::Stale;}
            // Native disabled/airborne/special-state gates may legitimately
            // stop before base dispatch. Never force a task or velocity.
        }
    }catch(...){state.faulted=true;throw;}
    state.slots[slot].stream=next;return Result::Applied;
}
ActorInputResult FinishRetailJumpTick(uint64_t tick,PPCContext& ctx,uint8_t* base){
    Environment environment;if(!Owner(base))return Result::WrongThread;
    if(state.faulted || !state.active || state.tick!=tick || tick==UINT64_MAX)return Result::InvalidTick;
    for(unsigned i=0;i<2;++i){RetailActorInput a;bool delivered=false;
        if(!state.slots[i].delivered || !Current(i,ctx,base) || !RetailInputTickFor(i,tick,ctx,base,a,delivered) || !delivered){state.faulted=true;return Result::InvalidContext;}}
    state.active=false;state.started=true;state.next=tick+1;for(auto& s:state.slots)s.delivered=false;return Result::Applied;
}
void DisarmRetailJump(){Environment environment;if(state.armed && state.thread!=GetCurrentThreadId())return;if(permit)return;SetRetailJumpAdmission(nullptr);state={};}
bool SuppressRetailJumpInput(PPCContext& ctx,uint8_t* base,bool base_handler){
    Environment environment;if(!Owner(base) || !Readable(ctx.r5.u32,4,base))return false;
    for(unsigned slot=0;slot<2;++slot){const auto& target=state.slots[slot].target;
        if(ctx.r3.u32!=target.actor.listener)continue;RetailActorInput current;
        if(!RetailInputQuarantineFor(slot,ctx,base,current) || current!=target.actor)return false;
        const auto uid=PPC_LOAD_U32(ctx.r5.u32);if(uid!=target.press && uid!=target.release)return false;
        if(!permit || permit->slot!=slot || permit->action!=ctx.r5.u32 || !Current(slot,ctx,base)){ctx.r3.u64=0;return true;}
        if(!base_handler){if(permit->derived){permit->invalid=true;ctx.r3.u64=0;return true;}permit->derived=true;return false;}
        if(!permit->derived || permit->base || ctx.lr!=0x82A3CD30){permit->invalid=true;ctx.r3.u64=0;return true;}
        permit->base=true;return false;
    }
    return false;
}
bool RetailJumpFixtureStimulus(PPCContext& ctx,uint8_t*){
    if(!stimulus || (ctx.lr!=0x8287E7C0 && ctx.lr!=0x8287E814))return false;
    for(unsigned i=0;i<2;++i)if(ctx.r3.u32==stimulus->physics[i]){++stimulus->count[i];stimulus->height=float(ctx.f1.f64);return true;}
    throw std::logic_error("retail Jump fixture targeted a foreign physics body");
}
}
namespace {
void FixtureZero(PPCContext& ctx,uint8_t*){ctx.r3.u64=0;}
void FixtureEarlierTask(PPCContext& ctx,uint8_t* base){
    if(!stimulus || (ctx.r3.u32!=0x4F000 && ctx.r3.u32!=0x4F100) || !Readable(ctx.r4.u32,436,base))
        throw std::logic_error("earlier native task received an incomplete action descriptor");
    const auto slot=(ctx.r3.u32-0x4F000)/0x100,record=ctx.r4.u32;
    const auto uid=PPC_LOAD_U32(record),kind=PPC_LOAD_U32(record+80),count=PPC_LOAD_U32(record+408);
    if(uid==PPC_LOAD_U32(0x833DAEB8+12*slot)){
        if(kind!=4 || count!=2)throw std::logic_error("earlier native task received an incomplete Movement action descriptor");
    }else{
        const bool press=uid==PPC_LOAD_U32(0x833DAEBC+12*slot),release=uid==PPC_LOAD_U32(0x833DAEC0+12*slot);
        if((!press && !release) || kind!=(press?2u:3u) || count!=1)
            throw std::logic_error("earlier native task received an incomplete Jump action descriptor");
        ++stimulus->descriptors[slot];
    }
    ctx.r3.u64=0;
}
void FixtureProperties(PPCContext& ctx,uint8_t* base){const float result=ctx.lr==0x827868D4?12.f:1.f;PPC_STORE_U32(ctx.r5.u32,std::bit_cast<uint32_t>(result));ctx.r3.u64=1;}
}
bool em2::net::TestRetailJumpReceiver(PPCContext& ctx,uint8_t* base,const std::function<bool(const RoomFrame&)>& drive){
    Preserve preserve(ctx,base);if(!preserve.memory || !drive || state.armed)return false;
    struct GuestRestore {
        struct Bytes{uint32_t address;std::vector<uint8_t> bytes;};uint8_t* base;std::vector<Bytes> saved;
        void Save(uint32_t at,size_t count){saved.push_back({at,{base+at,base+at+count}});}
        ~GuestRestore(){for(auto it=saved.rbegin();it!=saved.rend();++it)std::memcpy(base+it->address,it->bytes.data(),it->bytes.size());}
    }guest{base};
    struct DispatchRestore {
        uint8_t* base;std::vector<std::pair<uint32_t,PPCFunc*>> saved;
        bool Map(uint32_t at,PPCFunc* fn){auto* pointer=&PPC_LOOKUP_FUNC(base,at);
            if(!VirtualAlloc(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pointer)&~uintptr_t(4095)),4096,MEM_COMMIT,PAGE_READWRITE))return false;
            saved.push_back({at,*pointer});*pointer=fn;return true;}
        ~DispatchRestore(){for(auto [at,fn]:saved)PPC_LOOKUP_FUNC(base,at)=fn;}
    }dispatch{base};
    if(!dispatch.Map(0x8287EA38,__imp__sub_8287EA38) || !dispatch.Map(0x8287ECF8,__imp__sub_8287ECF8) ||
       !dispatch.Map(0x8287D048,__imp__sub_8287D048) || !dispatch.Map(0x8287ED00,__imp__sub_8287ED00) ||
       !dispatch.Map(uint32_t(PPC_CODE_BASE)+96,FixtureProperties) || !dispatch.Map(uint32_t(PPC_CODE_BASE)+100,FixtureZero) ||
       !dispatch.Map(uint32_t(PPC_CODE_BASE)+104,FixtureEarlierTask))return false;
    for(auto address:{0x833DAEBCu,0x833DAEC0u,0x833DAEC8u,0x833DAECCu,0x833DDF1Cu,0x833DDF94u,0x833DCCF8u,
                     0x833DA6F4u,0x820BC838u+68,0x8203D278u})guest.Save(address,4);
    constexpr uint32_t fsm_vt=0x820BC900,properties_vt=0x4E000,event_manager=0x4D000;
    guest.Save(fsm_vt,32);guest.Save(0x44000,0xC000);guest.Save(0x50000,0x1000);
    const auto input_manager=PPC_LOAD_U32(0x833DBAD8),event=input_manager+24;
    if(!Readable(event,24,base) || PPC_LOAD_U32(event+20)!=2)return false;
    const auto action_list=PPC_LOAD_U32(event+16);if(!Readable(action_list,24,base))return false;
    guest.Save(event,24);guest.Save(action_list,24);PPC_STORE_U32(event+20,6);
    for(unsigned i=0;i<4;++i){const auto record=0x50000u+i*0x200;const auto slot=i/2,edge=i%2;
        PPC_STORE_U32(action_list+(2+i)*4,record);
        PPC_STORE_U32(record,slot?(edge?0x820BC7A8:0x820BC794):(edge?0x820BC79C:0x820BC78C));
        PPC_STORE_U32(record+80,edge?3:2);PPC_STORE_U32(record+88,slot);PPC_STORE_U32(record+92,0x01400009);
        PPC_STORE_U32(record+408,1);
    }
    PPC_STORE_U32(fsm_vt+20,uint32_t(PPC_CODE_BASE)+100);PPC_STORE_U32(fsm_vt+28,uint32_t(PPC_CODE_BASE)+100);
    PPC_STORE_U32(properties_vt+212,uint32_t(PPC_CODE_BASE)+96);
    PPC_STORE_U32(0x4E100+40,uint32_t(PPC_CODE_BASE)+104);
    PPC_STORE_U32(0x820BC838+68,uint32_t(PPC_CODE_BASE)+100);PPC_STORE_U32(0x8203D278,0);
    PPC_STORE_U32(event_manager+1008,10);PPC_STORE_U32(0x833DA6F4,event_manager);
    PPC_STORE_U8(0x833DDF1C,12);PPC_STORE_U8(0x833DDF94,13);PPC_STORE_U8(0x833DCCF8,14);
    PPC_STORE_U32(0x833DAEBC,0x820BC78C);PPC_STORE_U32(0x833DAEC0,0x820BC79C);
    PPC_STORE_U32(0x833DAEC8,0x820BC794);PPC_STORE_U32(0x833DAECC,0x820BC7A8);
    std::array<RetailActorInput,2> actors{};std::array<uint32_t,2> tasks{0x44000,0x45000};Stimulus observed{{0x46000,0x47000}};
    for(unsigned i=0;i<2;++i){auto& a=actors[i];if(!RetailActorInputFor(i,ctx,base,a))return false;
        const auto types=PPC_LOAD_U32(a.entity+88),list=PPC_LOAD_U32(a.entity+36);
        guest.Save(a.controller,800);guest.Save(types,256);guest.Save(list,64);guest.Save(a.motion,896);
        const auto task=tasks[i],physics=observed.physics[i],gravity=0x48000u+i*0x1000,config=0x4B000u+i*0x1000;
        PPC_STORE_U32(task,0x820BC838);PPC_STORE_U32(task+4,a.entity);PPC_STORE_U32(task+8,0);PPC_STORE_U8(task+17,1);PPC_STORE_U8(task+18,1);
        PPC_STORE_U32(task+256,0x820BC78C);PPC_STORE_U32(task+260,task);PPC_STORE_U32(task+264,0);
        PPC_STORE_U32(task+272,task+256);
        const auto earlier=0x4F000u+i*0x100;PPC_STORE_U32(earlier,0x4E100);PPC_STORE_U32(earlier+4,a.entity);
        PPC_STORE_U32(task+288,earlier);PPC_STORE_U32(task+292,task);
        PPC_STORE_U32(a.controller+172,task+272);PPC_STORE_U32(a.controller+176,1);
        PPC_STORE_U32(a.controller+192,task+288);PPC_STORE_U32(a.controller+196,2);PPC_STORE_U32(a.controller+104,fsm_vt);
        PPC_STORE_U8(types+12,2);PPC_STORE_U8(types+13,3);PPC_STORE_U8(types+14,255);
        PPC_STORE_U32(list+8,gravity+8);PPC_STORE_U32(list+12,physics+8);
        PPC_STORE_U32(gravity+8,properties_vt);PPC_STORE_U32(gravity+48,a.entity-8);PPC_STORE_U32(gravity+72,std::bit_cast<uint32_t>(9.8f));
        PPC_STORE_U32(physics+48,a.entity-8);PPC_STORE_U32(config+8,properties_vt);PPC_STORE_U32(a.motion+120,config);
        PPC_STORE_U32(a.motion+384,0);PPC_STORE_U32(a.motion+208,0);PPC_STORE_U32(a.motion+876,std::bit_cast<uint32_t>(1.f));
    }
    struct Release {~Release(){DisarmRetailJump();stimulus=nullptr;}}release;
    if(!ArmRetailJump(ctx,base))return false;stimulus=&observed;
    unsigned checks=0;auto check=[&](bool value,const char* why){++checks;if(!value)REXLOG_ERROR("Retail Jump: {}",why);return value;};
    RoomFrame frame;frame.tick=10;frame.players_mask=3;
    for(unsigned i=0;i<2;++i){auto& c=frame.players[i];c.actor_epoch=actors[i].actor_epoch;c.source=RoomMovementSource::NativeActionV2;
        c.jump_button_mask=uint16_t(0x1000u<<i);c.pad.buttons=c.jump_button_mask;c.jump_count=1;c.jump_events[0]={1,RoomJumpEdge::Pressed};}
    auto bad=frame;bad.players[1].jump_count=2;bad.players[1].jump_events[1]={3,RoomJumpEdge::Released};
    if(!check(ValidateRetailJumpFrame(frame,ctx,base)==Result::Applied && ValidateRetailJumpFrame(bad,ctx,base)!=Result::Applied && observed.count==std::array<unsigned,2>{},
       "read-only whole batch preflight rejects a later gap before either native task"))return false;
    const auto old_modifier=PPC_LOAD_U32(0x50600+108);PPC_STORE_U32(0x50600+108,1);
    const auto modified_action=ValidateRetailJumpFrame(frame,ctx,base);PPC_STORE_U32(0x50600+108,old_modifier);
    if(!check(modified_action!=Result::Applied,"modified real P2 release descriptor rejects before either native callback"))return false;
    const auto old_count=PPC_LOAD_U32(event+20);PPC_STORE_U32(event+20,5);
    const auto retired_action=ValidateRetailJumpFrame(frame,ctx,base);PPC_STORE_U32(event+20,old_count);
    if(!check(retired_action!=Result::Applied,"retired action allocation with matching UID cannot satisfy current context membership"))return false;
    auto ownership=PPC_LOAD_U32(tasks[1]+4);PPC_STORE_U32(tasks[1]+4,actors[0].entity);
    const auto foreign=ValidateRetailJumpFrame(frame,ctx,base);PPC_STORE_U32(tasks[1]+4,ownership);
    if(!check(foreign!=Result::Applied,"foreign human P2 task owner rejected"))return false;
    auto ordinary=ctx;ordinary.r3.u64=actors[1].listener;ordinary.r5.u64=0x833DAEC8;
    if(!check(!SuppressRetailJumpInput(ordinary,base,false),"arming readiness leaves bootstrap ordinary Jump untouched"))return false;
    if(!check(drive(frame) && observed.count==std::array<unsigned,2>{1,1} && observed.height==12.f &&
       (PPC_LOAD_U32(tasks[0]+8)&16) && (PPC_LOAD_U32(tasks[1]+8)&16),
       "original human P1 Jump and P2 Jump2 callbacks reach independent original task starters and own native physics stimulus"))return false;
    ordinary=ctx;ordinary.r3.u64=actors[1].listener;ordinary.r5.u64=0x50400;
    if(!check(SuppressRetailJumpInput(ordinary,base,false) && !ordinary.r3.u32 && observed.count==std::array<unsigned,2>{1,1},
       "ordinary P2 Jump stays quarantined between completed agreed ticks"))return false;
    if(!check(ReplayRetailJumpBatch(1,frame.players[1],10,ctx,base)!=Result::Applied,"duplicate completed tick cannot replay P2 task"))return false;
    auto future=frame;future.tick=11;future.players[1].jump_events[0]={3,RoomJumpEdge::Pressed};
    if(!check(RetailJumpAllows(1,future.players[1],ctx,base) && ValidateRetailJumpFrame(future,ctx,base)!=Result::Applied &&
       observed.count==std::array<unsigned,2>{1,1},"future delayed source capability ignores receiver history; authoritative gap still rejects"))return false;
    ++frame.tick;for(unsigned i=0;i<2;++i){auto& c=frame.players[i];c.jump_count=0;c.jump_events={};}
    if(!check(drive(frame) && observed.count==std::array<unsigned,2>{1,1},"held native input has no repeated Jump starter"))return false;
    ++frame.tick;for(unsigned i=0;i<2;++i){auto& c=frame.players[i];c.pad.buttons=0;c.jump_count=1;c.jump_events[0]={2,RoomJumpEdge::Released};}
    if(!check(drive(frame) && observed.count==std::array<unsigned,2>{1,1} && !(PPC_LOAD_U32(tasks[0]+8)&16) && !(PPC_LOAD_U32(tasks[1]+8)&16),
       "original JumpRelease and JumpRelease2 stop only their own active native tasks"))return false;
    if(!check(observed.descriptors==std::array<unsigned,2>{2,2},"earlier native collection tasks receive complete actual Jump action bodies for each ordered edge"))return false;
    REXLOG_INFO("Retail Jump: {} checks passed (original task callbacks; fixture physics sink, no native contacts)",checks);return true;
}
