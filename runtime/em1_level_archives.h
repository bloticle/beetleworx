#pragma once
#include "em1_level_source.h"
#include "em1_native_scene_pak.h"
#include "em1_single_gsa_pak.h"
#include <atomic>

namespace em2::em1 {
struct LevelSceneArchive {
  std::string source_origin,archive_basename,registry_gsa,native_scene;
  size_t scene_index=0;
  std::shared_ptr<const std::vector<uint8_t>> bytes;
};
inline std::string ReserveLevelArchivePrefix(){
  static std::atomic<uint64_t> sequence{0};
  return "em1_level_"+std::to_string(sequence.fetch_add(1,std::memory_order_relaxed));
}
// The native registry keeps .gsa names but requests a basename-derived .pak
// and a .bin scene resource. Each scene gets an origin-owned unique basename.
// These archives contain structural scene conversion only. Platform assets,
// component compatibility and native publication must be completed separately.
inline bool PrepareLevelSceneArchives(const LevelContents& contents,std::string_view prefix,
    std::vector<LevelSceneArchive>& output,std::string& error){
  try {
    auto check=[](bool value,const char* why){if(!value)throw std::runtime_error(why);};
    check(prefix.starts_with("em1_level_") && prefix.size()>10 && prefix.size()<=64 &&
      prefix.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789")==std::string_view::npos,
      "Invalid level archive namespace");
    check(!contents.plan.origin_key.empty() && !contents.entry_scenes.empty() && contents.entry_scenes.size()<=1024,
      "Level archive preparation requires original scene contents");
    std::vector<LevelSceneArchive> result;size_t total=0;
    for(size_t i=0;i<contents.entry_scenes.size();++i){
      const auto& scene=contents.entry_scenes[i];
      check(!scene.resource.origin_key.empty(),"Level scene has no original resource identity");
      LevelSceneArchive archive;archive.scene_index=i;archive.source_origin=scene.resource.origin_key;
      archive.archive_basename=std::string(prefix)+"_s"+std::to_string(i);
      archive.registry_gsa="em1/runtime/"+archive.archive_basename+".gsa";
      archive.native_scene="em1/runtime/"+archive.archive_basename+".bin";
      NativeScenePakEntry entry{archive.native_scene,scene.resource.scene.bytes};
      archive.bytes=BuildNativeScenePak(std::span(&entry,1));
      check(archive.bytes->size()<=256ull*1024*1024-total,"Level scene archives exceed memory budget");
      total+=archive.bytes->size();result.push_back(std::move(archive));
    }
    output=std::move(result);error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
// Stages complete entry scenes in the native archive filesystem. This does not
// register zones or instantiate entities; callers must bind platform assets
// before asking the native engine to activate these registry resource names.
inline bool MountLevelSceneContents(rex::filesystem::VirtualFileSystem& fs,
    const LevelContents& contents,std::vector<LevelSceneArchive>& output,std::string& error){
  std::vector<LevelSceneArchive> archives;
  if(!PrepareLevelSceneArchives(contents,ReserveLevelArchivePrefix(),archives,error))return false;
  std::vector<NativeScenePakRequest> requests;requests.reserve(archives.size());
  for(const auto& archive:archives){
    const auto& bytes=contents.entry_scenes[archive.scene_index].resource.scene.bytes;
    requests.push_back({archive.archive_basename,{{archive.native_scene,bytes}}});
  }
  if(!MountNativeScenePaks(fs,requests,error))return false;
  output=std::move(archives);return true;
}

}
