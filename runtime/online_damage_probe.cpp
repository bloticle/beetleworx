#include "online_room_probe.h"
#include "online_room_gameplay.h"
#include "online_native_menu.h"
#include "online_actor_input.h"
#include "guest_read_batch.h"
#include "guest_shutdown.h"
#include "simulation.h"
#include "players.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <array>
#include <cstring>
#include <immintrin.h>
#include <windows.h>
REXCVAR_DEFINE_UINT32(em2_probe_extra_damage_peer,0,"Debug","Isolated native lobby only: dispatch original Damage event against this extra room peer at the scheduled tick; zero disables");
REXCVAR_DECLARE(uint32_t,em2_native_lobby_qa_restart_tick);
REXCVAR_DECLARE(uint32_t,em2_native_lobby_qa_restart_count);
REXCVAR_DECLARE(uint32_t,em2_native_lobby_qa_damage_player);
REXCVAR_DECLARE(std::string,em2_native_lobby_qa_level_target);
REXCVAR_DECLARE(std::string,em2_native_lobby_qa_level_route);
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(bool,em2_room_probe_scripted_input);
PPC_EXTERN_FUNC(__imp__sub_82761F40);
PPC_EXTERN_FUNC(__imp__sub_82821AC8);
PPC_EXTERN_FUNC(__imp__sub_828218B0);
PPC_EXTERN_FUNC(sub_827340E0);
PPC_EXTERN_FUNC(__imp__sub_827621C0);
namespace {
using Actor=em2::net::OwnedActorInput;
bool Readable(uint32_t address,size_t bytes,uint8_t* base){return base && address>=4096 && uint64_t(address)+bytes<=0x100000000ull && em2::GuestReadBatch::Readable(base+address,bytes);}
struct Borrow {
 PPCContext& ctx;PPCContext saved;unsigned csr=_mm_getcsr();DWORD error=GetLastError();uint8_t* memory=nullptr;uint32_t scratch=0;
 std::array<uint8_t,32768> bytes{};
 Borrow(PPCContext& c,uint8_t* base):ctx(c),saved(c){
  const auto top=c.r1.u32&~15u;
  if(top>=65536 && Readable(top-32768,bytes.size(),base)){
   memory=base+top-32768;std::memcpy(bytes.data(),memory,bytes.size());ctx.r1.u64=top-4096;PPC_STORE_U32(ctx.r1.u32,saved.r1.u32);scratch=top-1024;
  }
 }
 ~Borrow(){if(memory)std::memcpy(memory,bytes.data(),bytes.size());ctx=saved;_mm_setcsr(csr);SetLastError(error);}
};
struct Probe {em2::net::Room* room=nullptr;Actor actor{};uint32_t peer=0;bool sent=false;};
thread_local Probe probe;
bool Health(const Actor& actor,PPCContext& ctx,uint8_t* base,uint32_t& value){
 Borrow borrow(ctx,base);if(!borrow.memory || !Readable(actor.entity,4,base) || !Readable(0x833DBB14,4,base))return false;
 const auto table=PPC_LOAD_U32(actor.entity),property=PPC_LOAD_U32(0x833DBB14);
 if(!Readable(table,208,base) || !Readable(property,8,base))return false;
 const auto method=PPC_LOAD_U32(table+204);
 if((method&3) || method<PPC_CODE_BASE || method>=PPC_CODE_BASE+PPC_CODE_SIZE)return false;
 const auto output=borrow.scratch;PPC_STORE_U32(output,0);
 ctx.r3.u64=actor.entity;ctx.r4.u64=property+4;ctx.r5.u64=output;ctx.r6.u64=0;ctx.lr=0x827CF2F4;
 auto* read=PPC_LOOKUP_FUNC(base,method);if(!read)return false;read(ctx,base);value=PPC_LOAD_U32(output);return value<=1000;
}
bool Damage(const Actor& actor,PPCContext& ctx,uint8_t* base){
 Borrow borrow(ctx,base);if(!borrow.memory || !Readable(0x833DA6F4,4,base) || !PPC_LOAD_U32(0x833DA6F4))return false;
 const auto target=borrow.scratch,instigator=target+4,vector=target+16,event=target+64;
 PPC_STORE_U32(target,actor.handle);PPC_STORE_U32(instigator,0);
 for(unsigned i=0;i<4;++i)PPC_STORE_U32(vector+i*4,0);
 const auto stack=ctx.r1.u32;
 struct Destroy {PPCContext& ctx;uint8_t* base;uint32_t event,stack;bool ready=false;
  ~Destroy(){if(ready && !em2::GuestShutdownRequested()){ctx.r1.u64=stack;ctx.r3.u64=event;ctx.lr=0x827D0774;__imp__sub_827621C0(ctx,base);}}
 } destroy{ctx,base,event,stack};
 // Exact inner operation of the original 827D06A8 Damage Lua wrapper:
 // typed target/instigator event, physical stimulus, zero impulse, original
 // synchronous event dispatch and native destructor. No health is written.
 ctx.r3.u64=event;ctx.r4.u64=target;ctx.r5.u64=instigator;ctx.lr=0x827D0720;__imp__sub_82761F40(ctx,base);destroy.ready=true;
 ctx.r3.u64=event;ctx.r4.u64=2;ctx.f1.f64=1000;ctx.lr=0x827D0730;__imp__sub_82821AC8(ctx,base);
 ctx.r3.u64=event;ctx.r4.u64=vector;ctx.r5.u64=0;ctx.lr=0x827D0758;__imp__sub_828218B0(ctx,base);
 ctx.r3.u64=PPC_LOAD_U32(0x833DA6F4);ctx.r4.u64=event;ctx.r5.u64=0;ctx.lr=0x827D076C;sub_827340E0(ctx,base);
 return true;
}
}
bool em2::net::TickPrivateNativeExtraDamage(PPCContext& ctx,uint8_t* base){
 const auto peer=REXCVAR_GET(em2_probe_extra_damage_peer);if(!peer)return false;
 if(!REXCVAR_GET(em2_private_profile) || !REXCVAR_GET(em2_room_probe_scripted_input) || !NativeOnlineMenuQaActive())return true;
 uint64_t tick=0;const auto requested=REXCVAR_GET(em2_native_lobby_qa_restart_tick);
 if(ctx.lr!=0x829ED694 || !em2::CurrentAgreedSimulationTick(tick) || tick!=requested)return true;
 auto* room=PrivateNativeMenuRoom();auto* game=CurrentNativeRoomGameplay();
 if(!room || !game)return true;
 if(probe.room!=room)probe={room};
 auto reject=[&](const char* why){REXLOG_ERROR("Native lobby QA rejected: extra damage {}",why);game->ObservationFailed();};
 if(probe.sent || peer<3 || peer>16 || REXCVAR_GET(em2_native_lobby_qa_restart_count)!=1 ||
    REXCVAR_GET(em2_native_lobby_qa_damage_player) || !REXCVAR_GET(em2_native_lobby_qa_level_target).empty() || !REXCVAR_GET(em2_native_lobby_qa_level_route).empty()){
  reject("fixture configuration or repeated dispatch");return true;
 }
 Actor actor;uint32_t before=0,after=0;
 if(!game->OwnedActorForPeer(peer,ctx,base,actor) || !Health(actor,ctx,base,before) || !before){reject("live target or native health query unavailable");return true;}
 probe.sent=true;probe.actor=actor;probe.peer=peer;
 if(!Damage(actor,ctx,base) || !Health(actor,ctx,base,after)){reject("native damage/query did not finish");return true;}
 REXLOG_INFO("Private extra damage dispatched: tick {} peer {} generation {} handle {:08X} entity {:08X} health {}->{}; original native Damage event, no forced reload or death interception",tick,peer,actor.generation,actor.handle,actor.entity,before,after);
 return true;
}
void em2::net::ObservePrivateExtraDamage(uint64_t tick,PPCContext& ctx,uint8_t* base){
 if(REXCVAR_GET(em2_native_lobby_qa_damage_player)==3 && tick>=700 && REXCVAR_GET(em2_private_profile) && NativeOnlineMenuQaActive()){
  auto* game=CurrentNativeRoomGameplay();auto* room=PrivateNativeMenuRoom();if(!game || !room)return;
  Actor actor;uint32_t life=0;
  if(!game->OwnedActorForPeer(3,ctx,base,actor) || !Health(actor,ctx,base,life)){
   REXLOG_ERROR("Private retail-pair survivor health unavailable: tick {}",tick);game->ObservationFailed();return;
  }
  REXLOG_INFO("Private retail-pair survivor health: tick {} peer 3 generation {} entity {:08X} life {}; original native property query, no damage dispatched to survivor",tick,actor.generation,actor.entity,life);
 }

 if(!REXCVAR_GET(em2_probe_extra_damage_peer) || !probe.sent || !REXCVAR_GET(em2_private_profile) || !NativeOnlineMenuQaActive())return;
 auto* game=CurrentNativeRoomGameplay();if(!game || PrivateNativeMenuRoom()!=probe.room)return;
 Actor current;uint32_t life=0;
 if(!game->OwnedActorForPeer(probe.peer,ctx,base,current) || current.generation!=probe.actor.generation || current.entity!=probe.actor.entity || !Health(current,ctx,base,life)){
  REXLOG_ERROR("Private extra damage target lost: tick {} peer {} generation {}; no replacement retargeted",tick,probe.peer,probe.actor.generation);game->ObservationFailed();return;
 }
 REXLOG_INFO("Private extra damage health: tick {} peer {} generation {} life {}; native property query",tick,probe.peer,current.generation,life);
}

void em2::net::TracePrivateExtraPosition(PPCContext& ctx,uint8_t* base,const char* operation,unsigned offset){
 if(!REXCVAR_GET(em2_probe_extra_damage_peer) || !probe.sent || !REXCVAR_GET(em2_private_profile) || !NativeOnlineMenuQaActive() || ctx.r3.u32!=probe.actor.entity)return;
 uint64_t tick=0;if(!em2::CurrentAgreedSimulationTick(tick) || tick<930 || tick>980)return;
 const uint64_t address=uint64_t(ctx.r4.u32)+offset;if(address+12>0x100000000ull || !Readable(uint32_t(address),12,base))return;
 const auto input=uint32_t(address);
 REXLOG_INFO("Private extra position write: tick {} peer {} operation {} caller {:08X} bits {:08X}/{:08X}/{:08X}",tick,probe.peer,operation,uint32_t(ctx.lr),PPC_LOAD_U32(input),PPC_LOAD_U32(input+4),PPC_LOAD_U32(input+8));
 auto stack=ctx.r1.u32;
 for(unsigned depth=0;depth<8 && Readable(stack,4,base);++depth){
  const auto next=PPC_LOAD_U32(stack);if(next<=stack || next-stack>65536 || !Readable(next-8,4,base))break;
  REXLOG_INFO("Private extra position caller: tick {} depth {} return {:08X}",tick,depth,PPC_LOAD_U32(next-8));stack=next;
 }
}
