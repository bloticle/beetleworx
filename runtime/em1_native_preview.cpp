#include "em1_native_preview.h"
#include "em1_havok_collision_world.h"
#include "online_runtime.h"
#include "ppc_context.h"
#include <rex/kernel/kernel_state.h>
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <cstring>
#include <bit>
#include <cmath>
#include <set>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>

PPC_EXTERN_FUNC(__imp__sub_827A8C40);
PPC_EXTERN_FUNC(__imp__sub_827A9458);
PPC_EXTERN_FUNC(__imp__sub_827A8F48);
PPC_EXTERN_FUNC(__imp__sub_827A8908);
PPC_EXTERN_FUNC(__imp__sub_82510C20); // stock group insertion used by CreateEntity
PPC_EXTERN_FUNC(__imp__sub_82742F70); // stock complete entity admission
PPC_EXTERN_FUNC(__imp__sub_827403B0); // create/query script entity handle
PPC_EXTERN_FUNC(__imp__sub_82744E90); // stock deferred destruction request
PPC_EXTERN_FUNC(__imp__sub_826547C8); // parser output vector allocator's free
PPC_EXTERN_FUNC(__imp__sub_82533EB0); // original fresh runtime GUID generator
PPC_EXTERN_FUNC(__imp__sub_82517E50); // native entity GUID assignment
PPC_EXTERN_FUNC(__imp__sub_82A89930); // original SetRotatorMaxSpeed component lookup
PPC_EXTERN_FUNC(__imp__sub_828DB1C0); // original Rotator max-speed setter

PPC_EXTERN_FUNC(__imp__sub_82761C50);
PPC_EXTERN_FUNC(__imp__sub_828013F0);
PPC_EXTERN_FUNC(__imp__sub_8256C8A8);
namespace em2::em1 {
namespace {
struct ParticleTrace {uint64_t updates=0,updates_enabled=0,commits=0,commits_enabled=0,finalizations=0,removals=0,submissions=0,flagged_submissions=0;};
std::mutex particle_trace_mutex;
std::map<uint32_t,ParticleTrace> particle_traces;
std::atomic<bool> particle_trace_active=false;
constexpr uint32_t kCore=0x833da7e4,kGraphType=0x833db0ec,kStack=32768;
constexpr uint32_t kVector=512,kName=544,kDependencies=576,kHandle=608,kGuid=640,kInput=1024;
bool Mapped(uint8_t* base,uint32_t address,size_t bytes,bool write=false) {
  if(!base || address<4096 || !bytes || uint64_t(address)+bytes>0x100000000ull)return false;
  auto p=reinterpret_cast<uintptr_t>(base+address),end=p+bytes;
  while(p<end) {
    MEMORY_BASIC_INFORMATION m{};
    if(!VirtualQuery(reinterpret_cast<void*>(p),&m,sizeof(m)) || m.State!=MEM_COMMIT || (m.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
    auto access=m.Protect&255;bool writable=access==PAGE_READWRITE || access==PAGE_WRITECOPY || access==PAGE_EXECUTE_READWRITE || access==PAGE_EXECUTE_WRITECOPY;
    if(write?!writable:!(writable || access==PAGE_READONLY || access==PAGE_EXECUTE_READ))return false;
    auto next=reinterpret_cast<uintptr_t>(m.BaseAddress)+m.RegionSize;if(next<=p)return false;p=std::min(end,next);
  }return true;
}
struct Restore {
  PPCContext& c;PPCContext saved;unsigned csr;DWORD error;
  explicit Restore(PPCContext& ctx):c(ctx),saved(ctx),csr(_mm_getcsr()),error(GetLastError()){}
  ~Restore(){c=saved;_mm_setcsr(csr);SetLastError(error);}
};
bool Stack(PPCContext& ctx,uint8_t* base,std::string& error) {
  if(ctx.r1.u32<kStack || !Mapped(base,ctx.r1.u32-kStack,kStack,true)){error="Native preview lacks writable guest stack";return false;}
  ctx.r1.u64-=2048;return true;
}
uint32_t Method(uint8_t* base,uint32_t object,uint32_t slot) {
  if(!Mapped(base,object,4))return 0;auto table=PPC_LOAD_U32(object);
  if(!Mapped(base,table+slot,4))return 0;auto f=PPC_LOAD_U32(table+slot);
  return f>=0x822f0000 && f<0x83350000?f:0;
}
void Call(PPCContext& ctx,uint8_t* base,uint32_t object,uint32_t function) {
  ctx.r3.u64=object;PPC_CALL_INDIRECT_FUNC(function);
}
}
bool CreateNativeArchitectureEntity(PPCContext& ctx,uint8_t* base,const ArchitectureScene& scene,
    const NativeModelResource& model,NativeArchitectureEntity& output,std::string& error,bool instantiate_only,NativePrototype prototype) {
  Restore restore(ctx);auto* kernel=rex::kernel::kernel_state();
  const auto expected_components=instantiate_only?(prototype==NativePrototype::Gear?8u:4u):2u;
  if(output.scratch || !kernel || !kernel->memory() || em2::net::SynchronizedSessionActive() ||
      scene.scene.entity_count!=1 || scene.scene.component_count!=expected_components || scene.scene.bytes.size()>4*1024*1024 ||
      !model.record || !Mapped(base,kCore,4) || !Mapped(base,kGraphType,1)) {
    error="Native architecture preview prerequisites are not satisfied";return false;
  }
  const auto core=PPC_LOAD_U32(kCore);
  if(!Mapped(base,core,228) || PPC_LOAD_U32(core+216)!=0 || PPC_LOAD_U32(core+220)!=model.manager ||
      !Mapped(base,PPC_LOAD_U32(core+152),124)) {
    error="Native preview requires an idle loaded world and its live pack manager";return false;
  }
  if(!Stack(ctx,base,error))return false;
  const auto size=kInput+uint32_t(scene.scene.bytes.size());
  const auto allocation=kernel->memory()->SystemHeapAlloc(size,32);
  if(!allocation || !Mapped(base,allocation,size,true)) {error="Native preview scratch allocation failed";return false;}
  output.scratch=allocation;output.scratch_size=size;output.core=core;
  std::memset(base+allocation,0,size);std::memcpy(base+allocation+kInput,scene.scene.bytes.data(),scene.scene.bytes.size());
  ctx.r3.u64=allocation;__imp__sub_827A8C40(ctx,base);output.parser_constructed=true;
  ctx.r3.u64=allocation;ctx.r4.u64=allocation+kInput;ctx.r5.u64=scene.scene.bytes.size();
  ctx.r6.u64=allocation+kVector;ctx.r7.u64=0;ctx.r8.u64=0;ctx.r9.u64=allocation+kName;ctx.r10.u64=allocation+kDependencies;
  __imp__sub_827A9458(ctx,base);
  if(ctx.r3.u32!=1){error="Native architecture scene begin failed";return false;}
  ctx.r3.u64=allocation;ctx.r4.u64=allocation+kVector;ctx.r5.u64=0;
  __imp__sub_827A8F48(ctx,base);
  if(ctx.r3.u32!=1 || PPC_LOAD_U8(allocation+124) || PPC_LOAD_U32(allocation+kVector+4)!=1) {
    error="Native architecture scene did not create exactly one entity";return false;
  }
  const auto list=PPC_LOAD_U32(allocation+kVector);
  if(!Mapped(base,list,4)){error="Native architecture output vector is invalid";return false;}
  const auto entity=PPC_LOAD_U32(list),face=entity+8;
  if(!Mapped(base,entity,104) || !Method(base,face,0) || !Method(base,face,4)) {error="Native architecture entity is invalid";return false;}
  output.entity=entity;
  Call(ctx,base,face,Method(base,face,0));output.entity_reference=true;
  if(instantiate_only){
    if(prototype==NativePrototype::Gear || prototype==NativePrototype::Particle){
      const auto count=PPC_LOAD_U32(entity+48),list=PPC_LOAD_U32(entity+44);
      if(count!=expected_components || !Mapped(base,list,expected_components*4)){error="Native prototype did not retain its authored components";return false;}
      for(unsigned i=0;i<count;++i){
        const auto component=PPC_LOAD_U32(list+i*4);
        if(component<8 || !Mapped(base,component-8,16) || !Method(base,component,0)){
          error="Native prototype component interface unavailable";return false;
        }
        REXLOG_INFO("[em1] prototype component kind={} index={} base={:08X} vtable={:08X} interface={:08X}",prototype==NativePrototype::Gear?"gear":"particle",i,component-8,PPC_LOAD_U32(component-8),PPC_LOAD_U32(component));
      }
      error.clear();return true;
    }
    if(PPC_LOAD_U32(entity+48)!=4){error="Native trigger prototype component count mismatch";return false;}
    const auto list=PPC_LOAD_U32(entity+44);
    if(!Mapped(base,list,16)){error="Native trigger component vector unavailable";return false;}
    for(unsigned i=0;i<4;++i){
      const auto face=PPC_LOAD_U32(list+i*4);
      if(face<8 || !Mapped(base,face-8,16)){error="Native trigger component interface unavailable";return false;}
      REXLOG_INFO("[em1] trigger component index={} base={:08X} vtable={:08X} interface={:08X}",i,face-8,PPC_LOAD_U32(face-8),PPC_LOAD_U32(face));
    }
    const auto phantom=PPC_LOAD_U32(list+4),trigger=PPC_LOAD_U32(list+8);
    const bool native_phantom=PPC_LOAD_U32(phantom-8)==0x8209AEC8;
    if((!native_phantom && PPC_LOAD_U32(phantom-8)!=0x820DC310) || PPC_LOAD_U32(trigger-8)!=0x820DB488){error="Native trigger prototype class mismatch";return false;}
    // Read through the original reflected getters: generic values live in the
    // property bag, while Delay/Number Times have inline native backing.
    const auto property=allocation+672,result=allocation+688;
    auto read=[&](uint32_t component,uint32_t descriptor_global,uint32_t slot,uint32_t index=0){
      if(!Mapped(base,descriptor_global,4))return false;
      const auto descriptor=PPC_LOAD_U32(descriptor_global),method=Method(base,component,slot);
      if(!method || !Mapped(base,descriptor,8))return false;
      PPC_STORE_U32(property,PPC_LOAD_U32(descriptor+4));
      std::memset(base+result,0xCD,16);
      ctx.r3.u64=component;ctx.r4.u64=property;ctx.r5.u64=result;ctx.r6.u64=index;
      PPC_CALL_INDIRECT_FUNC(method);return ctx.r3.u32!=0;
    };
    const bool size_ok=read(phantom,native_phantom?0x833DC248:0x833DF5C8,180) && PPC_LOAD_U32(result)==0x3F0DCB58 && PPC_LOAD_U32(result+4)==0x4098767F && PPC_LOAD_U32(result+8)==0x41157736;
    const bool collide_ok=native_phantom ? (read(phantom,0x833DC24C,200) && PPC_LOAD_U32(result)==12 && read(phantom,0x833DC258,208) && PPC_LOAD_U8(result)==1) : (read(phantom,0x833DF5CC,208) && PPC_LOAD_U8(result)==0);
    const bool layers_ok=read(trigger,0x833DF540,200) && PPC_LOAD_U32(result)==3 && !read(trigger,0x833DF540,200,1);
    const bool disabled_ok=read(trigger,0x833DF544,208) && PPC_LOAD_U8(result)==0;
    const bool delay_ok=read(trigger,0x833DF548,212) && PPC_LOAD_U32(result)==0;
    const bool count_ok=read(trigger,0x833DF54C,200) && PPC_LOAD_U32(result)==1;
    REXLOG_INFO("[em1] trigger reflected fields size={} collision_properties={} layers={} disabled={} delay={} count={} native_phantom={}",size_ok,collide_ok,layers_ok,disabled_ok,delay_ok,count_ok,native_phantom);
    if(!(size_ok && collide_ok && layers_ok && disabled_ok && delay_ok && count_ok)){error="Native trigger reflected fields differ from original source";return false;}
    const auto types=PPC_LOAD_U32(entity+96);const auto kind=PPC_LOAD_U8(0x833DC244);
    if(!Mapped(base,types+kind,1)){error="Native phantom type map unavailable";return false;}
    const auto slot=PPC_LOAD_U8(types+kind);
    const bool resolved=slot<4 && PPC_LOAD_U32(list+uint32_t(slot)*4)==phantom;
    REXLOG_INFO("[em1] trigger physics-phantom dependency resolved={} slot={}",resolved,slot);
    if(native_phantom && !resolved){error="Native trigger phantom dependency unresolved";return false;}
    error.clear();return true;
  }
  const auto types=PPC_LOAD_U32(entity+96);
  const auto kind=PPC_LOAD_U8(kGraphType);
  if(!Mapped(base,types+kind,1)){error="Native scenegraph type map is missing";return false;}
  const auto slot=PPC_LOAD_U8(types+kind);
  const auto components=PPC_LOAD_U32(entity+44);
  if(slot==255 || !Mapped(base,components+uint32_t(slot)*4,4)){error="Native scenegraph component is missing";return false;}
  const auto component=PPC_LOAD_U32(components+uint32_t(slot)*4);
  if(component<8 || !Mapped(base,component-8,152)){error="Native scenegraph component is invalid";return false;}
  output.scenegraph=component-8;
  // Parsed placements initially use the authored source GUID. A live preview
  // must not collide with an EM2 placement: mirror the dynamic CreateEntity
  // path's fresh GUID generation and native assignment before world insertion.
  // The immutable original identifier remains in the retained scene blueprint.
  ctx.r3.u64=allocation+kGuid;__imp__sub_82533EB0(ctx,base);
  ctx.r3.u64=face;ctx.r4.u64=allocation+kGuid;__imp__sub_82517E50(ctx,base);
  // This is the same current-world insertion/admission pair used by the
  // original CreateEntity implementation82743AD8. Admission calls native
  // resource loading, component activation, render registration and completion.
  ctx.r3.u64=PPC_LOAD_U32(core+152);ctx.r4.u64=face;__imp__sub_82510C20(ctx,base);
  output.admitted=true;
  if(!PPC_LOAD_U32(entity+88)){error="Native architecture entity did not join its world group";return false;}
  ctx.r3.u64=core;ctx.r4.u64=face;ctx.r5.u64=0;__imp__sub_82742F70(ctx,base);
  ctx.r3.u64=allocation+kHandle;ctx.r4.u64=core;ctx.r5.u64=face;__imp__sub_827403B0(ctx,base);
  output.handle=PPC_LOAD_U32(allocation+kHandle);
  const auto root=PPC_LOAD_U32(output.scenegraph+88);
  if(!output.handle || !Mapped(base,root,32)){error="Native architecture activation did not retain a scene root";return false;}
  error.clear();return true;
}
bool ActivateNativeTrigger(PPCContext& ctx,uint8_t* base,NativeArchitectureEntity& state,std::string& error){
  Restore restore(ctx);error.clear();
  if(!state.entity_reference || state.admitted || !Mapped(base,state.entity,104) || !Mapped(base,state.core,228) || !Stack(ctx,base,error)){error="Native trigger admission prerequisites unavailable";return false;}
  const auto face=state.entity+8;
  ctx.r3.u64=state.scratch+kGuid;__imp__sub_82533EB0(ctx,base);
  ctx.r3.u64=face;ctx.r4.u64=state.scratch+kGuid;__imp__sub_82517E50(ctx,base);
  ctx.r3.u64=PPC_LOAD_U32(state.core+152);ctx.r4.u64=face;__imp__sub_82510C20(ctx,base);
  state.admitted=true;
  if(!PPC_LOAD_U32(state.entity+88)){error="Native trigger did not join world group";return false;}
  ctx.r3.u64=state.core;ctx.r4.u64=face;ctx.r5.u64=0;__imp__sub_82742F70(ctx,base);
  ctx.r3.u64=state.scratch+kHandle;ctx.r4.u64=state.core;ctx.r5.u64=face;__imp__sub_827403B0(ctx,base);
  state.handle=PPC_LOAD_U32(state.scratch+kHandle);
  return state.handle!=0;
}
void ResetNativeParticleTrace(){
  particle_trace_active.store(false,std::memory_order_release);
  std::lock_guard lock(particle_trace_mutex);particle_traces.clear();
}
void TraceNativeParticleUpdate(PPCContext& ctx,uint8_t* base,unsigned stage){
  if(!particle_trace_active.load(std::memory_order_acquire))return;
  Restore restore(ctx);
  try{
    std::lock_guard lock(particle_trace_mutex);
    const auto object=stage==4?ctx.r4.u32:ctx.r3.u32;
    auto found=particle_traces.find(object);if(found==particle_traces.end())return;
    auto& trace=found->second;
    if(stage==4){
      ++trace.submissions;
      if(Mapped(base,object,0x2DD) && (PPC_LOAD_U8(object+0x2DC)&4))++trace.flagged_submissions;
      return;
    }
    if(stage==2){++trace.finalizations;return;}
    if(stage==3){++trace.removals;return;}
    const auto context=ctx.r4.u32;if(!Mapped(base,context,0x16))return;
    const bool enabled=PPC_LOAD_U8(context+0x15)!=0;
    if(stage==0){++trace.updates;if(enabled)++trace.updates_enabled;}
    else {++trace.commits;if(enabled)++trace.commits_enabled;}
  }catch(...){}
}
bool ObserveNativeParticleGraph(PPCContext& ctx,uint8_t* base,const NativeArchitectureEntity& state,bool running){
  Restore restore(ctx);std::string error;
  if(!state.admitted || state.destroy_requested || !Mapped(base,state.scenegraph,152) || !Stack(ctx,base,error))return false;
  std::vector<uint32_t> pending{PPC_LOAD_U32(state.scenegraph+88)};std::set<uint32_t> seen;
  auto cast=[&](uint32_t object,uint32_t kind){ctx.r3.u64=object;ctx.r4.u64=kind;__imp__sub_8256C8A8(ctx,base);return ctx.r3.u32!=0;};
  while(!pending.empty()){
    const auto object=pending.back();pending.pop_back();if(!object || seen.contains(object))continue;
    if(seen.size()>=128 || !Mapped(base,object,16))return false;seen.insert(object);
    const auto method=Method(base,object,0xC);if(!method)return false;
    Call(ctx,base,object,method);const auto type=ctx.r3.u32;if(!Mapped(base,type,16))return false;
    const auto rtti=PPC_LOAD_U32(type+8);
    const auto name_pointer=Mapped(base,rtti,4)?PPC_LOAD_U32(rtti):0;std::string name;
    if(Mapped(base,name_pointer,64)){
      for(unsigned i=0;i<64 && PPC_LOAD_U8(name_pointer+i);++i){const auto c=PPC_LOAD_U8(name_pointer+i);if(c<32 || c>126){name.clear();break;}name.push_back(char(c));}
    }
    const bool particle=cast(object,0x833FA8D0);
    if(particle){
      if(!Mapped(base,object,0x244))return false;
      // Native removal830BA460 decrements this count; cloning830BCFF0 uses
      // it to copy per-particle arrays. Constructor830BE14C sets capacity.
      std::string label;const auto label_pointer=PPC_LOAD_U32(object+0xC);
      if(Mapped(base,label_pointer,128))for(unsigned i=0;i<128 && PPC_LOAD_U8(label_pointer+i);++i){
        const auto c=PPC_LOAD_U8(label_pointer+i);if(c<32 || c>126){label.clear();break;}label.push_back(char(c));
      }
      // NiObjectNET uses eight-byte {extra-data, interned-name} entries.
      // NiColorExtraData loader825D94F8 reads RGBA directly at object+10.
      const auto extra_count=PPC_LOAD_U16(object+0x18),extra_capacity=PPC_LOAD_U16(object+0x1A);
      const auto extras=PPC_LOAD_U32(object+0x14);
      if(extra_count>extra_capacity || extra_count>64 || (extra_count && !Mapped(base,extras,size_t(extra_count)*8)))return false;
      for(unsigned ei=0;ei<extra_count;++ei){
        const auto extra=PPC_LOAD_U32(extras+ei*8);
        if(!Mapped(base,extra,16))return false;
        if(PPC_LOAD_U32(extra)!=0x82048B00)continue;
        if(!Mapped(base,extra,32))return false;
        const auto name_pointer=PPC_LOAD_U32(extra+0xC);std::string parameter;
        if(!Mapped(base,name_pointer,128))return false;
        for(unsigned j=0;j<128 && PPC_LOAD_U8(name_pointer+j);++j){
          const auto ch=PPC_LOAD_U8(name_pointer+j);if(ch<32 || ch>126)return false;parameter.push_back(char(ch));
        }
        std::array<float,4> rgba;
        for(unsigned j=0;j<4;++j){rgba[j]=std::bit_cast<float>(PPC_LOAD_U32(extra+0x10+j*4));if(!std::isfinite(rgba[j]))return false;}
        REXLOG_INFO("[em1] particle colour metadata handle={} object={:08X} parameter='{}' rgba={},{},{},{}; GPU binding unverified",
          state.handle,object,parameter,rgba[0],rgba[1],rgba[2],rgba[3]);
      }
      const auto material_count=PPC_LOAD_U32(object+0x100),materials=PPC_LOAD_U32(object+0xF8);
      if(material_count>4 || (material_count && !Mapped(base,materials,size_t(material_count)*0x2C)))return false;
      for(uint32_t mi=0;mi<material_count;++mi){
        const auto material=PPC_LOAD_U32(materials+mi*0x2C+4);
        if(!Mapped(base,material,16))return false;
        const auto material_name=PPC_LOAD_U32(material+0xC);std::string technique;
        if(Mapped(base,material_name,128))for(unsigned j=0;j<128 && PPC_LOAD_U8(material_name+j);++j){
          const auto ch=PPC_LOAD_U8(material_name+j);if(ch<32 || ch>126){technique.clear();break;}technique.push_back(char(ch));
        }
        REXLOG_INFO("[em1] particle material binding handle={} object={:08X} slot={} material={:08X} class={:08X} technique='{}' selected={}",
          state.handle,object,mi,material,PPC_LOAD_U32(material),technique,PPC_LOAD_U32(object+0x104)==mi);
      }
      REXLOG_INFO("[em1] particle material slots handle={} object={:08X} count={}",state.handle,object,material_count);
      const auto capacity=PPC_LOAD_U32(object+0x23C),active=PPC_LOAD_U32(object+0x240);
      const bool valid=active<=capacity && capacity<=1048576;
      if(!Mapped(base,object,0x30E))return false;
      ParticleTrace trace;
      {std::lock_guard lock(particle_trace_mutex);
        if(!particle_traces.contains(object) && particle_traces.size()>=128)return false;
        trace=particle_traces[object];}
      particle_trace_active.store(true,std::memory_order_release);
      REXLOG_INFO("[em1] particle update trace handle={} object={:08X} updates={} enabled_updates={} commits={} enabled_commits={} finalizations={} removals={} submissions={} flagged_submissions={} simulator_flags={} simulation_time={} pending={} initialized={}",
        state.handle,object,trace.updates,trace.updates_enabled,trace.commits,trace.commits_enabled,trace.finalizations,trace.removals,trace.submissions,trace.flagged_submissions,PPC_LOAD_U8(object+0x2DC),
        std::bit_cast<float>(PPC_LOAD_U32(object+0x238)),PPC_LOAD_U8(object+0x305),PPC_LOAD_U8(object+0x30C));
      REXLOG_INFO("[em1] particle population handle={} running={} object={:08X} class='{}' label='{}' active={} capacity={} valid={}",state.handle,running,object,name,label,active,capacity,valid);
      if(!valid)return false;
    }
    // Exact child layouts used by retail recursive controller traversal82800398.
    if(cast(object,0x833CDE7C)){
      if(!Mapped(base,object,0xDC))return false;
      auto link=PPC_LOAD_U32(object+0xD8);std::set<uint32_t> links;
      while(link){if(links.size()>=128 || !links.insert(link).second || !Mapped(base,link,12))return false;
        pending.push_back(PPC_LOAD_U32(link+8));link=PPC_LOAD_U32(link);}
    }
    if(cast(object,0x833CDFC8)){
      if(!Mapped(base,object,0xFC))return false;
      const auto count=PPC_LOAD_U16(object+0xFA);const auto children=PPC_LOAD_U32(object+0xF4);
      if(count>128 || (count && !Mapped(base,children,size_t(count)*4)))return false;
      for(unsigned i=0;i<count;++i)pending.push_back(PPC_LOAD_U32(children+i*4));
    }
    if(pending.size()>256)return false;
  }
  REXLOG_INFO("[em1] particle graph observation handle={} running={} nodes={}",state.handle,running,seen.size());return !seen.empty();
}
bool SetNativeParticleEmission(PPCContext& ctx,uint8_t* base,const NativeArchitectureEntity& state,bool enabled,std::string& error){
  Restore restore(ctx);error.clear();
  if(!state.admitted || state.destroy_requested || !Mapped(base,state.entity,104) || !Stack(ctx,base,error)){
    error="Particle emitter requires an owned admitted actor";return false;
  }
  ctx.r3.u64=state.entity+8;__imp__sub_82761C50(ctx,base);
  const auto controller=ctx.r3.u32;
  if(!Mapped(base,controller,0x54) || PPC_LOAD_U32(controller)!=0x8209BB90 || !Mapped(base,0x833DC2C4,4)){
    error="Particle controller or property registry unavailable";return false;
  }
  const auto descriptor=PPC_LOAD_U32(0x833DC2C4),face=controller+8;
  const auto setter=Method(base,face,0x110),getter=Method(base,face,0xD0);
  if(!Mapped(base,descriptor,8) || !setter || !getter){error="Particle enabled property unavailable";return false;}
  // Exact controller/property sequence from retail StartEmitters/StopEmitters;
  // the outer script wrappers expect script arguments, not entity pointers.
  ctx.r3.u64=controller;ctx.r4.u64=enabled?1:0;ctx.r5.u64=0;__imp__sub_828013F0(ctx,base);
  ctx.r4.u64=descriptor+4;ctx.r5.u64=enabled?1:0;ctx.r6.u64=0;Call(ctx,base,face,setter);
  const auto result=state.scratch+672;
  PPC_STORE_U8(result,0xFF);
  ctx.r4.u64=descriptor+4;ctx.r5.u64=result;ctx.r6.u64=0;Call(ctx,base,face,getter);
  const bool matches=PPC_LOAD_U8(result)==(enabled?1:0);
  REXLOG_INFO("[em1] native particle emitter handle={} enabled={} property_matches={}",state.handle,enabled,matches);
  if(!matches)error="Particle enabled property did not match requested state";
  return matches;
}
bool ActivateNativeParticle(PPCContext& ctx,uint8_t* base,NativeArchitectureEntity& state,std::string& error){
  if(!Mapped(base,state.entity,104) || PPC_LOAD_U32(state.entity+48)!=4){error="Particle activation requires four components";return false;}
  const auto list=PPC_LOAD_U32(state.entity+44);
  if(!Mapped(base,list,16)){error="Particle component list unavailable";return false;}
  constexpr uint32_t classes[]={0x820904D0,0x82084A68,0x82075F98,0x8209BB90};
  for(unsigned i=0;i<4;++i){
    const auto face=PPC_LOAD_U32(list+i*4);
    if(face<8 || !Mapped(base,face-8,16) || PPC_LOAD_U32(face-8)!=classes[i]){error="Particle native class mismatch";return false;}
  }
  state.scenegraph=PPC_LOAD_U32(list+4)-8;
  if(!Mapped(base,state.scenegraph,152)){error="Particle scenegraph unavailable";return false;}
  if(!ActivateNativeTrigger(ctx,base,state,error))return false;
  const auto root=PPC_LOAD_U32(state.scenegraph+88);
  if(!Mapped(base,root,32)){error="Particle activation has no native scene root";return false;}
  REXLOG_INFO("[em1] native particle activated handle={} root={:08X}",state.handle,root);
  return true;
}
bool ActivateNativeGear(PPCContext& ctx,uint8_t* base,NativeArchitectureEntity& state,std::string& error){
  if(!Mapped(base,state.entity,104) || PPC_LOAD_U32(state.entity+48)!=8){error="Gear activation requires all eight components";return false;}
  // The fresh GUID/group insertion/complete admission sequence is common to
  // every original dynamic entity; no gear-specific lifecycle is bypassed.
  return ActivateNativeTrigger(ctx,base,state,error);
}
bool ObserveNativeGear(uint8_t* base,const NativeArchitectureEntity& state,uint32_t* body_address){
  if(body_address)*body_address=0;
  if(!state.admitted || !Mapped(base,state.entity,104))return false;
  const auto list=PPC_LOAD_U32(state.entity+44);
  if(PPC_LOAD_U32(state.entity+48)!=8 || !Mapped(base,list,32))return false;
  const auto graph=PPC_LOAD_U32(list+4),dynamic=PPC_LOAD_U32(list+12);
  if(graph<8 || !Mapped(base,graph-8,152) || !Mapped(base,dynamic,0x80))return false;
  const auto scene_root=PPC_LOAD_U32(graph-8+88);
  // Native dynamic startup82812468 and base startup827E1098 access the
  // PhysicsSystem at interface+78, then its rigid-body array at+8/count+c.
  const auto system=PPC_LOAD_U32(dynamic+0x78);
  if(!Mapped(base,scene_root,32) || !Mapped(base,system,0x44)){
    REXLOG_ERROR("[em1] gear activation missing native root={:08X} physics_system={:08X}",scene_root,system);return false;
  }
  const auto bodies=PPC_LOAD_U32(system+8),count=PPC_LOAD_U32(system+12);
  if(count!=1 || !Mapped(base,bodies,4))return false;
  const auto body=PPC_LOAD_U32(bodies);
  if(!Mapped(base,body,0x220))return false;
  const auto world=PPC_LOAD_U32(body+8),expected=CurrentNativeCollisionWorld(base);
  if(body_address)*body_address=body;
  REXLOG_INFO("[em1] native gear activation handle={} scene_root={:08X} physics_system={:08X} body={:08X} world={:08X} expected_world={:08X} motion={}",state.handle,scene_root,system,body,world,expected,PPC_LOAD_U8(body+0xe8));
  return state.handle && world && world==expected;
}
bool SetNativeGearRotationSpeed(PPCContext& ctx,uint8_t* base,const NativeArchitectureEntity& state,float speed,std::string& error){
  Restore restore(ctx);error.clear();
  if(!state.admitted || !std::isfinite(speed) || !Mapped(base,state.entity,104) || !Stack(ctx,base,error)){
    error="Gear rotation requires an admitted native entity and finite speed";return false;
  }
  // Same lookup/setter pair as the retail Lua export831F1830. The original
  // property is degrees/second; native Rotator owns integration and movement.
  ctx.r3.u64=state.entity+8;__imp__sub_82A89930(ctx,base);const auto rotator=ctx.r3.u32;
  if(!Mapped(base,rotator,0x54) || !Mapped(base,state.scratch+672,4,true)){error="Native Rotator unavailable";return false;}
  PPC_STORE_U32(state.scratch+672,std::bit_cast<uint32_t>(speed));
  ctx.r3.u64=rotator;ctx.r4.u64=state.scratch+672;__imp__sub_828DB1C0(ctx,base);
  if(PPC_LOAD_U32(rotator+0x50)!=std::bit_cast<uint32_t>(speed)){error="Native Rotator did not retain requested speed";return false;}
  return true;
}
bool ReadNativeGearRotations(uint8_t* base,const NativeArchitectureEntity& state,std::array<float,18>& output){
  uint32_t body=0;if(!ObserveNativeGear(base,state,&body))return false;
  const auto components=PPC_LOAD_U32(state.entity+44),graph=PPC_LOAD_U32(components+4);
  const auto root=PPC_LOAD_U32(graph-8+88);
  if(!Mapped(base,root+0x90,36))return false;
  for(unsigned row=0;row<3;++row)for(unsigned col=0;col<3;++col){
    const unsigned i=row*3+col;
    output[i]=std::bit_cast<float>(uint32_t(PPC_LOAD_U32(body+0xf0+row*16+col*4)));
    output[9+i]=std::bit_cast<float>(uint32_t(PPC_LOAD_U32(root+0x90+i*4)));
    if(!std::isfinite(output[i]) || !std::isfinite(output[9+i]))return false;
  }
  return true;
}
bool ObserveNativeTrigger(uint8_t* base,const NativeArchitectureEntity& state){
  if(!state.admitted || !Mapped(base,state.entity,104))return false;
  const auto list=PPC_LOAD_U32(state.entity+44);if(!Mapped(base,list,16))return false;
  const auto phantom=PPC_LOAD_U32(list+4),trigger=PPC_LOAD_U32(list+8);
  if(phantom<8 || trigger<8 || !Mapped(base,phantom-8,108) || !Mapped(base,trigger-8,152))return false;
  // PhysicsPhantom::GetPhantom (827FFC70) returns raw+72; trigger startup
  // retains that same Havok object at raw+80 (8291C834).
  const auto physics=PPC_LOAD_U32(phantom-8+72),bound=PPC_LOAD_U32(trigger-8+80);
  const bool linked=physics && physics==bound && Mapped(base,physics,208);
  REXLOG_INFO("[em1] native trigger activation handle={} physics={:08X} bound={:08X} linked={}",state.handle,physics,bound,linked);
  if(!linked)return false;
  // 82486600 builds hkpAabbPhantom, storing the native min/max at160/176.
  // 827FE328 constructs those bounds as world translation +/- rotated size/2.
  if(PPC_LOAD_U32(physics)!=0x8202E7DC){REXLOG_ERROR("[em1] trigger expected native AABB phantom");return false;}
  const auto world=PPC_LOAD_U32(physics+8),expected_world=CurrentNativeCollisionWorld(base);
  const bool world_ok=world && world==expected_world;
  constexpr uint32_t position_bits[]={0x42574DA6,0x41F8A4A8,0x40F9C512};
  constexpr uint32_t size_bits[]={0x3F0DCB58,0x4098767F,0x41157736};
  bool bounds_ok=true;
  for(unsigned axis=0;axis<3;++axis){
    const auto position=std::bit_cast<float>(position_bits[axis]),half=std::bit_cast<float>(size_bits[axis])*0.5f;
    const auto low=std::bit_cast<float>(PPC_LOAD_U32(physics+160+axis*4)),high=std::bit_cast<float>(PPC_LOAD_U32(physics+176+axis*4));
    const bool matches=std::isfinite(low) && std::isfinite(high) && std::abs(low-(position-half))<0.0001f && std::abs(high-(position+half))<0.0001f;
    bounds_ok&=matches;
    REXLOG_INFO("[em1] native trigger bounds axis={} min={} max={} expected_min={} expected_max={} matches={}",axis,low,high,position-half,position+half,matches);
  }
  REXLOG_INFO("[em1] native trigger geometry bounds={} active_world={} world={:08X} expected_world={:08X}",bounds_ok,world_ok,world,expected_world);
  return bounds_ok && world_ok;
}
bool DestroyNativeArchitectureEntity(PPCContext& ctx,uint8_t* base,NativeArchitectureEntity& state,
    bool& complete,std::string& error) {
  Restore restore(ctx);complete=false;error.clear();
  if(!state.scratch){complete=true;return true;}
  auto* kernel=rex::kernel::kernel_state();
  if(!kernel || !kernel->memory() || !Stack(ctx,base,error)){if(error.empty())error="Native memory unavailable";return false;}
  if(state.entity_reference) {
    if(!Mapped(base,state.entity,104)){error="Owned native entity became inaccessible";return false;}
    if(state.admitted && PPC_LOAD_U32(state.entity+88)) {
      if(!state.destroy_requested) {
        if(!Mapped(base,state.core,228)){error="Native world owner is unavailable for destruction";return false;}
        ctx.r3.u64=state.core;ctx.r4.u64=state.entity+8;__imp__sub_82744E90(ctx,base);state.destroy_requested=true;
      }
      // Keep parser/model alive while stock world deletion is pending.
      if(PPC_LOAD_U32(state.entity+88))return true;
    }
    const auto release=Method(base,state.entity+8,4);
    if(!release){error="Native entity release method is unavailable";return false;}
    Call(ctx,base,state.entity+8,release);state.entity_reference=false;
  }
  if(state.parser_constructed){ctx.r3.u64=state.scratch;__imp__sub_827A8908(ctx,base);state.parser_constructed=false;}
  const auto vector=PPC_LOAD_U32(state.scratch+kVector);
  if(vector){ctx.r3.u64=vector;__imp__sub_826547C8(ctx,base);}
  kernel->memory()->SystemHeapFree(state.scratch);state={};complete=true;return true;
}
}
