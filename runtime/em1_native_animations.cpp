#include "em1_native_animations.h"
namespace em2::em1 {
bool ReleaseNativeAnimationBundle(PPCContext& ctx,uint8_t* base,NativeAnimationBundle& bundle,std::string& error){
  if(!ReleaseNativeModel(ctx,base,bundle.animation,error))return false;
  for(auto i=bundle.keyframes.rbegin();i!=bundle.keyframes.rend();++i)if(!ReleaseNativeKeyframe(ctx,base,*i,error))return false;
  for(auto i=bundle.models.rbegin();i!=bundle.models.rend();++i)if(!ReleaseNativeModel(ctx,base,*i,error))return false;
  bundle={};error.clear();return true;
}
bool PublishNativeAnimationBundle(PPCContext& ctx,uint8_t* base,uint32_t manager,const KfmClosure& source,NativeAnimationBundle& output,std::string& error){
  if(output.animation.record || !output.models.empty() || !output.keyframes.empty()){error="Animation bundle output already owns resources";return false;}
  if(!source.source || source.origin_key.empty() || source.models.size()>16 || source.keyframes.size()>256){error="Invalid original animation closure";return false;}
  const auto name=ReserveNativeAnimationManagerName();std::map<std::string,std::string> bindings;
  NativeAnimationBundle bundle;bundle.models.resize(source.models.size());bundle.keyframes.resize(source.keyframes.size());
  // Complete host path allocations and rebinding before acquiring resources.
  std::vector<std::string> model_names(source.models.size()),keyframe_names(source.keyframes.size());
  for(size_t i=0;i<source.models.size();++i){model_names[i]=ReserveNativeModelName();bindings.emplace(source.models[i].source_path,model_names[i]);}
  for(size_t i=0;i<source.keyframes.size();++i){keyframe_names[i]=ReserveNativeKeyframeName();bindings.emplace(source.keyframes[i].source_path,keyframe_names[i]);}
  BoundKfmResource rebound;rebound.origin_key=source.origin_key;rebound.source_document=source.document;
  rebound.origin_key+=":"+name;for(const auto& [path,target]:bindings)rebound.origin_key+=":"+std::to_string(path.size())+":"+path+":"+target;
  if(!BindKfmPaths(source.original,source.resource,name,bindings,rebound.bytes,error))return false;
  // The stock KFM resolver now sees real, staged source resources.
  bool ready=true;
  for(size_t i=0;i<source.models.size() && ready;++i)ready=PublishNativeModel(ctx,base,manager,source.models[i].closure,bundle.models[i],error,model_names[i]);
  for(size_t i=0;i<source.keyframes.size() && ready;++i)ready=PublishNativeKeyframe(ctx,base,manager,source.source,source.keyframes[i].keyframe,bundle.keyframes[i],error,keyframe_names[i]);
  if(ready)ready=PublishNativeAnimationManager(ctx,base,manager,source.source,rebound,name,bundle.animation,error);
  if(!ready){const auto failure=error;std::string cleanup;if(!ReleaseNativeAnimationBundle(ctx,base,bundle,cleanup)){
      output=std::move(bundle);error=failure+"; cleanup incomplete: "+cleanup;return false;}
    error=failure;return false;}
  output=std::move(bundle);error.clear();return true;
}
}
