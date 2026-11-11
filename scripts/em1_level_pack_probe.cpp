// Developer-only DLL probe, called from Python; never installed with the game.
#include "em1_ammo_change.h"
extern "C" __declspec(dllexport) const char* ammo_property(size_t index,float* value){
  const auto properties=em2::em1::LegacyAmmoProperties();if(index>=properties.size())return nullptr;
  *value=properties[index].default_value;return properties[index].name.data();
}
extern "C" __declspec(dllexport) int ammo_change(float current,float delta,int has_limit,float limit,float normal_wait,float penalty_wait,float threshold,float* amount,float* applied,int* changes_wait,float* wait){
  em2::em1::AmmoChange result;
  if(!em2::em1::ChangeLegacyAmmo(current,delta,has_limit?std::optional<float>(limit):std::nullopt,{normal_wait,penalty_wait,threshold},result))return 0;
  *amount=result.amount;*applied=result.applied_delta;*changes_wait=result.recharge_delay.has_value();if(result.recharge_delay)*wait=*result.recharge_delay;return 1;
}
#include "em1_native_scene_pak.h"
static std::shared_ptr<const std::vector<uint8_t>> result;
extern "C" __declspec(dllexport) int build(const char** names,const unsigned char** data,const size_t* sizes,size_t count,const unsigned char** output,size_t* length){
  try {std::vector<em2::em1::NativeScenePakEntry> entries;
    for(size_t i=0;i<count;++i)entries.push_back({names[i],{data[i],sizes[i]}});
    result=em2::em1::BuildNativeScenePak(entries);*output=result->data();*length=result->size();return 1;
  }catch(...){return 0;}
}

#include "em1_level_source.h"
#include "em1_level_archives.h"
#include "em1_level_resource_bindings.h"
#include "em1_navigation_data.h"
static std::shared_ptr<em2::em1::RvzSource> navigation_source;
static std::vector<std::string> navigation_paths;
static std::vector<uint8_t> navigation_bytes;
static em2::em1::NavigationDatabase navigation_database;
static std::vector<std::pair<std::string,std::string>> navigation_databases;
static std::vector<em2::em1::LevelSceneArchive> archives;
static em2::em1::LevelContents prepared;
static std::string failure;
extern "C" __declspec(dllexport) const char* navigation_open(const wchar_t* image,size_t* count){
  try{auto owned=em2::em1::RvzSource::Open(image,failure);if(!owned)return failure.c_str();navigation_source=std::move(owned);navigation_paths.clear();
    for(const auto& file:navigation_source->Files()){auto lower=file.path;for(auto& c:lower)if(c>='A' && c<='Z')c+=32;
      if(lower.ends_with(".hpd") || lower.ends_with(".prelnk.obj"))navigation_paths.push_back(file.path);}
    *count=navigation_paths.size();return nullptr;
  }catch(const std::exception& e){failure=e.what();return failure.c_str();}
}
extern "C" __declspec(dllexport) const char* navigation_path(size_t index){return index<navigation_paths.size()?navigation_paths[index].c_str():nullptr;}
extern "C" __declspec(dllexport) const char* navigation_read(const char* path,const unsigned char** output,size_t* length){
  if(!navigation_source){failure="No original navigation source";return failure.c_str();}
  if(!navigation_source->ReadFile(path,navigation_bytes,failure))return failure.c_str();*output=navigation_bytes.data();*length=navigation_bytes.size();return nullptr;
}
extern "C" __declspec(dllexport) const char* navigation_sector(const unsigned char* data,size_t size,uint32_t* nodes,uint32_t* edges,size_t* mesh){
  em2::em1::NavigationSector sector;if(!em2::em1::ReadNavigationSector({data,size},sector,failure))return failure.c_str();
  *nodes=uint32_t(sector.nodes.size());*edges=uint32_t(sector.edges.size());*mesh=sector.mesh_offset;return nullptr;
}
static em2::em1::NavigationSector inspected_navigation_sector;
static em2::em1::NavigationRuntimeMesh query_navigation_mesh;
static em2::em1::NavigationDatabase query_navigation_database;
extern "C" __declspec(dllexport) const char* navigation_runtime_mesh(const unsigned char* data,size_t size,size_t* floors){
  if(!em2::em1::ReadNavigationRuntimeMesh({data,size},query_navigation_mesh,failure))return failure.c_str();
  query_navigation_database={};query_navigation_database.sectors.resize(1);query_navigation_database.sectors[0].runtime_mesh=query_navigation_mesh;
  *floors=query_navigation_mesh.floors.size();return nullptr;
}
extern "C" __declspec(dllexport) int navigation_runtime_contains(size_t floor,const float* point,float tolerance){
  if(floor>=query_navigation_mesh.floors.size())return -1;
  return em2::em1::NavigationFloorContainsPoint(query_navigation_mesh.floors[floor],{point[0],point[1],point[2]},tolerance);
}
extern "C" __declspec(dllexport) int navigation_runtime_find(const float* point){
  const auto* floor=em2::em1::FindNavigationFloor(query_navigation_mesh,{point[0],point[1],point[2]});
  return floor?int(floor-query_navigation_mesh.floors.data()):-1;
}
extern "C" __declspec(dllexport) int navigation_intersection(const float* movement,const float* edge,float* output){
  em2::em1::NavigationPoint hit,other;const em2::em1::NavigationSegment a{{movement[0],movement[1],movement[2]},{movement[3],movement[4],movement[5]}},b{{edge[0],edge[1],edge[2]},{edge[3],edge[4],edge[5]}};
  if(!em2::em1::NavigationDirectedIntersection(a,b,hit,&other))return 0;std::copy_n(hit.data(),3,output);std::copy_n(other.data(),3,output+3);return 1;
}
extern "C" __declspec(dllexport) int navigation_portal_step(size_t floor,const float* movement,float scale,uint32_t* history,int32_t* target,float* output){
  em2::em1::NavigationPortalHistory visited;visited.count=history[0];if(visited.count>16)return -1;
  for(uint32_t i=0;i<visited.count;++i)visited.floors[i]={0,history[i+1]};
  em2::em1::NavigationPortalStep step;const em2::em1::NavigationSegment segment{{movement[0],movement[1],movement[2]},{movement[3],movement[4],movement[5]}};
  const bool valid=em2::em1::StepNavigationPortal(query_navigation_database,{0,uint32_t(floor)},segment,scale,visited,step);
  *target=step.floor.floor_index==UINT32_MAX?-1:int32_t(step.floor.floor_index);std::copy_n(step.point.data(),3,output);std::copy_n(step.normal.data(),3,output+3);
  history[0]=visited.count;for(uint32_t i=0;i<visited.count;++i)history[i+1]=visited.floors[i].floor_index;return valid;
}
extern "C" __declspec(dllexport) const char* navigation_mesh_inspect(const unsigned char* data,size_t size,uint32_t* output){
  em2::em1::NavigationSector sector;if(!em2::em1::ReadNavigationSector({data,size},sector,failure))return failure.c_str();
  const auto& mesh=sector.runtime_mesh;uint32_t counts[11]={uint32_t(mesh.floors.size()),0,0,0,0,0,uint32_t(mesh.neighbors.size()),0,mesh.width,mesh.height,0};
  for(const auto& floor:mesh.floors){counts[1]+=floor.segments.size();counts[2]+=floor.portals.size();counts[3]+=floor.unbound_links.size();counts[4]+=floor.polylines.size();counts[5]+=floor.patches.size();}
  for(const auto& neighbor:mesh.neighbors)counts[7]+=neighbor.local_links.size();counts[10]=counts[3]-counts[7];
  inspected_navigation_sector=std::move(sector);std::copy_n(counts,11,output);return nullptr;
}
extern "C" __declspec(dllexport) int navigation_floor_portal(size_t floor,size_t portal,float* endpoints,uint32_t* target){
  const auto& floors=inspected_navigation_sector.runtime_mesh.floors;if(floor>=floors.size() || portal>=floors[floor].portals.size())return 0;
  const auto& link=floors[floor].portals[portal];std::copy_n(link.segment.from.data(),3,endpoints);std::copy_n(link.segment.to.data(),3,endpoints+3);*target=link.target_floor;return 1;
}
extern "C" __declspec(dllexport) int navigation_neighbor(size_t index,uint32_t* header,const uint32_t** links,size_t* count){
  const auto& neighbors=inspected_navigation_sector.runtime_mesh.neighbors;if(index>=neighbors.size())return 0;const auto& neighbor=neighbors[index];
  const uint32_t values[]={neighbor.sector,neighbor.x,neighbor.z,neighbor.layer};std::copy_n(values,4,header);*links=neighbor.local_links.data();*count=neighbor.local_links.size();return 1;
}
extern "C" __declspec(dllexport) const char* navigation_scan(size_t* count){
  navigation_databases.clear();if(!navigation_source){failure="No original navigation source";return failure.c_str();}
  for(const auto& file:navigation_source->Files()){auto lower=file.path;for(auto& c:lower)if(c>='A' && c<='Z')c+=32;
    if(!lower.starts_with("packfiles/") || !lower.ends_with(".pak"))continue;auto pak=em2::em1::PakSource::Open(navigation_source,file.path,failure);if(!pak)return failure.c_str();
    for(const auto& entry:pak->Entries()){auto name=entry.path;for(auto& c:name)if(c>='A' && c<='Z')c+=32;
      if(name.ends_with(".hpd"))navigation_databases.emplace_back(file.path,entry.path);}
  }
  *count=navigation_databases.size();return nullptr;
}
extern "C" __declspec(dllexport) const char* navigation_database_path(size_t index,const char** archive){
  if(index>=navigation_databases.size())return nullptr;*archive=navigation_databases[index].first.c_str();return navigation_databases[index].second.c_str();
}
extern "C" __declspec(dllexport) const char* navigation_load(const char* archive,const char* path,size_t* sectors,size_t* nodes,size_t* edges){
  if(!navigation_source){failure="No original navigation source";return failure.c_str();}
  if(!em2::em1::LoadNavigationDatabase(navigation_source,archive,path,navigation_database,failure))return failure.c_str();
  *sectors=navigation_database.sectors.size();*nodes=0;*edges=0;
  for(const auto& sector:navigation_database.sectors){*nodes+=sector.nodes.size();*edges+=sector.edges.size();}return nullptr;
}
extern "C" __declspec(dllexport) const unsigned char* navigation_loaded_hpd(size_t* length){*length=navigation_database.bytes.size();return navigation_database.bytes.data();}
extern "C" __declspec(dllexport) const char* navigation_loaded_sector_path(size_t index){return index<navigation_database.sectors.size()?navigation_database.sectors[index].source_path.c_str():nullptr;}
extern "C" __declspec(dllexport) const char* prepare(const wchar_t* image,const char* level,const unsigned char** output,size_t* length){
  try {auto owned=em2::em1::RvzSource::Open(image,failure);if(!owned)return failure.c_str();
    std::shared_ptr<em2::em1::RvzSource> source(std::move(owned));
    if(!em2::em1::ReadLevelContents(source,level,prepared,failure))return failure.c_str();
    if(!em2::em1::PrepareLevelSceneArchives(prepared,"em1_level_probe",archives,failure))return failure.c_str();
    std::vector<std::string> names;names.reserve(prepared.entry_scenes.size());
    std::vector<em2::em1::NativeScenePakEntry> entries;
    for(size_t i=0;i<prepared.entry_scenes.size();++i){names.push_back("em1/runtime/level_probe/scene_"+std::to_string(i)+".bin");entries.push_back({names.back(),prepared.entry_scenes[i].resource.scene.bytes});}
    result=em2::em1::BuildNativeScenePak(entries);*output=result->data();*length=result->size();return nullptr;
  }catch(const std::exception& e){failure=e.what();return failure.c_str();}
}

#include "em1_compiled_level.h"
static std::vector<uint8_t> compiled_result;
extern "C" __declspec(dllexport) const char* compiled(const unsigned char* data,size_t size,const unsigned char** output,size_t* length){
  em2::em1::CompiledLevel registry;
  if(!em2::em1::ReadCompiledLevel({data,size},registry,failure) || !em2::em1::WriteCompiledLevel(registry,compiled_result,failure))return failure.c_str();
  *output=compiled_result.data();*length=compiled_result.size();return nullptr;
}

extern "C" __declspec(dllexport) int targets(const em2::em1::CompiledZoneLink* old_links,size_t old_count,const em2::em1::CompiledZoneLink* new_links,size_t new_count,const uint32_t* current,size_t zones,uint32_t* desired,int64_t* interior){
  em2::em1::CompiledZoneTargets result;
  if(!em2::em1::EvaluateCompiledZoneLinks({old_links,old_count},{new_links,new_count},{current,zones},result,failure))return 0;
  std::copy(result.desired.begin(),result.desired.end(),desired);*interior=result.interior;return 1;
}

extern "C" __declspec(dllexport) const char* entry_partition(size_t index,int* mode){
  if(index>=prepared.plan.entry_partitions.size())return nullptr;
  const auto& entry=prepared.plan.entry_partitions[index];*mode=entry.mode==em2::em1::LevelEntryPartition::Mode::Interior?1:0;return entry.name.c_str();
}

extern "C" __declspec(dllexport) const char* partition_resource(size_t entry,size_t index,int* kind,const char** flags){
  if(entry>=prepared.plan.entry_partitions.size())return nullptr;
  const auto& name=prepared.plan.entry_partitions[entry].name;
  for(const auto& part:prepared.plan.partitions)if(part.name==name){
    if(index>=part.resources.size())return nullptr;
    const auto& resource=part.resources[index];*flags=resource.authored_flag.c_str();
    *kind=resource.kind==em2::em1::LevelPartitionResource::Kind::Audio?1:
        resource.kind==em2::em1::LevelPartitionResource::Kind::Ignored?2:0;
    return *kind==2?part.ignored.at(resource.index).c_str():
        *kind==1?part.audio.at(resource.index).c_str():part.scenes.at(resource.index).source_gsa.c_str();
  }
  return nullptr;
}

#include "em1_level_registry.h"
extern "C" __declspec(dllexport) const char* entry_registry(const unsigned char* original,size_t size,const unsigned char** output,size_t* length,int omit_binding){
  em2::em1::CompiledLevel registry;
  if(!em2::em1::ReadCompiledLevel({original,size},registry,failure))return failure.c_str();
  em2::em1::LevelEntryBindings bindings;bindings.level="em1_probe";bindings.state="em1_probe.entry";
  // Deliberately metadata-only test zones, NEVER installed in the game.
  if(!prepared.plan.shared_scenes.empty()){bindings.shared_zone="em1_probe.shared";em2::em1::CompiledLevel::Zone zone;zone.name=bindings.shared_zone;registry.zones.push_back(zone);}
  for(const auto& entry:prepared.plan.entry_partitions){
    auto name="em1_probe."+entry.name;bindings.partitions.emplace(entry.name,name);
    if(!omit_binding){em2::em1::CompiledLevel::Zone zone;zone.name=name;registry.zones.push_back(zone);}
  }
  if(!em2::em1::AppendLevelEntryState(prepared.plan,bindings,registry,failure))return failure.c_str();
  if(!em2::em1::WriteCompiledLevel(registry,compiled_result,failure))return failure.c_str();
  *output=compiled_result.data();*length=compiled_result.size();return nullptr;
}

extern "C" __declspec(dllexport) const char* scene_archive(size_t index,const char** gsa,const char** scene,const unsigned char** data,size_t* size){
  if(index>=archives.size())return nullptr;
  const auto& archive=archives[index];*gsa=archive.registry_gsa.c_str();*scene=archive.native_scene.c_str();
  *data=archive.bytes->data();*size=archive.bytes->size();return archive.archive_basename.c_str();
}

extern "C" __declspec(dllexport) const char* extension(const unsigned char* original,size_t original_size,const unsigned char* candidate,size_t candidate_size){
  static std::string error;em2::em1::CompiledLevel before,after;
  if(!em2::em1::ReadCompiledLevel(std::span(original,original_size),before,error) ||
     !em2::em1::ReadCompiledLevel(std::span(candidate,candidate_size),after,error))return error.c_str();
  em2::em1::ValidateLevelRegistryExtension(before,after,error);return error.c_str();
}

extern "C" __declspec(dllexport) const char* bind_groups(int mode,size_t* count){
  std::map<std::string,std::string> audio;
  for(const auto& part:prepared.plan.partitions)for(const auto& bank:part.audio)
    audio.emplace(bank,"em1/runtime/probe_audio_"+std::to_string(audio.size())+".bnk");
  if(mode==1)audio.clear();
  auto candidate=archives;if(mode==2 && !candidate.empty())candidate[0].source_origin="wrong-origin";
  std::vector<em2::em1::LevelResourceGroup> groups;
  if(!em2::em1::BindLevelEntryResources(prepared,candidate,audio,groups,failure))return failure.c_str();
  *count=0;for(const auto& group:groups){
    if(group.files.size()!=group.authored_flags.size()){failure="Flags lost";return failure.c_str();}
    for(const auto& file:group.files){if(file.path!=file.alias){failure="Alias mode differs";return failure.c_str();}++*count;}
  }
  return nullptr;
}

#include "em1_audio_dsp.h"
extern "C" __declspec(dllexport) const char* dsp(const unsigned char* input,size_t size,const int16_t** samples,size_t* count,uint32_t* rate){
  static em2::em1::NativeAudioPcm pcm;
  if(!em2::em1::DecodeWiiDspMedia(std::span(input,size),pcm,failure))return failure.c_str();
  *samples=pcm.samples.data();*count=pcm.samples.size();*rate=pcm.sample_rate;return nullptr;
}

#include "em1_audio_bank.h"
static em2::em1::WiiAudioBank audio_bank;
extern "C" __declspec(dllexport) const char* bank(const unsigned char* input,size_t size,size_t* objects,size_t* media,uint32_t* id){
  auto bytes=std::make_shared<const std::vector<uint8_t>>(input,input+size);
  if(!em2::em1::ReadWiiAudioBank(bytes,audio_bank,failure))return failure.c_str();
  *objects=audio_bank.objects.size();*media=audio_bank.media.size();*id=audio_bank.id;return nullptr;
}
extern "C" __declspec(dllexport) const unsigned char* bank_record(size_t index,int media,uint32_t* kind,uint32_t* id,size_t* size){
  if(media){if(index>=audio_bank.media.size())return nullptr;const auto& value=audio_bank.media[index];*kind=0;*id=value.id;*size=value.size;return audio_bank.bytes->data()+value.offset;}
  if(index>=audio_bank.objects.size())return nullptr;const auto& value=audio_bank.objects[index];*kind=value.kind;*id=value.id;*size=value.size;return audio_bank.bytes->data()+value.offset;
}
extern "C" __declspec(dllexport) const char* bank_event(uint32_t id,const uint32_t** actions,size_t* count){
  static std::vector<uint32_t> result;if(!em2::em1::WiiEventActions(audio_bank,id,result,failure))return failure.c_str();
  *actions=result.data();*count=result.size();return nullptr;
}

extern "C" __declspec(dllexport) const char* bank_source(const wchar_t* image,const char* name,uint32_t* id){
  auto source=em2::em1::RvzSource::Open(image,failure);if(!source)return failure.c_str();
  if(!em2::em1::ReadOriginalWiiAudioBank(*source,name,audio_bank,failure))return failure.c_str();
  if(audio_bank.source_origin.empty()){failure="Missing bank source identity";return failure.c_str();}
  *id=audio_bank.id;return nullptr;
}

#include "em1_audio_source.h"
extern "C" __declspec(dllexport) const char* bank_action(uint32_t id,uint32_t* kind,uint32_t* target,uint32_t* subsection,size_t* parameters){
  em2::em1::WiiAudioAction action;if(!em2::em1::ReadWiiAudioAction(audio_bank,id,action,failure))return failure.c_str();
  *kind=action.kind;*target=action.target;*subsection=action.subsection_size;*parameters=action.parameters.size();return nullptr;
}
extern "C" __declspec(dllexport) const char* bank_sound(uint32_t id,uint32_t* media,size_t* count,uint32_t* rate){
  em2::em1::WiiEmbeddedDspSound source;if(!em2::em1::ResolveWiiEmbeddedDspSound(audio_bank,id,source,failure))return failure.c_str();
  em2::em1::NativeAudioPcm pcm;if(!em2::em1::DecodeWiiEmbeddedDspSound(audio_bank,id,pcm,failure))return failure.c_str();
  *media=source.media_id;*count=pcm.samples.size();*rate=pcm.sample_rate;return nullptr;
}

#include "em1_audio_loop.h"
extern "C" __declspec(dllexport) const char* dsp_loop(const unsigned char* input,size_t size,uint16_t passes,int16_t* output,size_t capacity,size_t* written,size_t block){
  em2::em1::WiiEmbeddedDspSound source;source.owner=std::make_shared<const std::vector<uint8_t>>(input,input+size);source.media=*source.owner;
  em2::em1::WiiDspDefinition definition;if(!em2::em1::ReadWiiDspDefinition(source.media,definition,failure))return failure.c_str();source.sample_rate=definition.sample_rate;
  em2::em1::WiiDspVoiceReader reader;if(!reader.Begin(source,passes,failure))return failure.c_str();
  if(!block){failure="Invalid block";return failure.c_str();}*written=0;
  while(*written<capacity){const auto count=reader.Read(std::span(output+*written,std::min(block,capacity-*written)));*written+=count;if(!count)break;}
  return nullptr;
}

#include "em1_audio_parameters.h"
extern "C" __declspec(dllexport) const char* fixed_parameters(const uint32_t* bits,int32_t* output){
  std::array<float,4> values;for(size_t i=0;i<4;++i)values[i]=std::bit_cast<float>(bits[i]);em2::em1::WiiFixedParameters result;
  if(!em2::em1::LoadWiiFixedParameters(values,result,failure))return failure.c_str();output[0]=result.volume;output[1]=result.lfe;output[2]=result.pitch;output[3]=result.filter;return nullptr;
}
extern "C" __declspec(dllexport) const char* pitch_ratio(uint32_t bits,uint32_t* output){float result;if(!em2::em1::WiiPitchRatio(std::bit_cast<float>(bits),result,failure))return failure.c_str();*output=std::bit_cast<uint32_t>(result);return nullptr;}
extern "C" __declspec(dllexport) const char* random_parameters(const uint32_t* bits,size_t nodes,uint32_t* seed,uint64_t* draws,uint32_t* output){
  std::vector<em2::em1::WiiRandomRanges> ranges(nodes);size_t at=0;for(auto& node:ranges)for(auto& channel:node)for(auto& value:channel)value=std::bit_cast<float>(bits[at++]);
  std::array<float,4> result;
  if(!em2::em1::WiiRandomOffsets(ranges,[&](){++*draws;*seed=*seed*0x41c64e6du+0x3039u;return uint16_t((*seed>>16)&0x7fff);},result,failure))return failure.c_str();
  for(size_t i=0;i<4;++i)output[i]=std::bit_cast<uint32_t>(result[i]);return nullptr;
}

extern "C" __declspec(dllexport) const char* bank_chain(uint32_t sound,uint32_t* parent,size_t* nodes){
  std::vector<em2::em1::WiiOwnedNode> chain;if(!em2::em1::ReadWiiParameterChain(audio_bank,sound,chain,failure))return failure.c_str();
  *parent=chain[0].prefix.parent;*nodes=chain.size();return nullptr;
}

#include "em1_audio_positioning.h"
extern "C" __declspec(dllexport) const char* positioning(const unsigned char* input,size_t size,const uint32_t** output,size_t* count){
  em2::em1::WiiPositioning p;em2::em1::WiiVoiceSettings v;size_t consumed;
  if(!em2::em1::ReadWiiPositioning(std::span(input,size),p,v,consumed,failure))return failure.c_str();
  static std::vector<uint32_t> result;
  result={p.overridden,uint16_t(p.center),std::bit_cast<uint32_t>(p.pan[0]),std::bit_cast<uint32_t>(p.pan[1]),p.three_d,p.panner,p.kind,p.attenuation,p.spatialized,p.dynamic,p.mode,p.looping,uint32_t(p.transition),p.follow,uint32_t(p.vertices.size()),uint32_t(p.paths.size()),uint32_t(consumed),v.virtual_queue,v.maximum_instances,v.below_threshold,v.kill_newest,v.override_maximum,v.override_virtual};
  for(const auto& value:p.vertices){for(const auto n:value.position)result.push_back(std::bit_cast<uint32_t>(n));result.push_back(uint32_t(value.duration));}
  for(const auto& value:p.paths){result.push_back(value.first);result.push_back(value.count);for(const auto n:value.range)result.push_back(std::bit_cast<uint32_t>(n));}
  *output=result.data();*count=result.size();return nullptr;
}
extern "C" __declspec(dllexport) const char* virtual_policy(const uint32_t* values,size_t count,uint32_t* output){
  std::vector<em2::em1::WiiVoiceSettings> policies(count);for(size_t i=0;i<count;++i){policies[i].virtual_queue=uint8_t(values[i*3]);policies[i].below_threshold=uint8_t(values[i*3+1]);policies[i].override_virtual=values[i*3+2]!=0;}
  em2::em1::WiiVoiceSettings result;if(!em2::em1::WiiVirtualPolicy(policies,result,failure))return failure.c_str();output[0]=result.virtual_queue;output[1]=result.below_threshold;return nullptr;
}
extern "C" __declspec(dllexport) const char* bank_limit(uint32_t sound,uint32_t* node,uint32_t* maximum){
  em2::em1::WiiVoiceLimitDefinition result;if(!em2::em1::ReadWiiVoiceLimitDefinition(audio_bank,sound,result,failure))return failure.c_str();*node=result.node;*maximum=result.maximum;return nullptr;
}

#include "em1_audio_admission.h"
extern "C" __declspec(dllexport) uint64_t victim(float incoming,int kill_newest,const uint64_t* ids,const uint64_t* owners,const float* priorities,const uint32_t* flags,size_t count,uint64_t owner,uint32_t group){
  std::vector<em2::em1::WiiVoiceCandidate> entries(count);for(size_t i=0;i<count;++i)entries[i]={ids[i],owners[i],priorities[i],bool(flags[i]&1),bool(flags[i]&2)};
  return em2::em1::WiiSelectVoiceVictim(incoming,kill_newest!=0,entries,owner,group);
}

#include "em1_scene_files.h"
static std::vector<em2::em1::SceneFileReference> scene_references;
extern "C" __declspec(dllexport) uint32_t scene_file_storage(size_t index){return index<scene_references.size()?scene_references[index].storage_mode:UINT32_MAX;}
extern "C" __declspec(dllexport) const char* scene_declared_assets(size_t index,size_t* count){
  if(index>=prepared.entry_scenes.size()){failure="Scene index outside prepared entry";return failure.c_str();}
  std::vector<em2::em1::SceneFileReference> references;if(!em2::em1::CollectDeclaredSceneAssetReferences(prepared.entry_scenes[index].resource.scene,references,failure))return failure.c_str();
  *count=references.size();return nullptr;
}
extern "C" __declspec(dllexport) const char* scene_files(size_t index,size_t* count){
  if(index>=prepared.entry_scenes.size()){failure="Invalid scene";return failure.c_str();}
  std::vector<em2::em1::SceneFileReference> references;
  if(!em2::em1::CollectSceneFileReferences(prepared.entry_scenes[index].resource.scene,references,failure))return failure.c_str();
  scene_references=std::move(references);*count=scene_references.size();return nullptr;
}

extern "C" __declspec(dllexport) const char* scene_file(size_t index,uint32_t* entity,uint32_t* component,const char** name,const char** property,size_t* offset){
  if(index>=scene_references.size())return nullptr;const auto& value=scene_references[index];*entity=value.entity;*component=value.component;*name=value.component_name.c_str();*property=value.property.c_str();*offset=value.value_offset;return value.path.c_str();
}

extern "C" __declspec(dllexport) const char* bind_scene(size_t index,int invalid,const unsigned char** output,size_t* size){
  if(index>=prepared.entry_scenes.size()){failure="Invalid scene";return failure.c_str();}
  const auto& scene=prepared.entry_scenes[index].resource.scene;
  std::vector<em2::em1::SceneFileReference> references;if(!em2::em1::CollectSceneFileReferences(scene,references,failure))return failure.c_str();
  std::map<std::string,std::string> bindings;
  for(const auto& reference:references){
    auto path=reference.path;for(auto& c:path){if(c=='\\')c='/';if(c>='A' && c<='Z')c+=32;}
    auto suffix=std::string(em2::em1::SceneFileExtension(path));
    if(!bindings.contains(path))bindings.emplace(path,"em1/runtime/bound_"+std::to_string(bindings.size())+suffix);
  }
  if(!bindings.empty()){
    if(invalid==1)bindings.erase(bindings.begin());
    if(invalid==2)bindings.begin()->second="em1/runtime/wrong.resource";
    if(invalid==3)bindings.begin()->second="em1/runtime/../escape.nif";
    if(invalid>=5 && invalid<=8){const std::string_view missing=invalid==5?".bsq":invalid==6?".kfm":invalid==7?".hkw":".hpd";
      const auto removed=std::erase_if(bindings,[&](const auto& item){return em2::em1::SceneFileExtension(item.first)==missing;});
      if(!removed)return "Fixture has no requested dependency kind";
    }
    if(invalid==4){auto name=bindings.begin()->first;for(auto& c:name)if(c>='a' && c<='z')c-=32;auto value=bindings.begin()->second;value.insert(12,"conflict_");bindings.emplace(name,value);}
  }
  static em2::em1::SceneAdapterResult bound;
  if(!em2::em1::BindSceneFilePaths(scene,bindings,bound,failure))return failure.c_str();
  *output=bound.bytes.data();*size=bound.bytes.size();return nullptr;
}

extern "C" __declspec(dllexport) const char* bind_declared_scene(size_t index,int omit,const unsigned char** output,size_t* size){
  if(index>=prepared.entry_scenes.size())return "Invalid scene";const auto& scene=prepared.entry_scenes[index].resource.scene;
  std::vector<em2::em1::SceneFileReference> refs;if(!em2::em1::CollectDeclaredSceneAssetReferences(scene,refs,failure))return failure.c_str();
  std::map<std::string,std::string> bindings;
  for(const auto& ref:refs){auto path=ref.path;for(auto& c:path){if(c=='\\')c='/';if(c>='A' && c<='Z')c+=32;}
    if(!bindings.contains(path))bindings.emplace(path,"em1/runtime/declared_"+std::to_string(bindings.size())+std::string(em2::em1::SceneFileExtension(path)));}
  if(omit && !bindings.empty())bindings.erase(bindings.begin());static em2::em1::SceneAdapterResult result;
  if(!em2::em1::BindDeclaredSceneAssetPaths(scene,bindings,result,failure))return failure.c_str();*output=result.bytes.data();*size=result.bytes.size();return nullptr;
}
#include "em1_model_closure.h"
#include "em1_kfm.h"
#include "em1_kfm_closure.h"
#include "em1_resource_catalog.h"
static em2::em1::OriginalResourceCatalog original_catalog;
static std::vector<em2::em1::SceneResourceLookup> original_level_scope;
extern "C" __declspec(dllexport) const char* level_scope_item(size_t index,uint32_t* scene,uint32_t* entity,int* state,const char** component,const char** property){
  if(index>=original_level_scope.size())return nullptr;const auto& item=original_level_scope[index];*scene=uint32_t(item.scene_index);*entity=item.reference.entity;*state=int(item.query.presence);
  *component=item.reference.component_name.c_str();*property=item.reference.property.c_str();return item.reference.path.c_str();
}
extern "C" __declspec(dllexport) const char* level_resource_scope(const wchar_t* image,const char* level,uint32_t* counts,const char** bootstrap){
  std::shared_ptr<em2::em1::RvzSource> source=em2::em1::RvzSource::Open(image,failure);if(!source)return failure.c_str();
  if(!em2::em1::BuildOriginalResourceCatalog(source,original_catalog,failure))return failure.c_str();em2::em1::LevelContents contents;
  if(!em2::em1::ReadLevelContents(source,level,contents,failure))return failure.c_str();std::vector<em2::em1::SceneResourceLookup> plan;
  if(!em2::em1::PlanLevelResourceScope(original_catalog,contents,plan,failure))return failure.c_str();std::vector<std::string> names;
  if(!em2::em1::ReadOriginalBootstrapArchives(original_catalog,names,failure))return failure.c_str();static std::string manifest;manifest.clear();for(const auto& name:names)manifest+=name+"\n";
  counts[0]=counts[1]=counts[2]=0;for(const auto& lookup:plan)++counts[int(lookup.query.presence)];*bootstrap=manifest.c_str();original_level_scope=std::move(plan);return nullptr;
}
extern "C" __declspec(dllexport) const char* resource_catalog(const wchar_t* image,size_t* archives,size_t* resources){
  std::shared_ptr<em2::em1::RvzSource> source=em2::em1::RvzSource::Open(image,failure);if(!source)return failure.c_str();
  if(!em2::em1::BuildOriginalResourceCatalog(source,original_catalog,failure))return failure.c_str();*archives=original_catalog.packfiles;*resources=original_catalog.resources.size();return nullptr;
}
extern "C" __declspec(dllexport) const char* resource_query(const char* path,const char** archives,size_t count,int* state,const char** locations){
  std::vector<std::string> scope;for(size_t i=0;i<count;++i)scope.emplace_back(archives[i]);em2::em1::OriginalResourceQuery result;
  if(!em2::em1::QueryOriginalResource(original_catalog,path,scope,result,failure))return failure.c_str();static std::string names;names.clear();
  for(const auto& location:result.locations)names+=location.archive+"\t"+location.path+"\n";*state=int(result.presence);*locations=names.c_str();return nullptr;
}
extern "C" __declspec(dllexport) const char* find_original_resources(const wchar_t* image,const char* fragment,const char** matches,uint32_t* skipped){
  std::shared_ptr<em2::em1::RvzSource> source=em2::em1::RvzSource::Open(image,failure);if(!source)return failure.c_str();static std::string result;result.clear();*skipped=0;
  for(const auto& file:source->Files())if(file.path.starts_with("packfiles/") && file.path.ends_with(".pak")){
    auto pak=em2::em1::PakSource::Open(source,file.path,failure);if(!pak){++*skipped;continue;}
    for(const auto& entry:pak->Entries())if(entry.path.find(fragment)!=std::string::npos)result+=file.path+"\t"+entry.path+"\n";
  }*matches=result.c_str();failure.clear();return nullptr;
}
extern "C" __declspec(dllexport) const char* kfm_closure(const wchar_t* image,const char* archive,const char* resource,uint32_t* counts){
  std::shared_ptr<em2::em1::RvzSource> source=em2::em1::RvzSource::Open(image,failure);if(!source)return failure.c_str();
  em2::em1::KfmClosure result;if(!em2::em1::ReadKfmClosure(source,archive,resource,result,failure))return failure.c_str();
  counts[0]=uint32_t(result.models.size());counts[1]=uint32_t(result.keyframes.size());counts[2]=0;for(const auto& clip:result.keyframes)counts[2]+=uint32_t(clip.keyframe.document.sequences.size());return nullptr;
}
#include "em1_keyframe.h"
extern "C" __declspec(dllexport) const char* keyframe(const unsigned char* input,size_t size){
  em2::em1::KeyframeDocument doc;return em2::em1::InspectKeyframe(std::span(input,size),doc,failure)?nullptr:failure.c_str();
}
extern "C" __declspec(dllexport) const char* keyframe_source(const wchar_t* image,const char* archive,const char* resource,const char** origin,const unsigned char** output,size_t* length,uint32_t* counts){
  std::shared_ptr<em2::em1::RvzSource> source=em2::em1::RvzSource::Open(image,failure);if(!source)return failure.c_str();static em2::em1::KeyframeResource result;
  if(!em2::em1::ReadKeyframe(source,archive,resource,result,failure))return failure.c_str();
  *origin=result.origin_key.c_str();*output=result.bytes.data();*length=result.bytes.size();counts[0]=uint32_t(result.document.sequences.size());counts[1]=0;
  for(const auto& sequence:result.document.sequences)counts[1]+=sequence.evaluators;return nullptr;
}
static em2::em1::KfmDocument kfm_document;
extern "C" __declspec(dllexport) const char* kfm_inspect(const unsigned char* input,size_t size,uint32_t* counts){
  if(!em2::em1::InspectKfm(std::span(input,size),kfm_document,failure))return failure.c_str();
  counts[0]=kfm_document.sequences;counts[1]=kfm_document.transitions;counts[2]=kfm_document.groups;counts[3]=uint32_t(kfm_document.paths.size());return nullptr;
}
extern "C" __declspec(dllexport) const char* kfm_path(size_t index,size_t* offset,size_t* length,int* model){
  if(index>=kfm_document.paths.size())return nullptr;const auto& field=kfm_document.paths[index];*offset=field.offset;*length=field.bytes;*model=field.model;return field.authored.c_str();
}
extern "C" __declspec(dllexport) const char* kfm_bind(const unsigned char* input,size_t size,const char* source,const char* native,
    const char** keys,const char** values,size_t count,const unsigned char** output,size_t* length){
  std::map<std::string,std::string> bindings;for(size_t i=0;i<count;++i)bindings.emplace(keys[i],values[i]);
  static em2::em1::nif::Bytes result;if(!em2::em1::BindKfmPaths(std::span(input,size),source,native,bindings,result,failure))return failure.c_str();
  *output=result.data();*length=result.size();return nullptr;
}
extern "C" __declspec(dllexport) const char* kfm_source(const wchar_t* image,const char* archive,const char* resource,const char* native,
    const char** keys,const char** values,size_t count,const char** origin,const unsigned char** output,size_t* length){
  std::shared_ptr<em2::em1::RvzSource> source=em2::em1::RvzSource::Open(image,failure);if(!source)return failure.c_str();
  std::map<std::string,std::string> bindings;for(size_t i=0;i<count;++i)bindings.emplace(keys[i],values[i]);static em2::em1::BoundKfmResource result;
  if(!em2::em1::ReadBoundKfm(source,archive,resource,native,bindings,result,failure))return failure.c_str();
  *origin=result.origin_key.c_str();*output=result.bytes.data();*length=result.bytes.size();return nullptr;
}
#include "em1_nif_controllers.h"
#include "em1_nif_roots.h"
extern "C" __declspec(dllexport) const char* contained_roots(const unsigned char* input,size_t size,uint32_t* count,uint32_t* deepest){
  try{em2::em1::nif::Nif nif(std::span(input,size));const auto plan=em2::em1::PlanContainedNifRoots(nif);*count=uint32_t(plan.roots.size());*deepest=0;
    for(const auto& path:plan.child_paths)*deepest=std::max(*deepest,uint32_t(path.size()));return nullptr;}
  catch(const std::exception& e){failure=e.what();return failure.c_str();}
}
extern "C" __declspec(dllexport) const char* controller_records(const unsigned char* input,size_t size){
  try{em2::em1::nif::Nif nif(std::span(input,size));em2::em1::ValidateNifControllerBlocks(nif);return nullptr;}
  catch(const std::exception& e){failure=e.what();return failure.c_str();}
}
extern "C" __declspec(dllexport) const char* level_model_asset(const wchar_t* image,const char* level,const char* archive,const char* resource,
 const char** origin,const char** texture_origins,const unsigned char** output,size_t* length){
  std::shared_ptr<em2::em1::RvzSource> source=em2::em1::RvzSource::Open(image,failure);if(!source)return failure.c_str();
  em2::em1::LevelContents contents;if(!em2::em1::ReadLevelContents(source,level,contents,failure))return failure.c_str();
  std::vector<std::string> archives;for(const auto& scene:contents.entry_scenes)archives.push_back(scene.request.archive);
  static em2::em1::ModelClosure result;em2::em1::ModelClosure candidate;
  if(!em2::em1::ReadModelClosure(source,archive,resource,candidate,failure,nullptr,archives))return failure.c_str();
  result=std::move(candidate);static std::string identities;identities.clear();for(const auto& name:result.texture_origins)identities+=name+'\n';
  *origin=result.model.origin_key.c_str();*texture_origins=identities.c_str();*output=result.model.bytes.data();*length=result.model.bytes.size();return nullptr;
}
extern "C" __declspec(dllexport) const char* model_asset(const wchar_t* image,const char* archive,const char* resource,size_t* bytes,uint32_t* meshes,uint32_t* textures){
  static std::wstring image_name;static std::shared_ptr<em2::em1::RvzSource> source;static em2::em1::ModelClosure model;
  if(!source || image_name!=image){source=em2::em1::RvzSource::Open(image,failure);image_name=image;if(!source)return failure.c_str();}
  em2::em1::ModelClosure candidate;
  if(!em2::em1::ReadModelClosure(source,archive,resource,candidate,failure))return failure.c_str();
  model=std::move(candidate);*bytes=model.model.bytes.size();*meshes=uint32_t(model.model.mesh_origins.size());*textures=model.model.textures;return nullptr;
}

#include "em1_nif_textures.h"
extern "C" __declspec(dllexport) const char* texture_asset(const unsigned char* input,size_t size,const unsigned char** output,size_t* length){
  static em2::em1::NifTextureResult result;if(!em2::em1::AdaptNifTextures(std::span(input,size),result,failure))return failure.c_str();*output=result.bytes.data();*length=result.bytes.size();return nullptr;
}

#include "em1_portable_pixels.h"
extern "C" __declspec(dllexport) const char* portable_pixels(const unsigned char* input,size_t size,int big){
  if(!em2::em1::ValidatePortableRgbaPixels(std::span(input,size),big!=0,failure))return failure.c_str();return nullptr;
}

extern "C" __declspec(dllexport) const char* locate_original_model(const wchar_t* image,const char* resource,const char** matches){
  static std::shared_ptr<em2::em1::RvzSource> source;static std::string output;
  if(!source)source=em2::em1::RvzSource::Open(image,failure);if(!source)return failure.c_str();
  output.clear();
  for(const auto& file:source->Files()){
    auto name=file.path;for(auto& c:name)if(c>='A' && c<='Z')c+=32;
    if(!name.starts_with("packfiles/") || !name.ends_with(".pak"))continue;
    auto pak=em2::em1::PakSource::Open(source,file.path,failure);if(!pak)return failure.c_str();
    for(const auto& entry:pak->Entries())if(entry.path==resource){output+=name;output+='\n';}
  }
  *matches=output.c_str();return nullptr;
}

extern "C" __declspec(dllexport) const char* base_model_source(const wchar_t* image,const char* archive,const char* resource,const char** origin,const unsigned char** output,size_t* length){
  static std::shared_ptr<em2::em1::RvzSource> source;static em2::em1::AdaptedModelResource model;
  if(!source)source=em2::em1::RvzSource::Open(image,failure);if(!source)return failure.c_str();
  if(!em2::em1::ReadAdaptedModel(source,archive,resource,model,failure))return failure.c_str();
  *origin=model.origin_key.c_str();*output=model.bytes.data();*length=model.bytes.size();return nullptr;
}

static std::vector<em2::em1::NifMeshVariant> mesh_variants;
extern "C" __declspec(dllexport) const char* mesh_buffer(const unsigned char* input,size_t size,const unsigned char** output,size_t* length,uint32_t* counts){
  static em2::em1::NifMeshResult result;em2::em1::NifMeshResult candidate;
  if(!em2::em1::AdaptNifMeshes(std::span(input,size),candidate,failure))return failure.c_str();
  result=std::move(candidate);*output=result.bytes.data();*length=result.bytes.size();
  counts[0]=result.prepared_indexed_skin_meshes;counts[1]=result.unadapted_skin_meshes;counts[2]=result.promoted_fixed_uv_streams;return nullptr;
}
extern "C" __declspec(dllexport) const char* mesh_requirements(const wchar_t* image,const char* archive,const char* resource,size_t* count){
  static std::shared_ptr<em2::em1::RvzSource> source;if(!source)source=em2::em1::RvzSource::Open(image,failure);if(!source)return failure.c_str();
  auto pak=em2::em1::PakSource::Open(source,archive,failure);if(!pak)return failure.c_str();
  std::vector<uint8_t> bytes;if(!pak->ReadOriginal(resource,bytes,failure))return failure.c_str();
  if(!em2::em1::InspectNifMeshVariants(bytes,mesh_variants,failure))return failure.c_str();*count=mesh_variants.size();return nullptr;
}
extern "C" __declspec(dllexport) const char* mesh_requirement(size_t index,uint32_t* fields){
  static std::string description;if(index>=mesh_variants.size())return nullptr;
  const auto& value=mesh_variants[index];fields[0]=value.block;fields[1]=value.submeshes;fields[2]=value.instanced;fields[3]=value.skin;fields[4]=value.modifier_count;fields[5]=value.embedded_skin_bytes;
  description.clear();for(const auto& type:value.modifier_types)description+="modifier:"+type+"\n";for(const auto& semantic:value.semantics)description+="semantic:"+semantic+"\n";for(const auto format:value.formats)description+="format:"+std::to_string(format)+"\n";return description.c_str();
}

extern "C" __declspec(dllexport) int mesh_skin_counts(size_t index,uint32_t* counts){if(index>=mesh_variants.size())return 0;std::copy(mesh_variants[index].cooked_skin_counts.begin(),mesh_variants[index].cooked_skin_counts.end(),counts);return 1;}

#include "em1_nif_skin.h"
extern "C" __declspec(dllexport) const char* cooked_skin(const unsigned char* input,size_t size,int big,uint32_t user,const unsigned char** output,size_t* length){
  em2::em1::WiiCookedSkin skin;if(!em2::em1::ReadWiiCookedSkin(std::span(input,size),big!=0,user,skin,failure))return failure.c_str();
  static em2::em1::nif::Bytes bytes;if(!em2::em1::WriteWiiCookedSkin(skin,big!=0,user,bytes,failure))return failure.c_str();*output=bytes.data();*length=bytes.size();return nullptr;
}

#include "em1_skin_deform.h"
extern "C" __declspec(dllexport) const char* deform_skin(const unsigned char* input,size_t size,int big,uint32_t user,const float* matrices,size_t bones,const float* positions,const uint32_t* sources,size_t vertices,int translate,const float** output){
  em2::em1::WiiCookedSkin skin;if(!em2::em1::ReadWiiCookedSkin(std::span(input,size),big!=0,user,skin,failure))return failure.c_str();
  std::vector<em2::em1::SkinMatrix> poses(bones);for(size_t i=0;i<bones;++i)std::copy_n(matrices+i*16,16,poses[i].begin());
  std::vector<em2::em1::SkinMatrix> palette;if(!em2::em1::BuildWiiSkinPalette(skin,poses,palette,failure))return failure.c_str();
  em2::em1::WiiSkinDeformer deformer;if(!deformer.Bind(skin,std::span(sources,vertices),bones,failure))return failure.c_str();
  std::vector<em2::em1::SkinVector> vectors(vertices);for(size_t i=0;i<vertices;++i)std::copy_n(positions+i*3,3,vectors[i].begin());
  static std::vector<em2::em1::SkinVector> result;if(!deformer.Deform(palette,vectors,translate!=0,result,failure))return failure.c_str();*output=result.empty()?nullptr:result[0].data();return nullptr;
}

extern "C" __declspec(dllexport) const char* morph_skin(const unsigned char* input,size_t size,int big,uint32_t user,const float* slots,size_t width,const float* positions,const uint32_t* sources,size_t vertices,const float** output){
  em2::em1::WiiCookedSkin skin;if(!em2::em1::ReadWiiCookedSkin(std::span(input,size),big!=0,user,skin,failure))return failure.c_str();
  em2::em1::WiiMorphDeformer deformer;if(!deformer.Bind(skin,std::span(sources,vertices),failure))return failure.c_str();
  std::vector<em2::em1::SkinVector> base(vertices);for(size_t i=0;i<vertices;++i)std::copy_n(positions+i*3,3,base[i].begin());
  static std::vector<em2::em1::SkinVector> result;if(!deformer.Deform(base,std::span(slots,width),result,failure))return failure.c_str();*output=result.empty()?nullptr:result[0].data();return nullptr;
}

#include "em1_skeleton_pose.h"
extern "C" __declspec(dllexport) void qs_compose(const float* parent,const float* local,float* output){
  em2::em1::WiiQsPose a,b;std::copy_n(parent,12,a.begin());std::copy_n(local,12,b.begin());const auto result=em2::em1::ComposeWiiQsPose(a,b);std::copy(result.begin(),result.end(),output);
}

extern "C" __declspec(dllexport) const char* model_poses(const int32_t* parents,const float* poses,size_t count,float* output){
  std::vector<em2::em1::WiiRigBone> bones(count);for(size_t i=0;i<count;++i){bones[i].parent=parents[i];std::copy_n(poses+i*12,12,bones[i].local.begin());}
  std::vector<em2::em1::WiiQsPose> result;if(!em2::em1::ResolveWiiModelPoses(bones,result,failure))return failure.c_str();
  for(size_t i=0;i<count;++i)std::copy(result[i].begin(),result[i].end(),output+i*12);return nullptr;
}

extern "C" __declspec(dllexport) const char* mickey_havok_files(const wchar_t* image,const char** output){
  static std::string names;names.clear();auto source=em2::em1::RvzSource::Open(image,failure);if(!source)return failure.c_str();
  auto pak=em2::em1::PakSource::Open(std::move(source),"packfiles/_dynamic.pak",failure);if(!pak)return failure.c_str();
  for(const auto& entry:pak->Entries())if(entry.path.find("mickey")!=std::string::npos && entry.path.ends_with(".hkx_wii")){names+=entry.path;names+='\n';}
  *output=names.c_str();return nullptr;
}

#include "em1_skeleton_source.h"
static em2::em1::WiiSkeletonReference reference_skeleton;
extern "C" __declspec(dllexport) const char* skeleton_source(const wchar_t* image,const char* archive,const char* resource,size_t* count){
  std::shared_ptr<em2::em1::RvzSource> source=em2::em1::RvzSource::Open(image,failure);if(!source)return failure.c_str();auto pak=em2::em1::PakSource::Open(source,archive,failure);if(!pak)return failure.c_str();
  std::vector<uint8_t> bytes;if(!pak->ReadOriginal(resource,bytes,failure))return failure.c_str();
  if(!em2::em1::ReadWiiSkeletonReference(bytes,reference_skeleton,failure))return failure.c_str();*count=reference_skeleton.bones.size();return nullptr;
}
extern "C" __declspec(dllexport) const char* skeleton_bone(size_t index,int32_t* parent,uint8_t* lock,float* pose){
  if(index>=reference_skeleton.bones.size())return nullptr;*parent=reference_skeleton.bones[index].parent;*lock=reference_skeleton.locked_translations[index];std::copy(reference_skeleton.bones[index].local.begin(),reference_skeleton.bones[index].local.end(),pose);return reference_skeleton.names[index].c_str();
}

extern "C" __declspec(dllexport) const char* skeleton_blob(const unsigned char* input,size_t size,size_t* count){if(!em2::em1::ReadWiiSkeletonReference(std::span(input,size),reference_skeleton,failure))return failure.c_str();*count=reference_skeleton.bones.size();return nullptr;}
extern "C" __declspec(dllexport) const char* reference_skin_palette(const unsigned char* input,size_t size,int big,uint32_t user,const float** matrices,size_t* count){
  em2::em1::WiiCookedSkin skin;if(!em2::em1::ReadWiiCookedSkin(std::span(input,size),big!=0,user,skin,failure))return failure.c_str();
  static std::vector<em2::em1::SkinMatrix> palette;if(!em2::em1::BuildWiiRigSkinPalette(skin,reference_skeleton.bones,palette,failure))return failure.c_str();*count=palette.size();*matrices=palette.empty()?nullptr:palette[0].data();return nullptr;
}

#include "em1_havok_collision.h"
extern "C" __declspec(dllexport) const char* collision_pack_regression(){if(!em2::em1::TestEm1HavokCollision(failure))return failure.c_str();return nullptr;}

#include "em1_animation_source.h"
static em2::em1::WiiBoundSplineClip animation_clip;
extern "C" __declspec(dllexport) const char* animation_blob(const unsigned char* input,size_t size,uint32_t* counts,float* times){
  if(!em2::em1::ReadWiiBoundSplineClip(std::span(input,size),animation_clip,failure))return failure.c_str();
  const auto& a=animation_clip;const uint32_t values[]={a.transform_tracks,a.float_tracks,a.frames,a.blocks,a.max_frames_per_block,a.mask_bytes,a.data_size,uint32_t(a.blend_hint),uint32_t(a.has_extracted_motion)};std::copy_n(values,9,counts);
  const float t[]={a.duration,a.block_duration,a.inverse_block_duration,a.frame_duration};std::copy_n(t,4,times);return nullptr;
}
extern "C" __declspec(dllexport) const char* animation_rig(){if(!em2::em1::ValidateWiiClipRigIndices(animation_clip,reference_skeleton,failure))return failure.c_str();return nullptr;}
extern "C" __declspec(dllexport) const char* animation_indices(int floats,const int16_t** output,size_t* count){const auto& a=floats?animation_clip.float_indices:animation_clip.bone_indices;*output=a.data();*count=a.size();return animation_clip.skeleton_name.c_str();}
extern "C" __declspec(dllexport) const char* skeleton_slot(size_t index){return index<reference_skeleton.float_slots.size()?reference_skeleton.float_slots[index].c_str():nullptr;}
extern "C" __declspec(dllexport) int animation_offsets(unsigned kind,const uint32_t** output,size_t* count){
  const auto& a=animation_clip;const std::vector<uint32_t>* arrays[]={&a.block_offsets,&a.float_block_offsets,&a.transform_offsets,&a.float_offsets};if(kind>=4)return 0;*output=arrays[kind]->data();*count=arrays[kind]->size();return 1;
}
extern "C" __declspec(dllexport) void animation_pack(const unsigned char** output,size_t* count){*output=animation_clip.source_pack.data();*count=animation_clip.source_pack.size();}

#include "em1_jigsaw_havok.h"
extern "C" __declspec(dllexport) const char* jigsaw_rejects_unbound(const unsigned char* input,size_t size){
  em2::em1::ConvertedJigsawAggregate converted;
  if(em2::em1::ConvertJigsawAggregate(std::span(input,size),converted,failure) || !converted.pack.empty() || failure.find("explicit binding to external mesh")==std::string::npos){failure="Unbound source aggregate did not reject with explicit mesh-binding requirement";return failure.c_str();}
  return nullptr;
}

#include "em1_baked_lighting.h"
static std::vector<uint8_t> baked_source_bytes;
// Raw SOURCE-format color-stage probe; output is deliberately not a PC asset.
extern "C" __declspec(dllexport) const char* baked_source(const unsigned char* input,size_t size,const unsigned char* light,size_t light_size,uint32_t entity,const unsigned char** output,size_t* length,uint32_t* count){
  try{em2::em1::nif::Nif model(std::span(input,size));*count=em2::em1::ApplyBakedLighting(model,std::span(light,light_size),entity);baked_source_bytes=model.Write();*output=baked_source_bytes.data();*length=baked_source_bytes.size();return nullptr;}
  catch(const std::exception& exception){failure=exception.what();return failure.c_str();}
}

static std::vector<uint8_t> jigsaw_bound_bytes;
extern "C" __declspec(dllexport) const char* jigsaw_bind_model(const unsigned char* input,size_t size,const unsigned char** output,size_t* length,uint32_t* aggregates){
  try{em2::em1::nif::Nif model(std::span(input,size));std::vector<em2::em1::JigsawAggregateInfo> info;
    if(!em2::em1::AdaptJigsawHavokBlocks(model,info,failure))return failure.c_str();jigsaw_bound_bytes=model.Write();*output=jigsaw_bound_bytes.data();*length=jigsaw_bound_bytes.size();*aggregates=uint32_t(info.size());return nullptr;
  }catch(const std::exception& exception){failure=exception.what();return failure.c_str();}
}
#include "em1_ammo_recharge.h"
// Developer-only delegate fixture for differential testing the entire Update
// control flow. These callbacks are NOT a production inventory/timer bridge.
extern "C" __declspec(dllexport) int ammo_recharge(float* frame,int* flags){
  if(!frame || !flags)return 0;
  struct Access final:em2::em1::LegacyAmmoRechargeAccess {
    float* f;int* s;
    Access(float* a,int* b):f(a),s(b){}
    float RechargeRate(unsigned c)override{return f[1+c];}
    float RechargeMax(unsigned c)override{return f[3+c];}
    std::optional<float> InventoryLimit(unsigned c)override{return s[0]?std::optional<float>(f[5+c]):std::nullopt;}
    float Amount(unsigned c)override{return s[1]?em2::em1::kLegacyInfiniteAmmoAmount:!s[0]?0:f[7+(s[2]?0:c)];}
    void Add(unsigned c,float delta)override{
      f[13+c]=delta;++s[9+c];if(!s[0] || !s[11])return;const auto target=s[2]?0:c;
      const auto sum=f[7+target]+delta;f[7+target]=sum<f[5+target]?sum:f[5+target];
    }
    bool TimerActive(unsigned c)override{return s[3+c]!=0;}
    float Elapsed(unsigned c)override{return f[11+c];}
    void RestartTimer(unsigned c)override{++s[5+c];s[3+c]=1;}
    void ClearTimer(unsigned c)override{++s[7+c];s[3+c]=0;}
  } access(frame,flags);
  std::array<float,2> cooldowns{frame[9],frame[10]};
  em2::em1::TickLegacyAmmoRecharge(access,frame[0],cooldowns);
  frame[9]=cooldowns[0];frame[10]=cooldowns[1];return 1;
}
