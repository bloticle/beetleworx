#include "online_retail_buttons.h"
#include "online_retail_input.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <rex/cvar.h>
REXCVAR_DECLARE(bool,em2_private_profile);
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <cstring>
#include <thread>
#include <stdexcept>
PPC_EXTERN_FUNC(__imp__sub_827C3D78);
PPC_EXTERN_FUNC(sub_82A3C930);
namespace em2::net {
namespace {
struct Preserve {unsigned fp=_mm_getcsr();DWORD error=GetLastError();~Preserve(){_mm_setcsr(fp);SetLastError(error);}};
bool Readable(uint32_t p,size_t n,uint8_t* base){return base && p>=4096 && uint64_t(p)+n<=0x100000000ull && em2::GuestReadBatch::Readable(base+p,n);}
bool DefinitionOwner(const RetailActorInput& actor,const NativeTriggerBinding& b,PPCContext& ctx,uint8_t* base){
    const auto& p=b.parent;
    if(p.world_epoch!=actor.world_epoch || p.registry!=actor.registry || p.retail_slot!=actor.slot || p.entity!=actor.entity ||
       p.controller!=actor.controller || p.listener!=actor.listener || p.logical_user>1 ||
       !ValidateNativeButtonDefinitionSnapshot(b,base) ||
       ValidateRetailMovementAction(actor.slot,{p.event,p.action,0,0},ctx,base)!=ActorInputResult::Applied ||
       !Readable(p.action,152,base) || PPC_LOAD_U32(p.action+88)!=p.logical_user || PPC_LOAD_U32(p.action+120)!=p.logical_user)return false;
    const auto list=PPC_LOAD_U32(p.manager+4),count=PPC_LOAD_U32(p.manager+8);
    if(count>128 || (count && !Readable(list,size_t(count)*4,base)))return false;
    for(unsigned i=0;i<count;++i)if(!Readable(PPC_LOAD_U32(list+i*4),4,base))return false;
    // Original leaf selector reads only the bounded context table. Use a
    // copied context; no producer lease, event dispatch or guest stack writes.
    auto selected=ctx;selected.r3.u64=p.manager;selected.r4.u64=p.manager+16;
    __imp__sub_827C3D78(selected,base);if(selected.r3.u32!=p.active_event)return false;
    const auto a=PPC_LOAD_U32(p.action+92),c=PPC_LOAD_U32(p.action+124);
    return (a==0x02400017 && c==0x02400018) || (a==0x02400018 && c==0x02400017);
}
}
struct RetailButtonReceiver::Impl {
    DWORD thread=0;uint8_t* base=nullptr;bool armed=false,stale=false;
    RetailActorInput actor;NativeTriggerBinding definitions;std::array<uint8_t,32> schema{};
    bool Current(PPCContext& ctx,uint8_t* memory){
        if(!armed || thread!=GetCurrentThreadId() || base!=memory || stale)return false;
        RetailActorInput current;
        const bool body=RetailActorInputFor(actor.slot,ctx,memory,current) && current==actor;
        const bool definition=body && DefinitionOwner(actor,definitions,ctx,memory);
        if(!definition){
            stale=true;
            if(REXCVAR_GET(em2_private_profile))REXLOG_ERROR("Native retail button receiver stale: slot {} body lease {} definition snapshot {} active context {:08X}/{:08X}",actor.slot,body,ValidateNativeButtonDefinitionSnapshot(definitions,memory),definitions.parent.active_event_uid,Readable(definitions.parent.manager,20,memory)?PPC_LOAD_U32(definitions.parent.manager+16):0);
            return false;
        }
        return true;
    }
};
struct RetailButtonAccess {static auto& Get(RetailButtonReceiver& r){return *r.impl_;}};
namespace {
struct Delivery {
    DWORD thread=0;uint8_t* base=nullptr;
    std::array<RetailButtonReceiver*,2> receivers{};
    std::array<RoomJumpStream,2> streams{};
    std::array<bool,2> delivered{};
    bool active=false,started=false,faulted=false;uint64_t next=0;
    RoomFrame frame;
};
thread_local Delivery delivery;
struct ButtonPermit {unsigned slot;NativeActorAction action;bool derived=false,base=false,invalid=false;};
thread_local ButtonPermit* button_permit=nullptr;
bool DeliveryOwner(uint8_t* base){return delivery.thread==GetCurrentThreadId() && delivery.base==base;}
// Jump's wire history must describe exactly the native button records that
// will be delivered. It is metadata, not a second Jump callback path.
bool JumpMatches(const RoomMovement& c,const std::vector<NativeActorAction>& actions,unsigned slot,uint8_t* base){
    if(!Readable(0x833DAEBC+12*slot,8,base))return false;
    const auto press=PPC_LOAD_U32(0x833DAEBC+12*slot),release=PPC_LOAD_U32(0x833DAEC0+12*slot);
    unsigned count=0;
    for(const auto& a:actions){const auto uid=PPC_LOAD_U32(a.action);
        if(uid!=press && uid!=release)continue;
        if(count>=c.jump_count || c.jump_events[count].edge!=(uid==press?RoomJumpEdge::Pressed:RoomJumpEdge::Released))return false;
        ++count;
    }
    return count==c.jump_count;
}
bool ButtonAdmission(unsigned slot,const RoomMovement& c,PPCContext& ctx,uint8_t* base){
    if(!DeliveryOwner(base) || delivery.faulted || slot>1 || !delivery.receivers[slot])return false;
    std::vector<NativeActorAction> actions;
    auto stream=delivery.streams[slot];
    return PrepareRetailButtons(*delivery.receivers[slot],c,ctx,base,actions) &&
        JumpMatches(c,actions,slot,base) && AdvanceRoomJumpStream(stream,c);
}
struct CallbackContext {
    PPCContext& ctx;PPCContext saved;uint8_t* memory=nullptr;std::array<uint8_t,8192> bytes{};
    CallbackContext(PPCContext& c,uint8_t* base):ctx(c),saved(c){const auto top=c.r1.u32&~15u;
        if(base && top>=12288 && GuestReadBatch::Readable(base+top-8192,8192)){
            memory=base+top-8192;std::memcpy(bytes.data(),memory,bytes.size());ctx.r1.u64=top-4096;}}
    ~CallbackContext(){if(memory)std::memcpy(memory,bytes.data(),bytes.size());std::memcpy(&ctx,&saved,sizeof(ctx));}
};
struct FixtureButtons {std::array<unsigned,2> calls{};bool nested_blocked=false,throw_on_call=false;};
thread_local FixtureButtons* fixture_buttons=nullptr;
void FixtureButtonTask(PPCContext& ctx,uint8_t* base){
    if(!fixture_buttons || ctx.r3.u32<0x5A000 || ctx.r3.u32>0x5A100 ||
       !Readable(ctx.r4.u32,436,base))throw std::logic_error("native button task ABI");
    const auto slot=(ctx.r3.u32-0x5A000)/0x100;
    if(ctx.r4.u32==0xA000+slot*512 && PPC_LOAD_U32(ctx.r4.u32+80)==4 && PPC_LOAD_U32(ctx.r4.u32+408)==2){ctx.r3.u64=0;return;}
    if(ctx.r4.u32!=0x54000+slot*512 || PPC_LOAD_U32(ctx.r4.u32+80)!=2 || PPC_LOAD_U32(ctx.r4.u32+408)!=1)
        throw std::logic_error("native button task received wrong/incomplete descriptor");
    ++fixture_buttons->calls[slot];
    auto nested=ctx;nested.r3.u64=RetailButtonAccess::Get(*delivery.receivers[slot]).actor.listener;
    nested.r4.u64=button_permit->action.event;nested.r5.u64=button_permit->action.action;
    // An ordinary nested broadcast for the OTHER actor has no replay permit.
    nested.r3.u64=RetailButtonAccess::Get(*delivery.receivers[1-slot]).actor.listener;
    fixture_buttons->nested_blocked=FilterRetailButtonInput(nested,base,false)==RetailButtonFilter::Suppressed;
    if(fixture_buttons->throw_on_call)throw std::runtime_error("native button task injected fault");
    ctx.r3.u64=0;
}
}
RetailButtonReceiver::RetailButtonReceiver():impl_(std::make_unique<Impl>()){}
RetailButtonReceiver::~RetailButtonReceiver(){
    if(std::find(delivery.receivers.begin(),delivery.receivers.end(),this)!=delivery.receivers.end())DisarmRetailButtonDelivery();
}
bool ArmRetailButtons(RetailButtonReceiver& receiver,unsigned slot,const NativeTriggerBinding& definitions,PPCContext& ctx,uint8_t* base){
    Preserve preserve;auto& s=RetailButtonAccess::Get(receiver);RetailActorInput actor;
    if(slot>1 || (s.armed && (s.thread!=GetCurrentThreadId() || s.base!=base)) ||
       !RetailActorInputFor(slot,ctx,base,actor) || !DefinitionOwner(actor,definitions,ctx,base))return false;
    const auto schema=HashNativeButtonSchema(definitions);if(!schema)return false;
    if(s.armed)return s.Current(ctx,base) && actor==s.actor && *schema==s.schema;
    auto copied=definitions;RetailActorInput after;
    if(!RetailActorInputFor(slot,ctx,base,after) || after!=actor || !DefinitionOwner(actor,copied,ctx,base))return false;
    s.thread=GetCurrentThreadId();s.base=base;s.actor=actor;s.schema=*schema;s.definitions=std::move(copied);s.armed=true;return true;
}
bool ResetRetailButtons(RetailButtonReceiver& receiver){Preserve preserve;auto& s=RetailButtonAccess::Get(receiver);
    if((s.armed && s.thread!=GetCurrentThreadId()) ||
       std::find(delivery.receivers.begin(),delivery.receivers.end(),&receiver)!=delivery.receivers.end())return false;s={};return true;}
bool PrepareRetailButtons(RetailButtonReceiver& receiver,const RoomMovement& command,PPCContext& ctx,uint8_t* base,std::vector<NativeActorAction>& out){
    Preserve preserve;auto& s=RetailButtonAccess::Get(receiver);
    if(!s.Current(ctx,base) || !ValidRoomMovement(command) || command.source!=RoomMovementSource::NativeActionV3 ||
       command.actor_epoch!=s.actor.actor_epoch || command.input_epoch!=s.actor.input_epoch || command.button_actions.schema!=s.schema)return false;
    std::vector<NativeActorAction> next;
    if(!MatchNativeButtonActions(s.definitions,command.button_actions,next) || !s.Current(ctx,base))return false;
    out=std::move(next);return true;
}
bool EnableRetailButtonDelivery(const std::array<RetailButtonReceiver*,2>& receivers,PPCContext& ctx,uint8_t* base){
    Preserve preserve;if(delivery.thread || button_permit || !receivers[0] || !receivers[1] || receivers[0]==receivers[1])return false;
    for(unsigned i=0;i<2;++i){auto& s=RetailButtonAccess::Get(*receivers[i]);if(s.actor.slot!=i || !s.Current(ctx,base))return false;}
    const auto& a=RetailButtonAccess::Get(*receivers[0]).actor;const auto& b=RetailButtonAccess::Get(*receivers[1]).actor;
    if(a.world_epoch!=b.world_epoch || a.registry!=b.registry || a.generation!=b.generation || a.load_generation!=b.load_generation ||
       !SetRetailButtonAdmission(ButtonAdmission))return false;
    delivery.thread=GetCurrentThreadId();delivery.base=base;delivery.receivers=receivers;return true;
}
void DisarmRetailButtonDelivery(){Preserve preserve;
    if((delivery.thread && delivery.thread!=GetCurrentThreadId()) || button_permit)return;
    SetRetailButtonAdmission(nullptr);delivery={};
}
ActorInputResult ValidateRetailButtonFrame(const RoomFrame& frame,PPCContext& ctx,uint8_t* base){
    Preserve preserve;if(!DeliveryOwner(base))return ActorInputResult::WrongThread;
    if(button_permit || delivery.faulted || frame.players_mask!=3 || frame.tick==UINT64_MAX ||
       (delivery.started && frame.tick!=delivery.next) || (delivery.active && delivery.frame!=frame))return ActorInputResult::InvalidTick;
    for(unsigned i=0;i<2;++i)if(!ButtonAdmission(i,frame.players[i],ctx,base))return ActorInputResult::InvalidInput;
    return ActorInputResult::Applied;
}
ActorInputResult ReplayRetailButtonBatch(unsigned slot,const RoomFrame& frame,PPCContext& ctx,uint8_t* base){
    Preserve preserve;if(slot>1)return ActorInputResult::InvalidInput;
    const auto valid=ValidateRetailButtonFrame(frame,ctx,base);if(valid!=ActorInputResult::Applied)return valid;
    if(delivery.delivered[slot])return ActorInputResult::InvalidTick;
    // Both complete commands must already be staged and Movement delivered.
    for(unsigned i=0;i<2;++i){RetailActorInput actor;RoomMovement prepared;bool moved=false;
        if(!RetailInputTickFor(i,frame.tick,ctx,base,actor,moved,&prepared) || !moved || prepared!=frame.players[i] ||
           actor!=RetailButtonAccess::Get(*delivery.receivers[i]).actor)return ActorInputResult::InvalidTick;
    }
    std::vector<NativeActorAction> actions;
    if(!PrepareRetailButtons(*delivery.receivers[slot],frame.players[slot],ctx,base,actions))return ActorInputResult::Stale;
    CallbackContext caller(ctx,base);if(!caller.memory)return ActorInputResult::InvalidContext;
    delivery.active=true;delivery.frame=frame;delivery.delivered[slot]=true;
    try{for(const auto& action:actions){
        // Callback side effects can invalidate the other slot's metadata too.
        if(ValidateRetailButtonFrame(frame,ctx,base)!=ActorInputResult::Applied){delivery.faulted=true;return ActorInputResult::Stale;}
        ButtonPermit permit{slot,action};button_permit=&permit;struct Reset{~Reset(){button_permit=nullptr;}}reset;
        ctx.r3.u64=RetailButtonAccess::Get(*delivery.receivers[slot]).actor.listener;
        ctx.r4.u64=action.event;ctx.r5.u64=action.action;ctx.f1.f64=action.x;ctx.f2.f64=action.y;
        sub_82A3C930(ctx,base);
        if(!permit.derived || permit.invalid){delivery.faulted=true;return ActorInputResult::InvalidContext;}
        if(REXCVAR_GET(em2_private_profile)){
            const auto& definitions=RetailButtonAccess::Get(*delivery.receivers[slot]).definitions.actions;
            const auto definition=std::find_if(definitions.begin(),definitions.end(),[&](const auto& d){return d.event==action.event && d.action==action.action;});
            if(definition!=definitions.end())REXLOG_INFO("Native Room button callback: tick {} slot {} action '{}' kind {} axes {}/{} base {} return {}; original derived handler",frame.tick,slot,definition->name,definition->kind,action.x,action.y,permit.base,ctx.r3.u32);
        }
    }}catch(...){delivery.faulted=true;throw;}
    return ActorInputResult::Applied;
}
ActorInputResult FinishRetailButtonTick(uint64_t tick,PPCContext& ctx,uint8_t* base,bool movie_entry){
    Preserve preserve;if(!DeliveryOwner(base))return ActorInputResult::WrongThread;
    if(!delivery.active || delivery.frame.tick!=tick || !delivery.delivered[0] || !delivery.delivered[1]){
        if(REXCVAR_GET(em2_private_profile))REXLOG_ERROR("Native button completion header rejected: tick {} expected {} active {} delivered {}/{}",tick,delivery.frame.tick,delivery.active,delivery.delivered[0],delivery.delivered[1]);
        return ActorInputResult::InvalidTick;
    }
    // Preflight and each dispatch already validated these exact definitions.
    // The native update may deliberately change its input context. Completion
    // verifies the delivered transaction and live actor ownership, not whether
    // the next frame could still use the previous gameplay definitions.
    if(button_permit || delivery.faulted || delivery.frame.players_mask!=3 ||
       tick==UINT64_MAX || (delivery.started && delivery.next!=tick))return ActorInputResult::InvalidTick;
    for(unsigned i=0;i<2;++i){RetailActorInput actor;bool moved=false;RoomMovement prepared;
        if(!RetailInputTickFor(i,tick,ctx,base,actor,moved,&prepared,movie_entry) || !moved || prepared!=delivery.frame.players[i]){
            delivery.faulted=true;return ActorInputResult::InvalidContext;}}
    for(unsigned i=0;i<2;++i)AdvanceRoomJumpStream(delivery.streams[i],delivery.frame.players[i]);
    delivery.started=true;delivery.active=false;delivery.next=tick+1;delivery.delivered={};return ActorInputResult::Applied;
}
RetailButtonFilter FilterRetailButtonInput(PPCContext& ctx,uint8_t* base,bool base_handler){
    Preserve preserve;if(!DeliveryOwner(base) || !Readable(ctx.r5.u32,4,base))return RetailButtonFilter::Unowned;
    for(unsigned slot=0;slot<2;++slot){auto& s=RetailButtonAccess::Get(*delivery.receivers[slot]);
        if(ctx.r3.u32!=s.actor.listener)continue;RetailActorInput actor;
        if(!RetailInputQuarantineFor(slot,ctx,base,actor) || actor!=s.actor)return RetailButtonFilter::Unowned;
        const auto uid=PPC_LOAD_U32(ctx.r5.u32);
        // Movement has a separate replay permit and remains in its adapter.
        if(uid==PPC_LOAD_U32(s.definitions.parent.action))return RetailButtonFilter::Unowned;
        auto suppress=[&]{ctx.r3.u64=0;return RetailButtonFilter::Suppressed;};
        const auto* p=button_permit;
        if(!p || p->slot!=slot)return suppress();
        auto& permit=*button_permit;
        if(delivery.faulted || !delivery.active || ctx.r4.u32!=permit.action.event || ctx.r5.u32!=permit.action.action ||
           ctx.f1.f64!=permit.action.x || ctx.f2.f64!=permit.action.y || !s.Current(ctx,base)){permit.invalid=true;return suppress();}
        if(!base_handler){if(permit.derived){permit.invalid=true;return suppress();}permit.derived=true;return RetailButtonFilter::Authorized;}
        if(!permit.derived || permit.base || ctx.lr!=0x82A3CD30){permit.invalid=true;return suppress();}
        permit.base=true;return RetailButtonFilter::Authorized;
    }
    return RetailButtonFilter::Unowned;
}
bool TestRetailButtonReceivers(PPCContext& ctx,uint8_t* base,const std::function<bool(const RoomFrame&,const std::function<void()>&)>& drive){
    Preserve preserve;
    struct Restore {
        uint8_t* base;std::vector<uint8_t> bytes;uint32_t threshold;
        ~Restore(){std::memcpy(base,bytes.data(),bytes.size());PPC_STORE_U32(0x833B65F8,threshold);}
    }restore{base,std::vector<uint8_t>(base,base+0x60000),PPC_LOAD_U32(0x833B65F8)};
    const auto saved=ctx;const auto csr=_mm_getcsr();const auto last=GetLastError();
    unsigned checks=0;auto check=[&](bool okay,const char* why){++checks;if(!okay)REXLOG_ERROR("Retail native buttons: {}",why);return okay;};
    std::array<RetailActorInput,2> actors;
    for(unsigned slot=0;slot<2;++slot)if(!RetailActorInputFor(slot,ctx,base,actors[slot]))return false;
    constexpr uint32_t manager=0xC000,event=manager+24,name=0x58000;
    std::memcpy(base+name,"fixture-game",13);PPC_STORE_U32(manager+16,name);PPC_STORE_U32(manager+4,0x58100);PPC_STORE_U32(manager+8,1);PPC_STORE_U32(0x58100,event);
    PPC_STORE_U32(event,name);PPC_STORE_U32(event+20,4);PPC_STORE_U32(0x833B65F8,32);
    std::array<NativeTriggerBinding,2> definitions;
    std::array<RoomMovement,2> commands;
    for(unsigned slot=0;slot<2;++slot){const auto movement=0xA000u+slot*512,button=0x54000u+slot*512,label=0x59000u+slot*128;
        PPC_STORE_U32(movement,PPC_LOAD_U32(0x833DAEB8+slot*12));PPC_STORE_U32(movement+80,4);PPC_STORE_U32(movement+408,2);
        PPC_STORE_U32(movement+88,slot);PPC_STORE_U32(movement+120,slot);PPC_STORE_U32(movement+92,0x02400018);PPC_STORE_U32(movement+124,0x02400017);
        PPC_STORE_U32(0xB100+slot*4,movement);PPC_STORE_U32(0xB100+(slot+2)*4,button);
        PPC_STORE_U32(0xB000+slot*4,actors[slot].listener);std::memcpy(base+label,slot?"Interact2":"Interact",slot?10:9);
        // Native heap NiFixedString stores length before its data pointer.
        PPC_STORE_U16(label-2,slot?9:8);
        PPC_STORE_U32(button,label);PPC_STORE_U32(button+80,2);PPC_STORE_U32(button+88,slot);PPC_STORE_U32(button+92,0x01400010);PPC_STORE_U32(button+408,1);
    }
    std::array<RetailButtonReceiver,2> receivers;
    for(unsigned slot=0;slot<2;++slot){auto& b=definitions[slot];const auto& actor=actors[slot];
        auto& p=b.parent;p.world_epoch=actor.world_epoch;p.registry=actor.registry;p.retail_slot=slot;p.entity=actor.entity;p.controller=actor.controller;
        p.listener=actor.listener;p.manager=manager;p.event=event;p.action=0xA000+slot*512;p.active_event=event;p.active_event_uid=name;p.logical_user=slot;
        b.scope=NativeButtonScope::DigitalAndTriggers;b.threshold=32;b.active_context_name="fixture-game";
        NativeTriggerContext context;context.event=event;context.retail_listener=true;
        for(unsigned i=0;i<6;++i)context.header[i]=PPC_LOAD_U32(event+i*4);
        context.header[1]=0;context.header[2]=1;context.header[3]=0;context.listeners.push_back(p.listener);
        for(unsigned i=0;i<4;++i)context.action_order.push_back(PPC_LOAD_U32(0xB100+i*4));b.contexts.push_back(context);
        NativeTriggerAction a;a.event=event;a.action=0x54000+slot*512;a.ordinal=slot+2;a.uid=0x59000+slot*128;a.name=slot?"Interact2":"Interact";a.kind=2;a.key=0x01400010;
        std::memcpy(a.shape.data(),base+a.action,a.shape.size());b.actions.push_back(a);
        NativeTriggerBatch batch;batch.source_epoch=1;batch.source_tick=10;batch.count=1;batch.actions[0]={0,true,0,0};
        auto& command=commands[slot];command.source=RoomMovementSource::NativeActionV3;command.actor_epoch=actor.actor_epoch;command.jump_button_mask=0x1000;
        if(!check(EncodeNativeButtonActions(b,batch,command.button_actions) && ArmRetailButtons(receivers[slot],slot,b,ctx,base),"both real retail owners arm receiver definitions without a local physical producer lease"))return false;
    }
    std::vector<NativeActorAction> out;
    const auto before_preflight=std::vector<uint8_t>(base,base+0x60000);
    for(unsigned slot=0;slot<2;++slot)if(!check(PrepareRetailButtons(receivers[slot],commands[slot],ctx,base,out) && out.size()==1 && out[0].action==0x54000+slot*512,
        "independent Mickey/Oswald preflight resolves only its own live descriptor"))return false;
    if(!check(!std::memcmp(base,before_preflight.data(),before_preflight.size()),"native owner/context selection and metadata preflight leave guest memory and scratch unchanged"))return false;
    auto bad=commands[0];bad.button_actions.schema[0]^=1;
    if(!check(!PrepareRetailButtons(receivers[0],bad,ctx,base,out) && out.size()==1 && out[0].action==0x54200 &&
        PrepareRetailButtons(receivers[0],commands[0],ctx,base,out),"foreign packet leaves output and valid receiver retry intact"))return false;
    bad=commands[1];bad.actor_epoch=actors[0].actor_epoch;
    if(!check(!PrepareRetailButtons(receivers[1],bad,ctx,base,out),"actor incarnation cannot be substituted by a packet"))return false;
    PPC_STORE_U8(0x54000+105,1);PPC_STORE_U32(0x54000+112,0x12345678);
    if(!check(PrepareRetailButtons(receivers[0],commands[0],ctx,base,out),"ordinary digital latch/timer changes do not retire immutable definitions"))return false;
    PPC_STORE_U32(0x54000+92,0x01400013);
    if(!check(!PrepareRetailButtons(receivers[0],commands[0],ctx,base,out),"changed native binding rejects before any descriptor is published"))return false;
    PPC_STORE_U32(0x54000+92,0x01400010);
    if(!check(!PrepareRetailButtons(receivers[0],commands[0],ctx,base,out) && ResetRetailButtons(receivers[0]) &&
        ArmRetailButtons(receivers[0],0,definitions[0],ctx,base),"restoring bytes cannot resurrect a retired receiver without explicit reset"))return false;
    bool foreign=false;std::thread thread([&]{auto c=ctx;std::vector<NativeActorAction> result;foreign=PrepareRetailButtons(receivers[0],commands[0],c,base,result) || ResetRetailButtons(receivers[0]);});thread.join();
    if(!check(!foreign && PrepareRetailButtons(receivers[0],commands[0],ctx,base,out),"foreign threads cannot consume or reset the receiver"))return false;
    auto wrong=definitions[0];wrong.parent.listener=actors[1].listener;RetailButtonReceiver unowned;
    if(!check(!ArmRetailButtons(unowned,0,wrong,ctx,base),"another controller's listener cannot mint receiver ownership"))return false;
    PPC_STORE_U8(name,'F');
    if(!check(!PrepareRetailButtons(receivers[1],commands[1],ctx,base,out),"mutated context name retires a receiver even when its UID pointer is unchanged"))return false;
    PPC_STORE_U8(name,'f');
    if(!check(!PrepareRetailButtons(receivers[1],commands[1],ctx,base,out) && ResetRetailButtons(receivers[1]) &&
        ArmRetailButtons(receivers[1],1,definitions[1],ctx,base),"context restoration requires explicit receiver rearm"))return false;
    DWORD protection=0;if(!VirtualProtect(base+0x54000,4096,PAGE_NOACCESS,&protection))return false;
    const bool inaccessible=!PrepareRetailButtons(receivers[0],commands[0],ctx,base,out);DWORD ignored=0;
    VirtualProtect(base+0x54000,4096,protection,&ignored);
    if(!check(inaccessible && !PrepareRetailButtons(receivers[0],commands[0],ctx,base,out),"protected action metadata rejects safely and cannot resurrect after protection restoration"))return false;
    if(!check(ResetRetailButtons(receivers[0]) && ArmRetailButtons(receivers[0],0,definitions[0],ctx,base),"explicit new capture can rearm after protected metadata recovers"))return false;
    const auto entity=actors[1].entity,old_guid=PPC_LOAD_U32(entity+4);PPC_STORE_U32(entity+4,999);
    if(!check(!PrepareRetailButtons(receivers[1],commands[1],ctx,base,out),"native actor reuse retires the receiver"))return false;
    PPC_STORE_U32(entity+4,old_guid);
    if(!check(!PrepareRetailButtons(receivers[1],commands[1],ctx,base,out),"restoring an actor GUID does not silently rearm its old receiver"))return false;
    if(!check(!std::memcmp(&ctx,&saved,sizeof(ctx)) && _mm_getcsr()==csr && GetLastError()==last,"receiver preflight preserves full caller and host state"))return false;
    if(drive){
        for(unsigned slot=0;slot<2;++slot)if(!ResetRetailButtons(receivers[slot]) || !ArmRetailButtons(receivers[slot],slot,definitions[slot],ctx,base))return false;
        constexpr auto code=uint32_t(PPC_CODE_BASE)+108;auto* pointer=&PPC_LOOKUP_FUNC(base,code);
        if(!VirtualAlloc(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pointer)&~uintptr_t(4095)),4096,MEM_COMMIT,PAGE_READWRITE))return false;
        struct Cleanup {PPCFunc** pointer;PPCFunc* previous;~Cleanup(){DisarmRetailButtonDelivery();fixture_buttons=nullptr;*pointer=previous;}}cleanup{pointer,*pointer};
        *pointer=FixtureButtonTask;FixtureButtons observed;fixture_buttons=&observed;
        PPC_STORE_U32(0x5B000+40,code);
        for(unsigned slot=0;slot<2;++slot){const auto task=0x5A000u+slot*256,list=0x5B100u+slot*16;
            PPC_STORE_U32(task,0x5B000);PPC_STORE_U32(task+4,actors[slot].entity);PPC_STORE_U32(list,task);
            PPC_STORE_U32(actors[slot].listener+112,list);PPC_STORE_U32(actors[slot].listener+116,1);
        }
        if(!check(EnableRetailButtonDelivery({&receivers[0],&receivers[1]},ctx,base) &&
            !ResetRetailButtons(receivers[0]),"delivery pins both receiver snapshots until explicit teardown"))return false;
        RoomFrame frame;frame.tick=10;frame.players_mask=3;for(unsigned i=0;i<2;++i)frame.players[i]=commands[i];
        auto bad=frame;bad.players[1].button_actions.schema[0]^=1;
        if(!check(ValidateRetailButtonFrame(bad,ctx,base)!=ActorInputResult::Applied && observed.calls==std::array<unsigned,2>{},
            "later-slot schema mismatch rejects before either native callback"))return false;
        if(!check(ReplayRetailButtonBatch(0,frame,ctx,base)==ActorInputResult::InvalidTick && observed.calls==std::array<unsigned,2>{},
            "a valid packet without both actual staged native movements cannot dispatch"))return false;
        const bool first_driven=drive(frame,{});
        if(!first_driven || observed.calls!=std::array<unsigned,2>{1,1} || !observed.nested_blocked)
            REXLOG_ERROR("Native button fixture drive={} calls={}/{} nested={} active={} faulted={}",first_driven,observed.calls[0],observed.calls[1],observed.nested_blocked,delivery.active,delivery.faulted);
        if(!check(first_driven && observed.calls==std::array<unsigned,2>{1,1} && observed.nested_blocked,
            "actual native derived/base handlers deliver each full descriptor once and quarantine nested foreign input"))return false;
        if(!check(ReplayRetailButtonBatch(0,frame,ctx,base)!=ActorInputResult::Applied && observed.calls==std::array<unsigned,2>{1,1},
            "completed tick cannot redeliver a native button"))return false;
        auto ordinary=saved;ordinary.r3.u64=actors[0].listener;ordinary.r4.u64=event;ordinary.r5.u64=0x54000;
        if(!check(FilterRetailButtonInput(ordinary,base,false)==RetailButtonFilter::Suppressed,
            "ordinary button input remains quarantined between agreed ticks"))return false;
        ++frame.tick;for(auto& c:frame.players)if(c.source==RoomMovementSource::NativeActionV3)++c.button_actions.source_tick;
        // Clear both actions to exercise a real neutral batch, not missing input.
        frame.players[0].button_actions.fired={};frame.players[1].button_actions.fired={};
        RetailButtonReceiver changed_receiver;
        if(!ArmRetailButtons(changed_receiver,0,definitions[0],ctx,base))return false;
        if(!check(drive(frame,[&]{PPC_STORE_U32(0x54000+92,0x01400011);}) && observed.calls==std::array<unsigned,2>{1,1},
            "completed neutral native update may change input definitions without invalidating already delivered actions"))return false;
        auto changed=frame;++changed.tick;for(auto& c:changed.players)++c.button_actions.source_tick;
        std::vector<NativeActorAction> changed_actions;
        if(!check(!PrepareRetailButtons(changed_receiver,changed.players[0],ctx,base,changed_actions),
            "changed definition rejects next-frame receiver preflight before native callbacks"))return false;
        PPC_STORE_U32(0x54000+92,0x01400010);
        if(!check(!PrepareRetailButtons(changed_receiver,changed.players[0],ctx,base,changed_actions),
            "restoring changed metadata does not resurrect a retired button receiver"))return false;
        if(!check(ResetRetailButtons(changed_receiver) && ArmRetailButtons(changed_receiver,0,definitions[0],ctx,base) &&
            PrepareRetailButtons(changed_receiver,changed.players[0],ctx,base,changed_actions),
            "only explicit fresh receiver capture re-arms a definition retired at admission"))return false;
        ++frame.tick;frame.players[0]=commands[0];frame.players[1]=commands[1];
        frame.players[0].button_actions.source_tick=frame.players[1].button_actions.source_tick=12;
        observed.throw_on_call=true;bool threw=false;try{drive(frame,{});}catch(const std::runtime_error&){threw=true;}
        if(!check(threw && observed.calls==std::array<unsigned,2>{2,1} && delivery.faulted && !button_permit &&
            ValidateRetailButtonFrame(frame,ctx,base)!=ActorInputResult::Applied,
            "native callback exception retires the partial frame and clears permission without calling the second actor"))return false;
        ordinary=saved;ordinary.r3.u64=actors[0].listener;ordinary.r4.u64=event;ordinary.r5.u64=0x54000;
        if(!check(FilterRetailButtonInput(ordinary,base,false)==RetailButtonFilter::Suppressed &&
            !std::memcmp(&ctx,&saved,sizeof(ctx)),"faulted native delivery retains quarantine and restores caller context"))return false;
    }
    REXLOG_INFO("Retail native button receivers passed {} owner/definition/stale/thread/transaction checks; native callbacks exercised {}",checks,bool(drive));return true;
}
}
