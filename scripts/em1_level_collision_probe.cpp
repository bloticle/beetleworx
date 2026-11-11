#include "em1_level_collision.h"
#include "em1_level_collision_bindings.h"
static std::shared_ptr<em2::em1::RvzSource> source;
static em2::em1::LevelContents contents;
static em2::em1::LevelCollisionPlan plan;
static std::string error;
static std::vector<em2::em1::LevelSceneModelAssets> bound;
extern "C" __declspec(dllexport) const char* prepare(const wchar_t* image,const char* level,uint32_t* references,uint32_t* resources,uint32_t* components,uint32_t* unsupported){
 source=em2::em1::RvzSource::Open(image,error);
 if(!source || !em2::em1::ReadLevelContents(source,level,contents,error) || !em2::em1::PrepareLevelCollision(source,contents,plan,error))return error.c_str();
 bound.clear();for(const auto& scene:contents.entry_scenes){em2::em1::LevelSceneModelAssets item;item.bound_scene=scene.resource.scene;bound.push_back(std::move(item));}
 if(!em2::em1::BindNativeCollisionAssets(plan,"em1_level_probe",bound,error))return error.c_str();
 *references=uint32_t(plan.references);*resources=uint32_t(plan.resources);*components=uint32_t(plan.components.size());*unsupported=uint32_t(plan.unsupported.size());return nullptr;
}
extern "C" __declspec(dllexport) const uint8_t* scene(uint32_t index,size_t* size){if(index>=bound.size())return nullptr;*size=bound[index].bound_scene.bytes.size();return bound[index].bound_scene.bytes.data();}
extern "C" __declspec(dllexport) uint32_t native_hulls(){return uint32_t(plan.native_bindings.size());}
extern "C" __declspec(dllexport) int binding_kind(uint32_t component){return plan.native_components.contains(component)?2:plan.components.contains(component)?1:0;}
extern "C" __declspec(dllexport) const uint8_t* hull(uint32_t index,uint32_t ordinal,size_t* size,uint32_t* type){
 if(index>=bound.size() || ordinal>=bound[index].physics_resources.size())return nullptr;
 const auto& resource=bound[index].physics_resources[ordinal];*size=resource.bytes.size();*type=resource.resource_type;return resource.bytes.data();
}
extern "C" __declspec(dllexport) int placement(uint32_t entity,float* out){
 const auto found=plan.placements.find(entity);if(found==plan.placements.end())return 0;
 std::copy(found->second.pose.begin(),found->second.pose.end(),out);out[16]=found->second.scale;return 1;
}
extern "C" __declspec(dllexport) const char* failure(uint32_t index){return index<plan.unsupported.size()?plan.unsupported[index].c_str():nullptr;}
extern "C" __declspec(dllexport) int component(uint32_t id,uint32_t* bodies,uint64_t* triangles){
 const auto found=plan.components.find(id);if(found==plan.components.end())return 0;
 *bodies=uint32_t(found->second->bodies.size());*triangles=found->second->triangles;return 1;
}
static em2::em1::AuthoredStaticCollision accelerated;
extern "C" __declspec(dllexport) const char* accelerate(uint32_t id,uint32_t* bodies,uint64_t* triangles){
 const auto found=plan.components.find(id);if(found==plan.components.end())return "Unknown collision component";
 accelerated=*found->second;
 if(!em2::em1::AccelerateAuthoredStaticCollision(accelerated,error))return error.c_str();
 *bodies=uint32_t(accelerated.bodies.size());*triangles=accelerated.triangles;return nullptr;
}
extern "C" __declspec(dllexport) const uint8_t* accelerated_shape(uint32_t index,size_t* size){
 if(index>=accelerated.bodies.size())return nullptr;
 const auto& pack=accelerated.bodies[index].native_shape_pack;*size=pack.size();return pack.data();
}
extern "C" __declspec(dllexport) const uint8_t* original_shape(uint32_t component,uint32_t index,size_t* size){
 const auto found=plan.components.find(component);
 if(found==plan.components.end() || index>=found->second->bodies.size())return nullptr;
 const auto& pack=found->second->bodies[index].native_shape_pack;*size=pack.size();return pack.data();
}
