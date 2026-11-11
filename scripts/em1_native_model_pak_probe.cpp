// Developer DLL for testing the exact runtime serializers. No game entry point.
#include "em1_native_scene_pak.h"
#include "em1_scene_files.h"
static std::shared_ptr<const std::vector<uint8_t>> packed;
static em2::em1::SceneAdapterResult bound;
static std::string error;
extern "C" __declspec(dllexport) const char* build_typed(const char** names,const uint8_t** data,
    const size_t* sizes,const uint32_t* types,size_t count,const uint8_t** output,size_t* size){
  try {std::vector<em2::em1::NativeScenePakEntry> entries;
    for(size_t i=0;i<count;++i)entries.push_back({names[i],{data[i],sizes[i]},types[i]});
    packed=em2::em1::BuildNativeScenePak(entries);*output=packed->data();*size=packed->size();return nullptr;
  }catch(const std::exception& e){error=e.what();return error.c_str();}
}
extern "C" __declspec(dllexport) const char* bind_models(const uint8_t* bytes,size_t size,
    uint32_t entities,uint32_t components,const char** from,const char** to,size_t count,const uint8_t** output,size_t* output_size){
  em2::em1::SceneAdapterResult scene;scene.bytes.assign(bytes,bytes+size);
  scene.entity_count=entities;scene.component_count=components;
  std::map<std::string,std::string> bindings;
  for(size_t i=0;i<count;++i)bindings.emplace(from[i],to[i]);
  if(!em2::em1::BindDeclaredSceneModelPaths(scene,bindings,bound,error))return error.c_str();
  *output=bound.bytes.data();*output_size=bound.bytes.size();return nullptr;
}
extern "C" __declspec(dllexport) const char* bind_animation_paths(const uint8_t* bytes,size_t size,
    uint32_t entities,uint32_t components,const char** from,const char** to,size_t count,const char** keep,size_t keep_count,const uint8_t** output,size_t* output_size){
  em2::em1::SceneAdapterResult scene;scene.bytes.assign(bytes,bytes+size);
  scene.entity_count=entities;scene.component_count=components;
  std::map<std::string,std::string> bindings;
  for(size_t i=0;i<count;++i)bindings.emplace(from[i],to[i]);
  std::vector<std::string> retained;for(size_t i=0;i<keep_count;++i)retained.emplace_back(keep[i]);
  if(!em2::em1::BindSceneFilePaths(scene,bindings,bound,error,true,".kfm",retained))return error.c_str();
  *output=bound.bytes.data();*output_size=bound.bytes.size();return nullptr;
}
