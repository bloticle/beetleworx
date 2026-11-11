#pragma once
#include "em1_pack_source.h"
#include <utility>
#include <stdexcept>
struct Em1LuaWorld;
namespace em2::em1 {
struct NativeScriptResource {
  std::string origin_key,path;
  // Identical script copies can occur in several authored scene archives.
  // Execute one copy, retaining all contributing source identities.
  std::vector<std::string> source_origins;
  std::vector<uint8_t> bytecode;
  std::array<uint8_t,32> original_sha256{},executable_sha256{};
};
struct NativeScriptState {
  NativeScriptState()=default;
  NativeScriptState(const NativeScriptState&)=delete;
  NativeScriptState& operator=(const NativeScriptState&)=delete;
  NativeScriptState(NativeScriptState&& other){*this=std::move(other);}
  NativeScriptState& operator=(NativeScriptState&& other){
    if(this==&other)return *this;
    if(native_globals_store || cutscene_locks[0] || cutscene_locks[1])throw std::logic_error("Release native script resources before replacing their owner");
    cutscene_controllers=std::exchange(other.cutscene_controllers,{});
    cutscene_locks=std::exchange(other.cutscene_locks,{});
    vm=std::move(other.vm);scripts=other.scripts;functions=other.functions;globals=other.globals;
    player=other.player;player_binding_failed=other.player_binding_failed;definition_sha256=other.definition_sha256;
    native_globals_store=std::exchange(other.native_globals_store,0);
    native_globals_failed=std::exchange(other.native_globals_failed,false);return *this;
  }
  std::shared_ptr<Em1LuaWorld> vm;
  uint32_t scripts=0,functions=0,globals=0;
  uint32_t player=0;
  bool player_binding_failed=false;
  // Owned guest allocation. Create/release only on the simulation thread.
  std::array<uint8_t,32> definition_sha256{};
  uint32_t native_globals_store=0;
  bool native_globals_failed=false;
  std::array<uint32_t,2> cutscene_controllers{},cutscene_locks{};
};
// Import-worker only. Loads original bytes from RVZ, validates native Lua 5.1
// transport, and retains owned bytecode. No execution or HKS publication.
bool PrepareOriginalScripts(std::shared_ptr<RvzSource>,std::string_view archive,
    std::vector<NativeScriptResource>& output,std::string& error);
bool InitializeOriginalScripts(std::shared_ptr<RvzSource> source,std::span<const NativeScriptResource>,NativeScriptState&,std::string& error);
struct LevelContents;
class LevelOriginalAudio;
struct LevelOriginalScripts {
  std::string level_origin;
  std::vector<std::string> archives;
  std::vector<std::string> startup_modules;
  std::vector<std::array<uint8_t,16>> source_scene_ids;
  std::vector<NativeScriptResource> resources;
  std::shared_ptr<LevelOriginalAudio> audio;
  NativeScriptState state;
};
// Import-worker operation for the original always_loaded.txt archives and a
// complete authored entry (shared + initial + interior GSAs). Runs the original
// globalscripts root/Openfile chain, then declared ScriptComponent module roots.
// Deduplicates identical same-path chunks; conflicting definitions reject the
// entire transaction. Unreferenced library chunks are not executed. Owns a VM
// and globals schema, not native event/campaign activation or checkpoint state.
// Release native state resources on the simulation thread before replacement.
bool PrepareLevelOriginalScripts(std::shared_ptr<RvzSource>,const LevelContents&,
    LevelOriginalScripts&,std::string& error);
bool TestLevelOriginalScriptLibrary();
void UpdateOriginalScriptPlayer(NativeScriptState&,uint32_t entity);
void TickOriginalScripts(NativeScriptState&,double simulation_delta);
void CancelOriginalScripts(NativeScriptState&);
bool BindOriginalScriptEntity(NativeScriptState&,uint32_t native_handle);
bool UnbindOriginalScriptEntity(NativeScriptState&,uint32_t native_handle);
bool QueueOriginalCutsceneInput(NativeScriptState&,bool restricted);
bool QueueOriginalParticleGroup(NativeScriptState&,std::string_view group,bool enabled);
bool QueueOriginalParticleEmission(NativeScriptState&,uint32_t native_handle,bool enabled);
bool QueueOriginalGearRotation(NativeScriptState&,uint32_t native_handle,float speed);
}
