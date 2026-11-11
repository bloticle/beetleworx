#include "em1_level_collision.h"
#include "em1_motion_states.h"
#include "em1_resource_catalog.h"
#include "em1_jigsaw_havok.h"
#include <bit>
#include <cmath>
#include <functional>
namespace em2::em1 {
bool PrepareLevelCollision(std::shared_ptr<RvzSource> source,const LevelContents& contents,LevelCollisionPlan& result,std::string& error,
    std::string_view native_triangle_path,bool native_triangle_accelerated){
 using namespace motion_state_detail;
 try{
  LevelCollisionPlan plan;OriginalResourceCatalog catalog;std::vector<SceneResourceLookup> lookups;
  auto resource_key=[](std::string_view value){std::string result(value);for(auto& c:result){if(c=='\\')c='/';if(c>='A'&&c<='Z')c+='a'-'A';}
    if(result.ends_with("_wii"))result.resize(result.size()-4);return result;};
  const auto native_selection=resource_key(native_triangle_path);const bool all_native=native_selection=="*";size_t native_triangles=0;
  if(!BuildOriginalResourceCatalog(source,catalog,error)||!PlanLevelResourceScope(catalog,contents,lookups,error))return false;
  struct Definition{const SceneAdapterResult* scene;const Entity* entity;};
  std::vector<std::vector<Entity>> layouts;for(const auto& s:contents.entry_scenes)layouts.push_back(Parse(s.resource.scene));
  std::map<uint32_t,Definition> definitions;
  for(size_t i=0;i<layouts.size();++i)for(const auto& e:layouts[i])definitions.try_emplace(e.id[0],Definition{&contents.entry_scenes[i].resource.scene,&e});
  std::set<uint32_t> resolving;
  std::function<CollisionPlacement(uint32_t)> placement=[&](uint32_t id){
   if(auto found=plan.placements.find(id);found!=plan.placements.end())return found->second;
   const auto found=definitions.find(id);if(found==definitions.end()||!resolving.insert(id).second)throw std::runtime_error("Missing/cyclic collision transform ancestor");
   const auto& entity=*found->second.entity;const auto& bytes=found->second.scene->bytes;
   CollisionPlacement p;if(entity.id[1])p=placement(entity.id[1]);
   auto value=[&](size_t at){const auto f=std::bit_cast<float>(Word(bytes,at));if(!std::isfinite(f))throw std::runtime_error("Nonfinite source collision transform");return f;};
   for(const auto& component:entity.components)if(component.name=="JPSTransformationComponent")for(const auto& property:component.properties){
    const auto at=property.begin+16;
    if(property.name=="Translation")for(unsigned a=0;a<3;++a)p.pose[12+a]=value(at+a*4);
    if(property.name=="Rotation")for(unsigned c=0;c<3;++c)for(unsigned r=0;r<3;++r)p.pose[c*4+r]=value(at+(r*3+c)*4);
    if(property.name=="Scale")p.scale=value(at);
   }
   resolving.erase(id);plan.placements.emplace(id,p);return p;
  };
  for(const auto& [id,definition]:definitions)placement(id);
  // A source definition may be inherited by several placed instances. Keep
  // a shared resource on the legacy path if any authored user is scaled;
  // the stock static initializer applies rotation/translation, not scale.
  std::map<uint32_t,std::set<std::string>> own_hulls,effective_hulls;
  for(const auto& lookup:lookups){const auto& ref=lookup.reference;
   if(ref.component_name=="PhysicsRigidBodyStaticComponent" && ref.property=="RB Hull File Path")
    own_hulls[ref.entity].insert(resource_key(ref.path));
  }
  std::function<const std::set<std::string>&(uint32_t)> inherited_hulls=[&](uint32_t id)->const std::set<std::string>&{
   if(auto found=effective_hulls.find(id);found!=effective_hulls.end())return found->second;
   auto values=own_hulls[id];const auto parent=definitions.at(id).entity->id[1];
   if(parent){const auto& inherited=inherited_hulls(parent);values.insert(inherited.begin(),inherited.end());}
   return effective_hulls.emplace(id,std::move(values)).first->second;
  };
  std::set<std::string> scaled_resources;
  for(const auto& [id,definition]:definitions)if(plan.placements.at(id).scale!=1){
   const auto& paths=inherited_hulls(id);scaled_resources.insert(paths.begin(),paths.end());
  }
  auto retain_fallback=[&](uint32_t component,const std::shared_ptr<const AuthoredStaticCollision>& shape){
   const auto [found,inserted]=plan.components.emplace(component,shape);
   if(!inserted && found->second!=shape)throw std::runtime_error("Conflicting scoped static collision component ID");
  };
  std::map<std::string,std::shared_ptr<const AuthoredStaticCollision>> cache;
  std::map<std::string,std::shared_ptr<const LevelCollisionPlan::NativeHull>> native_cache;
  std::set<std::string> rejected;
  for(const auto& lookup:lookups){const auto& ref=lookup.reference;
   if(ref.component_name!="PhysicsRigidBodyStaticComponent" || ref.property!="RB Hull File Path")continue;
   ++plan.references;std::string why;
   if(lookup.query.presence!=OriginalResourcePresence::InScope || lookup.query.locations.size()!=1 || lookup.query.locations.front().archive.empty()){
    plan.unsupported.push_back(ref.path+": missing/ambiguous authored scope");continue;
   }
   const auto& location=lookup.query.locations.front();const auto key=location.archive+"\n"+location.path;
   if(rejected.contains(key))continue;
   if(auto found=native_cache.find(key);found!=native_cache.end()){
    plan.native_bindings.push_back({lookup.scene_index,ref.component,ref.path,found->second});plan.native_components.insert(ref.component);
    if(found->second->fallback_geometry)retain_fallback(ref.component,found->second->fallback_geometry);
    continue;
   }
   auto found=cache.find(key);
   if(found==cache.end()){
    auto pak=PakSource::Open(source,location.archive,why);std::vector<uint8_t> bytes;auto shape=std::make_shared<AuthoredStaticCollision>();
    if(!pak || !pak->ReadOriginal(location.path,bytes,why)){rejected.insert(key);plan.unsupported.push_back(ref.path+": "+why);continue;}
    const bool requested=all_native || (!native_selection.empty() && resource_key(ref.path)==native_selection);
    const bool triangle_candidate=requested && ReconstructAuthoredStaticCollision(bytes,*shape,why);
    if(requested && !all_native && !triangle_candidate)throw std::runtime_error("Requested native triangle resource could not be reconstructed: "+why);
    if(triangle_candidate){
     ConvertedConvexBodyResource converted;
     const bool unscaled=!scaled_resources.contains(resource_key(ref.path));
     const bool converted_ok=unscaled && ConvertStaticTriangleBodyResource(bytes,converted,why,native_triangle_accelerated);
     if(converted_ok){
      auto native=std::make_shared<LevelCollisionPlan::NativeHull>();
      native->origin=key+(native_triangle_accelerated?":native-static-system-mopp1":":native-static-system-unaccelerated1");native->bytes=std::move(converted.pack);
      native->fallback_geometry=shape;retain_fallback(ref.component,shape);
      if(native_triangle_accelerated){
       auto accelerated=std::make_shared<AuthoredStaticCollision>(*shape);
       if(!AccelerateAuthoredStaticCollision(*accelerated,why))throw std::runtime_error("Native static acceleration proof preparation failed: "+why);
       native->acceleration_proof=std::move(accelerated);
      }
      cache.emplace(key,shape);native_cache.emplace(key,native);
      plan.native_bindings.push_back({lookup.scene_index,ref.component,ref.path,std::move(native)});
      plan.native_components.insert(ref.component);++native_triangles;continue;
     }
     if(!unscaled)why="Shared source resource has an authored scaled placement";
     if(!all_native)throw std::runtime_error("Native static resource conversion failed: "+why);
     plan.native_fallbacks.push_back(ref.path+": "+why);
    }
    if(!triangle_candidate && !ReconstructAuthoredStaticCollision(bytes,*shape,why)){
     const auto mesh_error=why;ConvertedConvexBodyResource converted;
     if(!ConvertConvexBodyResource(bytes,converted,why) && !ConvertCompoundBodyResource(bytes,converted,why) && !ConvertBoxBodyResource(bytes,converted,why) && !ConvertConvexBodiesResource(bytes,converted,why)){
      rejected.insert(key);plan.unsupported.push_back(ref.path+": "+mesh_error+"; "+why);continue;
     }
     auto native=std::make_shared<LevelCollisionPlan::NativeHull>();native->origin=key;native->bytes=std::move(converted.pack);
     native_cache.emplace(key,native);plan.native_bindings.push_back({lookup.scene_index,ref.component,ref.path,std::move(native)});plan.native_components.insert(ref.component);continue;
    }
    found=cache.emplace(key,std::move(shape)).first;
   }
   const auto [bound,inserted]=plan.components.emplace(ref.component,found->second);
   if(!inserted && bound->second!=found->second)throw std::runtime_error("Conflicting scoped static collision component ID");
  }
  if(!native_selection.empty() && !all_native && !native_triangles)throw std::runtime_error("Requested original static triangle resource was not found in the level scope");
  std::set<std::string> resource_keys;for(const auto& [key,value]:cache)resource_keys.insert(key);for(const auto& [key,value]:native_cache)resource_keys.insert(key);
  plan.resources=resource_keys.size();result=std::move(plan);error.clear();return true;
 }catch(const std::exception& e){error=e.what();return false;}
}
}
