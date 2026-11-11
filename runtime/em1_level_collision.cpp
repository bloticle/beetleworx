#include "em1_level_collision.h"
#include "em1_motion_states.h"
#include "em1_resource_catalog.h"
#include "em1_jigsaw_havok_probe.h"
#include "native_entity_probe.h"
#include "ppc_context.h"
#include "guest_read_batch.h"
#include <rex/logging.h>
#include <rex/cvar.h>
#include <rex/kernel/kernel_state.h>
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <cstring>
REXCVAR_DEFINE_BOOL(em1_accelerate_registered_collision,true,"CPU","Validate original MOPP/native triangle parity before accelerating unscaled registered static collision");
REXCVAR_DEFINE_BOOL(em1_validate_collision_activation,false,"Debug","Private native detach/re-admit checks during registered collision loading; no simulation/input between toggles");
REXCVAR_DECLARE(bool,em2_private_profile);

namespace em2::em1 {
namespace {
struct Preserve {PPCContext& ctx;PPCContext saved;DWORD error;unsigned csr;
 explicit Preserve(PPCContext& c):ctx(c),saved(c),error(GetLastError()),csr(_mm_getcsr()){}
 ~Preserve(){ctx=saved;SetLastError(error);_mm_setcsr(csr);}};
std::map<uint32_t,std::unique_ptr<NativeCollisionAttachment>> live;
std::map<uint32_t,bool> activation_pending;
bool Readable(uint8_t* base,uint32_t at,size_t size){return at>=4096 && size && uint64_t(at)+size<=0x100000000ull && em2::GuestReadBatch::Readable(base+at,size);}
uint32_t Word(uint8_t* base,uint32_t at){return Readable(base,at,4)?PPC_LOAD_U32(at):0;}
bool StaticClass(uint8_t* base,uint32_t component){
 const auto name=Word(base,component+56);constexpr char expected[]="Physics - Static Rigid Body";
 return Readable(base,name,sizeof(expected)) && !std::memcmp(base+name,expected,sizeof(expected));
}
bool InitialNativeAdmission(PPCContext& ctx,uint8_t* base,uint32_t component,bool& active,std::string& error){
 Preserve preserve(ctx);
 const auto complete=component-12,owner=Word(base,complete+60);
 if(!owner || owner>0xFFFFFFF7 || !Readable(base,owner+8,4) || ctx.r1.u32<32768 ||
    !Readable(base,ctx.r1.u32-32768,32768)){error="Native static collision owner/stack unavailable";return false;}
 const auto face=owner+8,table=Word(base,face);
 if(!Readable(base,table,356)){error="Native static collision owner interface unavailable";return false;}
 const auto method=Word(base,table+352);
 if(method<0x82200000 || method>=0x83340000 || (method&3) || !Readable(base,method,4)){
  error="Native static collision owner admission method unavailable";return false;
 }
 const auto stack=ctx.r1.u32-4096;PPC_STORE_U32(stack,ctx.r1.u32);ctx.r1.u64=stack;
 // Exact828C3C88 initialization predicate: add the body's physics system
 // only when its owner virtual+352 returns false. Complete-object+92 is
 // Is Painted, NOT an enabled flag; real unpainted floors stay in the world.
 ctx.r3.u64=face;PPC_CALL_INDIRECT_FUNC(method);active=(ctx.r3.u32&255)==0;
 error.clear();return true;
}
bool NativeHullBodies(uint8_t* base,uint32_t component,uint32_t world,size_t& bodies){
 // Retail static component: complete-object +140 owns its hkpPhysicsSystem;
 // the component interface used here is complete-object +12.
 const auto system=Word(base,component+128);
 if(Word(base,system)!=0x8202e78c)return false;
 const auto array=Word(base,system+8),count=Word(base,system+12);
 if(!count || count>256 || !Readable(base,array,count*4))return false;
 for(uint32_t i=0;i<count;++i){const auto body=Word(base,array+i*4);
  if(Word(base,body)!=0x82030384 || Word(base,body+8)!=world)return false;
 }
 bodies+=count;return true;
}
bool ReleaseOne(PPCContext& ctx,uint8_t* base,uint32_t component,std::string& error){
 auto found=live.find(component);if(found==live.end())return true;
 if(!ReleaseNativeAuthoredCollision(ctx,base,*found->second,error))return false;
 live.erase(found);activation_pending.erase(component);return true;
}
}
bool ReleaseRegisteredLevelCollision(PPCContext& ctx,uint8_t* base,std::string& error){
 Preserve preserve(ctx);
 while(!live.empty())if(!ReleaseOne(ctx,base,live.begin()->first,error))return false;
 error.clear();return true;
}
bool ValidateNativeStaticCollisionAcceleration(PPCContext& ctx,uint8_t* base,LevelCollisionPlan& plan,std::string& error,bool allow_fallback){
 Preserve preserve(ctx);std::set<std::string> checked;std::set<const LevelCollisionPlan::NativeHull*> rejected;
 size_t bodies=0,rays=0,resources=0;
 for(const auto& binding:plan.native_bindings){
  const auto& resource=*binding.resource;
  if(!resource.acceleration_proof || !checked.insert(resource.origin).second)continue;
  size_t resource_bodies=0,resource_rays=0;
  const auto verify=[&](){
   if(!base || ctx.r1.u32<32768 || !Readable(base,ctx.r1.u32-32768,32768)){
    error="Native static acceleration needs initialized game-thread stack";return false;
   }
   auto* kernel=rex::kernel::kernel_state();
   if(!kernel || !kernel->memory()){error="Native static acceleration needs initialized native memory";return false;}
   for(const auto& body:resource.acceleration_proof->bodies){
    if(!body.accelerated || body.parts.empty()){error="Native static acceleration has no authored proof geometry";return false;}
    ConvertedJigsawAggregate source;source.pack=body.native_shape_pack;
    source.info.triangle_parts=uint32_t(body.parts.size());source.info.key_bits=body.key_bits;
    source.info.mopp_bytes=uint32_t(body.original_mopp_code.size());
    const size_t budget=std::min(size_t(24),size_t(4096)/body.parts.size());
    for(const auto& part:body.parts){const auto count=std::min(budget,part.triangles.size());
     for(size_t i=0;i<count;++i){const auto& triangle=part.triangles[i*part.triangles.size()/count];JigsawTriangleProof ray;
      for(unsigned k=0;k<3;++k)ray.vertices[k]=part.vertices[triangle[k]];source.info.triangles.push_back(ray);
     }
    }
    const auto size=NativeJigsawProbeScratchSize(source);
    if(!size || size>64*1024*1024){error="Native static acceleration scratch exceeds proof budget";return false;}
    auto& memory=*kernel->memory();const auto scratch=memory.SystemHeapAlloc(uint32_t(size),32);
    if(!scratch || !Readable(base,scratch,size)){if(scratch)memory.SystemHeapFree(scratch);error="Native static acceleration scratch unavailable";return false;}
    std::memset(base+scratch,0,size);NativeJigsawProof proof;
    const bool passed=ProbeNativeJigsawAggregate(ctx,base,scratch,size,source,proof,error,&body);memory.SystemHeapFree(scratch);
    if(!passed){error="Native static MOPP proof failed for '"+body.name+"': "+error;return false;}
    ++resource_bodies;resource_rays+=proof.compared_rays;
   }
   return true;
  };
  if(!verify()){
   if(!allow_fallback || !resource.fallback_geometry)return false;
   rejected.insert(&resource);plan.native_fallbacks.push_back(binding.path+": "+error);error.clear();
  }else{++resources;bodies+=resource_bodies;rays+=resource_rays;}
 }
 if(!rejected.empty()){
  std::erase_if(plan.native_bindings,[&](const auto& binding){return rejected.contains(binding.resource.get());});
  plan.native_components.clear();for(const auto& binding:plan.native_bindings)plan.native_components.insert(binding.component);
 }
 if(bodies)REXLOG_INFO("[em1] native static acceleration admission passed: resources={} bodies={} rays={}; original MOPP retained before world publication",resources,bodies,rays);
 error.clear();return true;
}
void UpdateRegisteredLevelCollisionActivity(PPCContext& ctx,uint8_t* base){
 Preserve preserve(ctx);
 for(auto it=activation_pending.begin();it!=activation_pending.end();){
  const auto component=it->first;const auto active=it->second;const auto owner=live.find(component);
  if(owner==live.end()){it=activation_pending.erase(it);continue;}
  std::string error;
  const bool valid=StaticClass(base,component) && Word(base,component-12)==0x820ca608;
  if(!valid){++it;if(!ReleaseOne(ctx,base,component,error))REXLOG_ERROR("[em1] pending collision owner cleanup: {}",error);continue;}
  if(!SetNativeAuthoredCollisionActive(ctx,base,*owner->second,active,error)){++it;continue;}
  REXLOG_INFO("[em1] registered static collision activation: component={:08X} enabled={} bodies={}",component,active,owner->second->objects.size());
  it=activation_pending.erase(it);
 }
}
void ObserveRegisteredStaticWorldChange(PPCContext& ctx,uint8_t* base,uint32_t complete,uint32_t world,bool active){
 Preserve preserve(ctx);const auto component=complete+12;
 const auto owner=live.find(component);if(owner==live.end())return;
 if(world!=owner->second->world){REXLOG_ERROR("[em1] registered collision world transition refused: component={:08X} retained={:08X} requested={:08X}",component,owner->second->world,world);return;}
 activation_pending[component]=active;UpdateRegisteredLevelCollisionActivity(ctx,base);
}
bool AttachRegisteredLevelCollision(PPCContext& ctx,uint8_t* base,std::string_view state,std::string& error){
 Preserve preserve(ctx);
 auto plan=RegisteredLevelCollision(state);if(!plan){error.clear();return true;}
 em2::GuestReadBatch batch;em2::NativeEntitySnapshot entities;const char* why=nullptr;uint32_t address=0;
 if(!em2::ReadNativeEntitySnapshot(base,entities,why,address)){error=std::string("Collision entity snapshot failed: ")+(why?why:"unknown");return false;}
 const auto world=CurrentNativeCollisionWorld(base);if(!world){error="Native collision world is not ready";return false;}
 size_t added=0,inactive=0,native_bodies=0,native_instances=0,unsupported=0;
 bool activation_tested=false;
 for(const auto& entity:entities.records){
  const auto placement=plan->placements.find(entity.id[0]);if(placement==plan->placements.end())continue;
  const auto components=Word(base,entity.debug_address+36),count=Word(base,entity.debug_address+40);
  if(count>256 || (count && !Readable(base,components,count*4))){error="Invalid collision component vector";return false;}
  for(uint32_t i=0;i<count;++i){const auto face=Word(base,components+i*4);if(face<8)continue;const auto component=face-8;
   if(!StaticClass(base,component) || live.contains(component))continue;
   auto definition=Word(base,component+60);std::shared_ptr<const AuthoredStaticCollision> asset;
   bool native_asset=false;
   std::set<uint32_t> seen;
   while(definition && seen.size()<512 && seen.insert(definition).second){
    if(plan->native_components.contains(Word(base,definition+4))){native_asset=true;break;}
    if(auto found=plan->components.find(Word(base,definition+4));found!=plan->components.end()){asset=found->second;break;}
    definition=Word(base,definition+48);
   }
   if(native_asset){
    if(!NativeHullBodies(base,component,world,native_bodies)){
     error="Converted static hull did not create a native world-owned body";return false;
    }
    if(REXCVAR_GET(em2_private_profile)){
     const auto system=Word(base,component+128),array=Word(base,system+8),number=Word(base,system+12);
     REXLOG_INFO("[em1] stock static physics owner: source_id={:08X} component={:08X} system={:08X} bodies={} painted={} real={}",
         entity.id[0],component,system,number,unsigned(base[component+80]),unsigned(base[component+88]));
     for(uint32_t j=0;j<number;++j){const auto body=Word(base,array+j*4),metadata=Word(base,body+12);
      REXLOG_INFO("[em1] stock static physics body: component={:08X} body={:08X} world={:08X} filter={:08X} metadata={:08X} metadata_owner={:08X}",
          component,body,Word(base,body+8),Word(base,body+44),metadata,Word(base,metadata));
     }
    }
    ++native_instances;continue;
   }
   if(!asset){++unsupported;continue;}
   bool active=false;if(!InitialNativeAdmission(ctx,base,component,active,error))return false;
   auto owner=std::make_unique<NativeCollisionAttachment>();const auto& pose=placement->second;
   AuthoredStaticCollision scaled;const AuthoredStaticCollision* geometry=asset.get();
   if(pose.scale!=1){scaled=*asset;if(!ScaleAuthoredStaticCollision(scaled,pose.scale,error))return false;geometry=&scaled;}
   else if(REXCVAR_GET(em1_accelerate_registered_collision)){
    scaled=*asset;if(!AccelerateAuthoredStaticCollision(scaled,error))return false;geometry=&scaled;
   }
   bool constructed=ConstructNativeAuthoredCollision(ctx,base,*geometry,pose.pose,*owner,error);
   if(!constructed && geometry->accelerated && owner->objects.empty() && !owner->world_reference){
    REXLOG_WARN("[em1] collision acceleration rejected; retaining authored unaccelerated geometry: {}",error);
    error.clear();constructed=ConstructNativeAuthoredCollision(ctx,base,*asset,pose.pose,*owner,error);
   }
   if(!constructed || !AdmitNativeAuthoredCollision(ctx,base,world,*owner,error)){
    std::string cleanup;if(!ReleaseNativeAuthoredCollision(ctx,base,*owner,cleanup))live.emplace(component,std::move(owner));
    return false;
   }
   const auto count=owner->objects.size();
   if(REXCVAR_GET(em2_private_profile)){
    REXLOG_INFO("[em1] registered collision owner: source_id={:08X} handle={} entity={:08X} component={:08X} definition={:08X} enabled={} bodies={}",
      entity.id[0],entity.handle,entity.debug_address,component,definition,active,count);
   }
   if(!active && !SetNativeAuthoredCollisionActive(ctx,base,*owner,false,error)){
    live.emplace(component,std::move(owner));activation_pending[component]=false;return false;
   }
   if(!active)inactive+=count;
   if(!activation_tested && REXCVAR_GET(em1_validate_collision_activation) && REXCVAR_GET(em2_private_profile)){
    if(!SetNativeAuthoredCollisionActive(ctx,base,*owner,!active,error)||!SetNativeAuthoredCollisionActive(ctx,base,*owner,active,error)){
     live.emplace(component,std::move(owner));return false;
    }
    for(const auto& object:owner->objects)if(Word(base,object.body+8)!=(active?world:0)){
     error="Collision activation validation did not restore original membership";live.emplace(component,std::move(owner));return false;
    }
    REXLOG_INFO("[em1] native collision activation roundtrip passed: component={:08X} bodies={} restored_enabled={}; no simulation/input between toggles",component,count,active);activation_tested=true;
   }
   added+=count;live.emplace(component,std::move(owner));
  }
 }
 REXLOG_INFO("[em1] registered level static collision: state='{}' native_bodies={} mesh_bodies={} inactive_mesh_bodies={} mesh_owners={} native_asset_instances={} unsupported_instances={} unsupported_resources={}; authored scoped geometry and placements",state,added-inactive+native_bodies,added,inactive,live.size(),native_instances,unsupported,plan->unsupported.size());
 if(plan->references && !added && plan->native_bindings.empty()){error="No authored static collision could be admitted";return false;}
 error.clear();return true;
}
void ReleaseRegisteredStaticComponent(PPCContext& ctx,uint8_t* base,uint32_t complete_object){
 Preserve preserve(ctx);
 std::string error;if(!ReleaseOne(ctx,base,complete_object+12,error))REXLOG_ERROR("[em1] retained static collision cleanup requires retry: {}",error);
}
}
PPC_EXTERN_FUNC(__imp__sub_828C4B50);
PPC_FUNC_IMPL(sub_828C4B50){
 em2::em1::ReleaseRegisteredStaticComponent(ctx,base,ctx.r3.u32);
 __imp__sub_828C4B50(ctx,base);
}
PPC_EXTERN_FUNC(__imp__sub_827DE8B8);
PPC_FUNC_IMPL(sub_827DE8B8){
 const auto complete=ctx.r3.u32,world=ctx.r4.u32;__imp__sub_827DE8B8(ctx,base);
 em2::em1::ObserveRegisteredStaticWorldChange(ctx,base,complete,world,true);
}
PPC_EXTERN_FUNC(__imp__sub_827DE7E8);
PPC_FUNC_IMPL(sub_827DE7E8){
 const auto complete=ctx.r3.u32,world=ctx.r4.u32;__imp__sub_827DE7E8(ctx,base);
 em2::em1::ObserveRegisteredStaticWorldChange(ctx,base,complete,world,false);
}
