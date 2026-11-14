#include "online_gameover_input.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <cstring>
#include <limits>
PPC_EXTERN_FUNC(__imp__sub_82770F00);
namespace em2::net {
namespace {
bool Readable(uint8_t* base,uint32_t p,size_t n){return base && p>=4096 && uint64_t(p)+n<=0x100000000ull && GuestReadBatch::Readable(base+p,n);}
struct Preserve {
    PPCContext& ctx;PPCContext saved;unsigned fp=_mm_getcsr();DWORD error=GetLastError();
    uint8_t* stack=nullptr;std::array<uint8_t,8192> bytes{};
    Preserve(PPCContext& c,uint8_t* base):ctx(c),saved(c){const auto top=c.r1.u32&~15u;
        if(top>=12288 && Readable(base,top-8192,bytes.size())){stack=base+top-8192;std::memcpy(bytes.data(),stack,bytes.size());ctx.r1.u64=top-4096;}}
    ~Preserve(){if(stack)std::memcpy(stack,bytes.data(),bytes.size());ctx=saved;_mm_setcsr(fp);SetLastError(error);}
};
}
bool ReadNativeGameOverState(PPCContext& ctx,uint8_t* base,NativeGameOverState& output){
    Preserve preserve(ctx,base);if(!preserve.stack || !Readable(base,0x833DAD98,4) || !Readable(base,0x833DA7E4,4) || !Readable(base,0x833E36CC,4))return false;
    const auto profile=PPC_LOAD_U32(0x833DAD98),registry=PPC_LOAD_U32(0x833DA7E4),dead=PPC_LOAD_U32(0x833E36CC);
    if(!registry || dead>kOnlineRoomCapacity || !Readable(base,profile,16))return false;
    const auto table=PPC_LOAD_U32(profile+12);if(!Readable(base,table,24))return false;
    const auto method=PPC_LOAD_U32(table+20);
    if((method&3) || method<PPC_CODE_BASE || uint64_t(method)>=uint64_t(PPC_CODE_BASE)+PPC_CODE_SIZE || !PPC_LOOKUP_FUNC(base,method))return false;
    ctx.r3.u64=profile;__imp__sub_82770F00(ctx,base);
    if(ctx.r3.u32!=7 || PPC_LOAD_U32(0x833DAD98)!=profile || PPC_LOAD_U32(0x833DA7E4)!=registry || PPC_LOAD_U32(0x833E36CC)!=dead)return false;
    output={registry,dead};return true;
}
RoomMovement NativeGameOverCommand(){RoomMovement result;result.actor_epoch=1;result.source=RoomMovementSource::NativeGameOverV4;return result;}
bool ValidNativeGameOverFrame(const RoomFrame& frame,uint16_t roster,uint64_t entry,uint8_t delay,
    const std::array<std::array<uint8_t,32>,kOnlineRoomCapacity>& schemas,bool buttons){
    if(!roster || frame.players_mask!=roster || entry>UINT64_MAX-delay || frame.tick<=entry || frame.tick==UINT64_MAX)return false;
    const auto end=entry+delay;
    for(unsigned i=0;i<kOnlineRoomCapacity;++i){const auto& c=frame.players[i];
        if(!(roster&(1u<<i))){if(c!=RoomMovement{})return false;continue;}
        if(c.actor_epoch!=1 || !ValidRoomMovement(c))return false;
        if(frame.tick>end){if(c!=NativeGameOverCommand())return false;continue;}
        if(c.source==RoomMovementSource::NativeGameOverV4)return false;
        // Explicit startup-neutral frames have no interpreted actions.
        RoomMovement neutral;neutral.actor_epoch=1;neutral.source=RoomMovementSource::NativeActionV1;
        if(c==neutral)continue;
        if(buttons){
            if(c.source!=RoomMovementSource::NativeActionV3 || schemas[i]==std::array<uint8_t,32>{} ||
               c.button_actions.schema!=schemas[i] || frame.tick<delay || c.button_actions.source_tick!=frame.tick-delay)return false;
        }else if(c.source!=RoomMovementSource::NativeActionV2)return false;
    }
    return true;
}
bool AdvanceRoomTransportInput(RoomTransportInputStream& state,const RoomMovement& command){
    if(command.source==RoomMovementSource::NativeGameOverV4){
        if(!ValidRoomMovement(command))return false;
        state.terminal=true;return true;
    }
    if(state.terminal)return false;
    return AdvanceRoomJumpStream(state.gameplay,command);
}
bool TestNativeGameOverInput(){
    unsigned checks=0;auto check=[&](bool value,const char* why){++checks;if(!value)REXLOG_ERROR("Native game-over input: {}",why);return value;};
    const auto neutral=NativeGameOverCommand();
    if(!check(ValidRoomMovement(neutral),"explicit typed native no-action command"))return false;
    for(unsigned field=0;field<10;++field){auto bad=neutral;
        if(field==0)bad.x=.1f;if(field==1)bad.y=-0.f;if(field==2)bad.forward_x=.1f;if(field==3)bad.forward_z=-1;
        if(field==4)bad.pad.buttons=0x1000;if(field==5)bad.pad.right_x=1;if(field==6)bad.pad.right_trigger=1;
        if(field==7)bad.jump_button_mask=0x1000;if(field==8)bad.button_actions.schema[0]=1;if(field==9)bad.actor_epoch=0;
        if(!check(!ValidRoomMovement(bad),"no gameplay intent or residual metadata in native game-over commands"))return false;
    }
    RoomJumpStream stream;
    if(!check(!AdvanceRoomJumpStream(stream,neutral),"gameplay edge streams cannot certify terminal native game-over"))return false;
    RoomTransportInputStream transport;auto invalid_terminal=neutral;invalid_terminal.pad.buttons=1;
    if(!check(!AdvanceRoomTransportInput(transport,invalid_terminal) && transport==RoomTransportInputStream{},"malformed terminal command cannot alter transport history"))return false;
    if(!check(AdvanceRoomTransportInput(transport,neutral) && transport.terminal && transport.gameplay==RoomJumpStream{},"transport terminal phase preserves gameplay history"))return false;
    const auto terminal_history=transport;
    if(!check(AdvanceRoomTransportInput(transport,neutral) && transport==terminal_history,"repeated terminal commands retain phase and history"))return false;
    auto legacy=neutral;legacy.source=RoomMovementSource::NativeActionV1;
    if(!check(!AdvanceRoomTransportInput(transport,legacy) && transport==terminal_history,"gameplay cannot resume before world rebootstrap"))return false;
    std::array<std::array<uint8_t,32>,kOnlineRoomCapacity> schemas{};RoomFrame frame;frame.tick=703;frame.players_mask=7;
    for(unsigned i=0;i<3;++i){frame.players[i]=neutral;schemas[i][0]=uint8_t(i+1);}
    if(!check(ValidNativeGameOverFrame(frame,7,700,2,schemas,true),"post-delay all-peer terminal frame"))return false;
    for(unsigned peer=0;peer<3;++peer){auto bad=frame;bad.players[peer].source=RoomMovementSource::NativeActionV1;
        if(!check(!ValidNativeGameOverFrame(bad,7,700,2,schemas,true),"no old gameplay command after delayed prefix"))return false;}
    frame.tick=701;
    if(!check(!ValidNativeGameOverFrame(frame,7,700,2,schemas,true),"terminal commands cannot replace previously produced input"))return false;
    for(unsigned i=0;i<3;++i){auto& c=frame.players[i];c.source=RoomMovementSource::NativeActionV3;c.jump_button_mask=0x1000;c.button_actions.schema=schemas[i];c.button_actions.source_tick=699;}
    if(!check(ValidNativeGameOverFrame(frame,7,700,2,schemas,true),"already produced delayed native actions may drain without callback dispatch"))return false;
    for(unsigned field=0;field<5;++field){auto bad=frame;
        if(field==0)bad.players[2].button_actions.schema[0]^=1;if(field==1)bad.players[2].button_actions.source_tick=700;
        if(field==2)bad.players_mask=3;if(field==3)bad.players[3]=neutral;if(field==4)bad.players[1].actor_epoch=2;
        if(!check(!ValidNativeGameOverFrame(bad,7,700,2,schemas,true),"prefix schema/source tick/roster/actor identity remain checked"))return false;
    }
    if(!check(!ValidNativeGameOverFrame(frame,7,UINT64_MAX,2,schemas,true),"terminal timeline overflow rejected"))return false;
    REXLOG_INFO("Native game-over input passed {} typed intent, delayed-prefix, schema, roster and timeline checks; native game-over and reload require live validation",checks);return true;
}
}
