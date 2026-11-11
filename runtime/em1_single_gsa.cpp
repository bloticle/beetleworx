#include "em1_single_gsa.h"
#include "em1_single_gsa_pak.h"
#include "em1_nif_materials.h"
#include "em1_native_effect_trace.h"
#include "online_runtime.h"
#include "ppc_context.h"
#include <rex/kernel/kernel_state.h>
#include <rex/logging.h>
#include <rex/cvar.h>
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <cstring>
#include <future>

PPC_EXTERN_FUNC(sub_829E3178);
REXCVAR_DEFINE_BOOL(em1_masked_material_candidate,false,"Debug","Use the privately staged native EM1 masked material after loader acceptance");
REXCVAR_DEFINE_BOOL(em1_dual_uv_material_candidate,false,"Debug","Use the privately staged EM1 dual UV material after loader acceptance");
REXCVAR_DEFINE_BOOL(em1_cutout_material_candidate,false,"Debug","Use private EM1 cutout material after native loader acceptance");
namespace em2::em1 {
namespace {
struct Restore {
  PPCContext& ctx;PPCContext saved;DWORD error;unsigned csr;
  explicit Restore(PPCContext& c):ctx(c),saved(c),error(GetLastError()),csr(_mm_getcsr()){}
  ~Restore(){ctx=saved;SetLastError(error);_mm_setcsr(csr);}
};
bool Mapped(uint8_t* base,uint32_t address,size_t bytes){
  if(!base || address<4096 || uint64_t(address)+bytes>0x100000000ull)return false;
  auto at=reinterpret_cast<uintptr_t>(base+address);const auto end=at+bytes;
  while(at<end){MEMORY_BASIC_INFORMATION m{};
    if(!VirtualQuery(reinterpret_cast<void*>(at),&m,sizeof(m)) || m.State!=MEM_COMMIT ||
       (m.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
    auto next=reinterpret_cast<uintptr_t>(m.BaseAddress)+m.RegionSize;
    if(next<=at)return false;at=std::min(end,next);
  }return true;
}
bool Matches(uint8_t* base,uint32_t address,std::string_view expected){
  std::string text;
  for(unsigned i=0;i<512;++i){
    if(!Mapped(base,address+i,1))return false;
    char c=char(base[address+i]);if(!c){
      if(text.starts_with("game:/"))text.erase(0,6);
      else if(text.starts_with("d:/"))text.erase(0,3);
      while(!text.empty() && text.front()=='/')text.erase(0,1);
      return text==expected;
    }
    if(c=='\\')c='/';if(c>='A' && c<='Z')c=char(c-'A'+'a');text+=c;
  }return false;
}
}
bool PrepareSingleGsaEntry(std::shared_ptr<RvzSource> source,SingleGsaEntry& entry,std::string& error){
  if(entry.prepared || entry.raw.owner.record || entry.model.record){error="SingleGsa entry already owns state";return false;}
  SingleGsaEntry prepared;
  prepared.model_name=ReserveNativeModelName();
  if(!source || !ReadCourtyardArchitectureScene(source,prepared.model_name,prepared.scene,error))return false;
  REXLOG_INFO("[em1] authored runtime lighting decoded: lights={}",prepared.scene.runtime_lights.size());
  REXLOG_INFO("[em1] authored event connections retained: count={} fingerprint={:016X}; source IDs require actor admission before dispatch",prepared.scene.event_links.size(),prepared.scene.event_link_fingerprint);
  REXLOG_INFO("[em1] authored entity groups retained: memberships={} fingerprint={:016X}; source IDs require actor admission",prepared.scene.group_members.size(),prepared.scene.group_fingerprint);
  for(const auto& member:prepared.scene.group_members)
    if(member.group.find("Steam_BlastGear")!=std::string::npos || member.group.find("Steam_Jet_01")!=std::string::npos)
      REXLOG_INFO("[em1] tutorial emitter group='{}' authored={} name='{}'",member.group,member.source_entity,member.source_name);
  try {
    if(!std::async(std::launch::async,[&]{return PrepareOriginalScripts(source,"packfiles/dbc_1st_zonec.pak",prepared.scripts,error) && InitializeOriginalScripts(source,prepared.scripts,prepared.script_state,error);}).get())return false;
  }catch(const std::exception& failure){error=std::string("EM1 script worker failed: ")+failure.what();return false;}
  size_t script_bytes=0;for(const auto& script:prepared.scripts){script_bytes+=script.bytecode.size();REXLOG_INFO("[em1] native Lua script prepared from RVZ: {} bytes={}; executable roundtrip exact, not executed",script.path,script.bytecode.size());}
  REXLOG_INFO("[em1] original scripts retained: count={} bytes={}; native Lua engine bindings pending",prepared.scripts.size(),script_bytes);
  REXLOG_INFO("[em1] persistent native script world initialized: scripts={} functions={} globals={}; original startup chunks executed, engine callbacks not dispatched",prepared.script_state.scripts,prepared.script_state.functions,prepared.script_state.globals);
  const ModelLighting lighting{"environments/thepark/darkbeautycastle/gsa/dbc_1st_zonec.lit_cooked",prepared.scene.source_entity_id};
  if(!source || !ReadModelClosure(source,"packfiles/dbc_1st_zonec.pak",
      "environments/thepark/darkbeautycastle/Zones/DBC_scaletest2.nif_wii",prepared.closure,error,&lighting))return false;
  REXLOG_INFO("[em1] authored baked lighting applied: entity={} meshes={}",lighting.entity,prepared.closure.model.baked_meshes);
  if(REXCVAR_GET(em1_masked_material_candidate)) {
    if(!NativeEm1BlendEffectsLoaded()){error="EM1 blend effects were not accepted by the native loader";return false;}
    const bool dual=REXCVAR_GET(em1_dual_uv_material_candidate);
    if(dual && !NativeEm1DualUvEffectLoaded()){error="EM1 dual UV effect was not accepted by the native loader";return false;}
    const bool cutout=REXCVAR_GET(em1_cutout_material_candidate);
    if(cutout && !NativeEm1CutoutEffectLoaded()){error="EM1 cutout effect was not accepted by the native loader";return false;}
    try {
      nif::Nif model(prepared.closure.model.bytes);
      const auto changed=AdaptPreviewDiffuseMaterials(model,true,dual,cutout);
      nif::Check(changed==((dual?44:33)+(cutout?12:0)),"Unexpected courtyard blend material count");
      prepared.closure.model.bytes=model.Write();
      prepared.closure.model.origin_key+=dual?":native-dual-uv-candidate1":":native-blend-candidate2";
      if(cutout)prepared.closure.model.origin_key+=":cutout-candidate1";
      REXLOG_INFO("[em1] private blend material candidate mapped {} original meshes",changed);
    }catch(const std::exception& exception){error=exception.what();return false;}
  }
  const auto model_id=prepared.model_name.substr(std::string("em1/runtime/").size());
  prepared.scene_name="em1/runtime/em1_courtyard_"+model_id.substr(0,model_id.size()-4)+".bin";
  prepared.prepared=true;entry=std::move(prepared);error.clear();return true;
}
void ServeSingleGsaRawRead(PPCContext& ctx,uint8_t* base,uint32_t manager,uint32_t path,SingleGsaEntry& entry){
  Restore restore(ctx);
  if(!entry.loading || !entry.error.empty() || !Matches(base,path,entry.scene_name))return;
  entry.source_requested=true;
  if(entry.raw.owner.record)return;
  if(!RetainNativeRawScene(ctx,base,manager,entry.closure.source,entry.scene.origin_key,
      entry.scene_name,entry.scene.scene.bytes,entry.raw,entry.error)){
    REXLOG_ERROR("[em1] SingleGsa raw retention failed: {}",entry.error);return;
  }
  REXLOG_INFO("[em1] SingleGsa original-source scene requested: {} bytes={} manager={:08X}",
      entry.scene_name,entry.scene.scene.bytes.size(),manager);
}
void AfterSingleGsaSceneBegin(PPCContext& ctx,uint8_t* base,uint32_t input,uint32_t size,bool success,SingleGsaEntry& entry){
  Restore restore(ctx);
  if(!entry.loading || !success || !entry.error.empty() || entry.model.record ||
      !entry.raw.owner.record || input!=entry.raw.data || size!=entry.scene.scene.bytes.size())return;
  if(!PublishNativeModel(ctx,base,entry.raw.owner.manager,entry.closure,entry.model,entry.error,entry.model_name)){
    REXLOG_ERROR("[em1] SingleGsa model publication failed: {}",entry.error);return;
  }
  // The guest loader consumed/freed its input. Host adaptation bytes are no
  // longer needed; source identity and the native resource stay retained.
  entry.closure.model.bytes.clear();entry.closure.model.bytes.shrink_to_fit();
  REXLOG_INFO("[em1] SingleGsa original courtyard model available inside native scene load: root={:08X}",entry.model.root);
}
bool EnterSingleGsa(PPCContext& ctx,uint8_t* base,uint32_t app,SingleGsaEntry& entry,std::string& error){
  Restore restore(ctx);
  auto* kernel=rex::kernel::kernel_state();
  if(!entry.prepared || entry.loading || entry.entered || !entry.error.empty() ||
      em2::net::SynchronizedSessionActive() || !kernel || !kernel->memory() ||
      !Mapped(base,app,67200) || ctx.r1.u32<32768 || !Mapped(base,ctx.r1.u32-32768,32768)){
    error="SingleGsa entry requires a prepared offline source and valid native app/stack";return false;
  }
  const auto basename=entry.scene_name.substr(std::string("em1/runtime/").size(),entry.scene_name.size()-std::string("em1/runtime/").size()-4);
  if(!kernel->file_system() || !MountSingleGsaPak(*kernel->file_system(),basename,entry.scene_name,entry.scene.scene.bytes,error))return false;
  const auto name=kernel->memory()->SystemHeapAlloc(512,32);
  if(!name){error="SingleGsa name allocation failed";return false;}
  struct NameOwner {rex::memory::Memory* memory;uint32_t ptr;~NameOwner(){memory->SystemHeapFree(ptr);}} owner{kernel->memory(),name};
  std::memset(base+name,0,512);std::memcpy(base+name,entry.scene_name.c_str(),entry.scene_name.size()+1);
  entry.loading=true;
  struct Loading {SingleGsaEntry& entry;~Loading(){entry.loading=false;}} loading{entry};
  REXLOG_INFO("[em1] entering native SingleGsa architecture preview: {}; scripts/quests/actors are not imported",entry.scene_name);
  ctx.r1.u64-=2048;ctx.r3.u64=app;ctx.r4.u64=name;sub_829E3178(ctx,base);
  const bool native_success=(ctx.r3.u32&255)!=0;
  if(!native_success || !entry.source_requested || !entry.raw.owner.record || !entry.model.record || !entry.error.empty()){
    error=entry.error.empty()?"Native SingleGsa load did not complete source/model admission":entry.error;
    entry.error=error;return false;
  }
  entry.entered=true;error.clear();
  REXLOG_INFO("[em1] native SingleGsa load returned; player/collision bootstrap and visible acceptance still required");
  return true;
}
bool ReleaseSingleGsaOwners(PPCContext& ctx,uint8_t* base,SingleGsaEntry& entry,std::string& error){
  if(entry.loading){error="Cannot release SingleGsa owners during native load";return false;}
  if(!ReleaseNativeModel(ctx,base,entry.model,error) || !ReleaseNativeRawScene(ctx,base,entry.raw,error))return false;
  entry={};error.clear();return true;
}
}
