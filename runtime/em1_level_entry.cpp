#include "em1_level_entry.h"
#include "em1_level_registry.h"
#include "em1_level_catalog_plan.h"
#include "em1_level_archives.h"
#include "em1_level_resource_bindings.h"
#include "em1_scene_files.h"
#include "em1_parser_probe.h"
#include "em1_vfs.h"
#include "em1_level_models.h"
#include "em1_level_animations.h"
#include "em1_motion_states.h"
#include "em1_level_collision.h"
#include "em1_level_collision_bindings.h"
#include "em1_script_source.h"
#include "em1_script_events.h"
#include "em1_native_globals.h"
#include "em1_native_component.h"
#include "native_engine.h"
#include "game_console.h"
#include "online_continue.h"
#include "simulation.h"
#include <rex/cvar.h>
#include "ppc_context.h"
#include <rex/logging.h>
#include <rex/kernel/kernel_state.h>
#include <mutex>
#include <future>
#include <cstdio>
#include <windows.h>
#include <cstring>
REXCVAR_DEFINE_BOOL(em1_probe_level_entry,false,"Debug","Internal experiment: attempt full registered original level through CleanLoadLevel before platform readiness; may fail or crash");
REXCVAR_DEFINE_BOOL(em1_validate_collision_reload,false,"Debug","Private one-shot registered-level reload after 300 ready simulation steps; no gameplay input");
REXCVAR_DEFINE_STRING(em1_native_static_resource,"","Debug","Private original HKX path to load through the stock static component");
REXCVAR_DEFINE_BOOL(em1_native_static_acceleration,true,"Debug","Require native MOPP parity before publishing the selected private static resource");
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(std::string,em1_register_level);
REXCVAR_DECLARE(bool,em2_auto_continue);
namespace em2::em1 {
namespace {
std::mutex entry_mutex;
struct Entry {LevelContents contents;std::string state;std::vector<LevelSceneArchive> archives;std::shared_ptr<const std::vector<NavigationDatabase>> navigation;std::vector<LevelSceneModelAssets> model_assets;std::shared_ptr<RegisteredBlueprintLibrary> blueprints;std::shared_ptr<LevelCollisionPlan> collision;std::shared_ptr<LevelOriginalScripts> scripts;bool reservation_verified=true;};
std::map<std::string,Entry> entries;
struct DeferredEntry {LevelCatalogReservation reservation;std::string error;};
std::map<std::string,DeferredEntry> deferred_entries;
std::vector<uint8_t> retail_registry;
uint8_t* registry_guest_base=nullptr;
std::map<uint32_t,std::shared_ptr<RegisteredBlueprintLibrary>> parser_blueprints;
thread_local std::shared_ptr<RegisteredBlueprintLibrary> actor_scope;
std::shared_ptr<LevelOriginalScripts> active_scripts;
std::atomic<bool> probe_requested=false;
std::string Normalize(std::string_view value){std::string name(value);if(name.starts_with("em1:"))name.erase(0,4);
  for(auto& c:name)if(c>='A' && c<='Z')c+='a'-'A';return name;}
}
bool ExtendNativeLevelRegistry(PPCContext& ctx,uint8_t* base,std::span<const uint8_t> original,std::string_view descriptor,std::vector<uint8_t>& output,std::string& error,
    std::string_view reserved_prefix,std::span<const uint32_t> reserved_group_ids){
  using namespace compiled_level_detail;
  try{
    const auto key=Normalize(descriptor);
    Check(key.ends_with(".level") && key.size()<=120 && key.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789.")==key.npos,"Invalid original level descriptor");
    auto* kernel=rex::kernel::kernel_state();Check(kernel && kernel->file_system(),"Native level filesystem unavailable");
    auto source=MountedRvz(*kernel->file_system());Check(bool(source),"Original EM1 disc is not mounted");
    Entry entry;entry.reservation_verified=reserved_prefix.empty();if(!ReadLevelContents(source,key,entry.contents,error))return false;
    auto navigation=std::make_shared<std::vector<NavigationDatabase>>();std::set<std::pair<std::string,std::string>> nav_keys;
    for(const auto& scene:entry.contents.entry_scenes){std::vector<SceneFileReference> references;
      if(!CollectSceneFileReferences(scene.resource.scene,references,error))return false;
      for(const auto& reference:references)if(SceneFileExtension(Normalize(reference.path))==".hpd"){
        const auto identity=std::pair{Normalize(scene.request.archive),Normalize(reference.path)};
        if(!nav_keys.insert(identity).second)continue;
        NavigationDatabase database;if(!LoadNavigationDatabase(source,scene.request.archive,reference.path,database,error))return false;
        navigation->push_back(std::move(database));
      }
    }
    entry.navigation=std::move(navigation);
    CompiledLevel registry;if(!ReadCompiledLevel(original,registry,error))return false;
    auto candidate=registry;
    const auto prefix=reserved_prefix.empty()?ReserveLevelArchivePrefix():std::string(reserved_prefix);
    if(!PrepareLevelSceneArchives(entry.contents,prefix,entry.archives,error) ||
       !PrepareLevelSceneModels(source,entry.contents,prefix,entry.model_assets,error) ||
       !PrepareLevelSceneAnimations(source,entry.contents,prefix,entry.model_assets,error))return false;
    entry.collision=std::make_shared<LevelCollisionPlan>();
    const auto native_static=REXCVAR_GET(em1_native_static_resource);
    Check(native_static.empty() || REXCVAR_GET(em2_private_profile),"Native static resource validation requires a private profile");
    if(!PrepareLevelCollision(source,entry.contents,*entry.collision,error,native_static,REXCVAR_GET(em1_native_static_acceleration)) ||
       !ValidateNativeStaticCollisionAcceleration(ctx,base,*entry.collision,error,native_static=="*"))return false;
    for(const auto& reason:entry.collision->native_fallbacks)REXLOG_WARN("[em1] native static resource retained on compatibility path: {}",reason);
    if(!BindNativeCollisionAssets(*entry.collision,prefix,entry.model_assets,error))return false;
    entry.scripts=std::make_shared<LevelOriginalScripts>();
    if(!std::async(std::launch::async,[&]{return PrepareLevelOriginalScripts(source,entry.contents,*entry.scripts,error);}).get())return false;
    REXLOG_INFO("[em1] original level script owner prepared: level='{}' archives={} scripts={} functions={}; callbacks still require native event routing",key,entry.scripts->archives.size(),entry.scripts->resources.size(),entry.scripts->state.functions);
    REXLOG_INFO("[em1] level static collision prepared: references={} resources={} unsupported={}",entry.collision->references,entry.collision->resources,entry.collision->unsupported.size());
    for(const auto& failure:entry.collision->unsupported)REXLOG_WARN("[em1] original collision remains unsupported: {}",failure);
    size_t staged_models=0,staged_bytes=0;
    for(auto& archive:entry.archives){const auto& assets=entry.model_assets.at(archive.scene_index);
      staged_models+=assets.models.size();for(const auto& model:assets.models)staged_bytes+=model.bytes.size();
    }
    // These are declared source-bank bindings, not converted-bank claims. The
    // entry preflight below refuses activation until a native audio adapter
    // owns every bank; no Wii bytes are passed to native Wwise as PC banks.
    std::map<std::string,std::string> banks;
    for(const auto& part:entry.contents.plan.partitions)for(const auto& bank:part.audio)
      banks.try_emplace(bank,"em1/runtime/source_bank_"+std::to_string(banks.size())+".bnk");
    std::vector<LevelResourceGroup> groups;
    if(!BindLevelEntryResources(entry.contents,entry.archives,banks,groups,error))return false;
    LevelEntryBindings bindings;bindings.level="em1_"+key.substr(0,key.size()-6);bindings.state=bindings.level+".entry";
    std::set<uint32_t> used_ids;for(const auto& zone:registry.zones)used_ids.insert(zone.metadata[0]);
    uint32_t id=0;
    Check(reserved_group_ids.empty() || reserved_group_ids.size()==groups.size(),"Deferred level resource group count changed");
    for(size_t i=0;i<groups.size();++i){const auto& group=groups[i];CompiledLevel::Zone zone;
      zone.name=bindings.level+".group_"+std::to_string(i);
      if(reserved_group_ids.empty()){
        do{Check(id<99999999,"Native resource group IDs exhausted");++id;}while(used_ids.contains(id));
      }else{id=reserved_group_ids[i];Check(id && id<=99999999 && !used_ids.contains(id),"Invalid or repeated reserved resource group ID");}
      used_ids.insert(id);
      char folder[32];std::snprintf(folder,sizeof(folder),"/zone@%08u/",id);zone.folder=folder;
      // Original registry has trusted_measurements=0 and uses zero for unknown
      // measurements/priority. Word4 is the actual resource-vector capacity.
      Check(!registry.trusted_measurements,"EM1 import requires untrusted native measurements");
      zone.metadata={id,0,0,0,uint32_t(group.files.size())};zone.files=group.files;
      // Every one of the retail registry's513 GSA resources has a distinct
      // scene alias. Banks may use identity aliases, scenes may not: the
      // native scene loader uses that alias as its registered scene identity.
      for(size_t file=0;file<zone.files.size();++file){auto& resource=zone.files[file];
        if(!resource.path.ends_with(".gsa"))continue;
        const auto slash=resource.path.rfind('/');char alias[80];
        std::snprintf(alias,sizeof(alias),"zone_%04u_file_%03zu.gsa",id,file);
        resource.alias=resource.path.substr(0,slash==std::string::npos?0:slash+1)+alias;
      }
      if(group.partition.empty())bindings.shared_zone=zone.name;
      else Check(bindings.partitions.emplace(group.partition,zone.name).second,"Repeated original entry partition");
      candidate.zones.push_back(std::move(zone));
    }
    if(!AppendLevelEntryState(entry.contents.plan,bindings,candidate,error) || !ValidateLevelRegistryExtension(registry,candidate,error))return false;
    std::vector<uint8_t> encoded;if(!WriteCompiledLevel(candidate,encoded,error))return false;
    // Keep each complete GSA, original plan and source provenance alive. Native
    // scene PAKs are published only after every registry check has succeeded.
    std::vector<NativeScenePakRequest> requests;
    MotionStateContext motion_context;MotionStateReport motion_total;
    for(const auto& assets:entry.model_assets)if(!ReserveMotionStateIds(assets.bound_scene,motion_context,error))return false;
    for(auto& assets:entry.model_assets){SceneAdapterResult adapted;MotionStateReport report;
      if(!AdaptLegacyMotionStates(assets.bound_scene,motion_context,adapted,report,error))return false;
      assets.bound_scene=std::move(adapted);motion_total.entities+=report.entities;motion_total.components+=report.components;motion_total.properties+=report.properties;
    }
    REXLOG_INFO("[em1] native motion-state adaptation: entities={} added_states={} mapped_source_properties={}; all original components retained",motion_total.entities,motion_total.components,motion_total.properties);
    for(const auto& a:entry.archives)requests.push_back({a.archive_basename,LevelSceneModelPakEntries(entry.model_assets.at(a.scene_index),a.native_scene)});
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> mounted;
    if(!MountNativeScenePaks(*kernel->file_system(),requests,error,&mounted))return false;
    for(size_t i=0;i<entry.archives.size();++i){
      entry.archives[i].bytes=std::move(mounted[i]);
      // VFS and the level share one immutable PAK. Drop temporary converted
      // buffers after their complete payload has been verified through VFS.
      for(auto& model:entry.model_assets[i].models)std::vector<uint8_t>{}.swap(model.bytes);
      for(auto& resource:entry.model_assets[i].animation_resources)std::vector<uint8_t>{}.swap(resource.bytes);
      for(auto& resource:entry.model_assets[i].physics_resources)std::vector<uint8_t>{}.swap(resource.bytes);
    }
    REXLOG_INFO("[em1] original level model archives staged: level='{}' scenes={} models={} native_bytes={}; complete scenes with declared model aliases, typed NIF resources; no world activation or complete platform readiness",key,entry.archives.size(),staged_models,staged_bytes);
    size_t managers=0,animation_resources=0,unresolved=0;
    for(const auto& assets:entry.model_assets){managers+=assets.animation_managers;animation_resources+=assets.animation_resources.size();unresolved+=assets.unresolved_animations.size();
      for(const auto& path:assets.unresolved_animations)REXLOG_WARN("[em1] original animation reference retained unresolved: '{}' (absent from authored source scope); not converted or ready",path);}
    REXLOG_INFO("[em1] original level animation archives staged: level='{}' managers={} resources={} unresolved={}; typed KFM/KF/NIF dependency ownership, normal scene bindings; no behavior-graph or world activation claim",key,managers,animation_resources,unresolved);
    entry.state=bindings.state;
    {std::lock_guard lock(entry_mutex);Check(!entries.contains(key),"Original level entry already registered");entries.emplace(key,std::move(entry));}
    output=std::move(encoded);error.clear();return true;
  }catch(const std::exception& failure){error=failure.what();return false;}
}
bool ExtendDeferredNativeLevelRegistry(uint8_t* base,std::span<const uint8_t> retail,
    std::span<const uint8_t> registered,std::vector<uint8_t>& output,std::string& error){
  try {
    auto* kernel=rex::kernel::kernel_state();if(!kernel || !kernel->file_system())throw std::runtime_error("Original catalog filesystem unavailable");
    auto source=MountedRvz(*kernel->file_system());if(!source)throw std::runtime_error("Original disc not mounted");
    std::vector<std::string> descriptors;if(!ListLevelDescriptors(source,descriptors,error))return false;
    CompiledLevel catalog;if(!ReadCompiledLevel(registered,catalog,error))return false;
    std::map<std::string,DeferredEntry> prepared;size_t reserved=0,failed=0;
    for(const auto& descriptor:descriptors){const auto key=Normalize(descriptor);
      {std::lock_guard lock(entry_mutex);if(entries.contains(key))continue;}
      LevelPlan plan;DeferredEntry entry;
      if(!ReadLevelPlan(source,descriptor,plan,entry.error) ||
          !AppendLevelCatalogPlan(plan,ReserveLevelArchivePrefix(),catalog,entry.reservation,entry.error)){
        ++failed;REXLOG_WARN("[em1] original level catalog rejected '{}': {}",descriptor,entry.error);
      }else ++reserved;
      prepared.emplace(key,std::move(entry));
    }
    std::vector<uint8_t> encoded;if(!WriteCompiledLevel(catalog,encoded,error))return false;
    {std::lock_guard lock(entry_mutex);retail_registry.assign(retail.begin(),retail.end());registry_guest_base=base;deferred_entries=std::move(prepared);}
    output=std::move(encoded);error.clear();
    REXLOG_INFO("[em1] complete debug level catalog: descriptors={} deferred={} rejected={}; native metadata only, assets prepared on explicit selection",descriptors.size(),reserved,failed);
    return true;
  }catch(const std::exception& e){error=e.what();return false;}
}
LevelPreparationStatus RegisteredLevelPreparation(std::string_view descriptor){
  std::lock_guard lock(entry_mutex);const auto key=Normalize(descriptor);
  if(const auto found=entries.find(key);found!=entries.end())return {true,bool(found->second.blueprints)&&found->second.reservation_verified,{}};
  if(const auto found=deferred_entries.find(key);found!=deferred_entries.end())return {!found->second.reservation.bindings.state.empty(),false,found->second.error};
  return {};
}
bool PrepareDeferredRegisteredLevel(PPCContext& ctx,uint8_t* base,std::string_view descriptor,std::string& error){
  try {
    const auto key=Normalize(descriptor);LevelCatalogReservation reservation;std::vector<uint8_t> baseline;bool already_prepared=false;
    {std::lock_guard lock(entry_mutex);
      if(const auto found=entries.find(key);found!=entries.end()){
        if(!found->second.reservation_verified)throw std::runtime_error("This level's prepared resources have not passed catalog verification");
        if(found->second.blueprints && found->second.blueprints->guest_base==base){error.clear();return true;}
        already_prepared=true;
      }else{
        const auto deferred=deferred_entries.find(key);
        if(deferred==deferred_entries.end())throw std::runtime_error("Level catalog is not ready for this descriptor");
        if(!deferred->second.error.empty())throw std::runtime_error(deferred->second.error);
        if(registry_guest_base!=base || retail_registry.empty())throw std::runtime_error("Native catalog belongs to another title instance");
        reservation=deferred->second.reservation;baseline=retail_registry;
      }
    }
    if(!already_prepared){
      std::vector<uint8_t> encoded;
      if(!ExtendNativeLevelRegistry(ctx,base,baseline,key,encoded,error,reservation.prefix,reservation.group_ids))return false;
      CompiledLevel original,actual;
      if(!ReadCompiledLevel(baseline,original,error) || !ReadCompiledLevel(encoded,actual,error))return false;
      // Compare the completed native records with the exact startup reservation.
      // Native resource aliases embed group IDs, so prefix equality alone is insufficient.
      actual.zones.erase(actual.zones.begin(),actual.zones.begin()+original.zones.size());
      actual.states.erase(actual.states.begin(),actual.states.begin()+original.states.size());
      actual.destinations.erase(actual.destinations.begin(),actual.destinations.begin()+original.destinations.size());
      CompiledLevel expected;expected.zones=reservation.zones;expected.states={reservation.entry_state};expected.destinations={reservation.destination};
      std::vector<uint8_t> a,b;if(!WriteCompiledLevel(actual,a,error) || !WriteCompiledLevel(expected,b,error))return false;
      if(a!=b)throw std::runtime_error("Prepared level differs from its reserved native resource layout");
      {std::lock_guard lock(entry_mutex);entries.at(key).reservation_verified=true;}
    }
    if(!RegisterNativeNavigationFacade(ctx,base,RegisteredLevelNavigation(key),error) ||
        !RetainRegisteredLevelBlueprints(ctx,base,key,error))return false;
    error.clear();return true;
  }catch(const std::exception& e){error=e.what();return false;}
}
std::string RegisteredLevelState(std::string_view descriptor){std::lock_guard lock(entry_mutex);const auto key=Normalize(descriptor);
  const auto found=entries.find(key);if(found!=entries.end())return found->second.state;
  const auto deferred=deferred_entries.find(key);return deferred==deferred_entries.end()?std::string{}:deferred->second.reservation.bindings.state;}
std::shared_ptr<LevelOriginalScripts> RegisteredLevelScripts(std::string_view value){std::lock_guard lock(entry_mutex);
  const auto key=Normalize(value);const auto found=entries.find(key);if(found!=entries.end())return found->second.scripts;
  for(const auto& [descriptor,entry]:entries)if(entry.state==key)return entry.scripts;
  return {};
}
std::shared_ptr<const LevelCollisionPlan> RegisteredLevelCollision(std::string_view state){std::lock_guard lock(entry_mutex);
 for(const auto& [name,entry]:entries)if(entry.state==state)return entry.collision;
 return {};
}
std::shared_ptr<const std::vector<NavigationDatabase>> RegisteredLevelNavigation(std::string_view descriptor){std::lock_guard lock(entry_mutex);
  const auto found=entries.find(Normalize(descriptor));return found==entries.end()?nullptr:found->second.navigation;}
std::shared_ptr<RegisteredBlueprintLibrary> RegisteredLevelBlueprints(std::string_view descriptor){std::lock_guard lock(entry_mutex);
  const auto key=Normalize(descriptor);const auto found=entries.find(key);if(found!=entries.end())return found->second.blueprints;
  for(const auto& [name,entry]:entries)if(entry.state==key)return entry.blueprints;
  return {};
}
RegisteredLevelActorScope::RegisteredLevelActorScope(std::shared_ptr<RegisteredBlueprintLibrary> value):previous_(std::move(actor_scope)){actor_scope=std::move(value);}
RegisteredLevelActorScope::~RegisteredLevelActorScope(){actor_scope=std::move(previous_);}
bool IsScopedRegisteredLevelActor(uint8_t* base,uint32_t entity){
  if(!actor_scope || actor_scope->guest_base!=base || entity<4096 || uint64_t(entity)+20>0x100000000ull)return false;
  std::array<uint32_t,4> id{};SIZE_T read=0;
  if(!ReadProcessMemory(GetCurrentProcess(),base+entity+4,id.data(),sizeof(id),&read) || read!=sizeof(id))return false;
  for(auto& word:id)word=_byteswap_ulong(word);
  return actor_scope->index.contains(id);
}
bool BeginRegisteredLevelScripts(uint8_t* base,std::string_view state,std::string& error){
  auto scripts=RegisteredLevelScripts(state);if(!scripts){error.clear();return true;}
  if(active_scripts){error="Previous original script world still owns native state";return false;}
  if(!BeginRegisteredScriptEvents(base,state,RegisteredLevelBlueprints(state),scripts,error))return false;
  active_scripts=std::move(scripts);return true;
}
bool CommitRegisteredLevelScripts(uint8_t* base,std::string& error){
  if(!active_scripts){error.clear();return true;}return CommitRegisteredScriptEvents(base,error);
}
bool EndRegisteredLevelScripts(PPCContext& ctx,uint8_t* base,std::string& error){
  if(!EndRegisteredScriptEvents(error))return false;
  if(active_scripts && !RetainNativeGlobalScripts(ctx,base,active_scripts->state)){
    error="Original script world retained native globals/cutscene ownership";return false;
  }
  active_scripts.reset();error.clear();return true;
}
void TickRegisteredLevelScripts(PPCContext& ctx,uint8_t* base,double delta){
  if(!active_scripts || !RegisteredScriptEventsReady())return;
  std::string error;
  if(!UpdateRegisteredScriptEvents(base,error)){
    REXLOG_ERROR("[em1] original script world update stopped: {}",error);
    EndRegisteredLevelScripts(ctx,base,error);return;
  }
  TickNativeGlobalScripts(ctx,base,active_scripts->state,delta);
  if(active_scripts->state.native_globals_failed){
    REXLOG_ERROR("[em1] original event admission stopped after native script service failure");
    if(!EndRegisteredLevelScripts(ctx,base,error))
      REXLOG_ERROR("[em1] original script service cleanup retained ownership: {}",error);
  }
}
bool RetainRegisteredLevelBlueprints(PPCContext& ctx,uint8_t* base,std::string_view descriptor,std::string& error){
  auto* kernel=rex::kernel::kernel_state();if(!kernel || !kernel->memory() || !base){error="Native blueprint allocator unavailable";return false;}
  auto library=std::make_shared<RegisteredBlueprintLibrary>();library->guest_base=base;
  {std::lock_guard lock(entry_mutex);const auto found=entries.find(Normalize(descriptor));
    if(found==entries.end()){error="Original level is not registered";return false;}
    if(found->second.blueprints){
      if(found->second.blueprints->guest_base!=base){error="Blueprint library belongs to another native memory instance";return false;}
      error.clear();return true;
    }
    for(const auto& assets:found->second.model_assets)library->scenes.push_back(assets.bound_scene);
    for(const auto& archive:found->second.archives)library->scene_names.push_back(archive.native_scene);
  }
  size_t total_bytes=0,total_definitions=0;
  for(const auto& scene:library->scenes){total_bytes+=scene.bytes.size()+kParserProbeScratchSize;total_definitions+=scene.entity_count;}
  if(total_bytes>256ull*1024*1024 || total_definitions>10000){error="Retained level blueprint library exceeds memory/definition budget";return false;}
  auto release=[&]{ReleaseNativeBlueprints(ctx,base,library->definitions);for(auto input:library->inputs)kernel->memory()->SystemHeapFree(input);library->inputs.clear();};
  for(const auto& scene:library->scenes){const auto size=kParserProbeScratchSize+scene.bytes.size();
    if(size>64*1024*1024+kParserProbeScratchSize){error="Bound scene exceeds native blueprint budget";release();return false;}
    const auto input=kernel->memory()->SystemHeapAlloc(uint32_t(size),32);
    if(!input){error="Bound blueprint input allocation failed";release();return false;}
    library->inputs.push_back(input);std::memset(base+input,0,size);NativeBlueprintReport report;
    if(!ReadNativeSceneBlueprintDefinitions(ctx,base,input,size,scene,report,error,library->definitions)){release();return false;}
  }
  NativeInheritanceReport report;
  if(!LinkNativeBlueprintInheritance(ctx,base,library->definitions,report,error)){release();return false;}
  for(auto definition:library->definitions){std::array<uint32_t,4> id{};for(unsigned i=0;i<4;++i)id[i]=PPC_LOAD_U32(definition+4+i*4);library->index.try_emplace(id,definition);}
  {std::lock_guard lock(entry_mutex);entries.at(Normalize(descriptor)).blueprints=library;}
  REXLOG_INFO("[em1] bound source blueprint library retained: level='{}' scenes={} definitions={} unique={} duplicates={}; native scoped masters and staged model names, no world admission",descriptor,library->scenes.size(),library->definitions.size(),report.unique,report.duplicates);
  error.clear();return true;
}
std::shared_ptr<RegisteredBlueprintLibrary> MatchRegisteredSceneBlueprints(uint8_t* base,uint32_t input,uint32_t size){
  if(!base || input<4096 || !size || size>64*1024*1024 || uint64_t(input)+size>0x100000000ull)return {};
  // Before touching a foreign/native scene buffer, validate its entire range.
  auto current=reinterpret_cast<uintptr_t>(base+input),end=current+size;
  while(current<end){MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(reinterpret_cast<void*>(current),&info,sizeof(info)) || info.State!=MEM_COMMIT || (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return {};
    const auto next=reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;if(next<=current)return {};current=std::min(end,next);
  }
  std::lock_guard lock(entry_mutex);
  for(const auto& [name,entry]:entries){const auto& library=entry.blueprints;if(!library || library->guest_base!=base)continue;
    for(const auto& scene:library->scenes)if(scene.bytes.size()==size && !std::memcmp(base+input,scene.bytes.data(),size))return library;
  }return {};
}
void SetRegisteredParserBlueprints(uint32_t parser,std::shared_ptr<RegisteredBlueprintLibrary> library){std::lock_guard lock(entry_mutex);
  if(library)parser_blueprints[parser]=std::move(library);else parser_blueprints.erase(parser);
}
std::shared_ptr<RegisteredBlueprintLibrary> RegisteredParserBlueprints(uint32_t parser){std::lock_guard lock(entry_mutex);
  const auto found=parser_blueprints.find(parser);return found==parser_blueprints.end()?nullptr:found->second;
}
bool CanEnterRegisteredLevel(uint8_t* base,std::string_view descriptor,std::string& error){
  std::lock_guard lock(entry_mutex);const auto found=entries.find(Normalize(descriptor));
  if(found==entries.end()){error="The original level was not registered during startup";return false;}
  if(!found->second.reservation_verified){error="Prepared level resources have not passed catalog verification";return false;}
  if(!found->second.blueprints || found->second.blueprints->guest_base!=base){error="Bound source blueprint library is not retained";return false;}
  std::map<std::string,uint32_t> factories;if(!ReadNativeComponentFactories(base,factories,error))return false;
  std::set<std::string> missing;
  for(const auto& scene:found->second.contents.entry_scenes){std::vector<SceneFileReference> files;std::vector<std::string> classes;
    if(!CollectSceneFileReferences(scene.resource.scene,files,error,false,&classes))return false;
    for(const auto& name:classes)if(!factories.contains(name))missing.insert(name);
  }
  if(!missing.empty()){error="Native level admission requires component adapters:";for(const auto& name:missing)error+=" "+name;return false;}
  if(REXCVAR_GET(em1_probe_level_entry)){
    REXLOG_WARN("[em1] INTERNAL normal level entry probe: '{}' has UNPROVEN platform/audio readiness; attempting complete registered descriptor to identify actual loader failure, not claiming playable support",descriptor);
    error.clear();return true;
  }
  for(const auto& part:found->second.contents.plan.partitions)if(!part.audio.empty()){
    error="Original level audio has no native bank owner yet";return false;
  }
  error="Original level platform-resource publication has not completed";return false;
}
void MaybeProbeRegisteredLevelEntry(){
  struct Restore{DWORD error=GetLastError();unsigned csr=_mm_getcsr();~Restore(){SetLastError(error);_mm_setcsr(csr);}} restore;
  if(REXCVAR_GET(em1_validate_collision_reload) && REXCVAR_GET(em2_private_profile) &&
      REXCVAR_GET(em1_probe_level_entry) && probe_requested.load(std::memory_order_acquire) && NativeConsoleWorldReady()){
    static bool queued=false;static std::optional<uint64_t> ready_step;
    const auto step=simulation_steps.load(std::memory_order_relaxed);
    if(!ready_step)ready_step=step;
    if(!queued && step>=*ready_step && step-*ready_step>=300){
      queued=true;const auto descriptor=REXCVAR_GET(em1_register_level);
      const auto result=ExecuteGameplayConsoleTool({"level.load","em1:"+descriptor});
      REXLOG_INFO("[em1] INTERNAL collision lifecycle reload: descriptor='{}' result='{}'; one-shot engine command, no gameplay input",descriptor,result.value_or("Command was not recognized"));
    }
  }
  if(!REXCVAR_GET(em1_probe_level_entry))return;
  if(probe_requested.load(std::memory_order_acquire))return;
  static bool attempted=false;if(attempted || !NativeConsoleWorldReady())return;
  // The title scene already owns player-like entities. Wait for the actual
  // checkpoint Continue, rather than interrupting profile/menu startup.
  if(REXCVAR_GET(em2_auto_continue) && !OfflineStartupContinueAccepted())return;
  static std::optional<uint64_t> campaign_step;
  const auto step=simulation_steps.load(std::memory_order_relaxed);
  if(!campaign_step){campaign_step=step;return;}
  if(step<*campaign_step || step-*campaign_step<30)return;
  const auto descriptor=REXCVAR_GET(em1_register_level);
  if(descriptor.empty() || RegisteredLevelState(descriptor).empty())return;
  attempted=true;
  const auto result=ExecuteGameplayConsoleTool({"level.load","em1:"+descriptor});
  probe_requested=result && result->starts_with("Queued original .level");
  REXLOG_INFO("[em1] INTERNAL full level entry command: '{}' result='{}'; no gameplay input sent",descriptor,result.value_or("Command was not recognized"));
}
bool RegisteredLevelEntryProbeRequested(){return probe_requested.load(std::memory_order_acquire);}
void ObserveRegisteredLevelEntryRequest(std::string_view state){
  if(!REXCVAR_GET(em1_probe_level_entry))return;
  std::lock_guard lock(entry_mutex);
  for(const auto& [descriptor,entry]:entries)if(entry.state==state){
    probe_requested.store(true,std::memory_order_release);
    REXLOG_INFO("[em1] full registered level reached native engine loader: '{}'; platform/audio readiness remains unproven",state);
    return;
  }
}
}
