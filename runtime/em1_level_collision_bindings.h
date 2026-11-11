#pragma once
#include "em1_level_collision.h"
#include "em1_level_models.h"
namespace em2::em1 {
inline bool BindNativeCollisionAssets(const LevelCollisionPlan& plan,std::string_view prefix,
    std::vector<LevelSceneModelAssets>& assets,std::string& error){
 try{
  if(!prefix.starts_with("em1_level_") || prefix.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789")!=prefix.npos)
   throw std::runtime_error("Invalid native collision namespace");
  auto normalize=[](std::string s){for(auto& c:s){if(c=='\\')c='/';if(c>='A'&&c<='Z')c+=32;}return s;};
  for(size_t i=0;i<assets.size();++i){auto& target=assets[i];
   if(!target.physics_resources.empty())throw std::runtime_error("Collision assets already bound");
   std::map<std::string,std::string> bindings,origins;
   for(const auto& binding:plan.native_bindings)if(binding.scene==i){
    const auto& resource=*binding.resource;auto found=origins.find(resource.origin);
    if(found==origins.end()){
     const auto name="em1/runtime/"+std::string(prefix)+"_s"+std::to_string(i)+"_hull_"+std::to_string(target.physics_resources.size())+".hkx";
     target.physics_resources.push_back({name,resource.origin,resource.bytes,0x484B5800});found=origins.emplace(resource.origin,name).first;
    }
    auto [current,inserted]=bindings.emplace(normalize(binding.path),found->second);
    if(!inserted && current->second!=found->second)throw std::runtime_error("Conflicting scoped hull path");
   }
   if(bindings.empty())continue;
   std::vector<SceneFileReference> refs;if(!CollectDeclaredSceneAssetReferences(target.bound_scene,refs,error))return false;
   std::vector<std::string> retained;
   for(const auto& ref:refs)if(SceneFileExtension(normalize(ref.path))==".hkx" && !bindings.contains(normalize(ref.path)))retained.push_back(ref.path);
   SceneAdapterResult bound;if(!BindSceneFilePaths(target.bound_scene,bindings,bound,error,true,".hkx",retained))return false;
   target.bound_scene=std::move(bound);
  }
  error.clear();return true;
 }catch(const std::exception& e){error=e.what();return false;}
}
}
