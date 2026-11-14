#include "online_cutscene_input.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include "allocation_trace.h"
#include <rex/logging.h>
#include <cstring>
#include <windows.h>
#include <immintrin.h>
#include <rex/cvar.h>
REXCVAR_DECLARE(bool,em2_private_profile);
PPC_EXTERN_FUNC(__imp__sub_827C3D78);
namespace em2::net {
namespace {
bool UiReadable(uint8_t* base,uint32_t p,size_t n){return base && p>=4096 && uint64_t(p)+n<=0x100000000ull && GuestReadBatch::Readable(base+p,n);}
bool UiName(uint8_t* base,uint32_t p,std::string& out){
    if(!UiReadable(base,p,96))return false;
    const auto* first=reinterpret_cast<const char*>(base+p);const auto* end=static_cast<const char*>(std::memchr(first,0,96));
    if(!end || first==end)return false;out.assign(first,end);return true;
}
struct UiPreserve {
    PPCContext& ctx;PPCContext saved;unsigned fp=_mm_getcsr();DWORD error=GetLastError();uint8_t* stack=nullptr;std::array<uint8_t,8192> bytes{};
    UiPreserve(PPCContext& c,uint8_t* base):ctx(c),saved(c){const auto top=c.r1.u32&~15u;
        if(top>=12288 && UiReadable(base,top-8192,8192)){stack=base+top-8192;std::memcpy(bytes.data(),stack,bytes.size());ctx.r1.u64=top-4096;}}
    ~UiPreserve(){if(stack)std::memcpy(stack,bytes.data(),bytes.size());ctx=saved;_mm_setcsr(fp);SetLastError(error);}
};
}
bool InspectNativeCutsceneUi(PPCContext& ctx,uint8_t* base,uint32_t registry,NativeCutsceneUiSnapshot& out){
    UiPreserve preserve(ctx,base);NativeGameOverState state;uint32_t mode=0;
    if(!preserve.stack || !ReadNativeControlState(ctx,base,state,mode) || (mode!=3 && mode!=6) || state.registry!=registry || !UiReadable(base,0x833DBAD8,4))return false;
    const auto manager=PPC_LOAD_U32(0x833DBAD8);if(!UiReadable(base,manager,48))return false;
    const auto uid=PPC_LOAD_U32(manager+16);std::string name;if(!UiName(base,uid,name) || name!="CutScene")return false;
    ctx.r3.u64=manager;ctx.r4.u64=manager+16;__imp__sub_827C3D78(ctx,base);const auto selected=ctx.r3.u32;
    if(!UiReadable(base,selected,24) || PPC_LOAD_U32(selected)!=uid)return false;
    NativeCutsceneUiSnapshot candidate;candidate.manager=manager;
    const std::array<uint32_t,2> events{manager+24,selected};
    for(unsigned context=0;context<2;++context){const auto event=events[context];if(context && event==events[0])continue;
        if(!UiReadable(base,event,24))return false;
        NativeCutsceneUiEvent snapshot;snapshot.event=event;
        if(!UiName(base,PPC_LOAD_U32(event),snapshot.name))return false;
        const auto listeners=PPC_LOAD_U32(event+4),listener_count=PPC_LOAD_U32(event+8),actions=PPC_LOAD_U32(event+16),count=PPC_LOAD_U32(event+20);
        if(listener_count>128 || count>256 || (listener_count && !UiReadable(base,listeners,size_t(listener_count)*4)) || (count && !UiReadable(base,actions,size_t(count)*4)))return false;
        for(unsigned i=0;i<listener_count;++i){const auto listener=PPC_LOAD_U32(listeners+i*4);
            if(!UiReadable(base,listener,4))return false;const auto table=PPC_LOAD_U32(listener);
            if(!UiReadable(base,table,4))return false;
            snapshot.listeners.push_back({listener,table,PPC_LOAD_U32(table)});
        }
        for(unsigned i=0;i<count;++i){const auto action=PPC_LOAD_U32(actions+i*4);if(!UiReadable(base,action,436))return false;
            NativeCutsceneUiAction item;item.event=event;item.ordinal=i;item.action=action;item.kind=PPC_LOAD_U32(action+80);item.condition=PPC_LOAD_U32(action+412);item.curve=PPC_LOAD_U32(action+432);
            if(!UiName(base,PPC_LOAD_U32(action),item.name))return false;
            const auto bindings=PPC_LOAD_U32(action+408);if(bindings>10)return false;
            for(unsigned b=0;b<bindings;++b){std::array<uint32_t,8> words{};for(unsigned k=0;k<8;++k)words[k]=PPC_LOAD_U32(action+88+b*32+k*4);item.bindings.push_back(words);}
            snapshot.actions.push_back(std::move(item));
        }
        if(PPC_LOAD_U32(event+4)!=listeners || PPC_LOAD_U32(event+8)!=listener_count || PPC_LOAD_U32(event+16)!=actions || PPC_LOAD_U32(event+20)!=count)return false;
        candidate.contexts.push_back(std::move(snapshot));
    }
    if(PPC_LOAD_U32(0x833DBAD8)!=manager || PPC_LOAD_U32(manager+16)!=uid)return false;
    out=std::move(candidate);return true;
}
void TracePrivateNativeCutsceneUi(uint64_t epoch,PPCContext& ctx,uint8_t* base,uint32_t registry){
    if(!REXCVAR_GET(em2_private_profile))return;
    static thread_local uint64_t recorded=0,recorded_load=UINT64_MAX;
    const auto load=em2::CurrentNativeLoadGeneration();if(recorded==epoch && recorded_load==load)return;
    NativeCutsceneUiSnapshot snapshot;if(!InspectNativeCutsceneUi(ctx,base,registry,snapshot))return;
    recorded=epoch;recorded_load=load;
    REXLOG_INFO("Private native cutscene UI inventory: input epoch {} manager {:08X} contexts {}; read-only, no action evaluation or dispatch",epoch,snapshot.manager,snapshot.contexts.size());
    for(const auto& context:snapshot.contexts){
        REXLOG_INFO("Private native cutscene UI context: '{}' event {:08X} listeners {} actions {}",context.name,context.event,context.listeners.size(),context.actions.size());
        for(const auto& listener:context.listeners)REXLOG_INFO("Private native cutscene UI listener: '{}' address {:08X} table {:08X} callback {:08X}",context.name,listener[0],listener[1],listener[2]);
        for(const auto& action:context.actions){
            REXLOG_INFO("Private native cutscene UI action: '{}' ordinal {} '{}' record {:08X} kind {} condition {} curve {} bindings {}",context.name,action.ordinal,action.name,action.action,action.kind,action.condition,action.curve,action.bindings.size());
            for(const auto& b:action.bindings)REXLOG_INFO("Private native cutscene UI binding: '{}' '{}' user {} key {:08X} destination {:08X} repeat {:08X} flags {:08X} state {:08X}/{:08X}/{:08X}",context.name,action.name,b[0],b[1],b[2],b[5],b[4],b[3],b[6],b[7]);
        }
    }
}

bool ReadNativeInputContextName(uint8_t* base,std::string& out){
    if(!base || !GuestReadBatch::Readable(base+0x833DBAD8,4))return false;
    const auto manager=PPC_LOAD_U32(0x833DBAD8);
    if(manager<4096 || !GuestReadBatch::Readable(base+manager,20))return false;
    const auto name=PPC_LOAD_U32(manager+16);
    if(name<4096 || !GuestReadBatch::Readable(base+name,96))return false;
    const auto* begin=reinterpret_cast<const char*>(base+name);const auto* end=static_cast<const char*>(std::memchr(begin,0,96));
    if(!end || begin==end)return false;
    out.assign(begin,end);return true;
}
bool ReadNativeCutsceneContext(PPCContext& ctx,uint8_t* base,uint32_t registry){
    NativeGameOverState state;uint32_t mode=0;
    if(!ReadNativeControlState(ctx,base,state,mode) || (mode!=3 && mode!=6) || state.registry!=registry ||
       !GuestReadBatch::Readable(base+0x833DBAD8,4))return false;
    if(mode==6){uint32_t profile=0,object=0,handler=0;
        return ReadNativeControlTarget(ctx,base,registry,6,profile,object,handler) && handler==0x82B210B0;
    }
    const auto manager=PPC_LOAD_U32(0x833DBAD8);
    if(manager<4096 || !GuestReadBatch::Readable(base+manager,20))return false;
    const auto name=PPC_LOAD_U32(manager+16);
    const bool selected=name>=4096 && GuestReadBatch::Readable(base+name,9) && !std::memcmp(base+name,"CutScene",9);
    // RestrictCutSceneInput updates state 3 and each retail info's balanced
    // restriction count before the normal input-context selector catches up.
    // These are actual native restriction counters, never missing-binding
    // inference. Both original human records must be restricted for this gate.
    uint32_t counts[2]{};bool restricted=false;
    if(GuestReadBatch::Readable(base+0x833DAB60,4)){
        const auto players=PPC_LOAD_U32(0x833DAB60);
        if(players>=4096 && GuestReadBatch::Readable(base+players,640)){
            restricted=true;
            for(unsigned i=0;i<2;++i){const auto info=PPC_LOAD_U32(players+i*320+28);
                if(info<4096 || !GuestReadBatch::Readable(base+info,936)){restricted=false;continue;}
                counts[i]=PPC_LOAD_U32(info+932);restricted=restricted && counts[i]>0 && counts[i]<=64;}
        }
    }
    if(REXCVAR_GET(em2_private_profile)){
        static thread_local uint32_t previous_name=0,previous_a=0,previous_b=0;
        if(previous_name!=name || previous_a!=counts[0] || previous_b!=counts[1]){
            std::string label="<unreadable>";
            if(name>=4098 && GuestReadBatch::Readable(base+name-2,2)){
                const auto size=PPC_LOAD_U16(name-2);
                if(size<=63 && GuestReadBatch::Readable(base+name,size+1) && base[name+size]==0)label.assign(reinterpret_cast<const char*>(base+name),size);}
            REXLOG_INFO("Private native cutscene admission snapshot: mode 3 context '{}' restrictions {}/{} selected {} both restricted {}",label,counts[0],counts[1],selected,restricted);
            previous_name=name;previous_a=counts[0];previous_b=counts[1];
        }
    }
    return selected || restricted;
}
RoomMovement NativeCutsceneCommand(uint64_t epoch){RoomMovement c;c.actor_epoch=1;c.input_epoch=epoch;c.source=RoomMovementSource::NativeCutsceneV5;return c;}
RoomMovement NativeCutsceneUiCommand(uint64_t epoch,bool down){auto c=NativeCutsceneCommand(epoch);c.source=RoomMovementSource::NativeCutsceneUiV6;c.pad.buttons=down?0x1000:0;return c;}
bool NativeCutsceneReturnPrefix(const RoomFrame& frame,uint16_t mask,uint64_t epoch){
    if(!mask || frame.players_mask!=mask || !epoch || (epoch&1))return false;
    for(unsigned i=0;i<kOnlineRoomCapacity;++i){const auto& c=frame.players[i];
        if(!(mask&(1u<<i))){if(c!=RoomMovement{})return false;continue;}
        if(c.actor_epoch!=1 || c.input_epoch!=epoch || !ValidRoomMovement(c) ||
           (c.source!=RoomMovementSource::NativeCutsceneV5 && c.source!=RoomMovementSource::NativeCutsceneUiV6))return false;
    }
    return true;
}
bool ValidNativeCutsceneFrame(const RoomFrame& frame,uint16_t mask,uint64_t entry,uint8_t delay,uint64_t epoch,
    const std::array<std::array<uint8_t,32>,kOnlineRoomCapacity>& schemas,bool buttons){
    if(!epoch || (epoch&1) || !mask || frame.players_mask!=mask || entry>UINT64_MAX-delay || frame.tick<=entry || frame.tick==UINT64_MAX)return false;
    if(frame.tick>entry+delay)return NativeCutsceneReturnPrefix(frame,mask,epoch);
    for(unsigned i=0;i<kOnlineRoomCapacity;++i){const auto& c=frame.players[i];
        if(!(mask&(1u<<i))){if(c!=RoomMovement{})return false;continue;}
        if(c.actor_epoch!=1 || c.input_epoch!=epoch-1 || !ValidRoomMovement(c))return false;
        RoomMovement neutral;neutral.actor_epoch=1;neutral.input_epoch=epoch-1;neutral.source=RoomMovementSource::NativeActionV1;
        if(c==neutral)continue;
        if(buttons){if(c.source!=RoomMovementSource::NativeActionV3 || schemas[i]==std::array<uint8_t,32>{} ||
            c.button_actions.schema!=schemas[i] || frame.tick<delay || c.button_actions.source_tick!=frame.tick-delay)return false;}
        else if(c.source!=RoomMovementSource::NativeActionV2)return false;
    }return true;
}
bool TestNativeCutsceneInput(){
    unsigned count=0;auto check=[&](bool yes,const char* why){++count;if(!yes)REXLOG_ERROR("Cutscene input epoch: {}",why);return yes;};
    auto pause=NativeCutsceneCommand(2);RoomTransportInputStream stream;RoomMovement active;active.actor_epoch=1;active.source=RoomMovementSource::NativeActionV2;active.jump_button_mask=0x1000;
    if(!check(AdvanceRoomTransportInput(stream,active) && AdvanceRoomTransportInput(stream,pause) && stream.suspended && stream.input_epoch==2,"explicit even epoch suspends existing gameplay stream"))return false;
    const auto frozen=stream;
    if(!check(!AdvanceRoomTransportInput(stream,active) && stream==frozen,"old gameplay epoch cannot resume suspended stream"))return false;
    auto skip=pause;skip.input_epoch=4;
    if(!check(!AdvanceRoomTransportInput(stream,skip) && stream==frozen,"context epochs cannot skip or relabel an ongoing suspension"))return false;
    active.input_epoch=3;
    if(!check(AdvanceRoomTransportInput(stream,active) && !stream.suspended && stream.input_epoch==3,"new odd epoch explicitly starts fresh edge history"))return false;
    const auto resumed=stream;
    if(!check(!AdvanceRoomTransportInput(stream,pause) && stream==resumed,"retired cutscene epoch cannot return after resume"))return false;
    pause.input_epoch=4;
    if(!check(AdvanceRoomTransportInput(stream,pause),"second authored cutscene gets a distinct epoch"))return false;
    RoomFrame frame;frame.tick=703;frame.players_mask=7;std::array<std::array<uint8_t,32>,kOnlineRoomCapacity> schemas{};
    for(unsigned i=0;i<3;++i){frame.players[i]=NativeCutsceneCommand(2);schemas[i][0]=uint8_t(i+1);}
    if(!check(ValidNativeCutsceneFrame(frame,7,700,2,2,schemas,true),"all-peer no-action phase after old input delay"))return false;
    auto bad=frame;bad.players[1].pad.buttons=1;
    if(!check(!ValidNativeCutsceneFrame(bad,7,700,2,2,schemas,true) && !ValidRoomMovement(bad.players[1]),"cutscene source cannot hide gameplay intent"))return false;
    frame.tick=701;
    if(!check(!ValidNativeCutsceneFrame(frame,7,700,2,2,schemas,true),"old produced prefix cannot be overwritten by new cutscene commands"))return false;
    for(unsigned i=0;i<3;++i){auto& c=frame.players[i];c=active;c.input_epoch=1;c.source=RoomMovementSource::NativeActionV3;c.button_actions.schema=schemas[i];c.button_actions.source_tick=699;}
    if(!check(ValidNativeCutsceneFrame(frame,7,700,2,2,schemas,true),"old schema/source epoch remains required inside delayed prefix"))return false;
    bad=frame;bad.players[2].input_epoch=3;
    if(!check(!ValidNativeCutsceneFrame(bad,7,700,2,2,schemas,true),"new gameplay epoch cannot be admitted as an old prefix"))return false;
    NativeCutsceneUiGate gate;
    if(!check(!gate.Sample(true) && !gate.Sample(true) && !gate.Sample(false) && gate.Sample(true) && !gate.Sample(false),"carried gameplay press cannot become cutscene UI until a real release"))return false;
    auto ui=NativeCutsceneUiCommand(2,true);RoomJumpStream gameplay;
    if(!check(ValidRoomMovement(ui) && !AdvanceRoomJumpStream(gameplay,ui),"typed skip intent is not native gameplay or Jump input"))return false;
    for(unsigned field=0;field<9;++field){auto bad=ui;
        if(field==0)bad.pad.buttons=0x2000;if(field==1)bad.pad.left_x=1;if(field==2)bad.pad.right_trigger=1;
        if(field==3)bad.button_actions.schema[0]=1;if(field==4)bad.input_epoch=3;if(field==5)bad.x=-0.f;
        if(field==6)bad.jump_button_mask=0x1000;if(field==7)bad.pad.menu_pause=true;if(field==8)bad.actor_epoch=0;
        if(!check(!ValidRoomMovement(bad),"UI command cannot hide other controls, gameplay metadata or wrong lifecycle"))return false;
    }
    RoomTransportInputStream transport;
    if(!check(AdvanceRoomTransportInput(transport,ui) && transport.suspended && transport.input_epoch==2 &&
        AdvanceRoomTransportInput(transport,NativeCutsceneUiCommand(2,false)),"UI down/release crosses explicit suspended transport epoch"))return false;
    frame.tick=704;for(unsigned i=0;i<3;++i)frame.players[i]=i==2?ui:NativeCutsceneCommand(2);
    if(!check(ValidNativeCutsceneFrame(frame,7,700,2,2,schemas,true),"extra peer skip intent coexists with neutral suspended retail peers"))return false;
    REXLOG_INFO("Cutscene input epoch passed {} stream/prefix/intent checks; native return and controls require real worlds",count);return true;
}
}
