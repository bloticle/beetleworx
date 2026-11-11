#include "em1_native_globals.h"
#include "em1_preview_runtime.h"
#include "em1_script_source.h"
#include "em1_script_events.h"
#include "em1_lua_core/include/em1_lua_core.h"
#include "ppc_context.h"
#include <rex/kernel/kernel_state.h>
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <array>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <set>
#include <algorithm>
#include "em1_single_gsa.h"
PPC_EXTERN_FUNC(__imp__sub_831BC098);
PPC_EXTERN_FUNC(__imp__sub_8288C7B8);
PPC_EXTERN_FUNC(__imp__sub_82828BB8);
PPC_EXTERN_FUNC(__imp__sub_82828010);
PPC_EXTERN_FUNC(__imp__sub_828280B8);
PPC_EXTERN_FUNC(__imp__sub_828288F0);
PPC_EXTERN_FUNC(__imp__sub_82829328);
PPC_EXTERN_FUNC(__imp__sub_82827EE0);
PPC_EXTERN_FUNC(__imp__sub_82828C28);
PPC_EXTERN_FUNC(__imp__sub_8276A018);
PPC_EXTERN_FUNC(__imp__sub_8276BC38);
PPC_EXTERN_FUNC(__imp__sub_82A0C740);
PPC_EXTERN_FUNC(__imp__sub_82A0C2D0);
namespace em2::em1 {
static_assert(sizeof(Em1EngineCallbacks)==152,"Original native engine callback ABI changed");
namespace {
bool Mapped(uint8_t* base,uint32_t p,size_t n){
  if(!base || p<4096 || uint64_t(p)+n>0x100000000ull)return false;
  MEMORY_BASIC_INFORMATION info{};
  return VirtualQuery(base+p,&info,sizeof(info)) && info.State==MEM_COMMIT && !(info.Protect&(PAGE_NOACCESS|PAGE_GUARD)) &&
    reinterpret_cast<uintptr_t>(base+p)+n<=reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;
}
void Require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct Context {
  PPCContext& ctx;PPCContext saved;unsigned csr;DWORD error;
  explicit Context(PPCContext& c):ctx(c),saved(c),csr(_mm_getcsr()),error(GetLastError()){}
  void Reset(){ctx=saved;ctx.r1.u64=saved.r1.u32-4096;}
  ~Context(){ctx=saved;_mm_setcsr(csr);SetLastError(error);}
};
struct GlobalDispatch {PPCContext* ctx;uint8_t* base;uint32_t store;unsigned reads=0,writes=0;NativeScriptState* scripts=nullptr;std::vector<uint8_t> prefab_bytes;};
uint32_t ResolveEntityName(uintptr_t opaque,const uint8_t* name,uint32_t length,uint32_t* handle) noexcept {
  try{
    if(!name || !length || length>256 || !handle)return 2;
    *handle=0;auto& d=*reinterpret_cast<GlobalDispatch*>(opaque);if(!d.scripts)return 2;
    std::string error;return FindRegisteredScriptEntity(*d.ctx,d.base,*d.scripts,
      std::string_view(reinterpret_cast<const char*>(name),length),*handle,error);
  }catch(...){return 2;}
}
uint32_t DestroyEntity(uintptr_t opaque,uint32_t handle) noexcept {
  try{
    auto& d=*reinterpret_cast<GlobalDispatch*>(opaque);if(!d.scripts)return 2;
    std::string error;const auto status=DestroyRegisteredScriptEntity(*d.ctx,d.base,*d.scripts,handle,error);
    if(status==2)REXLOG_WARN("[em1] original native DestroyEntity rejected handle={}: {}",handle,error);
    return status;
  }catch(...){return 2;}
}
uint32_t AnimateEntity(uintptr_t opaque,uint32_t handle,const uint8_t* name,uint32_t length) noexcept {
  try{
    if(!name || !length || length>127)return 2;
    auto& d=*reinterpret_cast<GlobalDispatch*>(opaque);if(!d.scripts)return 2;
    std::string error;const auto status=AnimateRegisteredScriptEntity(*d.ctx,d.base,*d.scripts,handle,
      std::string_view(reinterpret_cast<const char*>(name),length),error);
    if(status==2)REXLOG_WARN("[em1] original native AnimEvent rejected handle={}: {}",handle,error);
    return status;
  }catch(...){return 2;}
}
uint32_t GamebryoAnimation(uintptr_t opaque,uint32_t handle,uint32_t operation,const uint8_t* name,uint32_t length) noexcept {
  try{
    if(operation>1 || length>256 || (length && !name))return 2;
    auto& d=*reinterpret_cast<GlobalDispatch*>(opaque);if(!d.scripts)return 2;
    std::string error;const auto status=GamebryoRegisteredScriptEntity(*d.ctx,d.base,*d.scripts,handle,operation,
      length?std::string_view(reinterpret_cast<const char*>(name),length):std::string_view{},error);
    if(status==2)REXLOG_WARN("[em1] original native Gamebryo animation rejected handle={} operation={}: {}",handle,operation,error);
    return status;
  }catch(...){return 2;}
}
uint32_t ControlEntity(uintptr_t opaque,uint32_t handle,uint32_t operation) noexcept {
  try{
    auto& d=*reinterpret_cast<GlobalDispatch*>(opaque);if(!d.scripts)return 2;
    std::string error;const auto status=ControlRegisteredScriptEntity(*d.ctx,d.base,*d.scripts,handle,operation,error);
    if(status==2)REXLOG_WARN("[em1] original entity control rejected handle={} operation={}: {}",handle,operation,error);
    return status;
  }catch(...){return 2;}
}
uint32_t SetAnimationVariable(uintptr_t opaque,uint32_t handle,const uint8_t* name,uint32_t length,int32_t value) noexcept {
  try{
    if(!name || !length || length>127)return 2;auto& d=*reinterpret_cast<GlobalDispatch*>(opaque);if(!d.scripts)return 2;
    std::string error;const auto status=SetRegisteredScriptAnimationVariable(*d.ctx,d.base,*d.scripts,handle,
      std::string_view(reinterpret_cast<const char*>(name),length),value,error);
    if(status==2)REXLOG_WARN("[em1] original AnimVarInt rejected handle={}: {}",handle,error);return status;
  }catch(...){return 2;}
}
uint32_t ControlComponent(uintptr_t opaque,uint32_t handle,const uint8_t* name,uint32_t length,uint32_t disabled) noexcept {
  try{
    if(!name || !length || length>256 || disabled>1)return 2;auto& d=*reinterpret_cast<GlobalDispatch*>(opaque);if(!d.scripts)return 2;
    std::string error;const auto status=ControlRegisteredScriptComponent(*d.ctx,d.base,*d.scripts,handle,
      std::string_view(reinterpret_cast<const char*>(name),length),disabled,error);
    if(status==2)REXLOG_WARN("[em1] original component control rejected handle={}: {}",handle,error);return status;
  }catch(...){return 2;}
}
uint32_t GetPosition(uintptr_t opaque,uint32_t handle,float* output) noexcept {
  try{if(!output)return 2;auto& d=*reinterpret_cast<GlobalDispatch*>(opaque);if(!d.scripts)return 2;
    std::array<float,4> value{};std::string error;
    const auto status=ReadRegisteredScriptPosition(*d.ctx,d.base,*d.scripts,handle,value,error);
    if(status<2)std::copy(value.begin(),value.end(),output);
    else REXLOG_WARN("[em1] original GetPosition rejected handle={}: {}",handle,error);
    return status;
  }catch(...){return 2;}
}
uint32_t TeleportEntity(uintptr_t opaque,uint32_t handle,uint32_t destination,uint32_t reset,uint32_t orientation) noexcept {
  try{auto& d=*reinterpret_cast<GlobalDispatch*>(opaque);if(!d.scripts)return 2;std::string error;
    const auto status=TeleportRegisteredScriptEntity(*d.ctx,d.base,*d.scripts,handle,destination,reset,orientation,error);
    if(status==2)REXLOG_WARN("[em1] original TeleportToEntity rejected handle={} destination={}: {}",handle,destination,error);
    return status;
  }catch(...){return 2;}
}
uint32_t SetParentEntity(uintptr_t opaque,uint32_t child,uint32_t parent) noexcept {
  try{auto& d=*reinterpret_cast<GlobalDispatch*>(opaque);if(!d.scripts)return 2;std::string error;
    const auto status=ParentRegisteredScriptEntity(*d.ctx,d.base,*d.scripts,child,parent,error);
    if(status==2)REXLOG_WARN("[em1] original SetParentEntity rejected child={} parent={}: {}",child,parent,error);
    return status;
  }catch(...){return 2;}
}
uint32_t ResolveRelativeEntity(uintptr_t opaque,uint32_t owner,const uint8_t* name,uint32_t length,uint32_t* output) noexcept {
  try{if(!output || length>256 || (length && !name))return 2;auto& d=*reinterpret_cast<GlobalDispatch*>(opaque);if(!d.scripts)return 2;
    uint32_t candidate=0;std::string error;
    const auto status=FindRegisteredScriptRelativeEntity(*d.ctx,d.base,*d.scripts,owner,
      length?std::string_view(reinterpret_cast<const char*>(name),length):std::string_view{},candidate,error);
    if(status<2)*output=candidate;
    else REXLOG_WARN("[em1] original GetRelativePrefabEntity rejected owner={}: {}",owner,error);
    return status;
  }catch(...){return 2;}
}
uint32_t GetPrefabData(uintptr_t opaque,uint32_t handle,uint32_t* owner,const uint8_t** bytes,size_t* length) noexcept {
  try{if(!owner || !bytes || !length)return 2;auto& d=*reinterpret_cast<GlobalDispatch*>(opaque);if(!d.scripts)return 2;
    RegisteredPrefabData data;std::string error;
    const auto status=ReadRegisteredScriptPrefabData(*d.ctx,d.base,*d.scripts,handle,data,error);
    if(status==2){REXLOG_WARN("[em1] original GetPrefabData rejected handle={}: {}",handle,error);return 2;}
    std::vector<uint8_t> packet;
    auto word=[&](uint32_t value){for(int shift=24;shift>=0;shift-=8)packet.push_back(uint8_t(value>>shift));};
    for(const auto* rows:{&data.defaults,&data.overrides}){word(uint32_t(rows->size()));for(const auto& row:*rows){word(uint32_t(row.size()));packet.insert(packet.end(),row.begin(),row.end());}}
    d.prefab_bytes=std::move(packet);std::copy(data.owner.begin(),data.owner.end(),owner);owner[4]=data.handle;*bytes=d.prefab_bytes.data();*length=d.prefab_bytes.size();return status;
  }catch(...){return 2;}
}
uint32_t CameraControl(uintptr_t opaque,uint32_t operation,float a,float b,float c) noexcept {
  try{auto& d=*reinterpret_cast<GlobalDispatch*>(opaque);if(!d.scripts)return 2;std::string error;
    const auto status=ControlRegisteredScriptCamera(*d.ctx,d.base,*d.scripts,operation,a,b,c,error);
    if(status==2)REXLOG_WARN("[em1] original native camera rejected operation={}: {}",operation,error);
    return status;
  }catch(...){return 2;}
}
uint32_t FadeScreen(uintptr_t opaque,uint32_t operation,float seconds) noexcept {
  try{auto& d=*reinterpret_cast<GlobalDispatch*>(opaque);if(!d.scripts)return 2;std::string error;
    const auto status=FadeRegisteredScriptScreen(*d.ctx,d.base,*d.scripts,operation,seconds,error);
    if(status==2)REXLOG_WARN("[em1] original native fade rejected operation={}: {}",operation,error);
    return status;
  }catch(...){return 2;}
}
uint32_t ResolveGroup(uintptr_t opaque,const uint8_t* bytes,uint32_t length,uint32_t* output,uint32_t capacity,uint32_t* count) noexcept {
  try {
    if(!bytes || !length || length>256 || !output || !count || capacity>4096)return 2;
    *count=0;auto& d=*reinterpret_cast<GlobalDispatch*>(opaque);
    if(!d.scripts)return 2;
    const std::string_view name(reinterpret_cast<const char*>(bytes),length);
    if(name.find('\0')!=std::string_view::npos)return 2;
    std::vector<uint32_t> handles;
    if(!IsActiveArchitectureScriptState(*d.scripts)){
      std::string error;
      if(FindRegisteredScriptGroup(*d.ctx,d.base,*d.scripts,name,handles,error)!=0 || handles.size()>capacity){
        REXLOG_WARN("[em1] original registered group '{}' refused: {}",name,error.empty()?"Native group callback capacity exceeded":error);
        return 2;
      }
      std::copy(handles.begin(),handles.end(),output);*count=uint32_t(handles.size());
      REXLOG_INFO("[em1] original Lua native registered group '{}' resolved={} members",name,handles.size());
      return 0;
    }
    for(const auto& member:ActiveSingleGsaEntry().scene.group_members)if(member.group==name){
      const auto handle=NativeHandleForAuthoredEntity(member.source_entity);
      if(!handle || AuthoredEntityForNativeHandle(handle)!=member.source_entity)return 2;
      if(std::find(handles.begin(),handles.end(),handle)==handles.end())handles.push_back(handle);
      if(handles.size()>capacity)return 2;
    }
    std::copy(handles.begin(),handles.end(),output);*count=uint32_t(handles.size());
    REXLOG_INFO("[em1] original Lua native group '{}' resolved={} members",name,handles.size());
    return 0;
  }catch(...){return 2;}
}
uint32_t SetParticleEmission(uintptr_t opaque,uint32_t handle,uint32_t enabled) noexcept {
  try {
    auto& d=*reinterpret_cast<GlobalDispatch*>(opaque);
    if(enabled>1 || !d.scripts)return 2;
    if(!IsActiveArchitectureScriptState(*d.scripts)){
      std::string error;const auto status=SetRegisteredScriptParticleEmission(*d.ctx,d.base,*d.scripts,handle,enabled,error);
      if(status==2)REXLOG_WARN("[em1] registered particle control refused handle={}: {}",handle,error);
      return status;
    }
    const bool ok=SetOwnedArchitectureParticleEmission(*d.ctx,d.base,handle,enabled!=0);
    REXLOG_INFO("[em1] original Lua native emitter handle={} enabled={} applied={}",handle,enabled,ok);
    return ok?0:2;
  }catch(...){return 2;}
}
uint32_t SetRotator(uintptr_t opaque,uint32_t handle,float speed) noexcept {
  try {
    auto& d=*reinterpret_cast<GlobalDispatch*>(opaque);
    const bool ok=SetOwnedArchitectureGearSpeed(*d.ctx,d.base,handle,speed);
    REXLOG_INFO("[em1] original Lua native Rotator handle={} speed={} applied={}",handle,speed,ok);
    return ok?0:2;
  }catch(...){return 2;}
}
std::array<uint32_t,2> CutsceneControllers(PPCContext& ctx,uint8_t* base,uint32_t scratch){
  Require(Mapped(base,scratch,32) && Mapped(base,0x833DAB60,4),"Cutscene context unavailable");
  const auto manager=PPC_LOAD_U32(0x833DAB60);Require(Mapped(base,manager,4),"Player manager unavailable");
  Context context(ctx);std::array<uint32_t,2> result{};
  for(unsigned i=0;i<2;++i){
    context.Reset();ctx.r3.u64=scratch+8;ctx.r4.u64=manager;ctx.r5.u64=i;__imp__sub_8276A018(ctx,base);
    const auto handle=ctx.r3.u32;Require(Mapped(base,handle,4),"Player handle unavailable");
    context.Reset();ctx.r3.u64=manager;ctx.r4.u64=handle;__imp__sub_8276BC38(ctx,base);
    result[i]=ctx.r3.u32;Require(!result[i] || Mapped(base,result[i],0x3A8),"Player controller unavailable");
  }return result;
}
bool ChangeCutsceneInput(PPCContext& ctx,uint8_t* base,NativeScriptState& state,bool restricted){
  Context context(ctx);
  Require(ctx.r1.u32>=32768 && Mapped(base,ctx.r1.u32-32768,32768),"Cutscene stack unavailable");
  const auto scratch=state.native_globals_store+3072;
  const auto controllers=CutsceneControllers(ctx,base,scratch);
  std::array<uint32_t,2> before{};
  for(unsigned i=0;i<2;++i){
    if(state.cutscene_locks[i])Require(controllers[i]==state.cutscene_controllers[i],"Cutscene player ownership changed");
    if(controllers[i])before[i]=PPC_LOAD_U32(controllers[i]+0x3A4);
    // CleanLoadLevel may reset native restrictions before the level hook runs.
    // A zero counter revokes our outstanding counts; never decrement below zero.
    if(!before[i] && state.cutscene_locks[i]){
      REXLOG_INFO("[em1] cutscene ownership revoked after native reset player={} owned={}",i,state.cutscene_locks[i]);
      state.cutscene_locks[i]=0;state.cutscene_controllers[i]=0;
    }
    Require(before[i]>=state.cutscene_locks[i] && before[i]<1024 && state.cutscene_locks[i]<(restricted?64u:65u),"Cutscene restriction count invalid");
  }
  if(restricted){
    Require(controllers[0] || controllers[1],"No native player for cutscene restriction");
    PPC_STORE_U32(scratch,0);context.Reset();ctx.r3.u64=scratch;ctx.r4.u64=1;__imp__sub_82A0C740(ctx,base);
    // Record every increment before validating, so error cleanup can balance it.
    bool valid=true,acquired=false;
    for(unsigned i=0;i<2;++i)if(controllers[i]){
      const auto after=PPC_LOAD_U32(controllers[i]+0x3A4);
      if(after==before[i]+1){state.cutscene_controllers[i]=controllers[i];++state.cutscene_locks[i];acquired=true;}
      else if(after!=before[i])valid=false;
    }
    Require(valid && acquired,"Native cutscene restriction was not acquired");
  }else{
    for(unsigned i=0;i<2;++i)if(state.cutscene_locks[i]){
      context.Reset();ctx.r3.u64=controllers[i];ctx.r4.u64=1;__imp__sub_82A0C2D0(ctx,base);
      Require(PPC_LOAD_U32(controllers[i]+0x3A4)+1==before[i],"Native cutscene restriction was not released");
      if(!--state.cutscene_locks[i])state.cutscene_controllers[i]=0;
    }
  }
  REXLOG_INFO("[em1] native cutscene input restricted={} owned_counts={}/{}",restricted,state.cutscene_locks[0],state.cutscene_locks[1]);
  return true;
}
uint32_t SetCutsceneInput(uintptr_t opaque,uint32_t restricted) noexcept {
  try{auto& d=*reinterpret_cast<GlobalDispatch*>(opaque);if(!d.scripts || restricted>1)return 2;
    if(!IsActiveArchitectureScriptState(*d.scripts)){
      std::string error;if(!ValidateRegisteredScriptEngineScope(*d.ctx,d.base,*d.scripts,error)){
        REXLOG_WARN("[em1] registered cutscene input refused: {}",error);return 2;
      }
    }
    return ChangeCutsceneInput(*d.ctx,d.base,*d.scripts,restricted!=0)?0:2;
  }catch(const std::exception& e){REXLOG_ERROR("[em1] cutscene binding refused: {}",e.what());return 2;}
}
// Simulation-thread only; one unloaded campaign store plus the active scene.
// Keeping the native map preserves values without a host shadow representation.
struct RetainedGlobals {uint32_t store=0;std::array<uint8_t,32> schema{};};
RetainedGlobals retained_globals;
bool AdoptRetainedGlobals(NativeScriptState& state,RetainedGlobals& retained){
  if(state.native_globals_store || !retained.store || state.definition_sha256!=retained.schema)return false;
  state.native_globals_store=std::exchange(retained.store,0);state.native_globals_failed=false;return true;
}
bool RetainGlobals(PPCContext& ctx,uint8_t* base,NativeScriptState& state,RetainedGlobals& retained){
  if(!state.native_globals_store)return true;
  if(retained.store){
    NativeScriptState old;old.native_globals_store=retained.store;
    if(!ReleaseNativeGlobalScripts(ctx,base,old))return false;
    retained.store=0;
  }
  retained.schema=state.definition_sha256;retained.store=std::exchange(state.native_globals_store,0);
  REXLOG_INFO("[em1] native globals retained for next matching scene; memory only");return true;
}
uint32_t ReadGlobal(uintptr_t opaque,const uint8_t* key,size_t length,int32_t* out) noexcept {
  try {
    auto& d=*reinterpret_cast<GlobalDispatch*>(opaque);auto& ctx=*d.ctx;auto* base=d.base;
    if(!key || !out || !length || length>=1000 || std::memchr(key,0,length))return 2;
    Context context(ctx);context.Reset();std::memcpy(base+d.store+256,key,length);PPC_STORE_U8(d.store+256+uint32_t(length),0);
    ctx.r3.u64=d.store;ctx.r4.u64=d.store+256;ctx.r5.u64=d.store+2048;__imp__sub_82828010(ctx,base);++d.reads;
    if(!(ctx.r3.u32&255))return 1;*out=std::bit_cast<int16_t>(uint16_t(PPC_LOAD_U16(d.store+2048)));return 0;
  }catch(...){return 2;}
}
uint32_t WriteGlobal(uintptr_t opaque,const uint8_t* key,size_t length,int32_t value,uint32_t no_counter) noexcept {
  try {
    auto& d=*reinterpret_cast<GlobalDispatch*>(opaque);auto& ctx=*d.ctx;auto* base=d.base;
    if(!key || !length || length>=1000 || std::memchr(key,0,length))return 2;
    Context context(ctx);context.Reset();std::memcpy(base+d.store+256,key,length);PPC_STORE_U8(d.store+256+uint32_t(length),0);
    ctx.r3.u64=d.store;ctx.r4.u64=d.store+256;ctx.r5.s64=value;ctx.r6.u64=no_counter;__imp__sub_828280B8(ctx,base);++d.writes;
    return (ctx.r3.u32&255)?0:1;
  }catch(...){return 2;}
}
}
bool ReleaseNativeCutsceneInput(PPCContext& ctx,uint8_t* base,NativeScriptState& state){
  try{while(state.cutscene_locks[0] || state.cutscene_locks[1])ChangeCutsceneInput(ctx,base,state,false);return true;}
  catch(const std::exception& e){REXLOG_ERROR("[em1] cutscene cleanup failed: {}",e.what());return false;}
}
bool ReleaseNativeGlobalScripts(PPCContext& ctx,uint8_t* base,NativeScriptState& state){
  if(!ReleaseNativeCutsceneInput(ctx,base,state))return false;
  if(!state.native_globals_store)return true;
  Context context(ctx);
  try {
    auto* kernel=rex::kernel::kernel_state();Require(kernel && kernel->memory(),"Native state memory unavailable");
    Require(ctx.r1.u32>=32768 && Mapped(base,ctx.r1.u32-32768,32768),"Native state cleanup stack unavailable");
    context.Reset();ctx.r3.u64=state.native_globals_store+4;__imp__sub_82829328(ctx,base);
    kernel->memory()->SystemHeapFree(state.native_globals_store);state.native_globals_store=0;state.native_globals_failed=false;
    REXLOG_INFO("[em1] scene native global store released");return true;
  }catch(const std::exception& e){REXLOG_ERROR("[em1] scene native global release failed: {}",e.what());return false;}
}
bool RetainNativeGlobalScripts(PPCContext& ctx,uint8_t* base,NativeScriptState& state){
  return ReleaseNativeCutsceneInput(ctx,base,state) && RetainGlobals(ctx,base,state,retained_globals);
}
void TickNativeGlobalScripts(PPCContext& ctx,uint8_t* base,NativeScriptState& state,double delta){
  if(!state.vm || state.native_globals_failed)return;
  Context context(ctx);
  try {
    Require(ctx.r1.u32>=32768 && Mapped(base,ctx.r1.u32-32768,32768),"Native state stack unavailable");
    if(!state.native_globals_store){
      std::array<uint8_t,32> schema{};
      Require(em1_lua_world_global_schema(state.vm.get(),schema.data())==0 && schema==state.definition_sha256,"Native definition schema mismatch");
      if(AdoptRetainedGlobals(state,retained_globals))REXLOG_INFO("[em1] retained native globals attached to matching scene");
    }
    if(!state.native_globals_store){
      auto* kernel=rex::kernel::kernel_state();Require(kernel && kernel->memory(),"Native state memory unavailable");
      Require(Mapped(base,0x833DCA84,4),"Native manager unavailable");const auto manager=PPC_LOAD_U32(0x833DCA84);
      Require(Mapped(base,manager,40) && PPC_LOAD_U32(manager)==0x820A3CC0,"Unexpected native manager layout");
      Em1GlobalSummary summary{};Require(em1_lua_world_global_summary(state.vm.get(),&summary)==0 && summary.count>0,"Native definitions unavailable");
      const auto store=kernel->memory()->SystemHeapAlloc(4096,32);Require(store && Mapped(base,store,4096),"Native state allocation failed");
      std::memset(base+store,0,4096);std::memcpy(base+store,base+manager,16);state.native_globals_store=store;
      context.Reset();ctx.r3.u64=8;__imp__sub_831BC098(ctx,base);const auto buckets=ctx.r3.u32;
      Require(buckets>0 && buckets<1000000,"Invalid native bucket count");
      context.Reset();ctx.r3.u64=store+20;ctx.r4.u64=buckets;ctx.r5.u64=store+128;__imp__sub_8288C7B8(ctx,base);
      for(size_t i=0;i<summary.count;++i){
        Em1GlobalDefinitionView v{};Require(em1_lua_world_global_definition(state.vm.get(),i,&v)==0 && v.name_size>0 && v.name_size<1000,"Invalid native definition");
        std::memcpy(base+store+256,v.name,v.name_size);PPC_STORE_U8(store+256+uint32_t(v.name_size),0);
        context.Reset();ctx.r3.u64=store;ctx.r4.u64=store+256;ctx.r5.u64=v.native_id;ctx.r6.s64=v.initial;ctx.r7.u64=v.flag;__imp__sub_82828BB8(ctx,base);
      }
      Require(PPC_LOAD_U32(store+36)==summary.count,"Native name hash collision or entry loss");
      REXLOG_INFO("[em1] scene native global store ready: entries={}; simulation dispatch enabled",summary.count);
    }
    GlobalDispatch dispatch{&ctx,base,state.native_globals_store};const Em1GlobalCallbacks callbacks{reinterpret_cast<uintptr_t>(&dispatch),ReadGlobal,WriteGlobal};
    dispatch.scripts=&state;
    const Em1EngineCallbacks engine{reinterpret_cast<uintptr_t>(&dispatch),SetCutsceneInput,SetRotator,SetParticleEmission,ResolveGroup,ResolveEntityName,DestroyEntity,AnimateEntity,GamebryoAnimation,ControlEntity,SetAnimationVariable,ControlComponent,GetPosition,TeleportEntity,SetParentEntity,ResolveRelativeEntity,GetPrefabData,FadeScreen,CameraControl};
    const auto tick_status=em1_lua_world_tick_engine(state.vm.get(),delta,&callbacks,&engine);
    if(tick_status!=EM1_LUA_OK){
      Em1LuaWorldView view{};em1_lua_world_view(state.vm.get(),&view);
      REXLOG_ERROR("[em1] original callback failed: {}",view.error_size?std::string(reinterpret_cast<const char*>(view.error),view.error_size):"Invalid simulation tick");
      // Original82732770 reports a failed coroutine and clears that thread;
      //8273DC78 retains the other scheduled records. Do not cancel unrelated
      // authored work or roll back shared input state after a Lua error.
      if(tick_status!=EM1_LUA_SCRIPT_FAILED){
        CancelOriginalScripts(state);
        ReleaseNativeCutsceneInput(ctx,base,state);state.native_globals_failed=true;
      }
    }
  }catch(const std::exception& e){
    ReleaseNativeGlobalScripts(ctx,base,state);state.native_globals_failed=true;
    REXLOG_ERROR("[em1] native global dispatch disabled: {}",e.what());
  }
}
void VerifyNativeGlobalDefinitions(PPCContext& ctx,uint8_t* base,const NativeScriptState& scripts){
  Context context(ctx);
  uint32_t allocation=0,archive_allocation=0;bool map_initialized=false;
  auto* kernel=rex::kernel::kernel_state();auto* memory=kernel?kernel->memory():nullptr;
  auto cleanup=[&]{
    if(map_initialized){context.Reset();ctx.r3.u64=allocation+4;__imp__sub_82829328(ctx,base);map_initialized=false;}
    if(allocation){memory->SystemHeapFree(allocation);allocation=0;}
    if(archive_allocation){memory->SystemHeapFree(archive_allocation);archive_allocation=0;}
  };
  try {
    Require(memory && scripts.vm && ctx.r1.u32>=32768 && Mapped(base,ctx.r1.u32-32768,32768),"Native global validation context unavailable");
    Require(Mapped(base,0x833DCA84,4),"Global manager pointer unavailable");
    const auto manager=PPC_LOAD_U32(0x833DCA84);
    Require(Mapped(base,manager,40) && PPC_LOAD_U32(manager)==0x820A3CC0,"Unexpected native global manager layout");
    std::array<uint8_t,40> original{};std::memcpy(original.data(),base+manager,original.size());
    Em1GlobalSummary summary{};Require(em1_lua_world_global_summary(scripts.vm.get(),&summary)==0 && summary.count>0,"Original global definitions unavailable");
    allocation=memory->SystemHeapAlloc(4096,32);Require(allocation && Mapped(base,allocation,4096),"Native global staging allocation failed");
    std::memset(base+allocation,0,4096);
    // Reproduce only the map portion of 82828A20, copying its verified live
    // type/hash/equality pointers. Do not register another save participant.
    std::memcpy(base+allocation,original.data(),16);map_initialized=true;
    context.Reset();ctx.r3.u64=8;__imp__sub_831BC098(ctx,base);const auto buckets=ctx.r3.u32;
    Require(buckets>0 && buckets<1000000,"Invalid native bucket capacity");
    context.Reset();ctx.r3.u64=allocation+20;ctx.r4.u64=buckets;ctx.r5.u64=allocation+128;__imp__sub_8288C7B8(ctx,base);
    auto definition=[&](size_t i){Em1GlobalDefinitionView v{};Require(em1_lua_world_global_definition(scripts.vm.get(),i,&v)==0 && v.name_size<1000,"Invalid global definition");return v;};
    auto name=[&](const Em1GlobalDefinitionView& v){std::memcpy(base+allocation+256,v.name,v.name_size);PPC_STORE_U8(allocation+256+uint32_t(v.name_size),0);};
    for(size_t i=0;i<summary.count;++i){
      const auto v=definition(i);name(v);context.Reset();ctx.r3.u64=allocation;ctx.r4.u64=allocation+256;ctx.r5.u64=v.native_id;ctx.r6.s64=v.initial;ctx.r7.u64=v.flag;
      __imp__sub_82828BB8(ctx,base);
    }
    Require(PPC_LOAD_U32(allocation+36)==summary.count,"Native global name hash collision or entry loss");
    std::set<uint16_t> save_ids;
    for(size_t i=0;i<summary.count;++i){
      const auto v=definition(i);name(v);context.Reset();ctx.r3.u64=allocation+2080;ctx.r4.u64=allocation;ctx.r5.u64=allocation+256;
      __imp__sub_82827EE0(ctx,base);
      const auto node=PPC_LOAD_U32(allocation+2088);Require(Mapped(base,node,16),"Native global node unavailable");
      const auto id=uint16_t(PPC_LOAD_U16(node+12));
      Require(id==v.native_id && id!=0 && save_ids.insert(id).second,"Native global save ID collision or mismatch");
    }
    auto read=[&](const Em1GlobalDefinitionView& v){name(v);context.Reset();ctx.r3.u64=allocation;ctx.r4.u64=allocation+256;ctx.r5.u64=allocation+2048;__imp__sub_82828010(ctx,base);Require((ctx.r3.u32&255)!=0,"Native authored global lookup failed");return std::bit_cast<int16_t>(uint16_t(PPC_LOAD_U16(allocation+2048)));};
    auto write=[&](const Em1GlobalDefinitionView& v,int value){name(v);context.Reset();ctx.r3.u64=allocation;ctx.r4.u64=allocation+256;ctx.r5.s64=value;ctx.r6.u64=0;__imp__sub_828280B8(ctx,base);Require((ctx.r3.u32&255)!=0,"Native global update failed");};
    size_t flagged=summary.count,ordinary=summary.count;
    for(size_t i=0;i<summary.count;++i){const auto v=definition(i);Require(read(v)==v.initial,"Native global default mismatch");if(v.flag)flagged=i;else ordinary=i;}
    Require(flagged<summary.count && ordinary<summary.count,"Reset validation needs both flag kinds");
    GlobalDispatch dispatch{&ctx,base,allocation};
    const Em1GlobalCallbacks callbacks{reinterpret_cast<uintptr_t>(&dispatch),ReadGlobal,WriteGlobal};
    const auto probe=definition(flagged);
    const Em1LuaArgument args[]={{3,0,0,probe.name,probe.name_size},{2,0,70000,nullptr,0},{1,0,1,nullptr,0}};
    Require(em1_lua_world_start_args(scripts.vm.get(),reinterpret_cast<const uint8_t*>("SetGlobal"),9,args,3)==0,"Native Lua SetGlobal enqueue failed");
    Require(em1_lua_world_tick_native(scripts.vm.get(),1.0/30.0,&callbacks)==0 && dispatch.writes==1 && read(probe)==4464,"Lua-to-native global update failed");
    Require(em1_lua_world_start_args(scripts.vm.get(),reinterpret_cast<const uint8_t*>("GetGlobal"),9,args,1)==0,"Native Lua GetGlobal enqueue failed");
    Require(em1_lua_world_tick_native(scripts.vm.get(),1.0/30.0,&callbacks)==0 && dispatch.reads==1,"Lua-to-native global lookup failed");
    context.Reset();ctx.r3.u64=allocation;ctx.r4.u64=0;__imp__sub_828288F0(ctx,base);
    REXLOG_INFO("[em1] Lua native global bridge passed: scheduler GetGlobal/SetGlobal, native signed16 storage; dispatch scoped");
    // Exercise 82828C28 itself, including its temporary ID->name index. The
    // wrapper methods only require active/read flags and their inner archive
    // at +65568. The 827BFF70 field codec uses a 16-byte node header followed
    // by bounded named fields. No file or save-manager registration is needed.
    archive_allocation=memory->SystemHeapAlloc(131200,32);
    Require(archive_allocation && Mapped(base,archive_allocation,131200),"Native save archive allocation failed");
    std::memset(base+archive_allocation,0,131200);
    const uint32_t archive=archive_allocation,fields=archive+65568,node=archive+65632;
    PPC_STORE_U32(archive+8,node);PPC_STORE_U32(archive+24,1);
    PPC_STORE_U32(fields,node);PPC_STORE_U32(fields+8,65536);
    auto expected=[](size_t i){return std::bit_cast<int16_t>(uint16_t(i*11+32768));};
    for(size_t i=0;i<summary.count;++i)write(definition(i),expected(i));
    context.Reset();ctx.r3.u64=allocation;ctx.r4.u64=archive;ctx.r5.u64=0;__imp__sub_82828C28(ctx,base);
    const auto saved_bytes=PPC_LOAD_U32(fields+4);
    Require(PPC_LOAD_U32(fields+12)!=0xFFFFFFFFu && saved_bytes>summary.count*6 && saved_bytes<65536,"Native serialization failed or exceeded capacity");
    context.Reset();ctx.r3.u64=allocation;ctx.r4.u64=0;__imp__sub_828288F0(ctx,base);
    for(size_t i=0;i<summary.count;++i){const auto v=definition(i);Require(read(v)==v.initial,"Pre-load reset failed");}
    // Close the memory node's written extent, then reopen the same bytes for
    // reading. No host implementation of the field or restore algorithm.
    PPC_STORE_U32(node+8,saved_bytes);PPC_STORE_U32(fields+8,saved_bytes);
    PPC_STORE_U8(fields+16,1);PPC_STORE_U8(archive+20,1);
    context.Reset();ctx.r3.u64=allocation;ctx.r4.u64=archive;ctx.r5.u64=0;__imp__sub_82828C28(ctx,base);
    for(size_t i=0;i<summary.count;++i)Require(read(definition(i))==expected(i),"Native save/load value mismatch");
    context.Reset();ctx.r3.u64=allocation;ctx.r4.u64=0;__imp__sub_828288F0(ctx,base);
    REXLOG_INFO("[em1] native global save roundtrip passed: entries={} bytes={} original serializer and field codec; memory only",summary.count,saved_bytes);
    const auto a=definition(flagged),b=definition(ordinary);write(a,-12345);write(b,23456);
    Require(read(a)==-12345 && read(b)==23456,"Native signed global update mismatch");
    context.Reset();ctx.r3.u64=allocation;ctx.r4.u64=1;__imp__sub_828288F0(ctx,base);
    Require(read(a)==-12345 && read(b)==b.initial,"Native flagged reset mismatch");
    context.Reset();ctx.r3.u64=allocation;ctx.r4.u64=0;__imp__sub_828288F0(ctx,base);
    for(size_t i=0;i<summary.count;++i){const auto v=definition(i);Require(read(v)==v.initial,"Native full reset mismatch");}
    cleanup();
    Require(PPC_LOAD_U32(0x833DCA84)==manager && std::memcmp(original.data(),base+manager,40)==0,"Active global manager changed during isolated validation");
    NativeScriptState lifecycle;lifecycle.vm=scripts.vm;lifecycle.definition_sha256=scripts.definition_sha256;
    TickNativeGlobalScripts(ctx,base,lifecycle,0.0);
    Require(lifecycle.native_globals_store && !lifecycle.native_globals_failed,"Production store initialization failed");
    NativeScriptState moved=std::move(lifecycle);
    Require(!lifecycle.native_globals_store && moved.native_globals_store,"Native store ownership transfer failed");
    Require(ReleaseNativeGlobalScripts(ctx,base,moved) && !moved.native_globals_store,"Production store release failed");
    TickNativeGlobalScripts(ctx,base,moved,0.0);
    Require(moved.native_globals_store && !moved.native_globals_failed,"Production store recreation failed");
    GlobalDispatch retained_dispatch{&ctx,base,moved.native_globals_store};
    Require(WriteGlobal(reinterpret_cast<uintptr_t>(&retained_dispatch),probe.name,probe.name_size,-23456,0)==0,"Retention probe update failed");
    RetainedGlobals retained_test;
    Require(RetainGlobals(ctx,base,moved,retained_test) && !moved.native_globals_store,"Store retention failed");
    NativeScriptState next;next.vm=scripts.vm;next.definition_sha256=scripts.definition_sha256;next.definition_sha256[0]^=1;
    Require(!AdoptRetainedGlobals(next,retained_test) && retained_test.store,"Wrong schema consumed retained state");
    next.definition_sha256=scripts.definition_sha256;
    Require(AdoptRetainedGlobals(next,retained_test) && !retained_test.store,"Matching scene failed to adopt state");
    TickNativeGlobalScripts(ctx,base,next,0.0);
    GlobalDispatch adopted{&ctx,base,next.native_globals_store};int32_t retained_value=0;
    Require(ReadGlobal(reinterpret_cast<uintptr_t>(&adopted),probe.name,probe.name_size,&retained_value)==0 && retained_value==-23456,"Global value lost during scene-owner transfer");
    Require(ReleaseNativeGlobalScripts(ctx,base,next),"Adopted store release failed");
    REXLOG_INFO("[em1] native global retention passed: value preserved, wrong schema refused, matching new owner adopted");
    Require(ReleaseNativeGlobalScripts(ctx,base,moved),"Recreated store release failed");
    REXLOG_INFO("[em1] native script owner lifecycle passed: initialize, move, release, recreate, release");
    REXLOG_INFO("[em1] native global store validation passed: entries={} unique nonzero save IDs={} signed updates, flagged/full resets, native release; active manager unchanged",summary.count,save_ids.size());
  }catch(const std::exception& e){
    REXLOG_ERROR("[em1] native global store validation failed: {}",e.what());
    try{cleanup();}catch(...){REXLOG_ERROR("[em1] native global staging cleanup failed; allocation retained for diagnosis");}
  }
}
}
