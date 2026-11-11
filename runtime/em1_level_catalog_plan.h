#pragma once
#include "em1_level_registry.h"
#include <cstdio>
namespace em2::em1 {
struct LevelCatalogScene {
 size_t scene_index=0;
 std::string archive_basename,registry_gsa,native_scene;
};
struct LevelCatalogReservation {
 std::string descriptor,prefix;
 LevelEntryBindings bindings;
 // Exactly ReadLevelContents' unique shared+entry-partition request order.
 std::vector<LevelScene> entry_scene_requests;
 std::vector<LevelCatalogScene> scenes;
 // Native archive filenames, no directory or bytes: <archive_basename>.pak.
 std::vector<std::string> expected_scene_archives;
 std::vector<uint32_t> group_ids;
 std::vector<CompiledLevel::Zone> zones;
 CompiledLevel::State entry_state;
 CompiledLevel::Destinations destination;
};
// Reservation only. Does not read/convert scenes, generate placeholder assets,
// mount PAKs or establish playability. Both outputs are unchanged on failure.
// Deferred preparation MUST pass BOTH prefix and group_ids to the native
// extension, then compare its produced metadata against this reservation.
inline bool AppendLevelCatalogPlan(const LevelPlan& plan,std::string_view reserved_prefix,
    CompiledLevel& catalog,LevelCatalogReservation& output,std::string& error){
 using namespace compiled_level_detail;
 try{
  Validate(catalog);Check(!catalog.trusted_measurements,"EM1 catalog requires untrusted native measurements");
  Check(!plan.origin_key.empty(),"Original level catalog plan lacks source ownership");
  std::string key=Key(plan.descriptor);if(key.starts_with("levels/"))key.erase(0,7);
  Check(key.ends_with(".level")&&key.size()>6&&key.size()<=120&&key.find("..") == key.npos&&
    key.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789.")==key.npos,"Invalid original catalog descriptor");
  Check(reserved_prefix.starts_with("em1_level_")&&reserved_prefix.size()>10&&reserved_prefix.size()<=64&&
    reserved_prefix.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789")==reserved_prefix.npos,"Invalid reserved level archive namespace");
  LevelCatalogReservation ready;ready.descriptor=key;ready.prefix=reserved_prefix;ready.bindings.level="em1_"+key.substr(0,key.size()-6);ready.bindings.state=ready.bindings.level+".entry";
  auto identity=[](const LevelScene& scene){return scene.archive+"\n"+scene.binary_scene;};
  // ReadLevelContents deduplicates using archive+':' +scene. Original safe
  // paths cannot contain ':' or a newline, so these identities are equivalent.
  auto validScene=[](const LevelScene&scene){return !scene.archive.empty()&&!scene.binary_scene.empty()&&
    scene.archive.find_first_of(":\r\n") == scene.archive.npos&&scene.binary_scene.find_first_of(":\r\n") == scene.binary_scene.npos;};
  std::set<std::string> requested;size_t authored_requests=plan.shared_scenes.size();
  auto request=[&](const LevelScene& scene){Check(validScene(scene),"Invalid original catalog scene identity");if(requested.insert(identity(scene)).second)ready.entry_scene_requests.push_back(scene);};
  for(const auto& scene:plan.shared_scenes)request(scene);
  std::set<std::string> entries;
  for(const auto& entry:plan.entry_partitions){Check(entries.insert(entry.name).second,"Repeated original entry partition");
   auto part=std::find_if(plan.partitions.begin(),plan.partitions.end(),[&](const auto&p){return p.name==entry.name;});Check(part!=plan.partitions.end(),"Original catalog entry partition is absent");
   authored_requests+=part->scenes.size();Check(authored_requests<=1024,"Original catalog entry scene count exceeds limit");for(const auto&scene:part->scenes)request(scene);
  }
  Check(authored_requests>0&&authored_requests<=1024&&!ready.entry_scene_requests.empty(),"Original catalog has no entry scenes");
  std::map<std::string,std::string> scenes;std::set<std::string> occupied;
  for(const auto&zone:catalog.zones)for(const auto&file:zone.files){occupied.insert(Key(file.path));occupied.insert(Key(file.alias));}
  for(size_t i=0;i<ready.entry_scene_requests.size();++i){LevelCatalogScene scene;scene.scene_index=i;scene.archive_basename=ready.prefix+"_s"+std::to_string(i);scene.registry_gsa="em1/runtime/"+scene.archive_basename+".gsa";scene.native_scene="em1/runtime/"+scene.archive_basename+".bin";
   Check(!occupied.contains(Key(scene.registry_gsa)),"Reserved scene archive prefix already appears in catalog");
   Check(scenes.emplace(identity(ready.entry_scene_requests[i]),scene.registry_gsa).second,"Duplicate original catalog scene binding");ready.expected_scene_archives.push_back(scene.archive_basename+".pak");ready.scenes.push_back(std::move(scene));
  }
  // Match ExtendNativeLevelRegistry's bank alias allocation over ALL partitions.
  std::map<std::string,std::string> banks;
  for(const auto&part:plan.partitions)for(const auto&bank:part.audio)banks.try_emplace(bank,"em1/runtime/source_bank_"+std::to_string(banks.size())+".bnk");
  struct Group{std::string partition;std::vector<CompiledLevel::Resource>files;};std::vector<Group> groups;std::set<std::string> shared;
  auto sceneFile=[&](Group&group,const LevelScene&scene){auto i=scenes.find(identity(scene));Check(i!=scenes.end(),"Catalog scene resource has no reserved archive");group.files.push_back({i->second,i->second,0});};
  if(!plan.shared_scenes.empty()){Group group;for(const auto&scene:plan.shared_scenes){sceneFile(group,scene);shared.insert(identity(scene));}groups.push_back(std::move(group));}
  for(const auto&entry:plan.entry_partitions){auto part=std::find_if(plan.partitions.begin(),plan.partitions.end(),[&](const auto&p){return p.name==entry.name;});Check(part!=plan.partitions.end(),"Original catalog partition disappeared");Group group;group.partition=entry.name;
   for(const auto&resource:part->resources){switch(resource.kind){
    case LevelPartitionResource::Kind::Scene:{Check(resource.index<part->scenes.size(),"Catalog partition scene index is invalid");const auto&scene=part->scenes[resource.index];if(!shared.contains(identity(scene)))sceneFile(group,scene);break;}
    case LevelPartitionResource::Kind::Audio:{Check(resource.index<part->audio.size(),"Catalog partition audio index is invalid");auto i=banks.find(part->audio[resource.index]);Check(i!=banks.end(),"Catalog partition audio has no authored alias");group.files.push_back({i->second,i->second,0});break;}
    case LevelPartitionResource::Kind::Ignored:Check(resource.index<part->ignored.size(),"Catalog ignored-token index is invalid");break;
    default:Check(false,"Unknown original catalog resource kind");
   }}groups.push_back(std::move(group));
  }
  auto candidate=catalog;std::set<uint32_t> used;for(const auto&zone:catalog.zones)used.insert(zone.metadata[0]);uint32_t id=0;
  for(size_t i=0;i<groups.size();++i){const auto&group=groups[i];CompiledLevel::Zone zone;zone.name=ready.bindings.level+".group_"+std::to_string(i);
   do{Check(id<99999999,"Native resource group IDs exhausted");++id;}while(used.contains(id));used.insert(id);char folder[32];std::snprintf(folder,sizeof(folder),"/zone@%08u/",id);zone.folder=folder;zone.files=group.files;zone.metadata={id,0,0,0,uint32_t(zone.files.size())};
   for(size_t file=0;file<zone.files.size();++file){auto&resource=zone.files[file];if(!resource.path.ends_with(".gsa"))continue;const auto slash=resource.path.rfind('/');char alias[80];std::snprintf(alias,sizeof(alias),"zone_%04u_file_%03zu.gsa",id,file);resource.alias=resource.path.substr(0,slash==resource.path.npos?0:slash+1)+alias;Check(!occupied.contains(Key(resource.alias)),"Reserved scene alias collides with existing catalog");occupied.insert(Key(resource.alias));}
   if(group.partition.empty())ready.bindings.shared_zone=zone.name;else Check(ready.bindings.partitions.emplace(group.partition,zone.name).second,"Repeated original entry group");
   ready.group_ids.push_back(id);ready.zones.push_back(zone);candidate.zones.push_back(std::move(zone));
  }
  if(!AppendLevelEntryState(plan,ready.bindings,candidate,error)||!ValidateLevelRegistryExtension(catalog,candidate,error))return false;
  ready.entry_state=candidate.states.back();ready.destination=candidate.destinations.back();catalog=std::move(candidate);output=std::move(ready);error.clear();return true;
 }catch(const std::exception&e){error=e.what();return false;}
}
inline bool TestLevelCatalogPlan(std::string&error){
 using namespace compiled_level_detail;
 try{
  CompiledLevel retail;retail.zones={ {"retail.a","/zone@00000001/",{1,7,8,9,1},{{"retail/a.gsa","retail/a_alias.gsa",5}},{"retail.other"}}, {"retail.b","/zone@00000003/",{3,1,2,3,0},{},{}}};retail.states={{"retail.start","retail",{{1,"retail.a",17}}}};retail.destinations={{"retail.start",{{"retail.next",8}}}};
  std::vector<uint8_t> before;Check(WriteCompiledLevel(retail,before,error),"Catalog fixture registry failed serialization");
  auto makeScene=[](const char*id){LevelScene s;s.source_gsa=std::string(id)+".gsa";s.archive="packfiles/"+std::string(id)+".pak";s.binary_scene="environments/"+std::string(id)+".bin";s.authored_flag="SourceFlag";return s;};
  LevelPlan plan;plan.origin_key="original:test:level";plan.descriptor="levels/Test.level";const auto shared=makeScene("shared"),a=makeScene("a"),b=makeScene("b"),c=makeScene("c");plan.shared_scenes={shared};
  LevelPartition unused;unused.name="unused";unused.audio={"original_unused.bnk"};
  LevelPartition first;first.name="entry";first.scenes={shared,a,b};first.audio={"original_a.bnk"};first.ignored={"Prefab_Original"};first.resources={{LevelPartitionResource::Kind::Audio,0,"AudioFlag"},{LevelPartitionResource::Kind::Scene,0,"SharedFlag"},{LevelPartitionResource::Kind::Ignored,0,"KeepToken"},{LevelPartitionResource::Kind::Scene,1,"AFlag"},{LevelPartitionResource::Kind::Scene,2,"BFlag"}};
  LevelPartition second;second.name="interior";second.scenes={b,c};second.audio={"original_a.bnk","original_b.bnk"};second.resources={{LevelPartitionResource::Kind::Scene,0,"BFlag"},{LevelPartitionResource::Kind::Audio,1,"AudioFlag"},{LevelPartitionResource::Kind::Scene,1,"CFlag"}};
  plan.partitions={unused,first,second};plan.entry_partitions={{"entry",LevelEntryPartition::Mode::Initial},{"interior",LevelEntryPartition::Mode::Interior}};
  auto catalog=retail;LevelCatalogReservation reservation;Check(AppendLevelCatalogPlan(plan,"em1_level_77",catalog,reservation,error),"Valid metadata-only reservation failed");
  Check(reservation.descriptor=="test.level"&&reservation.bindings.level=="em1_test"&&reservation.bindings.state=="em1_test.entry","Catalog descriptor/state normalization differs");
  Check(reservation.entry_scene_requests.size()==4&&reservation.scenes.size()==4&&reservation.expected_scene_archives.size()==4,"Catalog unique scene request order/count differs");
  for(size_t i=0;i<4;++i){const auto names=std::array<std::string,4>{"shared","a","b","c"};Check(reservation.entry_scene_requests[i].binary_scene=="environments/"+names[i]+".bin","Catalog scene order differs from ReadLevelContents");Check(reservation.scenes[i].archive_basename=="em1_level_77_s"+std::to_string(i)&&reservation.scenes[i].registry_gsa=="em1/runtime/em1_level_77_s"+std::to_string(i)+".gsa"&&reservation.scenes[i].native_scene=="em1/runtime/em1_level_77_s"+std::to_string(i)+".bin"&&reservation.expected_scene_archives[i]=="em1_level_77_s"+std::to_string(i)+".pak","Reserved archive/GSA/BIN name differs from native preparation");}
  Check(reservation.group_ids==std::vector<uint32_t>{2,4,5}&&reservation.zones.size()==3,"Catalog group IDs did not fill native holes uniquely");
  Check(reservation.zones[0].files.size()==1&&reservation.zones[0].files[0].alias=="em1/runtime/zone_0002_file_000.gsa","Shared native scene alias differs");
  Check(reservation.zones[1].files.size()==3&&reservation.zones[1].files[0].path=="em1/runtime/source_bank_1.bnk"&&reservation.zones[1].files[1].path=="em1/runtime/em1_level_77_s1.gsa"&&reservation.zones[1].files[1].alias=="em1/runtime/zone_0004_file_001.gsa"&&reservation.zones[1].files[2].path=="em1/runtime/em1_level_77_s2.gsa","Mixed bank/scene/ignored/shared resource order differs");
  Check(reservation.zones[2].files.size()==3&&reservation.zones[2].files[0].path=="em1/runtime/em1_level_77_s2.gsa"&&reservation.zones[2].files[1].path=="em1/runtime/source_bank_2.bnk"&&reservation.zones[2].files[2].alias=="em1/runtime/zone_0005_file_002.gsa","Interior scene/bank ordering differs");
  const auto&links=reservation.entry_state.links;Check(links.size()==4&&links[0].kind==1&&links[0].zone=="em1_test.group_0"&&links[1].kind==1&&links[1].zone=="em1_test.group_1"&&links[2].kind==1&&links[2].zone=="em1_test.group_2"&&links[3].kind==5&&links[3].zone==links[2].zone,"Metadata initial/interior state links differ");Check(reservation.destination.owner=="em1_test.entry"&&reservation.destination.entries.empty(),"Metadata destination invented progression links");
  Check(ValidateLevelRegistryExtension(retail,catalog,error),"Metadata reservation changed retail prefix");auto restored=catalog;restored.zones.resize(retail.zones.size());restored.states.resize(retail.states.size());restored.destinations.resize(retail.destinations.size());std::vector<uint8_t> prefix;Check(WriteCompiledLevel(restored,prefix,error)&&prefix==before,"Retail metadata was not byte preserved");
  auto reject=[&](LevelPlan bad,std::string_view name){auto current=catalog;std::vector<uint8_t> start,finish;Check(WriteCompiledLevel(current,start,error),"Catalog rollback fixture invalid");LevelCatalogReservation untouched;untouched.prefix="keep";std::string why;Check(!AppendLevelCatalogPlan(bad,name,current,untouched,why)&&!why.empty()&&untouched.prefix=="keep","Bad metadata plan accepted/changed reservation");Check(WriteCompiledLevel(current,finish,error)&&finish==start,"Failed metadata plan changed compiled catalog");};
  auto another=plan;another.descriptor="levels/another.level";reject(another,"em1_level_77");reject(another,"retail_77");reject(another,"em1_level_../../77");reject(another,"em1_level_X");
  auto missing=another;missing.entry_partitions[0].name="absent";reject(missing,"em1_level_78");auto badIndex=another;badIndex.partitions[1].resources[3].index=99;reject(badIndex,"em1_level_78");auto badKind=another;badKind.partitions[1].resources[0].kind=static_cast<LevelPartitionResource::Kind>(99);reject(badKind,"em1_level_78");auto badScene=another;badScene.partitions[1].scenes[1].archive.clear();reject(badScene,"em1_level_78");auto repeated=another;repeated.entry_partitions.push_back(repeated.entry_partitions[0]);reject(repeated,"em1_level_78");
  LevelCatalogReservation later;Check(AppendLevelCatalogPlan(another,"em1_level_78",catalog,later,error)&&later.group_ids==std::vector<uint32_t>{6,7,8},"Later reservation reused earlier group IDs");Check(ValidateLevelRegistryExtension(retail,catalog,error),"Multiple metadata reservations changed retail records");
  error.clear();return true;
 }catch(const std::exception&e){error=e.what();return false;}
}
}
