#pragma once
#include "em1_level_models.h"
#include "em1_kfm_closure.h"
#include "em1_resource_catalog.h"

namespace em2::em1 {
// Stage complete original Gamebryo animation closures in the same typed PAK as
// their scene. The normal KFM factory can resolve its NIF/KF dependencies by
// name; it does not require detached diagnostic publication or extracted files.
inline bool PrepareLevelSceneAnimations(std::shared_ptr<RvzSource> source,const LevelContents& contents,
    std::string_view prefix,std::vector<LevelSceneModelAssets>& assets,std::string& error){
  try {
    auto check=[](bool ok,const char* why){if(!ok)throw std::runtime_error(why);};
    check(source && assets.size()==contents.entry_scenes.size(),"Animation scene ownership mismatch");
    auto key=[](std::string path){for(auto& c:path){if(c=='\\')c='/';if(c>='A'&&c<='Z')c+=32;}return path;};
    std::vector<std::string> archives;std::set<std::string> seen;
    for(const auto& scene:contents.entry_scenes)if(seen.insert(key(scene.request.archive)).second)archives.push_back(scene.request.archive);
    OriginalResourceCatalog catalog;
    if(!BuildOriginalResourceCatalog(source,catalog,error))return false;
    std::vector<SceneResourceLookup> plan;
    if(!PlanLevelResourceScope(catalog,contents,plan,error))return false;
    std::map<std::pair<size_t,std::string>,OriginalResourceQuery> queries;
    for(const auto& lookup:plan)if(SceneFileExtension(key(lookup.reference.path))==".kfm")
      queries.try_emplace(std::pair{lookup.scene_index,key(lookup.reference.path)},lookup.query);
    size_t total=0;
    for(size_t i=0;i<assets.size();++i){
      auto& target=assets[i];check(target.animation_resources.empty(),"Animation assets already prepared");
      std::vector<SceneFileReference> refs;
      if(!CollectDeclaredSceneAssetReferences(target.bound_scene,refs,error))return false;
      std::map<std::string,std::string> bindings;
      const auto stem="em1/runtime/"+std::string(prefix)+"_s"+std::to_string(i)+"_anim_";
      auto append=[&](std::string name,std::string origin,std::vector<uint8_t> bytes,uint32_t type){
        check(!bytes.empty() && bytes.size()<=512ull*1024*1024-total,"Animation payload budget exceeded");
        check(1+target.models.size()+target.animation_resources.size()<1024,"Animation archive entry budget exceeded");
        total+=bytes.size();target.animation_resources.push_back({std::move(name),std::move(origin),std::move(bytes),type});
      };
      for(const auto& ref:refs){const auto path=key(ref.path);
        if(SceneFileExtension(path)!=".kfm" || bindings.contains(path))continue;
        const auto& query=queries.at({i,path});
        if(query.presence!=OriginalResourcePresence::InScope){
          // Authored metadata can reference files absent even on the original
          // disc. Preserve that exact path/component; never invent an asset or
          // silently mark the unresolved resource ready.
          if(std::find(target.unresolved_animations.begin(),target.unresolved_animations.end(),path)==target.unresolved_animations.end())target.unresolved_animations.push_back(path);
          continue;
        }
        check(query.locations.size()==1 && !query.locations.front().archive.empty(),"Animation manager has ambiguous/loose source ownership");
        const auto& location=query.locations.front();
        check(location.type==std::array<uint8_t,4>{'K','F','M',0},"Declared animation manager has wrong type");
        const auto& owner=location.archive;const auto& resource=location.path;
        KfmClosure closure;if(!ReadKfmClosure(source,owner,resource,closure,error,archives)){
          error="Level animation '"+path+"': "+error;return false;
        }
        const auto name=stem+std::to_string(target.animation_managers)+".kfm";
        std::map<std::string,std::string> dependencies;
        for(auto& model:closure.models){std::string native;
          for(const auto& existing:target.models)if(existing.origin_key==model.closure.model.origin_key){native=existing.native_name;break;}
          for(const auto& existing:target.animation_resources)if(existing.resource_type==0x4E494600 && existing.origin_key==model.closure.model.origin_key){native=existing.native_name;break;}
          if(native.empty()){native=stem+"dep_"+std::to_string(target.animation_resources.size())+".nif";
            append(native,model.closure.model.origin_key,std::move(model.closure.model.bytes),0x4E494600);}
          dependencies.emplace(model.source_path,std::move(native));
        }
        for(auto& clip:closure.keyframes){std::string native;
          for(const auto& existing:target.animation_resources)if(existing.resource_type==0x4B460000 && existing.origin_key==clip.keyframe.origin_key){native=existing.native_name;break;}
          if(native.empty()){native=stem+"dep_"+std::to_string(target.animation_resources.size())+".kf";
            append(native,clip.keyframe.origin_key,std::move(clip.keyframe.bytes),0x4B460000);}
          dependencies.emplace(clip.source_path,std::move(native));
        }
        std::vector<uint8_t> rebound;
        if(!BindKfmPaths(closure.original,closure.resource,name,dependencies,rebound,error))return false;
        append(name,closure.origin_key,std::move(rebound),0x4B464D00);
        bindings.emplace(path,name);++target.animation_managers;
      }
      SceneAdapterResult bound;
      if(!BindSceneFilePaths(target.bound_scene,bindings,bound,error,true,".kfm",target.unresolved_animations))return false;
      target.bound_scene=std::move(bound);
    }
    error.clear();return true;
  }catch(const std::exception& failure){error=failure.what();return false;}
}
}
