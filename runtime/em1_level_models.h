#pragma once
#include "em1_level_source.h"
#include "em1_scene_files.h"
#include "em1_model_closure.h"
#include "em1_nif_materials.h"
#include "em1_native_scene_pak.h"
#include "em1_material_effect_vfs.h"
#include <rex/logging.h>
#include <set>

namespace em2::em1 {
struct LevelSceneModelAssets {
  struct Model {
    std::string source_path,native_name,origin_key;
    std::vector<std::string> texture_origins;
    std::vector<uint8_t> bytes;
    std::vector<std::string> missing_texture_origins;
  };
  SceneAdapterResult bound_scene;
  std::vector<Model> models;
  struct AnimationResource {
    std::string native_name,origin_key;
    std::vector<uint8_t> bytes;
    uint32_t resource_type=0;
  };
  std::vector<AnimationResource> animation_resources;
  std::vector<AnimationResource> physics_resources;
  size_t animation_managers=0;
  std::vector<std::string> unresolved_animations;
};
struct LevelModelImportFailure {
  size_t scene=0;
  std::string archive,path,message;
};
// Host preparation from the ORIGINAL descriptor's archive scope. Produces
// native NIF bytes and complete scenes with only declared NIF paths rebound.
// Does not register a native resource, activate a world or assert readiness of
// physics, scripts, audio, animation managers or baked lighting.
inline bool PrepareLevelSceneModels(std::shared_ptr<RvzSource> source,const LevelContents& contents,
    std::string_view prefix,std::vector<LevelSceneModelAssets>& output,std::string& error,
    std::vector<LevelModelImportFailure>* failures=nullptr){
  if(failures)failures->clear();
  try {
    auto check=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
    auto key=[](std::string text){for(auto& ch:text){if(ch=='\\')ch='/';if(ch>='A'&&ch<='Z')ch+=32;}return text;};
    check(source && prefix.starts_with("em1_level_") && prefix.size()<=64 &&
      prefix.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789")==prefix.npos,"Invalid level model namespace");
    std::vector<std::string> archives;std::set<std::string> seen;
    for(const auto& scene:contents.entry_scenes)if(seen.insert(key(scene.request.archive)).second)archives.push_back(scene.request.archive);
    const auto simple_effects=MountedSimpleMaterialEffects();
    std::array<size_t,4> alias_models{},alias_meshes{};
    size_t template_models=0,template_meshes=0;
    std::vector<LevelSceneModelAssets> prepared;size_t total=0;
    for(size_t index=0;index<contents.entry_scenes.size();++index){
      const auto& scene=contents.entry_scenes[index];LevelSceneModelAssets assets;
      std::vector<SceneFileReference> refs;if(!CollectDeclaredSceneAssetReferences(scene.resource.scene,refs,error))return false;
      std::map<std::string,std::string> bindings;std::set<std::string> visited;bool failed=false;
      for(const auto& ref:refs){const auto path=key(ref.path);
        if(SceneFileExtension(path)!=".nif" || !visited.insert(path).second)continue;
        const auto original=path.ends_with("_wii")?path:path+"_wii";
        try {
        check(assets.models.size()<1023,"Level scene model count exceeds native archive budget");
        ModelClosure closure;
        if(!ReadModelClosure(source,scene.request.archive,original,closure,error,nullptr,archives)){
          throw std::runtime_error(error);
        }
        // Full level models need the same audited blend/cutout mappings and
        // native depth-prepass metadata as the single-scene entry path.
        // Unknown techniques retain their authored properties.
        nif::Nif native_model(closure.model.bytes);
        std::array<uint32_t,4> mappings{};
        AdaptPreviewDiffuseMaterials(native_model,true,true,true,simple_effects,&mappings);
        const auto template_count=AdaptWiiTemplateRenderStates(native_model,simple_effects);
        template_models+=template_count!=0;template_meshes+=template_count;
        for(size_t i=0;i<4;++i){alias_models[i]+=mappings[i]!=0;alias_meshes[i]+=mappings[i];}
        closure.model.bytes=native_model.Write();
        closure.model.origin_key+=":native-level-materials-depth6-template-pass2-effects"+std::to_string(simple_effects);
        check(closure.model.bytes.size()<=512ull*1024*1024-total,"Level native model bytes exceed memory budget");
        total+=closure.model.bytes.size();
        const auto name="em1/runtime/"+std::string(prefix)+"_s"+std::to_string(index)+"_model_"+std::to_string(assets.models.size())+".nif";
        bindings.emplace(path,name);assets.models.push_back({path,name,std::move(closure.model.origin_key),std::move(closure.texture_origins),std::move(closure.model.bytes),std::move(closure.missing_texture_origins)});
        }catch(const std::exception& failure){
          error="Level model '"+original+"' in '"+scene.request.archive+"': "+failure.what();
          if(!failures)return false;
          check(failures->size()<4096,"Level model diagnostics exceeded failure budget");
          failures->push_back({index,scene.request.archive,original,failure.what()});failed=true;
        }
      }
      if(failed)continue;
      if(!BindDeclaredSceneModelPaths(scene.resource.scene,bindings,assets.bound_scene,error))return false;
      prepared.push_back(std::move(assets));
    }
    if(failures && !failures->empty()){
      error=std::to_string(failures->size())+" original level model imports failed; no partial level was published";return false;
    }
    REXLOG_INFO("[em1] native material aliases: mounted_mask={} S_models={} G127_models={} Q127_models={} H127_models={} S_meshes={} G127_meshes={} Q127_meshes={} H127_meshes={}",
        simple_effects,alias_models[0],alias_models[1],alias_models[2],alias_models[3],alias_meshes[0],alias_meshes[1],alias_meshes[2],alias_meshes[3]);
    REXLOG_INFO("[em1] Wii template PE states translated: models={} meshes={}; dedicated native depth passes when mounted, original shader inputs preserved",template_models,template_meshes);
    output=std::move(prepared);error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
inline std::vector<NativeScenePakEntry> LevelSceneModelPakEntries(
    const LevelSceneModelAssets& assets,std::string_view scene_name){
  std::vector<NativeScenePakEntry> entries{{scene_name,assets.bound_scene.bytes}};
  for(const auto& model:assets.models)entries.push_back({model.native_name,model.bytes,0x4E494600});
  for(const auto& resource:assets.animation_resources)entries.push_back({resource.native_name,resource.bytes,resource.resource_type});
  for(const auto& resource:assets.physics_resources)entries.push_back({resource.native_name,resource.bytes,resource.resource_type});
  return entries;
}
}
