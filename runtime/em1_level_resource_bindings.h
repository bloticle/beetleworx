#pragma once
#include "em1_level_archives.h"
#include "em1_compiled_level.h"
namespace em2::em1 {
struct LevelResourceGroup {
  std::string partition; // Empty denotes the shared group.
  std::vector<CompiledLevel::Resource> files;
  std::vector<std::string> authored_flags;
};
// Audio bindings must be supplied by the platform audio converter. This only
// binds entry resources; it does not make the referenced platform assets ready.
inline bool BindLevelEntryResources(const LevelContents& contents,
    std::span<const LevelSceneArchive> archives,const std::map<std::string,std::string>& audio,
    std::vector<LevelResourceGroup>& output,std::string& error){
  using namespace compiled_level_detail;
  try {
    Check(archives.size()==contents.entry_scenes.size(),"Incomplete scene archive bindings");
    std::map<std::string,std::string> scenes;
    std::set<size_t> indices;std::set<std::string> native_names;
    auto key=[](const LevelScene& scene){return scene.archive+"\n"+scene.binary_scene;};
    for(const auto& archive:archives){
      Check(archive.scene_index<contents.entry_scenes.size() && indices.insert(archive.scene_index).second,"Invalid scene binding index");
      const auto& source=contents.entry_scenes[archive.scene_index];
      Check(archive.source_origin==source.resource.origin_key && archive.bytes && !archive.bytes->empty(),"Scene binding origin/bytes missing");
      Check(archive.registry_gsa=="em1/runtime/"+archive.archive_basename+".gsa" &&
        archive.native_scene=="em1/runtime/"+archive.archive_basename+".bin" &&
        archive.archive_basename.starts_with("em1_level_") &&
        archive.archive_basename.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789")==std::string::npos &&
        native_names.insert(archive.registry_gsa).second,"Invalid native scene binding name");
      Check(scenes.emplace(key(source.request),archive.registry_gsa).second,"Duplicate source scene binding");
    }
    std::vector<LevelResourceGroup> groups;std::set<std::string> shared;
    auto scene=[&](LevelResourceGroup& group,const LevelScene& source,const std::string& flag){
      const auto found=scenes.find(key(source));Check(found!=scenes.end(),"Entry scene has no staged archive");
      group.files.push_back({found->second,found->second,0});group.authored_flags.push_back(flag);
    };
    if(!contents.plan.shared_scenes.empty()){
      LevelResourceGroup group;
      for(const auto& source:contents.plan.shared_scenes){scene(group,source,source.authored_flag);shared.insert(key(source));}
      groups.push_back(std::move(group));
    }
    for(const auto& entry:contents.plan.entry_partitions){
      auto part=std::find_if(contents.plan.partitions.begin(),contents.plan.partitions.end(),[&](const auto& value){return value.name==entry.name;});
      Check(part!=contents.plan.partitions.end(),"Entry partition is absent");
      LevelResourceGroup group;group.partition=entry.name;
      for(const auto& resource:part->resources){
        if(resource.kind==LevelPartitionResource::Kind::Scene){
          Check(resource.index<part->scenes.size(),"Partition scene index is invalid");
          const auto& source=part->scenes[resource.index];
          // Original partition callback2904A8 excludes the shared GSA.
          if(!shared.contains(key(source)))scene(group,source,resource.authored_flag);
        }else if(resource.kind==LevelPartitionResource::Kind::Ignored){
          Check(resource.index<part->ignored.size(),"Partition ignored-token index is invalid");
          // Original classifier2948C0 bypasses resource admission for type-1.
          // The complete source record remains in the retained LevelPlan.
          continue;
        }else{
          Check(resource.kind==LevelPartitionResource::Kind::Audio,"Invalid partition resource kind");
          Check(resource.index<part->audio.size(),"Partition bank index is invalid");
          const auto found=audio.find(part->audio[resource.index]);
          if(found==audio.end())throw std::runtime_error("Entry soundbank has no converted native binding: "+part->audio[resource.index]);
          const auto& path=found->second;
          Check(path.starts_with("em1/runtime/") && path.ends_with(".bnk") && path.find("..") == std::string::npos &&
            path.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789/.-")==std::string::npos,"Invalid converted soundbank path");
          group.files.push_back({path,path,0});group.authored_flags.push_back(resource.authored_flag);
        }
      }
      groups.push_back(std::move(group));
    }
    output=std::move(groups);error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
}
