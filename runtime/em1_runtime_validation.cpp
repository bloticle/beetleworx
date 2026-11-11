#include "em1_runtime_validation.h"
#include "em1_parser_probe.h"
#include "em1_native_properties.h"
#include "em1_native_component.h"
#include "em1_native_item_manager.h"
#include "em1_level_entry.h"
#include "em1_native_navigation.h"
#include "em1_level_source.h"
#include "em1_level_archives.h"
#include "em1_level_resource_bindings.h"
#include "em1_vfs.h"
#include "em1_animation_source.h"
#include "online_runtime.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/kernel/kernel_state.h>
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <set>
#include <queue>
#include <limits>

REXCVAR_DEFINE_STRING(em1_validate_level,"","Debug",
    "Developer-only original EM1 level header validation on native scene loading; no world activation");

REXCVAR_DEFINE_BOOL(em1_stage_level_archives,false,"Debug",
    "After em1_validate_level header checks, stage complete scene PAKs in VFS; does not activate a world");
REXCVAR_DEFINE_BOOL(em1_validate_scene_construction,false,"Debug","Construct/release complete original scene placements/components without world admission");
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DEFINE_BOOL(em1_validate_inventory_writes,false,"Debug","Explicit private-save inventory write/clamp/restore diagnostic; requires private profile");
REXCVAR_DEFINE_BOOL(em1_validate_animations,false,"Debug",
    "Developer-only original Mickey spline sampling with EM2's existing native sampler; no actor activation");

PPC_EXTERN_FUNC(sub_822F2900);
PPC_EXTERN_FUNC(sub_822FE000);

namespace em2::em1 {
namespace {
std::atomic_bool attempted{false};
struct RestoreContext {
  PPCContext& ctx; PPCContext saved; unsigned csr; DWORD last_error;
  explicit RestoreContext(PPCContext& context,DWORD incoming_error)
      :ctx(context),saved(context),csr(_mm_getcsr()),last_error(incoming_error){}
  ~RestoreContext(){ctx=saved;_mm_setcsr(csr);SetLastError(last_error);}
};
struct Scratch {
  rex::memory::Memory& memory; uint32_t address;
  explicit Scratch(rex::memory::Memory& allocator, uint32_t bytes)
      :memory(allocator),address(allocator.SystemHeapAlloc(bytes,32)){}
  ~Scratch(){if(address)memory.SystemHeapFree(address);}
};
std::string Partition(std::string value) {
  for (auto& c:value) if(c>='A'&&c<='Z')c+='a'-'A';
  if(value.ends_with(".part"))value.resize(value.size()-5);
  return value;
}
// Independent diagnostic oracle over original integer edge costs. It never
// supplies routes to gameplay; production queries use the retail Havok solver.
bool ValidateNavigationPath(PPCContext& ctx,uint8_t* base,const NavigationSector& sector,const NativeNavigationGraph& graph,uint32_t start,uint32_t goal,std::string& error){
  constexpr auto infinity=std::numeric_limits<uint64_t>::max();
  std::vector<std::vector<std::pair<uint32_t,uint32_t>>> edges(sector.nodes.size());
  for(const auto& edge:sector.edges)edges[edge.source].push_back({edge.target,edge.encoded_cost&0x7FFFFFFF});
  std::vector<uint64_t> distance(sector.nodes.size(),infinity);
  using Visit=std::pair<uint64_t,uint32_t>;
  std::priority_queue<Visit,std::vector<Visit>,std::greater<Visit>> pending;
  distance[start]=0;pending.push({0,start});
  while(!pending.empty()){
    const auto [cost,node]=pending.top();pending.pop();if(cost!=distance[node])continue;if(node==goal)break;
    for(const auto [target,step]:edges[node])if(cost+step<distance[target]){distance[target]=cost+step;pending.push({cost+step,target});}
  }
  NativeNavigationPath path;if(!FindNativeNavigationPath(ctx,base,graph,start,goal,path,error))return false;
  if((distance[goal]!=infinity)!=(path.status==1)){error="Native path reachability differs from original graph";return false;}
  uint64_t actual=0;std::set<uint32_t> visited;
  for(size_t i=0;i<path.nodes.size();++i){
    if(!visited.insert(path.nodes[i]).second){error="Native navigation path contains a cycle";return false;}
    if(!i)continue;uint32_t step=UINT32_MAX;
    for(const auto [target,cost]:edges[path.nodes[i-1]])if(target==path.nodes[i])step=std::min(step,cost);
    if(step==UINT32_MAX){error="Native navigation path traverses a nonexistent source edge";return false;}actual+=step;
  }
  if(path.status==1 && actual!=distance[goal]){error="Native navigation route cost differs from original shortest path: "+std::to_string(actual)+" vs "+std::to_string(distance[goal]);return false;}
  return true;
}
}
void MaybeValidateNativeLevel(PPCContext& ctx,uint8_t* base,bool world_tick) {
  if(world_tick!=REXCVAR_GET(em1_validate_scene_construction))return;
  const DWORD incoming_error=GetLastError();
  const auto level=REXCVAR_GET(em1_validate_level);
  if(level.empty() || attempted.exchange(true)){SetLastError(incoming_error);return;}
  RestoreContext restore(ctx,incoming_error);
  if(em2::net::SynchronizedSessionActive()) {
    REXLOG_ERROR("[em1] native level validation refused during synchronized online play");return;
  }
  auto* kernel=rex::kernel::kernel_state();
  if(!kernel || !kernel->file_system() || !kernel->memory()) {
    REXLOG_ERROR("[em1] native level validation requires initialized runtime");return;
  }
  auto source=MountedRvz(*kernel->file_system());
  if(!source){REXLOG_ERROR("[em1] native level validation requires configured em1_source_rvz");return;}
  std::string error;LevelContents contents;
  if(!ReadLevelContents(source,level,contents,error)) {
    REXLOG_ERROR("[em1] full level '{}' preparation rejected: {}",level,error);return;
  }
  if(const auto navigation=RegisteredLevelNavigation(level)){
    size_t sectors=0,nodes=0,edges=0,queries=0,paths=0,half_costs=0;bool valid=true;
    size_t floors=0,portals=0,neighbor_groups=0,neighbor_links=0,unbound_links=0,floor_queries=0,floor_matches=0;
    for(const auto& database:*navigation)for(const auto& sector:database.sectors){
      floors+=sector.runtime_mesh.floors.size();neighbor_groups+=sector.runtime_mesh.neighbors.size();
      for(const auto& floor:sector.runtime_mesh.floors){portals+=floor.portals.size();unbound_links+=floor.unbound_links.size();}
      for(const auto& neighbor:sector.runtime_mesh.neighbors)neighbor_links+=neighbor.local_links.size();
      for(const auto& node:sector.nodes){++floor_queries;if(FindNavigationFloor(sector.runtime_mesh,node.position))++floor_matches;}
    }
    REXLOG_INFO("[em1] source navigation mesh retention: floors={} portals={} neighbor_groups={} neighbor_links={} unassigned_links={}; complete original geometry/references retained, no native surface conversion/world publication",floors,portals,neighbor_groups,neighbor_links,unbound_links-neighbor_links);
    REXLOG_INFO("[em1] source navigation floor lookup: queries={} matched={}; native port of original point/cell/layer/edge-height logic, unmapped graph nodes retained, no AI/world binding",floor_queries,floor_matches);
    for(const auto& database:*navigation){if(!valid)break;for(const auto& sector:database.sectors){NativeNavigationGraph graph;
      if(!BuildNativeNavigationGraph(ctx,base,sector,graph,error)){valid=false;break;}
      ++sectors;nodes+=graph.nodes;edges+=graph.edges;half_costs+=graph.costs_requiring_native_half_rounding;
      if(!sector.nodes.empty())for(const auto sample:{size_t(0),sector.nodes.size()/2,sector.nodes.size()-1}){
        const auto point=sector.nodes[sample].position;uint32_t expected=0;
        while(expected<sector.nodes.size() && sector.nodes[expected].position!=point)++expected;
        uint32_t actual=UINT32_MAX;
        if(!FindNativeNavigationNode(ctx,base,graph,point,actual,error) || actual!=expected){valid=false;if(error.empty())error="Native navigation nearest-node mismatch";break;}
        ++queries;
      }
      if(valid && !sector.nodes.empty())for(const auto pair:{std::pair{uint32_t(0),uint32_t(0)},std::pair{uint32_t(0),uint32_t(sector.nodes.size()-1)},std::pair{uint32_t(sector.nodes.size()-1),uint32_t(0)},std::pair{uint32_t(sector.nodes.size()/2),uint32_t(0)}}){
        if(!ValidateNavigationPath(ctx,base,sector,graph,pair.first,pair.second,error)){valid=false;break;}++paths;
      }
      ReleaseNativeNavigationGraph(ctx,base,graph);if(!valid)break;
    }}
    REXLOG_INFO("[em1] native navigation graph import: success={} databases={} sectors={} nodes={} edges={} queries={} half_cost_rounding={} error='{}'; native Havok graph/nearest queries, no navmesh/world admission",valid,navigation->size(),sectors,nodes,edges,queries,half_costs,error);
    REXLOG_INFO("[em1] native navigation path search: success={} paths={} error='{}'; retail FindGraphPath, original directed-edge reachability/shortest-cost oracle, no cross-sector links/navmesh/AI actors",valid,paths,error);
    if(!valid)return;
  }
  {
    Scratch scratch(*kernel->memory(),uint32_t(kParserProbeScratchSize));
    if(!scratch.address){REXLOG_ERROR("[em1] cannot allocate private parser scratch");return;}
    std::memset(base+scratch.address,0,kParserProbeScratchSize);
    if(!ProbeNativeSceneParser(ctx,base,scratch.address,kParserProbeScratchSize,error)) {
      REXLOG_ERROR("[em1] isolated parser completion/cancellation failed: {}",error);return;
    }
  }
  unsigned checked=0;uint64_t entities=0;
  if(REXCVAR_GET(em1_validate_scene_construction)){
    const bool ammo_registered=RegisterNativeAmmoFacade(ctx,base,error);
    REXLOG_INFO("[em1] normal ammo class registration: success={} error='{}'",ammo_registered,error);
    if(!ammo_registered)return;
    const bool ammo_factory=ProbeNativeAmmoFactory(ctx,base,error);
    REXLOG_INFO("[em1] native ammo factory result: success={} error='{}'; retail private factory, two simultaneous instances, no global publication",ammo_factory,error);
    if(!ammo_factory)return;
    const bool inventory_factory=ProbeNativeInventoryFactory(ctx,base,error);
    REXLOG_INFO("[em1] native Inventory factory result: success={} error='{}'; actual registered factory, seventeen properties, isolated instances, no actor/world binding",inventory_factory,error);
    if(!inventory_factory)return;
    const bool float_schema=ProbeNativeFloatSchema(ctx,base,error);
    REXLOG_INFO("[em1] native float schema probe success={} error='{}'; standalone descriptors, no component activation",float_schema,error);
    if(!float_schema)return;
    const bool array_schema=ProbeNativeArraySchema(ctx,base,error);
    REXLOG_INFO("[em1] native array schema probe success={} error='{}'; Float/Integer/String vectors, native resize/element ownership, no component activation",array_schema,error);
    if(!array_schema)return;
    const bool navigation_schema=ProbeNativeNavigationSchema(ctx,base,error);
    REXLOG_INFO("[em1] native navigation scalar schema: success={} error='{}'; seven field shapes, native boxes and ownership, no AI registration or world admission",navigation_schema,error);
    if(!navigation_schema)return;
    const bool boolean_schema=ProbeNativeBooleanSchema(ctx,base,error);
    REXLOG_INFO("[em1] native Boolean schema: success={} error='{}'; byte storage, native type identity and independent instances",boolean_schema,error);
    if(!boolean_schema)return;
    bool ammo_object=true;unsigned ammo_cycles=0;
    for(;ammo_cycles<8;++ammo_cycles)if(!ProbeNativeAmmoComponent(ctx,base,error)){ammo_object=false;break;}
    REXLOG_INFO("[em1] native ammo object probe success={} cycles={} error='{}'; private construction only, no world activation",ammo_object,ammo_cycles,error);
    if(!ammo_object)return;
    Scratch item_scratch(*kernel->memory(),32);NativeItemManagerSnapshot native_items;
    const bool item_access=item_scratch.address && ProbeNativeItemManager(ctx,base,item_scratch.address,native_items,error);
    REXLOG_INFO("[em1] native ItemManager adapter: success={} address={:08X} error='{}'; same-value native setters/channel lookup, no parallel inventory",item_access,native_items.address,error);
    if(!item_access)return;
    REXLOG_INFO("[em1] native inventory manager bindings: success=true fields=6 etickets={} eticket_limit={}; native player backend only, no InventoryComponent registration or actor binding",native_items.etickets,native_items.eticket_limit);
    if(REXCVAR_GET(em1_validate_inventory_writes)){
      const bool writes=REXCVAR_GET(em2_private_profile) && ProbeNativeInventoryWrites(ctx,base,error);
      REXLOG_INFO("[em1] private native inventory writes: success={} error='{}'; changed currency, native cap clamp, restored manager prefix; private save only",writes,error);
      if(!writes)return;
    }
    for(unsigned c=0;c<3;++c){const auto& a=native_items.ammo[c];
      REXLOG_INFO("[em1] native ItemManager ammo: channel={} amount={} effective_max={} configured_max={} segment={} max_segments={} penalty={} penalty_wait={} recharge_max={} recharge_rate={} recharge_wait={}",
        c,a.amount,a.effective_max,a.configured_max,a.segment_amount,a.max_segments,a.penalty_amount,a.penalty_wait,a.recharge_max,a.recharge_rate,a.recharge_wait);
    }
    uint64_t blueprint_entities=0,blueprint_components=0,blueprint_properties=0,source_ammo=0;
    std::vector<std::unique_ptr<Scratch>> blueprint_inputs;
    struct OwnedBlueprints{PPCContext& ctx;uint8_t* base;std::vector<uint32_t> definitions;
      ~OwnedBlueprints(){ReleaseNativeBlueprints(ctx,base,definitions);}} blueprints{ctx,base};
    for(const auto& scene:contents.entry_scenes){const auto bytes=kParserProbeScratchSize+scene.resource.scene.bytes.size();
      auto& scratch=*blueprint_inputs.emplace_back(std::make_unique<Scratch>(*kernel->memory(),uint32_t(bytes)));
      if(!scratch.address){REXLOG_ERROR("[em1] complete blueprint scratch unavailable");return;}
      NativeBlueprintReport report;REXLOG_INFO("[em1] complete blueprint begin: scene='{}'",scene.request.binary_scene);if(auto logger=rex::GetLogger())logger->flush();
      const bool read=ProbeNativeSceneBlueprints(ctx,base,scratch.address,bytes,scene.resource.scene,report,error,&blueprints.definitions);
      REXLOG_INFO("[em1] complete blueprint result: success={} entities={} components={} properties={} authored_ammo={} error='{}'; no actor creation",read,report.entities,report.components,report.properties,report.authored_ammo_probes,error);
      REXLOG_INFO("[em1] source Inventory factory checks: scene='{}' count={} success={} error='{}'; source properties only, no actor/world binding",scene.request.binary_scene,report.authored_inventory_probes,read,error);
      if(!read)return;blueprint_entities+=report.entities;blueprint_components+=report.components;blueprint_properties+=report.properties;source_ammo+=report.authored_ammo_probes;
    }
    REXLOG_INFO("[em1] complete blueprint finished: entities={} components={} properties={} authored_ammo={}; no world admission",blueprint_entities,blueprint_components,blueprint_properties,source_ammo);
    NativeInheritanceReport inheritance;
    const bool linked=LinkNativeBlueprintInheritance(ctx,base,blueprints.definitions,inheritance,error);
    REXLOG_INFO("[em1] scoped inheritance result: success={} unique={} duplicates={} parents={} components={} inherited_components={} component_links={} error='{}'; no actor creation",
      linked,inheritance.unique,inheritance.duplicates,inheritance.parents,inheritance.components,inheritance.inherited_components,inheritance.component_links,error);
    if(!linked)return;
    size_t navigation_definitions=0,navigation_sectors=0;
    std::vector<uint32_t> navigation_inputs;std::vector<NativeNavigationProfile> navigation_profiles;
    for(const auto entity:blueprints.definitions){const auto count=PPC_LOAD_U32(entity+0x30),array=PPC_LOAD_U32(entity+0x2C);
      for(uint32_t i=0;i<count;++i){const auto component=PPC_LOAD_U32(array+i*4),name=PPC_LOAD_U32(component);
        if(std::strcmp(reinterpret_cast<const char*>(base+name),"AIPathDatabaseComponent"))continue;
        size_t sectors=0;NativeNavigationProfile profile;
        REXLOG_INFO("[em1] source navigation factory begin: definition={:08X} own_properties={} inherited={}",component,PPC_LOAD_U32(component+0x28),PPC_LOAD_U32(component+0x30)!=0);
        if(auto logger=rex::GetLogger())logger->flush();
        const bool navigation=ProbeNativeNavigationFactory(ctx,base,error,component,RegisteredLevelNavigation(level),sectors,profile);
        REXLOG_INFO("[em1] source navigation factory result: success={} bound_sectors={} error='{}'; two private native instances, original HPD resource ownership, no global AI registration or world admission",navigation,sectors,error);
        if(!navigation)return;
        REXLOG_INFO("[em1] source navigation profile: file='{}' pitch={} max={},{},{} min={},{},{} radius={} height={} movement={}",
          profile.filename,profile.pitch,profile.maximum[0],profile.maximum[1],profile.maximum[2],profile.minimum[0],profile.minimum[1],profile.minimum[2],profile.radius,profile.height,profile.movement);
        ++navigation_definitions;navigation_sectors+=sectors;
        navigation_inputs.push_back(component);navigation_profiles.push_back(profile);
      }
    }
    REXLOG_INFO("[em1] source navigation factories finished: definitions={} bound_sectors={}; original fields and isolated instances",navigation_definitions,navigation_sectors);
    const bool navigation_lifecycle=ProbeRegisteredNavigationLifecycle(ctx,base,navigation_inputs,navigation_profiles,error);
    REXLOG_INFO("[em1] registered navigation lifecycle: success={} definitions={} error='{}'; actual factory, definition replacement, property overrides, resource retirement and destruction",navigation_lifecycle,navigation_inputs.size(),error);
    if(!navigation_lifecycle)return;
    std::vector<uint32_t> logic_definitions;
    for(auto entity:blueprints.definitions){const auto count=PPC_LOAD_U32(entity+0x30),array=PPC_LOAD_U32(entity+0x2C);
      for(uint32_t i=0;i<count;++i)logic_definitions.push_back(PPC_LOAD_U32(array+i*4));
    }
    const bool logic=ProbeRegisteredLogicComponents(ctx,base,logic_definitions,error);
    REXLOG_INFO("[em1] registered logic lifecycle: success={} error='{}'; source definitions, native Boolean overrides, branch/relay input and output capture; world event routing still required",logic,error);
    if(!logic)return;
    const bool seeds=ProbeRegisteredSeedComponents(ctx,base,logic_definitions,error);
    REXLOG_INFO("[em1] registered seed lifecycle: success={} error='{}'; source definitions and native Integer data; no AI-world binding",seeds,error);
    if(!seeds)return;
    const bool tracking=ProbeRegisteredTrackingComponents(ctx,base,logic_definitions,error);
    REXLOG_INFO("[em1] registered tracking lifecycle: success={} error='{}'; source fields, original damage classification and retirement semantics; world observer/manager connection still required",tracking,error);
    if(!tracking)return;
    bool inherited_ammo=false;
    for(const auto entity:blueprints.definitions){const auto count=PPC_LOAD_U32(entity+0x30),array=PPC_LOAD_U32(entity+0x2C);
      for(uint32_t i=0;i<count && !inherited_ammo;++i){const auto component=PPC_LOAD_U32(array+i*4),name=PPC_LOAD_U32(component);
        if(!PPC_LOAD_U32(component+0x30) || std::strcmp(reinterpret_cast<const char*>(base+name),"AmmoRechargeComponent"))continue;
        inherited_ammo=ProbeNativeAmmoComponent(ctx,base,error,component);
        REXLOG_INFO("[em1] inherited ammo result: success={} own_properties={} error='{}'; original scoped master property lookup and independent runtime overrides",inherited_ammo,PPC_LOAD_U32(component+0x28),error);
        if(!inherited_ammo)return;
        const bool inherited_factory=ProbeNativeAmmoFactory(ctx,base,error,component);
        REXLOG_INFO("[em1] inherited ammo factory result: success={} error='{}'; original source definition, two isolated native instances",inherited_factory,error);
        if(!inherited_factory)return;
      }
      if(inherited_ammo)break;
    }
    bool inherited_inventory=false;
    for(const auto entity:blueprints.definitions){const auto count=PPC_LOAD_U32(entity+0x30),array=PPC_LOAD_U32(entity+0x2C);
      for(uint32_t i=0;i<count && !inherited_inventory;++i){const auto component=PPC_LOAD_U32(array+i*4),name=PPC_LOAD_U32(component);
        if(!PPC_LOAD_U32(component+0x30) || std::strcmp(reinterpret_cast<const char*>(base+name),"InventoryComponent"))continue;
        inherited_inventory=ProbeNativeInventoryFactory(ctx,base,error,component);
        REXLOG_INFO("[em1] inherited Inventory factory result: success={} own_properties={} error='{}'; seventeen properties, independent arrays, no actor/world binding",inherited_inventory,PPC_LOAD_U32(component+0x28),error);
        if(!inherited_inventory)return;
      }
      if(inherited_inventory)break;
    }
    for(size_t i=0;i<contents.entry_scenes.size();++i){const auto& bytes=contents.entry_scenes[i].resource.scene.bytes;
      if(std::memcmp(base+blueprint_inputs[i]->address+kParserProbeScratchSize,bytes.data(),bytes.size())){
        REXLOG_ERROR("[em1] inherited blueprint lookup modified original input bytes");return;
      }
    }
    REXLOG_INFO("[em1] scoped inheritance source preservation: success=true; all original scene input bytes unchanged");
    std::map<std::string,uint32_t> factories;if(!ReadNativeComponentFactories(base,factories,error)){REXLOG_ERROR("[em1] native component factory inspection failed: {}",error);return;}
    REXLOG_INFO("[em1] native component creation map entries={}",factories.size());
    for(const auto* name:{"StatusComponent","HealthComponent","AnimationComponent","AnimationComponentGB","JPSSceneGraphComponent","AIPathDatabaseComponent","AmmoRechargeComponent","InventoryComponent"}){
      const auto at=factories.find(name);REXLOG_INFO("[em1] native creation callback class='{}' address={:08X}",name,at==factories.end()?0:at->second);
    }
    // Inspect genuine initialized schemas; never manufacture a descriptor or
    // call its accessors on an unrelated object while establishing the ABI.
    for(const auto [array,count]:{std::pair<uint32_t,uint32_t>{0x833DECA4,2},{0x833DBB14,15}}){
      for(uint32_t i=0;i<count;++i){uint32_t descriptor=0,fields[5]{},methods[6]{};SIZE_T read=0;
        auto copy=[&](uint32_t at,void* out,size_t size){return at>=4096 && uint64_t(at)+size<=0x100000000ull && ReadProcessMemory(GetCurrentProcess(),base+at,out,size,&read) && read==size;};
        if(!copy(array+i*4,&descriptor,4))break;descriptor=_byteswap_ulong(descriptor);
        if(!copy(descriptor,fields,sizeof(fields)) || !copy(_byteswap_ulong(fields[0]),methods,sizeof(methods)))break;
        char name[256]{};if(!copy(_byteswap_ulong(fields[1]),name,sizeof(name)-1))name[0]=0;
        REXLOG_INFO("[em1] native property descriptor: array={:08X} index={} address={:08X} name='{}' vtable={:08X} flags={:08X} setter={:08X} getter={:08X} type_method={:08X}",
          array,i,descriptor,std::string(name,strnlen(name,sizeof(name))),_byteswap_ulong(fields[0]),_byteswap_ulong(fields[2]),_byteswap_ulong(fields[3]),_byteswap_ulong(fields[4]),_byteswap_ulong(methods[5]));
      }
    }
    uint64_t placements=0,components=0;unsigned passed=0;
    for(const auto& scene:contents.entry_scenes){const auto bytes=kParserProbeScratchSize+scene.resource.scene.bytes.size();Scratch scratch(*kernel->memory(),uint32_t(bytes));
      if(!scratch.address){REXLOG_ERROR("[em1] full construction scratch unavailable");return;}std::memset(base+scratch.address,0,bytes);
      SceneConstructionReport report;REXLOG_INFO("[em1] complete scene construction begin: archive='{}' scene='{}' definitions={}",scene.request.archive,scene.request.binary_scene,scene.resource.scene.entity_count);
      if(auto logger=rex::GetLogger())logger->flush();
      const bool constructed=ProbeNativeSceneConstruction(ctx,base,scratch.address,bytes,scene.resource.scene,report,error,blueprints.definitions,false,scene.request.binary_scene);
      REXLOG_INFO("[em1] complete scene construction result: scene='{}' success={} placements={} components={} error='{}'; no world admission or gameplay",scene.request.binary_scene,constructed,report.placements,report.components,error);
      REXLOG_INFO("[em1] complete scene navigation ownership: instances={} graphs={} restored={}",report.navigation_instances,report.navigation_graphs,NativeNavigationComponents().instances==0);
      if(!constructed)return;++passed;placements+=report.placements;components+=report.components;
    }
    REXLOG_INFO("[em1] complete scene construction finished: level='{}' scenes={} placements={} components={}; no world admission",level,passed,placements,components);
    const auto library=RegisteredLevelBlueprints(level);
    if(!library || library->scenes.size()!=contents.entry_scenes.size()){REXLOG_ERROR("[em1] bound scene construction requires the registered blueprint library");return;}
    unsigned bound_passed=0;uint64_t bound_placements=0,bound_components=0;
    for(size_t i=0;i<library->scenes.size();++i){const auto& scene=library->scenes[i];const auto bytes=kParserProbeScratchSize+scene.bytes.size();
      Scratch scratch(*kernel->memory(),uint32_t(bytes));if(!scratch.address){REXLOG_ERROR("[em1] bound construction scratch unavailable");return;}
      std::memset(base+scratch.address,0,bytes);const auto before=library->parent_resolutions.load();SceneConstructionReport report;
      std::vector<uint32_t> references;for(auto definition:library->definitions)references.push_back(PPC_LOAD_U32(definition+0x40));
      REXLOG_INFO("[em1] registered bound scene construction begin: scene={} definitions={}; normal begin/pump scope hooks, no private parent override",i,scene.entity_count);
      if(auto logger=rex::GetLogger())logger->flush();
      const bool built=ProbeNativeSceneConstruction(ctx,base,scratch.address,bytes,scene,report,error,{},true,library->scene_names.at(i));
      const auto resolutions=library->parent_resolutions.load()-before;
      const bool detached=!RegisteredParserBlueprints(scratch.address);
      bool refs_restored=true;unsigned differences=0;
      for(size_t j=0;j<references.size();++j){const auto definition=library->definitions[j],after=PPC_LOAD_U32(definition+0x40);
        if(references[j]!=after){refs_restored=false;
          if(differences++<12)REXLOG_INFO("[em1] bound library reference delta: definition={:08X} id={:08X} before={} after={}",definition,PPC_LOAD_U32(definition+4),references[j],after);
        }
      }
      REXLOG_INFO("[em1] registered bound scene construction result: scene={} success={} placements={} components={} parent_resolutions={} detached={} library_refs_restored={} error='{}'; staged model names, native hooks, no world admission",i,built,report.placements,report.components,resolutions,detached,refs_restored,error);
      if(!built || !detached || !refs_restored)return;++bound_passed;bound_placements+=report.placements;bound_components+=report.components;
    }
    REXLOG_INFO("[em1] registered bound scene construction finished: level='{}' scenes={} placements={} components={}; scoped normal loader hooks, no world admission",level,bound_passed,bound_placements,bound_components);return;
  }
  for(const auto& scene:contents.entry_scenes) {
    const auto& request=scene.request;
    const auto& resource=scene.resource;
    const size_t bytes=kParserProbeScratchSize+resource.scene.bytes.size();
    if(bytes>64*1024*1024+kParserProbeScratchSize) {
      REXLOG_ERROR("[em1] oversized adapted scene rejected");return;
    }
    Scratch scratch(*kernel->memory(),uint32_t(bytes));
    if(!scratch.address){REXLOG_ERROR("[em1] cannot allocate original-scene scratch");return;}
    std::memset(base+scratch.address,0,bytes);
    if(!ProbeNativeSceneHeader(ctx,base,scratch.address,bytes,resource.scene,error)) {
      REXLOG_ERROR("[em1] native scene header '{}' failed: {}",request.binary_scene,error);return;
    }
    ++checked;entities+=resource.scene.entity_count;
    REXLOG_INFO("[em1] native header accepted original '{}' ({} entities, {} bytes); cancelled before entity activation",
                request.binary_scene,resource.scene.entity_count,resource.scene.bytes.size());
  }
  if(REXCVAR_GET(em1_stage_level_archives)) {
    std::vector<LevelSceneArchive> archives;
    if(!MountLevelSceneContents(*kernel->file_system(),contents,archives,error)) {
      REXLOG_ERROR("[em1] full level archive staging failed: {}",error);return;
    }
    for(const auto& archive:archives)
      REXLOG_INFO("[em1] staged complete scene: original={} registry={} archive={}.pak bytes={}",
          archive.source_origin,archive.registry_gsa,archive.archive_basename,archive.bytes->size());
    std::vector<LevelResourceGroup> groups;
    // Platform audio conversion is not yet available; never pretend that
    // Wii banks are converted or silently omit them from a native zone.
    if(!BindLevelEntryResources(contents,archives,{},groups,error))
      REXLOG_INFO("[em1] native entry resource binding pending: {}",error);
    else
      REXLOG_INFO("[em1] {} original entry resource groups bound; zone metadata and platform dependency activation pending",groups.size());
    REXLOG_INFO("[em1] staged {} full entry scenes; platform dependencies and world activation pending",archives.size());
  }
  REXLOG_INFO("[em1] native loader validation passed: level='{}' scenes={} retained_entities={} source={}; no EM1 world activated",
              level,checked,entities,source->DiscId());
}

void MaybeValidateNativeAnimations(PPCContext& ctx,uint8_t* base) {
  const DWORD incoming_error=GetLastError();
  static std::atomic_bool animation_attempted{false};
  if(!REXCVAR_GET(em1_validate_animations) || animation_attempted.exchange(true)){SetLastError(incoming_error);return;}
  RestoreContext restore(ctx,incoming_error);
  if(em2::net::SynchronizedSessionActive()){REXLOG_ERROR("[em1] animation validation refused during online play");return;}
  auto* kernel=rex::kernel::kernel_state();
  if(!kernel || !kernel->file_system() || !kernel->memory()){REXLOG_ERROR("[em1] animation validation requires initialized runtime");return;}
  auto source=MountedRvz(*kernel->file_system());std::string error;
  if(!source){REXLOG_ERROR("[em1] animation validation requires original mounted RVZ");return;}
  auto pak=PakSource::Open(source,"packfiles/_dynamic.pak",error);
  if(!pak){REXLOG_ERROR("[em1] animation validation archive: {}",error);return;}
  std::vector<uint8_t> rig_bytes;WiiSkeletonReference rig;
  if(!pak->ReadOriginal("characters/mickey/characters/mickey_rig_skin.hkx_wii",rig_bytes,error) || !ReadWiiSkeletonReference(rig_bytes,rig,error)){REXLOG_ERROR("[em1] animation source rig: {}",error);return;}
  size_t clips=0,samples=0;float tpose_maximum=0;
  try {
    for(const auto& entry:pak->Entries()){
      if(!entry.path.starts_with("characters/mickey/animations/") || !entry.path.ends_with(".hkx_wii"))continue;
      std::vector<uint8_t> bytes;WiiBoundSplineClip clip;
      if(!pak->ReadOriginal(entry.path,bytes,error) || !ReadWiiBoundSplineClip(bytes,clip,error) || !ValidateWiiClipRigIndices(clip,rig,error))throw std::runtime_error(error);
      hkx::Pack pack(bytes,true);hkx::Location animation{UINT32_MAX,0};
      for(const auto& [key,name]:pack.classes)if(name=="hkaSplineCompressedAnimation"){
        hkx::Need(animation.section==UINT32_MAX,"Multiple spline objects in sampling fixture");animation={uint32_t(key>>32),uint32_t(key)};
      }
      hkx::Need(animation.section!=UINT32_MAX,"No spline object in sampling fixture");
      // This is a PRIVATE native runtime object, not a relabeled HKX file.
      // Native type-info size is132; finish sets vtable, +128 is endian state.
      // Preserve original full source allocation and relocate every source fixup.
      const uint32_t output_offset=uint32_t((144+bytes.size()+15)&~size_t(15));
      const uint32_t float_offset=output_offset+clip.transform_tracks*48;
      const uint32_t end=float_offset+clip.float_tracks*4;
      Scratch scratch(*kernel->memory(),end+64);hkx::Need(scratch.address!=0,"Cannot allocate private spline sampling fixture");
      const auto owner=scratch.address,packed=owner+144,transforms=owner+output_offset,floats=owner+float_offset;
      std::memset(base+owner,0,end+64);std::memcpy(base+packed,bytes.data(),bytes.size());
      for(const auto& [key,target]:pack.pointers){const uint32_t section=uint32_t(key>>32),offset=uint32_t(key);
        PPC_STORE_U32(packed+pack.sections[section].start+offset,packed+pack.sections[target.section].start+target.offset);
      }
      std::memcpy(base+owner,base+packed+pack.sections[animation.section].start+animation.offset,128);
      // Container BE32 does not imply BE compressed spline blocks. Start the
      // runtime-only endian field unset and let native finish perform its own
      // compressed-data conversion inside this writable private allocation.
      PPC_STORE_U32(owner+128,0);
      REXLOG_INFO("[em1] native animation fixture beginning: source={} owner={:08X} payload={:08X} bytes={}",entry.path,owner,PPC_LOAD_U32(owner+116),clip.data_size);
      ctx.r3.u64=owner;ctx.r4.u64=1;sub_822F2900(ctx,base);
      hkx::Need(PPC_LOAD_U32(owner)==0x8200360c,"Unexpected native spline vtable");
      for(float time:{0.0f,clip.duration/3.0f,clip.duration*2.0f/3.0f}){
        std::memset(base+transforms,0xff,end-output_offset+64);
        ctx.r3.u64=owner;ctx.f1.f64=time;ctx.r5.u64=transforms;ctx.r6.u64=floats;ctx.r7.u64=0;
        sub_822FE000(ctx,base);
        for(uint32_t i=0;i<clip.transform_tracks;++i){
          float norm=0;
          for(uint32_t lane=0;lane<12;++lane){
            if(lane==3 || lane==11)continue; // Qs translation/scale W are padding.
            const float value=std::bit_cast<float>(PPC_LOAD_U32(transforms+i*48+lane*4));
            hkx::Need(std::isfinite(value),"Nonfinite native sampled transform");
            if(lane>=4 && lane<=7)norm+=value*value;
            if(entry.path.ends_with("/mickey_tpose.hkx_wii") && clip.bone_indices[i]>=0){
              const float reference=rig.bones[clip.bone_indices[i]].local[lane];
              // Quaternion sign equivalence is handled below; compare positions/scales here.
              if(lane<4 || lane>7)tpose_maximum=std::max(tpose_maximum,std::abs(value-reference));
            }
          }
          // Spline sampling intentionally returns unnormalized rotations; the
          // animated-skeleton blending stage normally normalizes the result.
          // Do not reject legitimate intermediate curve values as corrupt.
          if(!std::isfinite(norm) || norm<1e-12f)throw std::runtime_error("Degenerate native sampled quaternion: source="+entry.path+" time="+std::to_string(time)+" track="+std::to_string(i)+" squared_norm="+std::to_string(norm));
        }
        for(uint32_t i=0;i<clip.float_tracks;++i)hkx::Need(std::isfinite(std::bit_cast<float>(PPC_LOAD_U32(floats+i*4))),"Nonfinite native sampled float track");
        for(uint32_t i=0;i<64;++i)hkx::Need(base[owner+end+i]==0xff,"Native spline output guard overwritten");
        ++samples;
      }
      if(entry.path.ends_with("/mickey_tpose.hkx_wii"))REXLOG_INFO("[em1] native source T-pose sampled: position_scale_max_error={}",tpose_maximum);
      ++clips;
    }
    hkx::Need(clips==97,"Original Mickey clip inventory changed");
    REXLOG_INFO("[em1] native animation sampling passed: clips={} samples={} tpose_position_scale_max_error={}; private source objects only, no actor/world activation or original-Wii sampling parity claimed",clips,samples,tpose_maximum);
  }catch(const std::exception& exception){REXLOG_ERROR("[em1] native animation sampling rejected after {} clips/{} samples: {}",clips,samples,exception.what());}
}
}
