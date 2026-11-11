#include "em1_asset_preflight.h"
#include "em1_level_models.h"
#include "em1_level_animations.h"
#include "em1_level_collision.h"
#include "em1_level_collision_bindings.h"
#include "em1_script_source.h"
#include "em1_script_events.h"
#include "em1_lua_core/include/em1_lua_core.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <filesystem>
namespace em2::em1 {
bool CheckLevelAssets(std::string_view rvz,std::string_view descriptor,std::string& error,std::string_view callbacks,
    const std::map<std::string,uint32_t>* native_variables,std::string_view native_triangle_path,bool native_triangle_accelerated){
  try {
    if(rvz.empty()){error="Level asset checks require an explicit original RVZ source";return false;}
    const std::filesystem::path path(std::u8string(rvz.begin(),rvz.end()));
    std::shared_ptr<RvzSource> source=RvzSource::Open(path,error);if(!source)return false;
    LevelContents contents;if(!ReadLevelContents(source,descriptor,contents,error))return false;
    size_t ignored_tokens=0;for(const auto& part:contents.plan.partitions)ignored_tokens+=part.ignored.size();
    REXLOG_INFO("[em1] headless full-level asset check: '{}' scenes={} partitions={} original_ignored_tokens={}; read-only source, no window/world publication",descriptor,contents.entry_scenes.size(),contents.plan.partitions.size(),ignored_tokens);
    std::vector<LevelSceneModelAssets> models;std::vector<LevelModelImportFailure> failures;
    std::string model_error,script_error,collision_error;
    const bool models_ready=PrepareLevelSceneModels(source,contents,"em1_level_preflight",models,model_error,&failures);
    for(const auto& failure:failures)REXLOG_ERROR("[em1] model import failure: scene={} archive='{}' path='{}' reason='{}'",failure.scene,failure.archive,failure.path,failure.message);
    bool animations_ready=false;
    if(models_ready)animations_ready=PrepareLevelSceneAnimations(source,contents,"em1_level_preflight",models,model_error);
    LevelOriginalScripts scripts;
    const bool scripts_ready=PrepareLevelOriginalScripts(source,contents,scripts,script_error);
    if(!scripts_ready)REXLOG_ERROR("[em1] original script startup failed: {}",script_error);
    bool callbacks_ready=true;
    uint32_t capabilities=kRegisteredScriptEngineCapabilities;
    if(scripts_ready && native_variables){
      std::vector<Em1LuaAnimationVariableTarget> records;
      for(const auto& [name,id]:*native_variables)records.push_back({reinterpret_cast<const uint8_t*>(name.data()),name.size(),id,0});
      if(em1_lua_world_install_native_animation_variables(scripts.state.vm.get(),records.data(),records.size())!=EM1_LUA_OK){
        Em1LuaWorldView view{};em1_lua_world_view(scripts.state.vm.get(),&view);
        error=view.error_size?std::string(reinterpret_cast<const char*>(view.error),view.error_size):"Native animation variable catalog was rejected";return false;
      }
      capabilities|=512;
    }
    if(!callbacks.empty()){
      if(callbacks.size()>4096){error="Original callback preflight list exceeds bounds";return false;}
      size_t at=0,count=0;
      while(at<=callbacks.size()){
        auto end=callbacks.find(',',at);if(end==callbacks.npos)end=callbacks.size();
        const auto name=callbacks.substr(at,end-at);
        if(name.empty() || name.size()>128 || ++count>64 || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")!=name.npos){error="Invalid original callback preflight name/list";return false;}
        bool ready=false;std::string reason;
        if(scripts_ready){
          ready=em1_lua_world_preflight_callback(scripts.state.vm.get(),reinterpret_cast<const uint8_t*>(name.data()),name.size(),capabilities)==EM1_LUA_OK;
          if(!ready){Em1LuaWorldView view{};em1_lua_world_view(scripts.state.vm.get(),&view);if(view.error_size)reason.assign(reinterpret_cast<const char*>(view.error),view.error_size);}
        }else reason="Original source script startup failed";
        REXLOG_INFO("[em1] original prepared callback preflight: '{}' ready={} reason='{}'; dependency inspection only, no callback execution",name,ready,reason);
        if(!ready){callbacks_ready=false;script_error+=" ["+std::string(name)+": "+reason+"]";}
        if(end==callbacks.size())break;at=end+1;
      }
    }
    LevelCollisionPlan collision;
    bool collision_ready=PrepareLevelCollision(source,contents,collision,collision_error,native_triangle_path,native_triangle_accelerated);
    if(collision_ready && native_triangle_path=="*" && native_triangle_accelerated){
      size_t candidates=0;for(const auto& binding:collision.native_bindings)candidates+=bool(binding.resource->acceleration_proof);
      if(candidates){
        // Real source geometry and multi-scene bindings, deliberately without
        // a game thread. Strict admission must leave the plan untouched;
        // automatic admission must keep every rejected resource's geometry.
        PPCContext absent{};auto strict=collision;std::string rejected;
        nif::Check(!ValidateNativeStaticCollisionAcceleration(absent,nullptr,strict,rejected,false) && !rejected.empty(),"Strict native admission accepted an absent game thread");
        nif::Check(strict.native_bindings.size()==collision.native_bindings.size() && strict.native_components==collision.native_components &&
            strict.native_fallbacks==collision.native_fallbacks,"Strict native rejection partially changed resource bindings");
        auto fallback=collision;
        nif::Check(ValidateNativeStaticCollisionAcceleration(absent,nullptr,fallback,rejected,true),"Native compatibility fallback was rejected");
        size_t retained=0;
        for(const auto& binding:collision.native_bindings){
          if(binding.resource->acceleration_proof){const auto found=fallback.components.find(binding.component);
            nif::Check(found!=fallback.components.end() && found->second==binding.resource->fallback_geometry,"Native fallback lost source collision ownership");++retained;
          }else nif::Check(std::any_of(fallback.native_bindings.begin(),fallback.native_bindings.end(),[&](const auto& current){
              return current.scene==binding.scene && current.component==binding.component && current.resource==binding.resource && current.path==binding.path;
            }),"Native fallback removed an unrelated native hull");
        }
        for(const auto& binding:fallback.native_bindings)nif::Check(!binding.resource->acceleration_proof,"Rejected native resource remained bound");
        REXLOG_INFO("[em1] native static unavailable-thread fallback check: original_bindings={} retained_triangle_bindings={} unrelated_native_bindings={}; original geometry retained, no world publication",
            collision.native_bindings.size(),retained,fallback.native_bindings.size());
      }
    }
    if(collision_ready && !native_triangle_path.empty()){
      collision_ready=models_ready && BindNativeCollisionAssets(collision,"em1_level_preflight",models,collision_error);
      size_t resources=0;for(const auto& scene:models)resources+=scene.physics_resources.size();
      REXLOG_INFO("[em1] native static component binding check: path='{}' ready={} native_components={} bound_resources={}; no world publication",
          native_triangle_path,collision_ready,collision.native_components.size(),resources);
    }
    for(const auto& failure:collision.unsupported)REXLOG_WARN("[em1] unsupported original collision: {}",failure);
    const bool imported=models_ready && animations_ready && scripts_ready && callbacks_ready && collision_ready && collision.unsupported.empty();
    REXLOG_INFO("[em1] full-level asset result: '{}' models={} animation_resources={} scripts={} collision={} unsupported_collision={} model_failures={}; gameplay/audio/progression acceptance is separate",descriptor,models_ready,animations_ready,scripts_ready,collision_ready,collision.unsupported.size(),failures.size());
    if(!imported){error=!model_error.empty()?model_error:!script_error.empty()?script_error:!collision_error.empty()?collision_error:"Original collision resources remain unsupported";return false;}
    error.clear();return true;
  }catch(const std::exception& failure){error=failure.what();return false;}
}
}
