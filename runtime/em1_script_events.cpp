#include "em1_script_events.h"
#include "em1_preview_capture.h"
#include "em1_animation_variables.h"
#include "em1_level_entry.h"
#include "em1_lua_core/include/em1_lua_core.h"
#include "native_entity_probe.h"
#include "native_engine.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <map>
#include <set>
#include <cmath>
#include <algorithm>
#include <bit>
#include <cstring>
PPC_EXTERN_FUNC(__imp__sub_82732A18);
PPC_EXTERN_FUNC(__imp__sub_82755710);
PPC_EXTERN_FUNC(__imp__sub_82730050);
PPC_EXTERN_FUNC(__imp__sub_82730080);
PPC_EXTERN_FUNC(__imp__sub_827548D8);
PPC_EXTERN_FUNC(__imp__sub_82754A18);
PPC_EXTERN_FUNC(__imp__sub_82755D50);
PPC_EXTERN_FUNC(__imp__sub_827B84B0);
PPC_EXTERN_FUNC(__imp__sub_827B84F8);
PPC_EXTERN_FUNC(__imp__sub_827B9310);
PPC_EXTERN_FUNC(__imp__sub_827B8858);
PPC_EXTERN_FUNC(__imp__sub_825430D8);
PPC_EXTERN_FUNC(__imp__sub_827427E8);
PPC_EXTERN_FUNC(__imp__sub_8276A1B0);
PPC_EXTERN_FUNC(__imp__sub_82745270);
PPC_EXTERN_FUNC(__imp__sub_829AD330);
PPC_EXTERN_FUNC(__imp__sub_82843878);
PPC_EXTERN_FUNC(__imp__sub_8283E778);
PPC_EXTERN_FUNC(__imp__sub_82393300);
PPC_EXTERN_FUNC(__imp__sub_82761CA8);
PPC_EXTERN_FUNC(__imp__sub_82808C28);
PPC_EXTERN_FUNC(__imp__sub_82804F98);
PPC_EXTERN_FUNC(__imp__sub_8300A3A8);
PPC_EXTERN_FUNC(__imp__sub_82808930);
PPC_EXTERN_FUNC(__imp__sub_82804940);
PPC_EXTERN_FUNC(__imp__sub_831EFB78);
PPC_EXTERN_FUNC(__imp__sub_8275D0E0);
PPC_EXTERN_FUNC(__imp__sub_831F4698);
PPC_EXTERN_FUNC(__imp__sub_82761938);
PPC_EXTERN_FUNC(__imp__sub_827D87D8);
PPC_EXTERN_FUNC(__imp__sub_827404F0);
PPC_EXTERN_FUNC(__imp__sub_827406D0);
PPC_EXTERN_FUNC(__imp__sub_82A010E8);
PPC_EXTERN_FUNC(__imp__sub_82A01000);
PPC_EXTERN_FUNC(__imp__sub_827578B0);
PPC_EXTERN_FUNC(__imp__sub_822F1C10);
PPC_EXTERN_FUNC(__imp__sub_826547C8);
PPC_EXTERN_FUNC(__imp__sub_82733970);
PPC_EXTERN_FUNC(__imp__sub_827338B8);
PPC_EXTERN_FUNC(__imp__sub_82851710);
PPC_EXTERN_FUNC(__imp__sub_82504410);
PPC_EXTERN_FUNC(__imp__sub_82761C50);
PPC_EXTERN_FUNC(__imp__sub_828013F0);
PPC_EXTERN_FUNC(__imp__sub_8275C160);
PPC_EXTERN_FUNC(__imp__sub_82740440);
PPC_EXTERN_FUNC(__imp__sub_82786850);
PPC_EXTERN_FUNC(__imp__sub_827610C0);
PPC_EXTERN_FUNC(__imp__sub_8276BC38);
PPC_EXTERN_FUNC(__imp__sub_82A459F0);
PPC_EXTERN_FUNC(__imp__sub_831D38C8);
PPC_EXTERN_FUNC(__imp__sub_82850CF0);
PPC_EXTERN_FUNC(__imp__sub_82850D10);
PPC_EXTERN_FUNC(__imp__sub_82A5C478);
PPC_EXTERN_FUNC(__imp__sub_8299E7B0);
PPC_EXTERN_FUNC(__imp__sub_82A166D8);
PPC_EXTERN_FUNC(__imp__sub_831D6730);
PPC_EXTERN_FUNC(__imp__sub_82A52A10);
namespace em2::em1 {
namespace {
using Guid=std::array<uint32_t,4>;
struct Identity {
  Guid guid{};uint32_t address=0;
  bool operator==(const Identity&)const=default;
};
struct PrimaryRole {
  uint32_t manager=0,registry=0,handle=0;
  Identity entity{};
  bool operator==(const PrimaryRole&)const=default;
};
struct BindingSink {void* context;bool(*bind)(void*,uint32_t);bool(*unbind)(void*,uint32_t);};
struct Bindings {
  std::set<Guid> source;
  std::map<uint32_t,Identity> live;
  PrimaryRole primary;
  bool Owns(uint32_t handle,const Identity& identity)const{
    return source.contains(identity.guid) || (primary.manager && primary.registry &&
      primary.handle==handle && primary.entity==identity);
  }
  bool SetPrimary(PrimaryRole next,BindingSink sink){
    if(next==primary)return true;
    // A changed manager/role is a new generation even when native storage and
    // handle were recycled. Never let an earlier coroutine regain authority.
    if(primary.handle){
      const auto old=live.find(primary.handle);
      if(old!=live.end()){
        if(!sink.unbind(sink.context,old->first))return false;
        live.erase(old);
      }
    }
    primary=next;
    return !next.handle || Admit(next.handle,next.entity,sink);
  }
  bool Clear(BindingSink sink){
    bool ok=true;
    for(auto i=live.begin();i!=live.end();){
      if(sink.unbind(sink.context,i->first))i=live.erase(i);else{ok=false;++i;}
    }
    return ok;
  }
  bool Admit(uint32_t handle,Identity identity,BindingSink sink){
    if(!handle || identity.address<4096 || !Owns(handle,identity))return false;
    auto found=live.find(handle);
    if(found!=live.end()){
      if(found->second==identity)return true;
      if(!sink.unbind(sink.context,handle))return false;
      live.erase(found);
    }
    if(!sink.bind(sink.context,handle))return false;
    live.emplace(handle,identity);return true;
  }
  bool Refresh(std::span<const NativeEntityRecord> records,BindingSink sink){
    std::map<uint32_t,Identity> desired;std::set<uint32_t> handles;std::set<Guid> guids;
    for(const auto& record:records){
      if(!record.handle || record.debug_address<4096 || !handles.insert(record.handle).second || !guids.insert(record.id).second)return false;
      const Identity identity{record.id,record.debug_address};
      if(Owns(record.handle,identity))desired.emplace(record.handle,identity);
    }
    // Revoke all departed/reused handles before binding any replacements, so a
    // coroutine's old userdata can never regain ownership by handle reuse.
    for(auto i=live.begin();i!=live.end();){
      const auto next=desired.find(i->first);
      if(next!=desired.end() && next->second==i->second){++i;continue;}
      if(!sink.unbind(sink.context,i->first))return false;
      i=live.erase(i);
    }
    for(const auto& [handle,identity]:desired)if(!Admit(handle,identity,sink))return false;
    return true;
  }
};
struct World {
  enum class Phase {Loading,Active,Failed};
  Phase phase=Phase::Loading;
  uint8_t* base=nullptr;uint64_t lifecycle=0;uint32_t registry=0;
  uint64_t updates=0,native_entries=0,native_scopes=0,lookups=0,fallbacks=0,queued=0,rejected=0;
  uint64_t initializers=0,output_lookups=0,output_nil=0,listeners=0,source_listeners=0;
  uint64_t property_checks=0,property_missing=0,source_property_missing=0,source_initializers=0,core_candidates=0;
  uint64_t source_arrays_zero=0,source_arrays_nonzero=0;
  std::array<uint64_t,4> getter_traces{};
  std::string state,origin;
  std::shared_ptr<RegisteredBlueprintLibrary> blueprints;
  std::shared_ptr<LevelOriginalScripts> scripts;
  std::shared_ptr<Em1LuaWorld> vm;
  Bindings entities;
  std::set<std::string> diagnostics;
  std::map<std::string,uint32_t> animation_events;
  std::map<std::string,uint32_t> animation_variables;
  uint64_t animation_sent=0,animation_noop=0;
  uint64_t variable_sent=0,variable_noop=0;
  bool native_variable_catalog_ready=false;
  BindingSink Sink(){return {vm.get(),
    +[](void* vm,uint32_t h){return em1_lua_world_bind_entity(static_cast<Em1LuaWorld*>(vm),h)==EM1_LUA_OK;},
    +[](void* vm,uint32_t h){return em1_lua_world_unbind_entity(static_cast<Em1LuaWorld*>(vm),h)==EM1_LUA_OK;}};}
};
std::unique_ptr<World> world;
bool Readable(uint8_t* base,uint32_t at,size_t size){
  return base && at>=4096 && size && uint64_t(at)+size<=0x100000000ull && GuestReadBatch::Readable(base+at,size);
}
struct Preserve {
  PPCContext& ctx;PPCContext saved;DWORD error=GetLastError();unsigned csr=_mm_getcsr();
  explicit Preserve(PPCContext& c):ctx(c),saved(c){}
  ~Preserve(){ctx=saved;SetLastError(error);_mm_setcsr(csr);}
};
bool Current(uint8_t* base){
  return world && world->phase!=World::Phase::Failed && world->base==base &&
    world->blueprints->guest_base==base && world->scripts->state.vm==world->vm &&
    world->scripts->level_origin==world->origin && NativeConsoleWorldState().lifecycle==world->lifecycle;
}
void TraceCounters(std::string_view stage){
  if(world)REXLOG_INFO("[em1] original event diagnostics {}: lifecycle={} updates={} native_entries={} owned_scopes={} lookups={} native_fallback={} queued={} rejected={}",
    stage,world->lifecycle,world->updates,world->native_entries,world->native_scopes,world->lookups,world->fallbacks,world->queued,world->rejected);
  if(world)REXLOG_INFO("[em1] original IO startup diagnostics {}: initializers={} output_constants={} nil_constants={} listener_deliveries={} source_deliveries={}",
    stage,world->initializers,world->output_lookups,world->output_nil,world->listeners,world->source_listeners);
  if(world)REXLOG_INFO("[em1] original IO property diagnostics {}: output_checks={} missing={} source_initializers={} core_interface_candidates={}",
    stage,world->property_checks,world->property_missing,world->source_initializers,world->core_candidates);
  if(world)REXLOG_INFO("[em1] original IO source Output arrays {}: zero={} nonzero={}",stage,world->source_arrays_zero,world->source_arrays_nonzero);
  Em1LuaSchedulerStats tasks{};
  if(world && em1_lua_world_scheduler_stats(world->vm.get(),&tasks)==EM1_LUA_OK)
    REXLOG_INFO("[em1] original callback scheduler {}: submitted={} resumes={} completed={} failed={} cancelled={} pending={}",
      stage,tasks.submitted,tasks.resumes,tasks.completed,tasks.failed,tasks.cancelled,tasks.pending);
}
void Fail(){
  if(world){world->phase=World::Phase::Failed;em1_lua_world_cancel(world->vm.get());em1_lua_world_set_bound_player(world->vm.get(),0);world->entities.Clear(world->Sink());}
}
bool ReadIdentity(PPCContext& ctx,uint8_t* base,uint32_t registry,uint32_t handle,Identity& identity){
  Preserve preserve(ctx);const auto scratch=ctx.r1.u32-512;
  PPC_STORE_U32(scratch,ctx.r1.u32);PPC_STORE_U32(scratch+96,handle);
  ctx.r1.u64=scratch;ctx.r3.u64=registry;ctx.r4.u64=scratch+96;
  __imp__sub_82732A18(ctx,base);
  const auto entity=ctx.r3.u32;
  if(!Readable(base,entity,96) || (entity&3))return false;
  identity.address=entity;
  for(unsigned i=0;i<4;++i)identity.guid[i]=PPC_LOAD_U32(entity+4+i*4);
  return true;
}
bool SetPrimary(PrimaryRole role,std::string& error){
  if(!world->entities.SetPrimary(role,world->Sink()) ||
     em1_lua_world_set_bound_player(world->vm.get(),role.handle)!=EM1_LUA_OK){
    error="Could not revoke/bind verified native primary-player role";Fail();return false;
  }
  return true;
}
PrimaryRole ReadPrimaryRole(uint8_t* base,uint32_t registry){
  PrimaryRole role;
  if(!Readable(base,0x833DAB60,4))return role;
  const auto manager=PPC_LOAD_U32(0x833DAB60);
  if(!Readable(base,manager,40))return role;
  const auto handle=PPC_LOAD_U32(manager+20);
  if(handle)role={manager,registry,handle,{}};
  return role;
}
bool RoleUnchanged(uint8_t* base,const PrimaryRole& role){
  return Readable(base,0x833DA7E4,4) && PPC_LOAD_U32(0x833DA7E4)==role.registry &&
    ReadPrimaryRole(base,role.registry).manager==role.manager &&
    ReadPrimaryRole(base,role.registry).handle==role.handle;
}
bool Resolve(PPCContext& ctx,uint8_t* base,uint32_t handle,Identity& identity,std::string& error){
  Preserve preserve(ctx);
  if(!Current(base) || !handle || ctx.r1.u32<32768 || !Readable(base,ctx.r1.u32-32768,32768) || !Readable(base,0x833DA7E4,4)){
    error="Original script event has no current source world, handle or native stack";return false;
  }
  const auto registry=PPC_LOAD_U32(0x833DA7E4);
  if(!Readable(base,registry,24) || (world->registry && world->registry!=registry)){
    error="Original script event registry changed";return false;
  }
  auto role=ReadPrimaryRole(base,registry);
  if(role.handle && (!ReadIdentity(ctx,base,registry,role.handle,role.entity) || !RoleUnchanged(base,role)))role={};
  if(!SetPrimary(role,error))return false;
  if(!ReadIdentity(ctx,base,registry,handle,identity)){error="Original script event entity is no longer live";return false;}
  if(!world->entities.Owns(handle,identity)){error="Event target is outside the retained blueprint scope and verified primary role";return false;}
  if(!world->entities.Admit(handle,identity,world->Sink())){error="Could not bind original script event entity";Fail();return false;}
  world->registry=registry;return true;
}
std::string VmError(){Em1LuaWorldView view{};if(em1_lua_world_view(world->vm.get(),&view)==EM1_LUA_REENTRANT)return "Synchronous same-world Lua reentry rejected; no nested event or one-shot admitted";return view.error_size?std::string(reinterpret_cast<const char*>(view.error),view.error_size):"Original callback admission failed";}
bool CString(uint8_t*,uint32_t,std::string&,size_t);
bool BuiltinScope(PPCContext& ctx,uint8_t* base,const NativeScriptState& scripts,std::string& error){
  if(!Current(base) || world->phase!=World::Phase::Active || scripts.vm!=world->vm ||
    ctx.r1.u32<32768 || !Readable(base,ctx.r1.u32-32768,32768) || !Readable(base,0x833DA7E4,4) ||
    !world->registry || PPC_LOAD_U32(0x833DA7E4)!=world->registry){error="Native builtin has no current retained script world/registry";return false;}
  return true;
}
bool BuiltinEntity(PPCContext& ctx,uint8_t* base,uint32_t handle,std::string& error){
  const auto found=world->entities.live.find(handle);Identity actual;
  if(!handle || found==world->entities.live.end() || !world->entities.Owns(handle,found->second) ||
    !ReadIdentity(ctx,base,world->registry,handle,actual) || actual!=found->second){
    error="Native builtin entity is stale, foreign or not yet admitted";return false;
  }
  return true;
}
}
bool ValidateRegisteredScriptEngineScope(PPCContext& ctx,uint8_t* base,const NativeScriptState& scripts,std::string& error){
  Preserve preserve(ctx);const bool valid=BuiltinScope(ctx,base,scripts,error);if(valid)error.clear();return valid;
}
uint32_t ControlRegisteredScriptCamera(PPCContext& ctx,uint8_t* base,const NativeScriptState& scripts,
    uint32_t operation,float a,float b,float c,std::string& error){
  Preserve preserve(ctx);
  if(operation>1 || !std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c)){
    error="Invalid original primary camera operation/values";return 2;}
  if(!BuiltinScope(ctx,base,scripts,error))return 2;
  const auto role=world->entities.primary;
  if(!role.handle || !RoleUnchanged(base,role) || !BuiltinEntity(ctx,base,role.handle,error)){
    error="Original primary camera role is stale/unavailable";return 2;}
  const auto stack=ctx.r1.u32-4096,reference=stack+128;
  PPC_STORE_U32(stack,ctx.r1.u32);PPC_STORE_U32(reference,role.handle);ctx.r1.u64=stack;
  ctx.r3.u64=role.manager;ctx.r4.u64=reference;__imp__sub_8276BC38(ctx,base);
  const auto camera=ctx.r3.u32;
  // Native camera exports explicitly skip a player without a camera.
  if(!camera){error.clear();return 1;}
  if(!Readable(base,camera,940) || !Readable(base,PPC_LOAD_U32(camera+392),560)){
    error="Original primary native camera is not ready";return 2;}
  const auto controller=PPC_LOAD_U32(camera+392);
  if(operation==1){
    // Exact three-argument defaults of82A0F900 after HKS marshaling. Its
    // unspecified alternate values mirror the three authored source values.
    ctx.r3.u64=controller;ctx.r4.u64=1;
    ctx.f1.f64=a;ctx.f2.f64=b;ctx.f3.f64=c;ctx.f4.f64=0;
    ctx.f5.f64=a;ctx.f6.f64=b;ctx.f7.f64=c;__imp__sub_82A52A10(ctx,base);
  }else{
    // Exact82A0F780 camera reset body, restricted to original source's primary
    // camera instead of iterating every retail split-screen participant.
    ctx.r4.u64=0;ctx.f1.f64=0;__imp__sub_82A459F0(ctx,base);
    ctx.r3.u64=camera+364;__imp__sub_831D38C8(ctx,base);
    PPC_STORE_U32(camera+372,0);PPC_STORE_U8(camera+376,0);
    ctx.r3.u64=camera;__imp__sub_82850CF0(ctx,base);
    ctx.r3.u64=camera;__imp__sub_82850D10(ctx,base);
    ctx.r3.u64=controller;__imp__sub_82A5C478(ctx,base);
    ctx.r3.u64=reference;ctx.r4.u64=controller;__imp__sub_8299E7B0(ctx,base);
    __imp__sub_82A166D8(ctx,base);if(ctx.r3.u32)__imp__sub_831D6730(ctx,base);
  }
  error.clear();return 0;
}
uint32_t FadeRegisteredScriptScreen(PPCContext& ctx,uint8_t* base,const NativeScriptState& scripts,
    uint32_t operation,float seconds,std::string& error){
  Preserve preserve(ctx);
  if(operation>2 || !std::isfinite(seconds)){error="Invalid original screen fade operation/duration";return 2;}
  if(!BuiltinScope(ctx,base,scripts,error) || !Readable(base,0x833E1F2C,4))return 2;
  const auto controller=PPC_LOAD_U32(0x833E1F2C);
  if(!Readable(base,controller,356)){error="Original native screen fade controller unavailable";return 2;}
  const auto stack=ctx.r1.u32-4096;PPC_STORE_U32(stack,ctx.r1.u32);ctx.r1.u64=stack;
  ctx.r3.u64=controller;ctx.r4.u64=operation==2?1:0;ctx.f1.f64=double(seconds);
  //82A01550/82A01608/82A016E0 use these exact controller operations.
  // Preserve their HUD/sequence notifications and native fade clock/state.
  if(operation==0){ctx.r6.u64=0;__imp__sub_82A010E8(ctx,base);}
  else __imp__sub_82A01000(ctx,base);
  error.clear();return 0;
}
uint32_t FindRegisteredScriptGroup(PPCContext& ctx,uint8_t* base,const NativeScriptState& scripts,
    std::string_view name,std::vector<uint32_t>& handles,std::string& error){
  Preserve preserve(ctx);
  if(!BuiltinScope(ctx,base,scripts,error))return 2;
  if(name.empty() || name.size()>256 || name.find('\0')!=name.npos || !Readable(base,0x833DA6F4,4)){
    error="Invalid native source group name/manager";return 2;
  }
  const auto manager=PPC_LOAD_U32(0x833DA6F4);
  if(!Readable(base,manager,928)){error="Native IO group manager is unavailable";return 2;}
  const auto stack=ctx.r1.u32-4096,text=stack+256,reference=stack+128,iterator=stack+136;
  PPC_STORE_U32(stack,ctx.r1.u32);std::memcpy(base+text,name.data(),name.size());PPC_STORE_U8(text+uint32_t(name.size()),0);
  PPC_STORE_U32(reference,0);PPC_STORE_U32(iterator,0);PPC_STORE_U32(iterator+4,0);ctx.r1.u64=stack;
  ctx.r3.u64=text;__imp__sub_825430D8(ctx,base);PPC_STORE_U32(reference,ctx.r3.u32);
  if(!Readable(base,ctx.r3.u32,1)){error="Native source group string could not be interned";return 2;}
  // Exact native82738E80 lookup/iterator sequence. These helpers query the IO
  // manager's real group map; they do not synthesize membership from names.
  ctx.r3.u64=manager;ctx.r4.u64=reference;__imp__sub_82733970(ctx,base);
  ctx.r3.u64=iterator;ctx.r4.u64=manager;ctx.r5.u64=reference;__imp__sub_827338B8(ctx,base);
  auto node=PPC_LOAD_U32(iterator+4);
  ctx.r3.u64=reference;__imp__sub_822F1C10(ctx,base);
  std::vector<uint32_t> candidate;std::set<uint32_t> visited;
  while(node){
    if(candidate.size()>=4096 || !Readable(base,node,8) || !visited.insert(node).second){error="Native source group exceeds bounds or contains a cycle";return 2;}
    const auto handle=PPC_LOAD_U32(node);
    if(handle && !BuiltinEntity(ctx,base,handle,error))return 2;
    candidate.push_back(handle);node=PPC_LOAD_U32(node+4);
  }
  handles=std::move(candidate);error.clear();return 0;
}
uint32_t FindRegisteredScriptEntity(PPCContext& ctx,uint8_t* base,const NativeScriptState& scripts,
    std::string_view name,uint32_t& handle,std::string& error){
  Preserve preserve(ctx);handle=0;
  if(!BuiltinScope(ctx,base,scripts,error) || name.empty() || name.size()>256 || name.find('\0')!=name.npos)return 2;
  const auto stack=ctx.r1.u32-4096,text=stack+256,reference=stack+128,result=stack+132;
  PPC_STORE_U32(stack,ctx.r1.u32);std::memcpy(base+text,name.data(),name.size());PPC_STORE_U8(text+uint32_t(name.size()),0);
  PPC_STORE_U32(result,0);
  ctx.r1.u64=stack;ctx.r3.u64=text;__imp__sub_825430D8(ctx,base);PPC_STORE_U32(reference,ctx.r3.u32);
  ctx.r3.u64=result;ctx.r4.u64=world->registry;ctx.r5.u64=reference;__imp__sub_827427E8(ctx,base);
  handle=PPC_LOAD_U32(result);
  ctx.r3.u64=reference;__imp__sub_822F1C10(ctx,base);
  if(!handle){error.clear();return 1;}
  if(!BuiltinEntity(ctx,base,handle,error)){handle=0;return 2;}
  error.clear();return 0;
}
uint32_t FindRegisteredScriptRelativeEntity(PPCContext& ctx,uint8_t* base,const NativeScriptState& scripts,
    uint32_t owner,std::string_view name,uint32_t& handle,std::string& error){
  Preserve preserve(ctx);handle=0;
  if(!BuiltinScope(ctx,base,scripts,error))return 2;
  if(name.size()>256 || name.find('\0')!=name.npos){error="Invalid original relative prefab entity name";return 2;}
  if(!owner){error.clear();return 1;}
  if(!BuiltinEntity(ctx,base,owner,error))return 2;
  const auto entity=world->entities.live.at(owner).address,stack=ctx.r1.u32-4096;
  const auto reference=stack+128,result=stack+132,text=stack+256;
  PPC_STORE_U32(stack,ctx.r1.u32);ctx.r1.u64=stack;
  auto method=[&](uint32_t face,uint32_t slot,uint32_t& address){
    if(!Readable(base,face,4))return false;
    const auto table=PPC_LOAD_U32(face);
    if(!Readable(base,table,slot+4))return false;
    address=PPC_LOAD_U32(table+slot);
    return address>=0x82200000 && address<0x83340000 && !(address&3) && Readable(base,address,4);
  };
  uint32_t owner_method=0;
  if(!method(entity,436,owner_method)){error="Original prefab owner interface unavailable";return 2;}
  // Exact82763228: resolve the receiver's own prefab owner, never a global
  // basename match. The original export queries this interface twice.
  ctx.r3.u64=entity;PPC_CALL_INDIRECT_FUNC(owner_method);
  if(!ctx.r3.u32){error.clear();return 1;}
  ctx.r3.u64=entity;PPC_CALL_INDIRECT_FUNC(owner_method);
  if(!Readable(base,ctx.r3.u32,4)){error="Original prefab owner became unavailable";return 2;}
  __imp__sub_827404F0(ctx,base);
  const auto prefab=ctx.r3.u32;
  if(!prefab){error.clear();return 1;}
  uint32_t lookup=0;
  if(!method(prefab,112,lookup)){error="Original relative prefab lookup unavailable";return 2;}
  if(!name.empty())std::memcpy(base+text,name.data(),name.size());PPC_STORE_U8(text+uint32_t(name.size()),0);
  ctx.r3.u64=text;__imp__sub_825430D8(ctx,base);PPC_STORE_U32(reference,ctx.r3.u32);
  if(!name.empty() && !ctx.r3.u32){error="Original relative prefab name could not be interned";return 2;}
  ctx.r3.u64=prefab;ctx.r4.u64=reference;PPC_CALL_INDIRECT_FUNC(lookup);
  const auto found=ctx.r3.u32;
  ctx.r3.u64=reference;__imp__sub_822F1C10(ctx,base);
  if(!found){error.clear();return 1;}
  if(found>0xFFFFFFF7 || !Readable(base,found+8,4)){error="Original relative prefab result is invalid";return 2;}
  PPC_STORE_U32(result,0);ctx.r3.u64=result;ctx.r4.u64=world->registry;ctx.r5.u64=found+8;
  __imp__sub_827406D0(ctx,base);
  const auto candidate=PPC_LOAD_U32(result);
  if(!candidate){error.clear();return 1;}
  if(!BuiltinEntity(ctx,base,candidate,error))return 2;
  if(world->entities.live.at(candidate).address!=found+8){error="Original relative prefab result identity changed";return 2;}
  handle=candidate;error.clear();return 0;
}
uint32_t GetRegisteredScriptPrefabOwner(PPCContext& ctx,uint8_t* base,const NativeScriptState& scripts,
    uint32_t handle,std::array<uint32_t,4>& guid,std::string& error,uint32_t* owner_handle){
  Preserve preserve(ctx);
  if(!BuiltinScope(ctx,base,scripts,error))return 2;
  if(!handle){guid={};if(owner_handle)*owner_handle=0;error.clear();return 1;}
  if(!BuiltinEntity(ctx,base,handle,error))return 2;
  const auto entity=world->entities.live.at(handle).address,table=PPC_LOAD_U32(entity);
  if(!Readable(base,table,440)){error="Original prefab owner interface unavailable";return 2;}
  const auto method=PPC_LOAD_U32(table+436);
  if(method<0x82200000 || method>=0x83340000 || (method&3) || !Readable(base,method,4)){
    error="Original prefab owner method unavailable";return 2;
  }
  const auto stack=ctx.r1.u32-4096,result=stack+128;
  PPC_STORE_U32(stack,ctx.r1.u32);ctx.r1.u64=stack;
  ctx.r3.u64=entity;PPC_CALL_INDIRECT_FUNC(method);
  const auto owner=ctx.r3.u32;
  if(!owner){guid={};if(owner_handle)*owner_handle=0;error.clear();return 1;}
  if(!Readable(base,owner,96)){error="Original prefab owner is unreadable";return 2;}
  // virtual+436 already returns the entity face. Unlike the result of the
  // relative lookup, it needs no+8 adjustment when obtaining its registry ID.
  PPC_STORE_U32(result,0);ctx.r3.u64=result;ctx.r4.u64=world->registry;ctx.r5.u64=owner;
  __imp__sub_827406D0(ctx,base);
  const auto candidate=PPC_LOAD_U32(result);
  if(!BuiltinEntity(ctx,base,candidate,error))return 2;
  const auto identity=world->entities.live.at(candidate);
  if(identity.address!=owner){error="Original prefab owner identity changed";return 2;}
  ctx.r3.u64=owner;__imp__sub_827404F0(ctx,base);
  if(!ctx.r3.u32){guid={};if(owner_handle)*owner_handle=0;error.clear();return 1;}
  if(!Readable(base,ctx.r3.u32,4)){error="Original prefab component is unreadable";return 2;}
  guid=identity.guid;if(owner_handle)*owner_handle=candidate;error.clear();return 0;
}
uint32_t ReadRegisteredScriptPrefabData(PPCContext& ctx,uint8_t* base,const NativeScriptState& scripts,
    uint32_t handle,RegisteredPrefabData& data,std::string& error){
  Preserve preserve(ctx);RegisteredPrefabData candidate;
  const auto status=GetRegisteredScriptPrefabOwner(ctx,base,scripts,handle,candidate.owner,error,&candidate.handle);
  if(status==1){data={};return 1;}if(status!=0)return 2;
  const auto root=world->entities.live.find(candidate.handle);
  if(root==world->entities.live.end() || root->second.guid!=candidate.owner || !BuiltinEntity(ctx,base,root->first,error)){error="Original prefab data root is no longer admitted";return 2;}
  candidate.handle=root->first;
  const auto stack=ctx.r1.u32-4096,reference=stack+128,count_at=stack+132,row_at=stack+136,text=stack+256;
  PPC_STORE_U32(stack,ctx.r1.u32);ctx.r1.u64=stack;ctx.r3.u64=root->second.address;__imp__sub_827404F0(ctx,base);
  const auto component=ctx.r3.u32;
  if(component>0xFFFFFFF7 || !Readable(base,component+8,4)){error="Original prefab data property interface unavailable";return 2;}
  const auto face=component+8,table=PPC_LOAD_U32(face);
  if(!Readable(base,table,192)){error="Original prefab data property vtable unavailable";return 2;}
  const auto size_method=PPC_LOAD_U32(table+136),row_method=PPC_LOAD_U32(table+188);
  for(auto method:{size_method,row_method})if(method<0x82200000 || method>=0x83340000 || (method&3) || !Readable(base,method,4)){
    error="Original prefab data property method unavailable";return 2;
  }
  size_t total=0;
  // Exact native827636E0 / originalREL34DE70 order and reflected reads. The
  // native property loader owns inheritance/scalar/array behavior; this bridge
  // does not flatten component master chains or borrow native HKS references.
  for(unsigned property=0;property<2;++property){
    const std::string_view name=property?"Prefab Data Override":"Prefab Data";
    std::memcpy(base+text,name.data(),name.size());PPC_STORE_U8(text+uint32_t(name.size()),0);
    ctx.r3.u64=text;__imp__sub_825430D8(ctx,base);PPC_STORE_U32(reference,ctx.r3.u32);
    if(!ctx.r3.u32){error="Original prefab data property name could not be interned";return 2;}
    auto release_name=[&]{ctx.r3.u64=reference;__imp__sub_822F1C10(ctx,base);};
    PPC_STORE_U32(count_at,0);ctx.r3.u64=face;ctx.r4.u64=reference;ctx.r5.u64=count_at;PPC_CALL_INDIRECT_FUNC(size_method);
    const auto count=(ctx.r3.u32&255)?PPC_LOAD_U32(count_at):0;
    if(count>16384){release_name();error="Original prefab data row count exceeds budget";return 2;}
    auto& rows=property?candidate.overrides:candidate.defaults;rows.reserve(count);
    for(uint32_t index=0;index<count;++index){
      PPC_STORE_U32(row_at,0);ctx.r3.u64=face;ctx.r4.u64=reference;ctx.r5.u64=row_at;ctx.r6.u64=index;PPC_CALL_INDIRECT_FUNC(row_method);
      const bool found=(ctx.r3.u32&255)!=0;const auto value=PPC_LOAD_U32(row_at);std::string row;
      const bool valid=!found || !value || CString(base,value,row,4096);
      ctx.r3.u64=row_at;__imp__sub_822F1C10(ctx,base);
      if(!valid){release_name();error="Original prefab data row is unterminated or exceeds budget";return 2;}
      if(found && value){total+=row.size();if(total>4*1024*1024){release_name();error="Original prefab data byte budget";return 2;}rows.push_back(std::move(row));}
    }
    release_name();
  }
  data=std::move(candidate);error.clear();return 0;
}
uint32_t DestroyRegisteredScriptEntity(PPCContext& ctx,uint8_t* base,const NativeScriptState& scripts,
    uint32_t handle,std::string& error){
  Preserve preserve(ctx);
  if(!BuiltinScope(ctx,base,scripts,error) || !BuiltinEntity(ctx,base,handle,error) || !Readable(base,0x833DAB60,4))return 2;
  const auto manager=PPC_LOAD_U32(0x833DAB60);
  if(!Readable(base,manager,640)){error="Native player manager unavailable for original DestroyEntity refusal";return 2;}
  const auto stack=ctx.r1.u32-4096,reference=stack+128;
  PPC_STORE_U32(stack,ctx.r1.u32);PPC_STORE_U32(reference,handle);ctx.r1.u64=stack;
  // Exact audited82737858 export: refuse managed players before asking the
  // native entity registry to destroy an ordinary actor. No raw frees or
  // source-only deletion list substitutes for the original engine lifecycle.
  ctx.r3.u64=manager;ctx.r4.u64=reference;__imp__sub_8276A1B0(ctx,base);
  if(ctx.r3.u32){error.clear();return 1;}
  if(!BuiltinEntity(ctx,base,handle,error))return 2;
  ctx.r3.u64=world->registry;ctx.r4.u64=reference;__imp__sub_82745270(ctx,base);
  error.clear();return 0;
}
uint32_t ReadRegisteredScriptPosition(PPCContext& ctx,uint8_t* base,const NativeScriptState& scripts,
    uint32_t handle,std::array<float,4>& position,std::string& error){
  Preserve preserve(ctx);
  if(!BuiltinScope(ctx,base,scripts,error))return 2;
  if(!handle){position={};error.clear();return 1;}
  if(!BuiltinEntity(ctx,base,handle,error))return 2;
  const auto stack=ctx.r1.u32-4096,result=stack+128;
  PPC_STORE_U32(stack,ctx.r1.u32);ctx.r1.u64=stack;
  ctx.r3.u64=world->entities.live.at(handle).address;ctx.r4.u64=result;
  // Exact8273C088 export: native transform component supplies its current
  // position; missing transform produces original zero vector, not a host pose.
  __imp__sub_8275C160(ctx,base);
  std::array<float,4> candidate{};
  for(unsigned i=0;i<4;++i)candidate[i]=std::bit_cast<float>(PPC_LOAD_U32(result+i*4));
  if(!std::all_of(candidate.begin(),candidate.end(),[](float v){return std::isfinite(v);})){error="Native position is non-finite";return 2;}
  position=candidate;error.clear();return 0;
}
uint32_t TeleportRegisteredScriptEntity(PPCContext& ctx,uint8_t* base,const NativeScriptState& scripts,
    uint32_t handle,uint32_t destination,uint32_t reset,uint32_t orientation,std::string& error){
  Preserve preserve(ctx);
  if(reset>1 || orientation>1){error="Invalid native teleport flags";return 2;}
  if(!BuiltinScope(ctx,base,scripts,error))return 2;
  // Native export resolves both references before testing either for null.
  if((handle && !BuiltinEntity(ctx,base,handle,error)) ||
     (destination && !BuiltinEntity(ctx,base,destination,error)))return 2;
  if(!handle || !destination){error.clear();return 1;}
  const auto entity=world->entities.live.at(handle).address,target=world->entities.live.at(destination).address;
  const auto stack=ctx.r1.u32-4096;PPC_STORE_U32(stack,ctx.r1.u32);ctx.r1.u64=stack;
  // Wii26B384 compares the source handle with the registry's primary role and
  // forces both flags. Keep that source rule even when the role is Oswald.
  if(handle==world->entities.primary.handle)reset=orientation=1;
  // Also preserve the target game's character-specific rule from8273BB90;
  //827610C0 handles physics, orientation and transform notifications.
  ctx.r3.u64=entity;__imp__sub_82740440(ctx,base);
  if(ctx.r3.u32){__imp__sub_82786850(ctx,base);if(ctx.r3.u32==8)reset=orientation=1;}
  ctx.r3.u64=entity;ctx.r4.u64=target;ctx.r5.u64=reset;ctx.r6.u64=orientation;ctx.r7.u64=1;
  __imp__sub_827610C0(ctx,base);error.clear();return 0;
}
uint32_t ParentRegisteredScriptEntity(PPCContext& ctx,uint8_t* base,const NativeScriptState& scripts,
    uint32_t handle,uint32_t parent,std::string& error){
  Preserve preserve(ctx);
  if(!BuiltinScope(ctx,base,scripts,error))return 2;
  if(!handle){error.clear();return 1;}
  if(!BuiltinEntity(ctx,base,handle,error) || (parent && !BuiltinEntity(ctx,base,parent,error)))return 2;
  const auto child=world->entities.live.at(handle).address;
  const auto destination=parent?world->entities.live.at(parent).address:0;
  const auto stack=ctx.r1.u32-4096,reference=stack+128;
  PPC_STORE_U32(stack,ctx.r1.u32);ctx.r1.u64=stack;
  ctx.r3.u64=child;__imp__sub_82761938(ctx,base);
  const auto component=ctx.r3.u32;
  if(!component){error.clear();return 1;}
  if(!Readable(base,component,415)){error="Original child parent component unavailable";return 2;}
  //827D87D8 uses the destination's parent component when linking its native
  // child list. Validate this prerequisite before any detach/attach effects.
  if(destination){
    ctx.r3.u64=destination;__imp__sub_82761938(ctx,base);
    if(!ctx.r3.u32 || !Readable(base,ctx.r3.u32,415)){
      error="Original destination parent component unavailable";return 2;
    }
  }
  // Exact827D8A18 export: face-8 is the complete parent object, null detaches.
  // The native helper retains its cycle/eligibility checks, old-parent removal,
  // child-list insertion, scene hierarchy and transform notifications.
  PPC_STORE_U32(reference,destination?destination-8:0);
  ctx.r3.u64=component;ctx.r4.u64=reference;__imp__sub_827D87D8(ctx,base);
  error.clear();return 0;
}
uint32_t ControlRegisteredScriptEntity(PPCContext& ctx,uint8_t* base,const NativeScriptState& scripts,
    uint32_t handle,uint32_t operation,std::string& error){
  Preserve preserve(ctx);
  if(operation>5){error="Invalid original entity control operation";return 2;}
  if(!BuiltinScope(ctx,base,scripts,error) || !BuiltinEntity(ctx,base,handle,error))return 2;
  const auto entity=world->entities.live.at(handle).address,stack=ctx.r1.u32-4096;
  PPC_STORE_U32(stack,ctx.r1.u32);ctx.r1.u64=stack;ctx.r3.u64=entity;
  // Actual exports831EFC58/831EFD08 resolve one native entity and call
  //831EFB78 with disabled=false/true. It protects managed players and calls
  // eligible native components' lifecycle methods, including physical state.
  if(operation<2){ctx.r4.u64=operation;__imp__sub_831EFB78(ctx,base);}
  // Actual831EF3D8/831EF488 invoke native hide/show recursively, preserving
  // native tools/children, scene membership and renderer notifications.
  else if(operation<4){ctx.r4.u64=operation==2?1:0;__imp__sub_8275D0E0(ctx,base);}
  //831F48C8/831F4A38 resolve the same last entity argument and call
  //831F4698(entity, true/false). This recursively visits native children
  //before setting NiAVObject application culling through82555170; distinct
  //from Hide/Show and without changing physics/component enabled state.
  else{ctx.r4.u64=operation==4?1:0;__imp__sub_831F4698(ctx,base);}
  REXLOG_INFO("[em1] original entity control handle={} operation={} native traversal complete",handle,operation);
  error.clear();return 0;
}
uint32_t ControlRegisteredScriptComponent(PPCContext& ctx,uint8_t* base,const NativeScriptState& scripts,
    uint32_t handle,std::string_view component,uint32_t disabled,std::string& error){
  Preserve preserve(ctx);
  if(disabled>1 || component.size()>256 || component.find('\0')!=component.npos){error="Invalid original component control arguments";return 2;}
  if(!BuiltinScope(ctx,base,scripts,error) || !BuiltinEntity(ctx,base,handle,error))return 2;
  const auto stack=ctx.r1.u32-4096,entity=stack+128,name=stack+132,argument=stack+136,text=stack+256;
  PPC_STORE_U32(stack,ctx.r1.u32);PPC_STORE_U32(entity,handle);PPC_STORE_U32(name,0);PPC_STORE_U32(argument,0);
  if(!component.empty())std::memcpy(base+text,component.data(),component.size());PPC_STORE_U8(text+uint32_t(component.size()),0);ctx.r1.u64=stack;
  ctx.r3.u64=text;__imp__sub_825430D8(ctx,base);PPC_STORE_U32(name,ctx.r3.u32);
  if((!ctx.r3.u32 && !component.empty()) || (ctx.r3.u32 && !Readable(base,ctx.r3.u32,1))){error="Original component name could not be interned";return 2;}
  // Exact82851858/82851980 sequence: keep the outer string owner, and pass
  // one additional owned reference to82851710, which consumes it on all paths.
  ctx.r3.u64=name;__imp__sub_82504410(ctx,base);PPC_STORE_U32(argument,PPC_LOAD_U32(name));
  ctx.r3.u64=entity;ctx.r4.u64=argument;ctx.r5.u64=disabled;__imp__sub_82851710(ctx,base);
  ctx.r3.u64=name;__imp__sub_822F1C10(ctx,base);
  REXLOG_INFO("[em1] original component control handle={} component='{}' disabled={} native lookup/traversal returned",handle,component,disabled);
  error.clear();return 0;
}
uint32_t SetRegisteredScriptParticleEmission(PPCContext& ctx,uint8_t* base,const NativeScriptState& scripts,
    uint32_t handle,uint32_t enabled,std::string& error){
  Preserve preserve(ctx);
  if(enabled>1){error="Invalid original particle enabled value";return 2;}
  if(!BuiltinScope(ctx,base,scripts,error) || !BuiltinEntity(ctx,base,handle,error))return 2;
  const auto stack=ctx.r1.u32-4096,result=stack+128;PPC_STORE_U32(stack,ctx.r1.u32);ctx.r1.u64=stack;
  ctx.r3.u64=world->entities.live.at(handle).address;__imp__sub_82761C50(ctx,base);
  const auto controller=ctx.r3.u32;
  if(!controller){error.clear();return 1;}
  if(!Readable(base,controller,0x54) || PPC_LOAD_U32(controller)!=0x8209BB90 || !Readable(base,0x833DC2C4,4)){
    error="Unsupported native particle controller/property registry";return 2;
  }
  const auto descriptor=PPC_LOAD_U32(0x833DC2C4),face=controller+8,table=PPC_LOAD_U32(face);
  if(!Readable(base,descriptor,8) || !Readable(base,table,0x114)){error="Native particle property interface unavailable";return 2;}
  const auto setter=PPC_LOAD_U32(table+0x110),getter=PPC_LOAD_U32(table+0xD0);
  auto method=[&](uint32_t p){return p>=0x82200000 && p<0x83340000 && !(p&3) && Readable(base,p,4);};
  if(!method(setter) || !method(getter)){error="Native particle property methods unavailable";return 2;}
  // Original82801768/82801890 controller and reflected-property sequence.
  ctx.r3.u64=controller;ctx.r4.u64=enabled;ctx.r5.u64=0;__imp__sub_828013F0(ctx,base);
  ctx.r3.u64=face;ctx.r4.u64=descriptor+4;ctx.r5.u64=enabled;ctx.r6.u64=0;PPC_CALL_INDIRECT_FUNC(setter);
  PPC_STORE_U8(result,0xFF);ctx.r3.u64=face;ctx.r4.u64=descriptor+4;ctx.r5.u64=result;ctx.r6.u64=0;PPC_CALL_INDIRECT_FUNC(getter);
  if(PPC_LOAD_U8(result)!=enabled){error="Original particle Enabled property did not match native request";return 2;}
  REXLOG_INFO("[em1] original registered particle handle={} enabled={} native property verified",handle,enabled);
  error.clear();return 0;
}
uint32_t AnimateRegisteredScriptEntity(PPCContext& ctx,uint8_t* base,const NativeScriptState& scripts,
    uint32_t handle,std::string_view original_name,std::string& error){
  Preserve preserve(ctx);
  if(!BuiltinScope(ctx,base,scripts,error) || !BuiltinEntity(ctx,base,handle,error))return 2;
  if(!original_name.starts_with("EVENT_") || original_name.size()>127 ||
     !std::all_of(original_name.begin(),original_name.end(),[](unsigned char c){return (c>='A'&&c<='Z') || (c>='a'&&c<='z') || (c>='0'&&c<='9') || c=='_';})){
    error="Invalid original animation event symbol";return 2;
  }
  const auto found=world->animation_events.find(std::string(original_name));uint32_t event=UINT32_MAX;
  if(found!=world->animation_events.end())event=found->second;
  else{
    constexpr uint32_t registry=0x833DCDC0,count=675;
    if(!Readable(base,registry,count*4)){error="Native Xbox animation event registry unavailable";return 2;}
    for(uint32_t i=0;i<count;++i){std::string name;
      if(CString(base,PPC_LOAD_U32(registry+i*4),name,127) && name==original_name){
        if(event!=UINT32_MAX){error="Ambiguous native animation event mapping";return 2;}event=i;
      }
    }
    if(event==UINT32_MAX){error="Original animation event has no native Xbox semantic equivalent: "+std::string(original_name);return 2;}
    world->animation_events.emplace(original_name,event);
  }
  const auto address=world->entities.live.at(handle).address;
  const auto stack=ctx.r1.u32-4096;PPC_STORE_U32(stack,ctx.r1.u32);ctx.r1.u64=stack;
  // Exact82844240 export after argument marshaling: obtain the actor's native
  // animation interface; absent interface is its original no-op, not a stub.
  ctx.r3.u64=address;__imp__sub_829AD330(ctx,base);
  const auto animation=ctx.r3.u32;if(!animation){
    if(++world->animation_noop<=4)REXLOG_INFO("[em1] native AnimEvent original no-op: handle={} symbol='{}' has no animation interface",handle,original_name);
    error.clear();return 1;
  }
  if(!Readable(base,animation,212)){error="Native animation interface became unreadable";return 2;}
  if(!PPC_LOAD_U32(animation+192)){
    if(++world->animation_noop<=4)REXLOG_INFO("[em1] native AnimEvent original no-op: handle={} symbol='{}' has no animation graph",handle,original_name);
    error.clear();return 1;
  }
  const auto context=PPC_LOAD_U32(animation+208);
  if(!Readable(base,context,16)){error="Native animation graph context is not ready";return 2;}
  auto queue=PPC_LOAD_U32(context+12);
  if(!queue){
    const auto table=PPC_LOAD_U32(context);if(!Readable(base,table,96)){error="Native animation event queue interface is not ready";return 2;}
    queue=PPC_LOAD_U32(table+92);
  }
  if(!Readable(base,queue,20) || PPC_LOAD_U32(queue+4)>65536 || PPC_LOAD_U32(queue+16)>PPC_LOAD_U32(queue+4)){
    error="Native animation event queue is not ready";return 2;
  }
  ctx.r3.u64=animation;ctx.r4.u64=event;__imp__sub_82843878(ctx,base);
  if(++world->animation_sent<=8)REXLOG_INFO("[em1] native AnimEvent queued: handle={} source_symbol='{}' native_id={}",handle,original_name,event);
  error.clear();return 0;
}
uint32_t SetRegisteredScriptAnimationVariable(PPCContext& ctx,uint8_t* base,const NativeScriptState& scripts,
    uint32_t handle,std::string_view original_name,int32_t value,std::string& error){
  Preserve preserve(ctx);
  if(!BuiltinScope(ctx,base,scripts,error) || !BuiltinEntity(ctx,base,handle,error))return 2;
  if(!original_name.starts_with("VAR_") || original_name.size()>127 ||
      original_name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_0123456789")!=original_name.npos){error="Invalid original animation variable symbol";return 2;}
  if(!world->native_variable_catalog_ready || world->animation_variables.empty()){error="Native variable targets were not admitted for this source world";return 2;}
  const auto found=world->animation_variables.find(std::string(original_name));
  if(found==world->animation_variables.end()){error="Original variable has no native Xbox semantic equivalent: "+std::string(original_name);return 2;}
  const auto id=found->second,entity=world->entities.live.at(handle).address,stack=ctx.r1.u32-4096;
  PPC_STORE_U32(stack,ctx.r1.u32);ctx.r1.u64=stack;ctx.r3.u64=entity;__imp__sub_829AD330(ctx,base);
  const auto animation=ctx.r3.u32;
  if(!animation){++world->variable_noop;error.clear();return 1;}
  if(!Readable(base,animation,212)){error="Native animation variable interface unreadable";return 2;}
  const auto graph=PPC_LOAD_U32(animation+192);
  if(!graph){++world->variable_noop;error.clear();return 1;}
  if(!Readable(base,graph,128)){error="Native animation variable graph unreadable";return 2;}
  const auto map=PPC_LOAD_U32(graph+116);
  if(!map){++world->variable_noop;error.clear();return 1;}
  if(!Readable(base,map,24)){error="Native animation variable map unreadable";return 2;}
  ctx.r3.u64=map+20;ctx.r4.u64=id;ctx.r5.s64=-1;__imp__sub_82393300(ctx,base);
  const auto index=ctx.r3.s32;
  if(index<0){++world->variable_noop;error.clear();return 1;}
  const auto store=PPC_LOAD_U32(graph+124);
  if(index>1000000 || !Readable(base,store,12)){error="Native animation variable storage unavailable";return 2;}
  const auto values=PPC_LOAD_U32(store+8);const uint64_t slot=uint64_t(values)+uint32_t(index)*4ull;
  if(slot>UINT32_MAX || !Readable(base,uint32_t(slot),4)){error="Native animation variable slot unreadable";return 2;}
  // Exact8283F8E8 after marshaling, with semantic-ID translation first.
  //8283E778 applies the graph's actual variable map and writes its own slot.
  ctx.r3.u64=animation;ctx.r4.u64=id;ctx.r5.s64=value;__imp__sub_8283E778(ctx,base);
  if(PPC_LOAD_U32(uint32_t(slot))!=uint32_t(value)){error="Native animation variable setter did not update the admitted slot";return 2;}
  if(++world->variable_sent<=8)REXLOG_INFO("[em1] native AnimVarInt applied handle={} symbol='{}' native_id={} value={}",handle,original_name,id,value);
  error.clear();return 0;
}
uint32_t GamebryoRegisteredScriptEntity(PPCContext& ctx,uint8_t* base,const NativeScriptState& scripts,
    uint32_t handle,uint32_t operation,std::string_view sequence,std::string& error){
  Preserve preserve(ctx);
  if(!BuiltinScope(ctx,base,scripts,error) || !BuiltinEntity(ctx,base,handle,error) || operation>1)return 2;
  if(operation==1 && (sequence.empty() || sequence.size()>256 || sequence.find('\0')!=sequence.npos)){
    error="Invalid original Gamebryo sequence name";return 2;
  }
  const auto entity=world->entities.live.at(handle).address,stack=ctx.r1.u32-4096;
  PPC_STORE_U32(stack,ctx.r1.u32);ctx.r1.u64=stack;ctx.r3.u64=entity;__imp__sub_82761CA8(ctx,base);
  const auto component=ctx.r3.u32;
  if(!component){error="Original target has no native Gamebryo animation component";return operation?2:1;}
  if(!Readable(base,component,244)){error="Native Gamebryo animation component became unreadable";return 2;}
  if(operation==0){
    // Exact82805258 export after entity resolution: stop/reset its own native
    // sequence component, then synchronize the entity's movement transform.
    ctx.r3.u64=component;ctx.r4.u64=1;__imp__sub_82808C28(ctx,base);
    ctx.r3.u64=entity;__imp__sub_82804F98(ctx,base);error.clear();return 0;
  }
  const auto manager=PPC_LOAD_U32(component+156);
  if(!Readable(base,manager,4) || PPC_LOAD_U8(component+160)){
    error="Original Gamebryo sequence manager is absent/not ready; source clip conversion is required";return 2;
  }
  const auto text=stack+256,reference=stack+128,result=stack+132;
  std::memcpy(base+text,sequence.data(),sequence.size());PPC_STORE_U8(text+uint32_t(sequence.size()),0);
  ctx.r3.u64=text;__imp__sub_825430D8(ctx,base);PPC_STORE_U32(reference,ctx.r3.u32);
  // Validate the actual manager's descriptor before changing any playback
  // state. An EM1-only Mickey clip absent from EM2's manager must be reported,
  // never replaced with an unrelated clip or counted as successful playback.
  ctx.r3.u64=manager;ctx.r4.u64=PPC_LOAD_U32(reference);__imp__sub_8300A3A8(ctx,base);const auto id=ctx.r3.u32;
  bool available=Readable(base,0x821D4B78,4) && id!=PPC_LOAD_U32(0x821D4B78);
  if(available){ctx.r3.u64=manager;ctx.r4.u64=id;__imp__sub_82808930(ctx,base);available=ctx.r3.u32 && Readable(base,ctx.r3.u32,36);}
  if(!available){
    ctx.r3.u64=reference;__imp__sub_822F1C10(ctx,base);
    error="Original clip '"+std::string(sequence)+"' is absent from this actor's native manager; source asset injection/retargeting is required";return 2;
  }
  // Verified original opening form is entity/name. Match the native export's
  // defaults exactly: wait=false, duration/start=0, speed sentinel=0, no teleport.
  PPC_STORE_U32(result,0);ctx.r3.u64=component;ctx.r4.u64=reference;ctx.r5.u64=0;ctx.r6.u64=result;ctx.r7.u64=1;
  ctx.f1.f64=0;ctx.f2.f64=0;__imp__sub_82804940(ctx,base);
  const bool selected=PPC_LOAD_U32(component+176)==id;
  if(selected)PPC_STORE_U8(component+184,1);
  ctx.r3.u64=reference;__imp__sub_822F1C10(ctx,base);
  if(!selected){error="Native Gamebryo sequence request was not accepted";return 2;}
  error.clear();return 0;
}
bool EndRegisteredScriptEvents(std::string& error){
  NotifyRegisteredEm1GraphicsReady(false);
  if(!world){error.clear();return true;}
  TraceCounters("end");
  em1_lua_world_cancel(world->vm.get());
  if(em1_lua_world_reset_map(world->vm.get())!=EM1_LUA_OK){error="Could not clear original map manager";return false;}
  if(em1_lua_world_set_bound_player(world->vm.get(),0)!=EM1_LUA_OK){error="Original primary role revocation failed";return false;}
  if(!world->entities.Clear(world->Sink())){world->phase=World::Phase::Failed;error="Original script entity revocation failed";return false;}
  world.reset();error.clear();return true;
}
bool BeginRegisteredScriptEvents(uint8_t* base,std::string_view state,std::shared_ptr<RegisteredBlueprintLibrary> blueprints,
    std::shared_ptr<LevelOriginalScripts> scripts,std::string& error){
  if(!base || !state.starts_with("em1_") || state.size()>240 || !blueprints || blueprints->guest_base!=base || blueprints->index.empty() ||
     blueprints->scenes.empty() || !scripts || !scripts->state.vm || scripts->level_origin.empty()){
    error="Original script world requires retained registered blueprints and a source-owned Lua world";return false;
  }
  Em1LuaWorldView view{};if(em1_lua_world_view(scripts->state.vm.get(),&view)!=EM1_LUA_OK){error="Original script VM is not initialized";return false;}
  if(scripts->source_scene_ids.size()!=blueprints->scenes.size()){
    error="Original script world and retained blueprint scene scopes differ";return false;
  }
  for(size_t i=0;i<blueprints->scenes.size();++i)if(blueprints->scenes[i].source_uuid!=scripts->source_scene_ids[i]){
    error="Original script world and retained blueprint source identities differ";return false;
  }
  std::map<std::string,uint32_t> variables;
  if(!ReadXboxAnimationVariables([&](uint32_t address,uint32_t& value){if(!Readable(base,address,4))return false;value=PPC_LOAD_U32(address);return true;},
    [&](uint32_t address,std::string& name){return CString(base,address,name,127);},variables,error))return false;
  std::vector<Em1LuaAnimationVariableTarget> targets;targets.reserve(variables.size());
  for(const auto& [name,id]:variables)targets.push_back({reinterpret_cast<const uint8_t*>(name.data()),name.size(),id,0});
  if(em1_lua_world_install_native_animation_variables(scripts->state.vm.get(),targets.data(),targets.size())!=EM1_LUA_OK){
    Em1LuaWorldView view{};em1_lua_world_view(scripts->state.vm.get(),&view);error=view.error_size?std::string(reinterpret_cast<const char*>(view.error),view.error_size):"Could not install checked native variable targets";return false;
  }
  if(!EndRegisteredScriptEvents(error))return false;
  auto next=std::make_unique<World>();next->base=base;next->state=state;next->origin=scripts->level_origin;
  next->lifecycle=NativeConsoleWorldState().lifecycle;next->blueprints=std::move(blueprints);next->scripts=std::move(scripts);next->vm=next->scripts->state.vm;
  next->animation_variables=std::move(variables);
  next->native_variable_catalog_ready=true;
  for(const auto& [guid,definition]:next->blueprints->index){
    if(!Readable(base,definition,20)){error="Original script blueprint owner became unreadable";return false;}
    for(unsigned i=0;i<4;++i)if(PPC_LOAD_U32(definition+4+i*4)!=guid[i]){error="Original script blueprint identity changed";return false;}
    next->entities.source.insert(guid);
  }
  if(em1_lua_world_reset_prefab_data(next->vm.get())!=EM1_LUA_OK){error="Could not reset source prefab table generation";return false;}
  if(em1_lua_world_reset_map(next->vm.get())!=EM1_LUA_OK){error="Could not reset original map manager";return false;}
  world=std::move(next);
  REXLOG_INFO("[em1] original event world loading: state='{}' source_definitions={}; native callbacks may enqueue, dispatch waits for commit",world->state,world->entities.source.size());
  error.clear();return true;
}
bool UpdateRegisteredScriptEvents(uint8_t* base,std::string& error){
  if(!world){error.clear();return true;}
  if(!Current(base)){
    const auto actual=NativeConsoleWorldState();
    TraceCounters("invalidated");
    error="Original script world ownership changed before event retirement (expected lifecycle "+std::to_string(world->lifecycle)+", actual "+std::to_string(actual.lifecycle)+", actor version "+std::to_string(actual.version)+")";
    Fail();return false;
  }
  NativeEntitySnapshot snapshot;const char* why=nullptr;uint32_t address=0;
  if(!ReadNativeEntitySnapshot(base,snapshot,why,address)){Fail();error=std::string("Original event entity snapshot: ")+(why?why:"invalid registry");return false;}
  auto role=ReadPrimaryRole(base,snapshot.registry);
  if(role.handle){
    const auto found=std::find_if(snapshot.records.begin(),snapshot.records.end(),[&](const auto& record){return record.handle==role.handle;});
    if(found==snapshot.records.end() || !RoleUnchanged(base,role))role={};
    else role.entity={found->id,found->debug_address};
  }
  if(!SetPrimary(role,error))return false;
  if((world->registry && world->registry!=snapshot.registry) || !world->entities.Refresh(snapshot.records,world->Sink())){
    Fail();error="Original script entity refresh/revocation failed";return false;
  }
  world->registry=snapshot.registry;
  ++world->updates;
  if(world->updates==1 || world->updates==30 || world->updates==300)TraceCounters("update");
  error.clear();return true;
}
bool CommitRegisteredScriptEvents(uint8_t* base,std::string& error){
  if(!world || world->phase!=World::Phase::Loading){error="No loading original script world to commit";return false;}
  if(!UpdateRegisteredScriptEvents(base,error))return false;
  world->phase=World::Phase::Active;
  NotifyRegisteredEm1GraphicsReady(true);
  REXLOG_INFO("[em1] original event world committed: state='{}' live_source_entities={}; native eligibility/typed callback routing enabled",world->state,world->entities.live.size());
  error.clear();return true;
}
// The caller immediately runs Update, which validates ownership and reports an
// explicit failure. Hiding stale ownership here would silently disable scripts
// forever and prevent their retained native state from being retired.
bool RegisteredScriptEventsReady(){return world && world->phase==World::Phase::Active;}
namespace {
ScriptEventRoute PrepareEventArguments(PPCContext& ctx,uint8_t* base,uint32_t target,std::string_view callback,
    std::span<const ScriptEventArgument> parameters,double delay,std::vector<Em1LuaArgument>& arguments,std::string& error){
  Preserve preserve(ctx);
  error.clear();if(!Current(base))return ScriptEventRoute::NotOwned;
  uint32_t exists=0;
  if(em1_lua_world_callback_exists(world->vm.get(),reinterpret_cast<const uint8_t*>(callback.data()),callback.size(),&exists)!=EM1_LUA_OK){error=VmError();return ScriptEventRoute::Rejected;}
  if(!exists){error.clear();return ScriptEventRoute::NativeFallback;}
  // This is a known original callback. An unbound/retired receiver (including
  // a retail-spawned player not yet admitted to the source scope) is rejected,
  // never mistaken for an ordinary HKS fallback that consumes its one-shot.
  Identity receiver;
  if(!Resolve(ctx,base,target,receiver,error))return ScriptEventRoute::Rejected;
  // Match original82755710 admission: a callable receiver gets a scheduled
  // thread;82755D50 consumes its one-shot without waiting for Lua execution.
  // Future call-graph inspection is diagnostic, not an engine admission gate:
  // legitimate Lua table calls/branches need not be statically decidable.
  // Every actual native operation still validates its service, arguments and
  // owned live identities at invocation. Lua execution errors remain failures.
  if(parameters.size()>15 || !std::isfinite(delay) || delay<0){error="Invalid original callback argument count/delay";return ScriptEventRoute::Rejected;}
  arguments.clear();arguments.reserve(parameters.size()+1);arguments.push_back({5,0,double(target),nullptr,0});
  size_t bytes=0;
  for(const auto& p:parameters){
    Em1LuaArgument a{};
    switch(p.kind){
      case ScriptEventArgumentKind::Nil:break;
      case ScriptEventArgumentKind::Boolean:if(p.number!=0 && p.number!=1){error="Invalid original callback Boolean";return ScriptEventRoute::Rejected;}a.kind=1;a.number=p.number;break;
      case ScriptEventArgumentKind::Number:if(!std::isfinite(p.number)){error="Nonfinite original callback number";return ScriptEventRoute::Rejected;}a.kind=2;a.number=p.number;break;
      case ScriptEventArgumentKind::String:bytes+=p.text.size();if(p.text.size()>65536 || bytes>262144){error="Original callback strings exceed admission budget";return ScriptEventRoute::Rejected;}a.kind=3;a.bytes=reinterpret_cast<const uint8_t*>(p.text.data());a.size=p.text.size();break;
      case ScriptEventArgumentKind::Entity:{Identity identity;if(!Resolve(ctx,base,p.handle,identity,error))return ScriptEventRoute::Rejected;a.kind=5;a.number=p.handle;break;}
      default:error="Unsupported original callback parameter";return ScriptEventRoute::Rejected;
    }
    arguments.push_back(a);
  }
  error.clear();return ScriptEventRoute::Queued;
}
}
ScriptEventRoute QueueRegisteredScriptEvent(PPCContext& ctx,uint8_t* base,uint32_t target,std::string_view callback,
    std::span<const ScriptEventArgument> parameters,double delay,std::string& error){
  std::vector<Em1LuaArgument> arguments;
  const auto route=PrepareEventArguments(ctx,base,target,callback,parameters,delay,arguments,error);
  if(route!=ScriptEventRoute::Queued)return route;
  const Em1LuaEventInput event{reinterpret_cast<const uint8_t*>(callback.data()),callback.size(),arguments.data(),arguments.size(),delay};
  if(em1_lua_world_start_batch(world->vm.get(),&event,1)!=EM1_LUA_OK){error=VmError();return ScriptEventRoute::Rejected;}
  error.clear();return ScriptEventRoute::Queued;
}
namespace {
struct PendingEvent {uint32_t target=0;std::string name;std::vector<ScriptEventArgument> arguments;double delay=0;};
struct ConnectionScope {
  uint32_t connection=0,owner=0,event=0,marshaller=0;
  std::shared_ptr<Em1LuaWorld> vm;
  bool claimed=false,rejected=false;
  std::vector<PendingEvent> pending;
};
thread_local ConnectionScope* connection_scope=nullptr;
bool CString(uint8_t* base,uint32_t at,std::string& out,size_t maximum){
  out.clear();for(size_t i=0;i<=maximum;++i){
    if(uint64_t(at)+i>=0x100000000ull || !Readable(base,at+uint32_t(i),1))return false;
    const auto c=PPC_LOAD_U8(at+uint32_t(i));if(!c)return true;if(i==maximum)return false;out.push_back(char(c));
  }return false;
}
void Reject(ConnectionScope& scope,const std::string& name,const std::string& reason){
  scope.claimed=true;scope.rejected=true;
  if(world)++world->rejected;
  if(world && world->diagnostics.size()<128 && world->diagnostics.insert(name+"\n"+reason).second)
    REXLOG_WARN("[em1] original event rejected before one-shot consumption: '{}' - {}",name,reason);
}
bool OwnedCompleteEntity(uint8_t* base,uint32_t complete){
  if(!Current(base) || complete<4096 || !Readable(base,complete+8,96))return false;
  Guid guid{};for(unsigned i=0;i<4;++i)guid[i]=PPC_LOAD_U32(complete+12+i*4);
  return world->entities.source.contains(guid);
}
bool ReadParameters(PPCContext& ctx,uint8_t* base,uint32_t wrapper,const ConnectionScope& scope,
    std::vector<ScriptEventArgument>& output,std::string& error){
  Preserve preserve(ctx);const auto saved=ctx;output.clear();
  if(ctx.r1.u32<32768 || !Readable(base,ctx.r1.u32-32768,32768) || !Readable(base,wrapper,8)){error="Native original parameter stack/wrapper unavailable";return false;}
  const auto state=PPC_LOAD_U32(wrapper+4);
  if(!Readable(base,state,56)){error="Native original parameter VM unavailable";return false;}
  const auto storage=PPC_LOAD_U32(state+48),top=PPC_LOAD_U32(state+36),bottom=PPC_LOAD_U32(state+40),calls=PPC_LOAD_U32(state+20);
  if(top<storage || bottom<storage || bottom>top || (top-storage)%8 || (bottom-storage)%8 || top-storage>8*1024*1024){error="Native parameter VM stack layout changed";return false;}
  const auto top_offset=top-storage,base_offset=bottom-storage;
  struct RestoreStack {uint8_t* base;uint32_t state,top,low;~RestoreStack(){
    if(Readable(base,state,56)){const auto storage=PPC_LOAD_U32(state+48);if(Readable(base,storage+top,8) && Readable(base,storage+low,8)){
      PPC_STORE_U32(state+36,storage+top);PPC_STORE_U32(state+40,storage+low);
    }}
  }} restore_stack{base,state,top_offset,base_offset};
  const auto scratch=saved.r1.u32-2048,vector=scratch+128;std::memset(base+vector,0,16);
  auto reset=[&]{ctx=saved;ctx.r1.u64=scratch;PPC_STORE_U32(scratch,saved.r1.u32);};
  struct FreeVector {PPCContext& ctx;PPCContext saved;uint8_t* base;uint32_t vector,scratch;
    ~FreeVector(){
      const auto data=PPC_LOAD_U32(vector),count=PPC_LOAD_U32(vector+4);
      if(!data)return;if(count>4096 || !Readable(base,data,size_t(count)*4))return;
      for(uint32_t i=0;i<count;++i){ctx=saved;ctx.r1.u64=scratch;ctx.r3.u64=data+i*4;__imp__sub_822F1C10(ctx,base);}
      ctx=saved;ctx.r1.u64=scratch;ctx.r3.u64=data;__imp__sub_826547C8(ctx,base);
    }
  } free_vector{ctx,saved,base,vector,scratch};
  reset();ctx.r3.u64=scope.connection+24;ctx.r4.u64=vector;__imp__sub_827548D8(ctx,base);
  const auto data=PPC_LOAD_U32(vector),count=PPC_LOAD_U32(vector+4);
  if(count>15 || (count && !Readable(base,data,size_t(count)*4))){error="Original parameter vector exceeds supported callback arity";return false;}
  if(scope.marshaller && (!Readable(base,scope.marshaller,8) || PPC_LOAD_U32(scope.marshaller)!=0x82075B28)){
    error="Original connection parameter marshaler requires a dedicated typed adapter";return false;
  }
  for(uint32_t i=0;i<count;++i){
    ScriptEventArgument value;bool handled=false;const auto current_storage=PPC_LOAD_U32(state+48),before=PPC_LOAD_U32(state+36);
    if(scope.marshaller){
      reset();ctx.r3.u64=scope.marshaller;ctx.r4.u64=data+i*4;ctx.r5.u64=scope.connection;ctx.r6.u64=scope.event;ctx.r7.u64=scope.owner;
      __imp__sub_827578B0(ctx,base);handled=ctx.r3.u8!=0;
    }
    const auto after_storage=PPC_LOAD_U32(state+48),after=PPC_LOAD_U32(state+36);
    const auto offset=before-current_storage;
    if(PPC_LOAD_U32(state+20)!=calls || after<after_storage || after-after_storage!=offset+(handled?8:0)){
      error="Original typed parameter marshaler changed native VM call/stack ownership";return false;
    }
    if(!handled){
      value.kind=ScriptEventArgumentKind::String;
      if(!CString(base,PPC_LOAD_U32(data+i*4),value.text,65536)){error="Original parameter string is unreadable/oversized";return false;}
    }else{
      const auto slot=after_storage+offset;if(!Readable(base,slot,8)){error="Original typed parameter is unreadable";return false;}
      const auto word=PPC_LOAD_U32(slot),type=PPC_LOAD_U32(slot+4)&15;
      switch(type){
        case 0:break;
        case 1:value.kind=ScriptEventArgumentKind::Boolean;value.number=word!=0;break;
        // Verified827576B8 entity-token cases use native31310 integer handles.
        // Literal numeric parameters remain original strings; float payloads
        // use type3. Do not conflate an arbitrary native VM integer with one.
        case 2:value.kind=ScriptEventArgumentKind::Entity;value.handle=word;break;
        case 3:value.kind=ScriptEventArgumentKind::Number;value.number=std::bit_cast<float>(word);if(!std::isfinite(value.number)){error="Nonfinite original event parameter";return false;}break;
        case 4:{if(!Readable(base,word,12)){error="Original VM string is unreadable";return false;}const auto length=PPC_LOAD_U32(word+4)&0x3fffffff;
          if(length>65536 || (length && !Readable(base,word+12,length))){error="Original VM string exceeds argument bounds";return false;}
          value.kind=ScriptEventArgumentKind::String;value.text.assign(reinterpret_cast<const char*>(base+word+12),length);break;}
        default:error="Original event userdata/table requires an owned payload adapter";return false;
      }
      PPC_STORE_U32(state+36,after_storage+offset);
    }
    output.push_back(std::move(value));
  }
  error.clear();return true;
}
bool CommitConnection(PPCContext& ctx,uint8_t* base,ConnectionScope& scope,std::string& error){
  if(!Current(base) || world->vm!=scope.vm){error="Original source world changed during event admission";return false;}
  std::vector<std::vector<Em1LuaArgument>> arguments(scope.pending.size());std::vector<Em1LuaEventInput> events;events.reserve(scope.pending.size());
  for(size_t i=0;i<scope.pending.size();++i){const auto& p=scope.pending[i];
    if(PrepareEventArguments(ctx,base,p.target,p.name,p.arguments,p.delay,arguments[i],error)!=ScriptEventRoute::Queued)return false;
    events.push_back({reinterpret_cast<const uint8_t*>(p.name.data()),p.name.size(),arguments[i].data(),arguments[i].size(),p.delay});
  }
  if(em1_lua_world_start_batch(world->vm.get(),events.data(),events.size())!=EM1_LUA_OK){error=VmError();return false;}
  const auto previous=world->queued;world->queued+=events.size();
  if(previous<16)REXLOG_INFO("[em1] original event batch queued: targets={} first='{}' total={}",events.size(),scope.pending.empty()?"":scope.pending.front().name,world->queued);
  return true;
}
void NativeConnection(PPCContext& ctx,uint8_t* base,PPCFunc* original){
  const auto current=Current(base);
  const bool native_scope=uint32_t(ctx.lr)==0x82755E78 && ctx.r3.u32==ctx.r1.u32+144;
  const bool owned=current && native_scope && OwnedCompleteEntity(base,ctx.r5.u32);
  if(world){
    ++world->native_entries;if(owned)++world->native_scopes;
    if(world->native_entries<=8){Preserve preserve(ctx);
      REXLOG_INFO("[em1] original IO executor observed: caller={:08X} connection={:08X} owner={:08X} current={} native_scope={} source_owner={}",uint32_t(ctx.lr),ctx.r3.u32,ctx.r5.u32,current,native_scope,owned);
    }
  }
  if(!owned){
    original(ctx,base);return;
  }
  ConnectionScope scope;scope.connection=ctx.r3.u32;scope.event=ctx.r4.u32;scope.owner=ctx.r5.u32;scope.marshaller=ctx.r7.u32;scope.vm=world->vm;
  const auto previous=connection_scope;connection_scope=&scope;
  struct Reset {ConnectionScope* previous;~Reset(){connection_scope=previous;}} reset{previous};
  original(ctx,base);
  Preserve preserve(ctx);
  if(!scope.rejected && !scope.pending.empty()){
    std::string error;if(!CommitConnection(ctx,base,scope,error))Reject(scope,scope.pending.front().name,error);
  }
  if(scope.claimed && scope.rejected && Readable(base,scope.connection,44)){
    // This is the listener's temporary connection COPY. Clearing its one-shot
    // bit retains the authored entry only for failed host admission (ownership,
    // typed parameters or queue limits). Once scheduled, a later Lua error
    // cannot undo original82755D50 one-shot consumption.
    PPC_STORE_U16(scope.connection+28,uint16_t(PPC_LOAD_U16(scope.connection+28)&~1u));
  }
}
void NativeLookup(PPCContext& ctx,uint8_t* base,PPCFunc* original){
  // An earlier genuine HKS lookup resolves authored OUTPUT names to numeric
  // native event kinds before listeners exist. Observe its actual result;
  // never replace constants or bypass the native registration gate.
  if(Current(base) && uint32_t(ctx.lr)==0x82753E28){
    const auto wrapper=ctx.r3.u32;std::string output;
    {Preserve preserve(ctx);CString(base,ctx.r4.u32,output,256);}
    original(ctx,base);
    Preserve preserve(ctx);++world->output_lookups;
    uint32_t type=UINT32_MAX;
    if(Readable(base,wrapper,8)){
      const auto state=PPC_LOAD_U32(wrapper+4);
      if(Readable(base,state,52)){
        const auto top=PPC_LOAD_U32(state+36);
        if(top>=8 && Readable(base,top-8,8))type=PPC_LOAD_U32(top-4)&15;
      }
    }
    if(type==0)++world->output_nil;
    if(world->output_lookups<=32)REXLOG_INFO("[em1] original IO output constant: '{}' native_type={} component={:08X}",output,type,ctx.r25.u32);
    return;
  }
  auto* scope=connection_scope;
  if(!scope || !Current(base) || uint32_t(ctx.lr)!=0x82755A58 || ctx.r25.u32!=scope->connection || !Readable(base,ctx.r26.u32,4)){
    original(ctx,base);return;
  }
  const auto wrapper=ctx.r3.u32;const auto target=PPC_LOAD_U32(ctx.r26.u32);std::string name,error;
  if(!CString(base,ctx.r4.u32,name,256)){original(ctx,base);return;}
  std::vector<Em1LuaArgument> unused;
  auto route=PrepareEventArguments(ctx,base,target,name,{},0,unused,error);
  if(world){
    ++world->lookups;if(route==ScriptEventRoute::NativeFallback)++world->fallbacks;
    if(world->lookups<=16){Preserve preserve(ctx);
      REXLOG_INFO("[em1] original IO callback lookup: '{}' target={:08X} route={} phase={}",name,target,unsigned(route),world->phase==World::Phase::Loading?"loading":"active");
    }
  }
  if(route==ScriptEventRoute::NotOwned || route==ScriptEventRoute::NativeFallback){original(ctx,base);return;}
  scope->claimed=true;
  if(route==ScriptEventRoute::Rejected)Reject(*scope,name,error);
  else{
    PendingEvent pending;pending.target=target;pending.name=name;
    pending.delay=std::bit_cast<float>(PPC_LOAD_U32(scope->connection+32));
    if(scope->pending.size()>=256 || !ReadParameters(ctx,base,wrapper,*scope,pending.arguments,error))Reject(*scope,name,error.empty()?"Original event target budget exceeded":error);
    else{
      route=PrepareEventArguments(ctx,base,target,name,pending.arguments,pending.delay,unused,error);
      if(route!=ScriptEventRoute::Queued)Reject(*scope,name,error);else scope->pending.push_back(std::move(pending));
    }
  }
  // Preserve the original native lookup/type-gate path using its genuine nil
  // push; the owned callback is scheduled in standard Lua, never fake HKS code.
  ctx.r3.u64=wrapper;__imp__sub_82730080(ctx,base);
}
}
void ObserveRegisteredNativeIOInitialization(PPCContext& ctx,uint8_t* base,PPCFunc* original){
  uint32_t component=ctx.r3.u32,owner=0;bool owned=false,core_candidate=false,trace=false;
  if(Current(base)){
    Preserve preserve(ctx);++world->initializers;trace=world->initializers<=16;
    if(Readable(base,component,192)){
      owner=PPC_LOAD_U32(component+48);owned=OwnedCompleteEntity(base,owner);
      // Diagnostic only: determine the actual interface layout without
      // admitting an alternate GUID offset to script ownership.
      if(Readable(base,owner,20)){
        Guid guid{};for(unsigned i=0;i<4;++i)guid[i]=PPC_LOAD_U32(owner+4+i*4);
        core_candidate=world->entities.source.contains(guid);
      }
      if(owned)++world->source_initializers;
      if(core_candidate)++world->core_candidates;
      if((owned && world->source_initializers<=8) || (core_candidate && world->core_candidates<=8))trace=true;
    }
  }
  original(ctx,base);
  if(trace && Current(base)){
    Preserve preserve(ctx);
    const auto readable=Readable(base,component,192);
    REXLOG_INFO("[em1] original IO initialized: component={:08X} owner={:08X} source_owner={} core_candidate={} event_map_count={} secondary_map_count={} named_source_map_count={}",
      component,owner,owned,core_candidate,readable?PPC_LOAD_U32(component+112):UINT32_MAX,readable?PPC_LOAD_U32(component+148):UINT32_MAX,readable?PPC_LOAD_U32(component+184):UINT32_MAX);
  }
}
void ObserveRegisteredNativeIOProperty(PPCContext& ctx,uint8_t* base,PPCFunc* original){
  const bool inspect=Current(base) && uint32_t(ctx.lr)==0x82754A80;
  uint32_t layer=ctx.r3.u32,key=0;
  if(inspect && Readable(base,ctx.r4.u32,4))key=PPC_LOAD_U32(ctx.r4.u32);
  original(ctx,base);
  if(inspect && Current(base)){
    Preserve preserve(ctx);++world->property_checks;
    if(ctx.r3.u32 && Readable(base,ctx.r3.u32,16) && Readable(base,ctx.r20.u32,52)){
      const auto owner=PPC_LOAD_U32(ctx.r20.u32+48);
      if(OwnedCompleteEntity(base,owner)){
        const auto property=ctx.r3.u32,count=PPC_LOAD_U32(property+12);
        const auto ordinal=count?++world->source_arrays_nonzero:++world->source_arrays_zero;
        if(ordinal<=8){
          std::string first;uint32_t first_word=0;
          const auto values=PPC_LOAD_U32(property+8);
          if(count && Readable(base,values,4)){first_word=PPC_LOAD_U32(values);CString(base,first_word,first,128);}
          REXLOG_INFO("[em1] original IO source Output native value: owner_guid={:08X}{:08X}{:08X}{:08X} layer={:08X} property={:08X} metadata={:08X} value={:08X} count={} inherited_only={}",
            PPC_LOAD_U32(owner+12),PPC_LOAD_U32(owner+16),PPC_LOAD_U32(owner+20),PPC_LOAD_U32(owner+24),layer,property,PPC_LOAD_U32(property+4),PPC_LOAD_U32(property+8),count,ctx.r30.u32);
          if(count)REXLOG_INFO("[em1] original IO source first Output element: word={:08X} text='{}'",first_word,first);
        }
      }
    }
    if(!ctx.r3.u32){
      ++world->property_missing;
      bool candidate=false;
      if(Readable(base,ctx.r20.u32,52)){
        const auto owner=PPC_LOAD_U32(ctx.r20.u32+48);
        candidate=OwnedCompleteEntity(base,owner);
        if(Readable(base,owner,20)){
          Guid guid{};for(unsigned i=0;i<4;++i)guid[i]=PPC_LOAD_U32(owner+4+i*4);
          candidate|=world->entities.source.contains(guid);
        }
      }
      if(candidate)++world->source_property_missing;
      if(world->property_missing<=8 || (candidate && world->source_property_missing<=8)){
        std::string expected,names;CString(base,key,expected,128);
        uint32_t count=UINT32_MAX;
        if(Readable(base,layer,44)){
          count=PPC_LOAD_U32(layer+40);const auto items=PPC_LOAD_U32(layer+36);
          if(count<=4096 && (!count || Readable(base,items,size_t(count)*4))){
            for(uint32_t i=0;i<std::min(count,8u);++i){
              const auto property=PPC_LOAD_U32(items+i*4);std::string name;
              if(Readable(base,property,4))CString(base,PPC_LOAD_U32(property),name,128);
              if(i)names+=",";names+=name;
            }
          }
        }
        REXLOG_INFO("[em1] original IO Output property absent: layer={:08X} source_candidate={} expected='{}' key={:08X} count={} names='{}'",layer,candidate,expected,key,count,names);
      }
    }
  }
}
void ObserveRegisteredNativeIOGetter(PPCContext& ctx,uint8_t* base,PPCFunc* original,unsigned kind){
  const auto caller=uint32_t(ctx.lr),out=ctx.r5.u32;
  const bool site=kind==0?caller==0x82754AC0:kind==1?(caller==0x82754C10 || caller==0x82754C34 || caller==0x82754C58):caller==0x82754BD8;
  bool inspect=false;
  if(site && Current(base) && Readable(base,ctx.r20.u32,52))inspect=OwnedCompleteEntity(base,PPC_LOAD_U32(ctx.r20.u32+48));
  original(ctx,base);
  if(inspect && Current(base)){
    Preserve preserve(ctx);uint32_t value=UINT32_MAX;std::string text;
    if(Readable(base,out,4)){
      value=kind==2?PPC_LOAD_U16(out):PPC_LOAD_U32(out);
      if(kind==1)CString(base,value,text,128);
    }
    const unsigned bucket=kind==0 && value?3:kind;
    if(++world->getter_traces[bucket]<=12)REXLOG_INFO("[em1] original IO native typed getter: caller={:08X} kind={} success={} value={:08X} text='{}'",caller,kind,ctx.r3.u32,value,text);
  }
}
void ObserveRegisteredNativeIOListener(PPCContext& ctx,uint8_t* base,PPCFunc* original){
  if(Current(base)){
    Preserve preserve(ctx);++world->listeners;
    const auto listener=ctx.r3.u32,event=ctx.r4.u32;
    const auto owner=listener>=16 && Readable(base,listener-16,4)?PPC_LOAD_U32(listener-16):0;
    const auto owned=OwnedCompleteEntity(base,owner);if(owned)++world->source_listeners;
    if(world->listeners<=16)REXLOG_INFO("[em1] original IO listener delivery: listener={:08X} owner={:08X} source_owner={} event_kind={}",
      listener,owner,owned,Readable(base,event,6)?PPC_LOAD_U16(event+4):UINT32_MAX);
  }
  original(ctx,base);
}
void ExecuteRegisteredNativeConnection(PPCContext& ctx,uint8_t* base){NativeConnection(ctx,base,__imp__sub_82755710);}
void LookupRegisteredNativeCallback(PPCContext& ctx,uint8_t* base){NativeLookup(ctx,base,__imp__sub_82730050);}
bool TestRegisteredScriptEventOwnership(){
  if(!TestXboxAnimationVariables())return false;
  const auto csr=_mm_getcsr();struct RestoreCsr{unsigned value;~RestoreCsr(){_mm_setcsr(value);}} restore_csr{csr};
  if(world || connection_scope)return false;
  struct Calls {std::vector<std::pair<char,uint32_t>> values;bool fail_unbind=false;} calls;
  BindingSink sink{&calls,+[](void* p,uint32_t h){static_cast<Calls*>(p)->values.emplace_back('b',h);return true;},
    +[](void* p,uint32_t h){auto& c=*static_cast<Calls*>(p);if(c.fail_unbind)return false;c.values.emplace_back('u',h);return true;}};
  {
    Bindings roles;const Identity retail{{10,20,30,40},0x5000};
    PrimaryRole role{0x6000,0x7000,44,retail};
    if(!roles.SetPrimary(role,sink) || !roles.Owns(44,retail) ||
      roles.Admit(45,retail,sink) || roles.Admit(44,{retail.guid,0x5004},sink))return false;
    calls.values.clear();
    if(!roles.SetPrimary(role,sink) || !calls.values.empty())return false;
    role.manager+=128;
    if(!roles.SetPrimary(role,sink) || calls.values!=std::vector<std::pair<char,uint32_t>>{{'u',44},{'b',44}})return false;
    calls.values.clear();role.registry+=128;
    if(!roles.SetPrimary(role,sink) || calls.values!=std::vector<std::pair<char,uint32_t>>{{'u',44},{'b',44}})return false;
    calls.values.clear();
    if(!roles.SetPrimary({},sink) || roles.Owns(44,retail) || roles.Admit(44,retail,sink) ||
      calls.values!=std::vector<std::pair<char,uint32_t>>{{'u',44}})return false;
    calls.values.clear();
  }
  Bindings bindings;const Guid a{1,2,3,4},b{5,6,7,8},foreign{1,2,3,9};bindings.source={a,b};
  auto record=[](Guid guid,uint32_t handle,uint32_t address){NativeEntityRecord r;r.id=guid;r.handle=handle;r.debug_address=address;return r;};
  std::vector<NativeEntityRecord> records{record(a,10,0x1000),record(foreign,11,0x2000)};
  if(!bindings.Refresh(records,sink) || calls.values!=std::vector<std::pair<char,uint32_t>>{{'b',10}})return false;
  calls.values.clear();if(!bindings.Refresh(records,sink) || !calls.values.empty())return false;
  records={record(b,10,0x3000),record(a,12,0x4000)};
  if(!bindings.Refresh(records,sink) || calls.values!=std::vector<std::pair<char,uint32_t>>{{'u',10},{'b',10},{'b',12}})return false;
  calls.values.clear();records.push_back(record(foreign,12,0x5000));
  if(bindings.Refresh(records,sink) || !calls.values.empty() || bindings.live.size()!=2)return false;
  calls.fail_unbind=true;if(bindings.Refresh({},sink) || bindings.live.size()!=2)return false;
  calls.fail_unbind=false;if(!bindings.Refresh({},sink) || bindings.live.size()!=0)return false;
  calls.values.clear();if(bindings.Admit(13,{foreign,0x1000},sink) || !calls.values.empty())return false;
  if(!bindings.Admit(13,{a,0x1000},sink) || !bindings.Admit(13,{a,0x2000},sink) ||
    calls.values!=std::vector<std::pair<char,uint32_t>>{{'b',13},{'u',13},{'b',13}} || !bindings.Clear(sink))return false;
  const auto original=+[](PPCContext& c,uint8_t*){c.r3.u64=0x12345678;c.r24.u64^=0xabcdef;c.lr=0xfeedfeed;_mm_setcsr(_mm_getcsr()^_MM_FLUSH_ZERO_MASK);};
  PPCContext context{};context.r1.u64=0x20000;context.r3.u64=0x20090;context.r24.u64=7;context.lr=0x82755E78;
  auto expected=context;original(expected,nullptr);const auto expected_csr=_mm_getcsr();_mm_setcsr(csr);
  NativeConnection(context,nullptr,original);
  if(std::memcmp(&context,&expected,sizeof context) || _mm_getcsr()!=expected_csr)return false;
  _mm_setcsr(csr);context={};context.lr=0x82755A58;expected=context;original(expected,nullptr);_mm_setcsr(csr);
  NativeLookup(context,nullptr,original);
  if(std::memcmp(&context,&expected,sizeof context) || _mm_getcsr()!=expected_csr)return false;
  for(const auto hook:{ObserveRegisteredNativeIOInitialization,ObserveRegisteredNativeIOListener,ObserveRegisteredNativeIOProperty}){
    _mm_setcsr(csr);context={};context.r3.u64=0x4000;context.r24.u64=17;expected=context;
    original(expected,nullptr);_mm_setcsr(csr);hook(context,nullptr,original);
    if(std::memcmp(&context,&expected,sizeof context) || _mm_getcsr()!=expected_csr)return false;
  }
  for(unsigned kind=0;kind<3;++kind){
    _mm_setcsr(csr);context={};context.r3.u64=0x4000;context.r24.u64=17;expected=context;
    original(expected,nullptr);_mm_setcsr(csr);ObserveRegisteredNativeIOGetter(context,nullptr,original,kind);
    if(std::memcmp(&context,&expected,sizeof context) || _mm_getcsr()!=expected_csr)return false;
  }
  {
    NativeScriptState scripts;std::string error;uint32_t handle=123;
    context={};context.r24.u64=0xabcdef;expected=context;_mm_setcsr(csr);
    SetLastError(0x5A1234);
    if(ValidateRegisteredScriptEngineScope(context,nullptr,scripts,error) || error.empty() ||
      GetLastError()!=0x5A1234 || std::memcmp(&context,&expected,sizeof context) || _mm_getcsr()!=csr)return false;
    for(uint32_t operation=0;operation<4;++operation){
      if(FadeRegisteredScriptScreen(context,nullptr,scripts,operation,0.0f,error)!=2 || error.empty() ||
        GetLastError()!=0x5A1234 || std::memcmp(&context,&expected,sizeof context) || _mm_getcsr()!=csr)return false;
    }
    for(uint32_t operation=0;operation<3;++operation){
      if(ControlRegisteredScriptCamera(context,nullptr,scripts,operation,0,0,0,error)!=2 || error.empty() ||
        GetLastError()!=0x5A1234 || std::memcmp(&context,&expected,sizeof context) || _mm_getcsr()!=csr)return false;
    }
    std::vector<uint32_t> group{123};
    if(FindRegisteredScriptGroup(context,nullptr,scripts,"foreign",group,error)!=2 || group!=std::vector<uint32_t>{123} ||
      error.empty() || std::memcmp(&context,&expected,sizeof context) || _mm_getcsr()!=csr)return false;
    if(FindRegisteredScriptEntity(context,nullptr,scripts,"foreign",handle,error)!=2 || handle || error.empty() ||
      std::memcmp(&context,&expected,sizeof context) || _mm_getcsr()!=csr)return false;
    if(DestroyRegisteredScriptEntity(context,nullptr,scripts,123,error)!=2 || error.empty() ||
      std::memcmp(&context,&expected,sizeof context) || _mm_getcsr()!=csr)return false;
    handle=123;
    if(FindRegisteredScriptRelativeEntity(context,nullptr,scripts,123,"owned",handle,error)!=2 || handle || error.empty() ||
      std::memcmp(&context,&expected,sizeof context) || _mm_getcsr()!=csr || GetLastError()!=0x5A1234)return false;
    std::array<uint32_t,4> prefab_guid{1,2,3,4};
    if(GetRegisteredScriptPrefabOwner(context,nullptr,scripts,123,prefab_guid,error)!=2 || prefab_guid!=std::array<uint32_t,4>{1,2,3,4} ||
      error.empty() || std::memcmp(&context,&expected,sizeof context) || _mm_getcsr()!=csr || GetLastError()!=0x5A1234)return false;
    if(AnimateRegisteredScriptEntity(context,nullptr,scripts,123,"EVENT_Force_Idle",error)!=2 || error.empty() ||
      std::memcmp(&context,&expected,sizeof context) || _mm_getcsr()!=csr)return false;
    if(SetRegisteredScriptAnimationVariable(context,nullptr,scripts,123,"VAR_In_Conversation",1,error)!=2 || error.empty() ||
      std::memcmp(&context,&expected,sizeof context) || _mm_getcsr()!=csr)return false;
    if(GamebryoRegisteredScriptEntity(context,nullptr,scripts,123,1,"Missing",error)!=2 || error.empty() ||
      std::memcmp(&context,&expected,sizeof context) || _mm_getcsr()!=csr)return false;
    std::array<float,4> position{1,2,3,4};
    if(ReadRegisteredScriptPosition(context,nullptr,scripts,123,position,error)!=2 || position!=std::array<float,4>{1,2,3,4} ||
      error.empty() || std::memcmp(&context,&expected,sizeof context) || _mm_getcsr()!=csr || GetLastError()!=0x5A1234)return false;
    if(TeleportRegisteredScriptEntity(context,nullptr,scripts,123,124,0,1,error)!=2 || error.empty() ||
      std::memcmp(&context,&expected,sizeof context) || _mm_getcsr()!=csr || GetLastError()!=0x5A1234)return false;
    if(ParentRegisteredScriptEntity(context,nullptr,scripts,123,124,error)!=2 || error.empty() ||
      std::memcmp(&context,&expected,sizeof context) || _mm_getcsr()!=csr || GetLastError()!=0x5A1234)return false;
    RegisteredPrefabData prefab_data;prefab_data.owner={1,2,3,4};prefab_data.defaults={"retained=1"};
    if(ReadRegisteredScriptPrefabData(context,nullptr,scripts,123,prefab_data,error)!=2 ||
      prefab_data.owner!=std::array<uint32_t,4>{1,2,3,4} || prefab_data.defaults!=std::vector<std::string>{"retained=1"} ||
      error.empty() || std::memcmp(&context,&expected,sizeof context) || _mm_getcsr()!=csr || GetLastError()!=0x5A1234)return false;
    for(uint32_t operation=0;operation<7;++operation){
      if(ControlRegisteredScriptEntity(context,nullptr,scripts,123,operation,error)!=2 || error.empty() ||
        std::memcmp(&context,&expected,sizeof context) || _mm_getcsr()!=csr)return false;
    }
    for(uint32_t disabled=0;disabled<3;++disabled){
      if(ControlRegisteredScriptComponent(context,nullptr,scripts,123,"PhysicsStaticCollisionComponent",disabled,error)!=2 ||
        error.empty() || std::memcmp(&context,&expected,sizeof context) || _mm_getcsr()!=csr)return false;
    }
    for(uint32_t enabled=0;enabled<3;++enabled){
      if(SetRegisteredScriptParticleEmission(context,nullptr,scripts,123,enabled,error)!=2 || error.empty() ||
        std::memcmp(&context,&expected,sizeof context) || _mm_getcsr()!=csr)return false;
    }
  }
  REXLOG_INFO("[em1] script event ownership tests passed: exact source GUIDs, verified primary handle/address/manager/registry role, role reuse revocation, loading-time admission, handle/address reuse, revoke-before-bind, foreign rejection, duplicate rollback, revocation failure and ordinary native full-context/FP fallback");return true;
}
}
PPC_FUNC_IMPL(sub_82755710){em2::em1::ExecuteRegisteredNativeConnection(ctx,base);}
PPC_FUNC_IMPL(sub_82730050){em2::em1::LookupRegisteredNativeCallback(ctx,base);}
PPC_FUNC_IMPL(sub_82754A18){em2::em1::ObserveRegisteredNativeIOInitialization(ctx,base,__imp__sub_82754A18);}
PPC_FUNC_IMPL(sub_82755D50){em2::em1::ObserveRegisteredNativeIOListener(ctx,base,__imp__sub_82755D50);}
PPC_FUNC_IMPL(sub_827B84B0){em2::em1::ObserveRegisteredNativeIOProperty(ctx,base,__imp__sub_827B84B0);}
PPC_FUNC_IMPL(sub_827B84F8){em2::em1::ObserveRegisteredNativeIOGetter(ctx,base,__imp__sub_827B84F8,0);}
PPC_FUNC_IMPL(sub_827B9310){em2::em1::ObserveRegisteredNativeIOGetter(ctx,base,__imp__sub_827B9310,1);}
PPC_FUNC_IMPL(sub_827B8858){em2::em1::ObserveRegisteredNativeIOGetter(ctx,base,__imp__sub_827B8858,2);}
