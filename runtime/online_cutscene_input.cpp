#include "online_cutscene_input.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <cstring>
#include <rex/cvar.h>
REXCVAR_DECLARE(bool,em2_private_profile);
namespace em2::net {
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
    if(!ReadNativeControlState(ctx,base,state,mode) || mode!=3 || state.registry!=registry ||
       !GuestReadBatch::Readable(base+0x833DBAD8,4))return false;
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
bool NativeCutsceneReturnPrefix(const RoomFrame& frame,uint16_t mask,uint64_t epoch){
    if(!mask || frame.players_mask!=mask || !epoch || (epoch&1))return false;
    for(unsigned i=0;i<kOnlineRoomCapacity;++i)if(frame.players[i]!=((mask&(1u<<i))?NativeCutsceneCommand(epoch):RoomMovement{}))return false;
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
    REXLOG_INFO("Cutscene input epoch passed {} stream/prefix/intent checks; native return and controls require real worlds",count);return true;
}
}
