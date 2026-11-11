#include "online_retail_buttons.h"
#include "online_retail_input.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <cstring>
#include <thread>
PPC_EXTERN_FUNC(__imp__sub_827C3D78);
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
        if(!RetailActorInputFor(actor.slot,ctx,memory,current) || current!=actor || !DefinitionOwner(actor,definitions,ctx,memory)){
            stale=true;return false;
        }
        return true;
    }
};
struct RetailButtonAccess {static auto& Get(RetailButtonReceiver& r){return *r.impl_;}};
RetailButtonReceiver::RetailButtonReceiver():impl_(std::make_unique<Impl>()){}
RetailButtonReceiver::~RetailButtonReceiver()=default;
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
    if(s.armed && s.thread!=GetCurrentThreadId())return false;s={};return true;}
bool PrepareRetailButtons(RetailButtonReceiver& receiver,const RoomMovement& command,PPCContext& ctx,uint8_t* base,std::vector<NativeActorAction>& out){
    Preserve preserve;auto& s=RetailButtonAccess::Get(receiver);
    if(!s.Current(ctx,base) || !ValidRoomMovement(command) || command.source!=RoomMovementSource::NativeActionV3 ||
       command.actor_epoch!=s.actor.actor_epoch || command.button_actions.schema!=s.schema)return false;
    std::vector<NativeActorAction> next;
    if(!MatchNativeButtonActions(s.definitions,command.button_actions,next) || !s.Current(ctx,base))return false;
    out=std::move(next);return true;
}
bool TestRetailButtonReceivers(PPCContext& ctx,uint8_t* base){
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
        PPC_STORE_U32(button,label);PPC_STORE_U32(button+80,2);PPC_STORE_U32(button+88,slot);PPC_STORE_U32(button+92,0x01400010);PPC_STORE_U32(button+408,1);
    }
    std::array<RetailButtonReceiver,2> receivers;
    for(unsigned slot=0;slot<2;++slot){auto& b=definitions[slot];const auto& actor=actors[slot];
        auto& p=b.parent;p.world_epoch=actor.world_epoch;p.registry=actor.registry;p.retail_slot=slot;p.entity=actor.entity;p.controller=actor.controller;
        p.listener=actor.listener;p.manager=manager;p.event=event;p.action=0xA000+slot*512;p.active_event=event;p.active_event_uid=name;p.logical_user=slot;
        b.scope=NativeButtonScope::DigitalAndTriggers;b.threshold=32;b.active_context_name="fixture-game";
        NativeTriggerContext context;context.event=event;context.retail_listener=true;
        for(unsigned i=0;i<6;++i)context.header[i]=PPC_LOAD_U32(event+i*4);
        for(unsigned i=0;i<2;++i)context.listeners.push_back(PPC_LOAD_U32(0xB000+i*4));
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
    REXLOG_INFO("Retail native button receivers passed {} owner/definition/stale/thread/transaction checks; read-only preflight, no callbacks",checks);return true;
}
}
