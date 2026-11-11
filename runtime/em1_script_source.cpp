#include "em1_script_source.h"
#include "em1_level_source.h"
#include "em1_scene_files.h"
#include "em1_script_audio.h"
#include "em1_lua_core/include/em1_lua_core.h"
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <map>
#include <set>
#include <rex/logging.h>
namespace em2::em1 {
namespace {
std::string OriginalRelativePathKey(std::string_view path) {
  if(path.empty() || path.size()>2048 || path.front()=='/' || path.front()=='\\')
    throw std::runtime_error("Invalid original resource path");
  std::string result(path);
  for(char& c:result){
    if(static_cast<unsigned char>(c)<32 || c==127 || c==':')throw std::runtime_error("Invalid original resource path");
    if(c=='\\')c='/';else if(c>='A' && c<='Z')c+=32;
  }
  if(result.back()=='/')throw std::runtime_error("Original resource path names a directory");
  for(size_t start=0;start<result.size();){
    const auto end=result.find('/',start);const auto part=std::string_view(result).substr(start,end==result.npos?result.size()-start:end-start);
    if(part.empty() || part=="." || part=="..")throw std::runtime_error("Invalid original resource path component");
    start=end==result.npos?result.size():end+1;
  }
  return result;
}
std::string ScriptPathKey(std::string_view path){
  auto key=OriginalRelativePathKey(path);
  if(!key.ends_with(".lua"))throw std::runtime_error("Original script path must name a Lua resource");
  return key;
}
bool IsOriginalAnimationExecutable(const RvzFile& file){
  const auto key=OriginalRelativePathKey(file.path);
  return key.ends_with(".rel") && file.size>0 && file.size<=32*1024*1024;
}
void AppendOriginalScripts(std::vector<NativeScriptResource>& prepared,
    std::map<std::string,size_t>& paths,size_t& total,std::vector<NativeScriptResource> incoming) {
  for(auto& script:incoming){
    const auto key=ScriptPathKey(script.path);
    if(script.origin_key.empty() || script.bytecode.empty() || script.bytecode.size()>2*1024*1024)
      throw std::runtime_error("Invalid original level script resource");
    const auto found=paths.find(key);
    if(found!=paths.end()){
      auto& previous=prepared.at(found->second);
      if(previous.original_sha256!=script.original_sha256 || previous.executable_sha256!=script.executable_sha256 || previous.bytecode!=script.bytecode)
        throw std::runtime_error("Conflicting original level script '"+key+"' in '"+previous.origin_key+"' and '"+script.origin_key+"'");
      if(std::find(previous.source_origins.begin(),previous.source_origins.end(),script.origin_key)==previous.source_origins.end())
        previous.source_origins.push_back(std::move(script.origin_key));
      continue;
    }
    if(prepared.size()>=512 || total+script.bytecode.size()>64*1024*1024)
      throw std::runtime_error("Original level script library exceeds import limits");
    total+=script.bytecode.size();paths.emplace(key,prepared.size());
    script.source_origins={script.origin_key};prepared.push_back(std::move(script));
  }
}
bool InitializeOriginalScriptModules(std::shared_ptr<RvzSource>,std::span<const NativeScriptResource>,
    const std::vector<std::string>*,NativeScriptState&,std::string&);
bool ReadBootstrapScriptArchives(RvzSource& source,std::vector<std::string>& output,std::string& error){
  std::vector<uint8_t> bytes;if(!source.ReadFile("always_loaded.txt",bytes,error))return false;
  if(bytes.empty() || bytes.size()>65536 || std::find(bytes.begin(),bytes.end(),uint8_t(0))!=bytes.end())
    throw std::runtime_error("Invalid original script bootstrap manifest");
  const std::string text(bytes.begin(),bytes.end());std::vector<std::string> result;
  for(size_t at=0;at<text.size();){
    auto end=text.find('\n',at);if(end==text.npos)end=text.size();auto line=std::string_view(text).substr(at,end-at);at=end+1;
    const auto first=line.find_first_not_of(" \t\r");if(first==line.npos || line[first]==';')continue;
    line=line.substr(first,line.find_last_not_of(" \t\r")-first+1);std::string name(line);
    for(char& c:name)if(c>='A' && c<='Z')c+=32;
    if(!name.ends_with(".pak") || name.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789.-")!=name.npos || name.find("..")!=name.npos || result.size()>=256)
      throw std::runtime_error("Unsupported original script bootstrap archive");
    result.push_back("packfiles/"+name);
  }
  if(result.empty())throw std::runtime_error("Empty original script bootstrap archive list");
  output=std::move(result);return true;
}
}
bool PrepareLevelOriginalScripts(std::shared_ptr<RvzSource> source,const LevelContents& contents,
    LevelOriginalScripts& output,std::string& error) {
  try {
    if(!source || contents.plan.origin_key.empty() || contents.entry_scenes.empty() || contents.entry_scenes.size()>1024)
      throw std::runtime_error("Original level scripts require a bounded authored level entry");
    if(!contents.plan.origin_key.starts_with(source->SourceIdentity()+":pak-v1:"))
      throw std::runtime_error("Original level script source differs from the descriptor owner");
    if(output.state.native_globals_store || output.state.cutscene_locks[0] || output.state.cutscene_locks[1])
      throw std::runtime_error("Release native level script resources before replacing their owner");
    LevelOriginalScripts prepared;prepared.level_origin=contents.plan.origin_key;
    if(!PrepareLevelOriginalAudio(source,contents.plan,prepared.audio,error))return false;
    REXLOG_INFO("[em1] original level audio ownership indexed: declarations={} unique_banks={} retained_bytes={}; exact mounted Audio/Wii declarations, lazy reads; no native62 playback/activation capability",
      prepared.audio->Declarations().size(),prepared.audio->UniqueBanks(),prepared.audio->RetainedBytes());
    std::set<std::string> seen;std::map<std::string,size_t> paths;size_t total=0;
    auto import_archive=[&](std::string archive){
      for(char& c:archive){if(c=='\\')c='/';else if(c>='A' && c<='Z')c+=32;}
      if(!seen.insert(archive).second)return true;
      std::vector<NativeScriptResource> scripts;
      if(!PrepareOriginalScripts(source,archive,scripts,error)){error="Original level script archive '"+archive+"': "+error;return false;}
      AppendOriginalScripts(prepared.resources,paths,total,std::move(scripts));prepared.archives.push_back(std::move(archive));
      return true;
    };
    std::vector<std::string> bootstrap;
    if(!ReadBootstrapScriptArchives(*source,bootstrap,error))return false;
    for(const auto& archive:bootstrap)if(!import_archive(archive))return false;
    constexpr auto global_root="environments/_shared/scripts/globalscripts.lua";
    if(!paths.contains(global_root))throw std::runtime_error("Original bootstrap has no globalscripts.lua module");
    prepared.startup_modules.push_back(global_root);std::set<std::string> roots{global_root};
    for(const auto& scene:contents.entry_scenes){
      prepared.source_scene_ids.push_back(scene.resource.scene.source_uuid);
      if(!import_archive(scene.request.archive))return false;
      std::vector<SceneFileReference> references;
      if(!CollectSceneFileReferences(scene.resource.scene,references,error))return false;
      for(const auto& reference:references)if(reference.component_name=="ScriptComponent" && reference.property=="Lua File Path"){
        const auto key=ScriptPathKey(reference.path);
        if(roots.insert(key).second)prepared.startup_modules.push_back(key);
      }
    }
    for(const auto& root:prepared.startup_modules)if(!paths.contains(root))throw std::runtime_error("Original ScriptComponent module is absent from its authored scope: "+root);
    // Reorder roots only, preserving library-only modules for original Openfile.
    // ScriptManager starts globalscripts first; its nested Openfile statements
    // determine shared helper execution order rather than PAK directory order.
    std::vector<NativeScriptResource> ordered;ordered.reserve(prepared.resources.size());std::set<size_t> consumed;
    for(const auto& root:prepared.startup_modules){const auto index=paths.at(root);ordered.push_back(std::move(prepared.resources[index]));consumed.insert(index);}
    for(size_t i=0;i<prepared.resources.size();++i)if(!consumed.contains(i))ordered.push_back(std::move(prepared.resources[i]));
    prepared.resources=std::move(ordered);
    if(!InitializeOriginalScriptModules(source,prepared.resources,&prepared.startup_modules,prepared.state,error))return false;
    REXLOG_INFO("[em1] complete entry script library prepared: descriptor='{}' archives={} unique_scripts={} startup_modules={} executable_bytes={} functions={}; original global/Openfile/component module initialization, native event/progression dispatch remains separate",
      contents.plan.descriptor,prepared.archives.size(),prepared.resources.size(),prepared.startup_modules.size(),total,prepared.state.functions);
    output=std::move(prepared);error.clear();return true;
  }catch(const std::exception& failure){error=failure.what();return false;}
}
bool TestLevelOriginalScriptLibrary() {
  try {
    // The mount contains ordinary files before its executable. Its discovery
    // must use resource-path validation, never Lua-only module validation.
    const std::vector<RvzFile> mounted{{"opening.bnr",1024},{"Apprentice/State.bsq",4096},
      {"RS5\\Final.REL",8192},{"Environments/Scripts/startup.lua",256},
      {"large.rel",32*1024*1024+1ull},{"empty.rel",0},{"looks.rel.backup",32}};
    std::vector<std::string> executables;
    for(const auto& file:mounted)if(IsOriginalAnimationExecutable(file))executables.push_back(OriginalRelativePathKey(file.path));
    if(executables!=std::vector<std::string>{"rs5/final.rel"} || ScriptPathKey("SCRIPTS\\Startup.LUA")!="scripts/startup.lua")return false;
    try{ScriptPathKey("RS5/Final.rel");return false;}catch(const std::exception&){}
    for(const auto& path:std::vector<std::string>{"../game.rel","/game.rel","disc:game.rel","rs5//game.rel","rs5/./game.rel","rs5/../game.rel","rs5/game.rel/",std::string(2049,'x'),std::string("bad\0.rel",8)}){
      try{IsOriginalAnimationExecutable({path,1024});return false;}catch(const std::exception&){}
    }
    auto resource=[](std::string path,std::string origin,uint8_t value){
      NativeScriptResource r;r.path=std::move(path);r.origin_key=std::move(origin);
      r.bytecode={0,0x1b,value,0xff};r.original_sha256[0]=value;r.executable_sha256[0]=value;return r;
    };
    std::vector<NativeScriptResource> scripts;std::map<std::string,size_t> paths;size_t total=0;
    AppendOriginalScripts(scripts,paths,total,{resource("Scripts/Startup.lua","shared:startup",1),resource("Scripts/Area.lua","shared:area",2)});
    AppendOriginalScripts(scripts,paths,total,{resource("scripts\\STARTUP.lua","entry:startup",1),resource("Scripts/Interior.lua","interior:new",3)});
    if(scripts.size()!=3 || total!=12 || scripts[0].path!="Scripts/Startup.lua" ||
       scripts[0].source_origins!=std::vector<std::string>{"shared:startup","entry:startup"} || scripts[2].path!="Scripts/Interior.lua")return false;
    // Same origin is not repeatedly retained. Separate path names remain
    // separate executable chunks even when their bytes happen to be identical.
    AppendOriginalScripts(scripts,paths,total,{resource("scripts/startup.lua","entry:startup",1),resource("Scripts/Another.lua","entry:another",1)});
    if(scripts.size()!=4 || total!=16 || scripts[0].source_origins.size()!=2)return false;
    auto rejects=[&](NativeScriptResource r){
      auto candidate=scripts;auto candidate_paths=paths;auto candidate_total=total;
      try{AppendOriginalScripts(candidate,candidate_paths,candidate_total,{std::move(r)});return false;}catch(const std::exception&){return true;}
    };
    auto different=resource("scripts/startup.lua","other:changed",1);different.bytecode[0]=7;
    if(!rejects(different) || !rejects(resource("Scripts/Startup.lua","other:hash",7)))return false;
    for(const char* name:{"../startup.lua","/startup.lua","scripts//startup.lua","scripts/../startup.lua","scripts/startup.lua/","scripts/startup.bin","disc:startup.lua"})
      if(!rejects(resource(name,"unsafe",1)))return false;
    auto no_origin=resource("scripts/no-origin.lua","",1);if(!rejects(no_origin))return false;
    auto empty=resource("scripts/empty.lua","entry:empty",1);empty.bytecode.clear();if(!rejects(empty))return false;
    size_t full_bytes=64*1024*1024;auto full_scripts=scripts;auto full_paths=paths;
    try{AppendOriginalScripts(full_scripts,full_paths,full_bytes,{resource("scripts/overflow.lua","entry:overflow",1)});return false;}catch(const std::exception&){}
    std::vector<NativeScriptResource> many;std::map<std::string,size_t> many_paths;size_t many_bytes=0;
    for(unsigned i=0;i<512;++i)AppendOriginalScripts(many,many_paths,many_bytes,{resource("scripts/"+std::to_string(i)+".lua","entry:"+std::to_string(i),1)});
    try{AppendOriginalScripts(many,many_paths,many_bytes,{resource("scripts/513.lua","entry:513",1)});return false;}catch(const std::exception&){}
    REXLOG_INFO("[em1] level script library tests passed: mixed mount REL discovery independent of Lua paths, safe relative normalization, authored order, case/slash aliases, binary/hash conflicts, source provenance, path rejection and aggregate byte/chunk bounds");
    return true;
  }catch(const std::exception& failure){REXLOG_ERROR("[em1] level script library test failed: {}",failure.what());return false;}
}
bool BindOriginalScriptEntity(NativeScriptState& state,uint32_t handle){
  return state.vm && handle && em1_lua_world_bind_entity(state.vm.get(),handle)==EM1_LUA_OK;
}
bool UnbindOriginalScriptEntity(NativeScriptState& state,uint32_t handle){
  return !state.vm || !handle || em1_lua_world_unbind_entity(state.vm.get(),handle)==EM1_LUA_OK;
}
bool QueueOriginalCutsceneInput(NativeScriptState& state,bool restricted){
  const std::string_view name=restricted?"RestrictCutSceneInput":"UnrestrictCutSceneInput";
  return state.vm && em1_lua_world_start_args(state.vm.get(),reinterpret_cast<const uint8_t*>(name.data()),name.size(),nullptr,0)==EM1_LUA_OK;
}
bool QueueOriginalParticleGroup(NativeScriptState& state,std::string_view group,bool enabled){
  constexpr std::string_view name="ForEachEntityInGroup";
  const std::string_view callback=enabled?"StartEmitters":"StopEmitters";
  const Em1LuaArgument args[]={{6,0,0,reinterpret_cast<const uint8_t*>(callback.data()),callback.size()},
    {3,0,0,reinterpret_cast<const uint8_t*>(group.data()),group.size()}};
  return state.vm && em1_lua_world_start_args(state.vm.get(),reinterpret_cast<const uint8_t*>(name.data()),name.size(),args,2)==EM1_LUA_OK;
}
bool QueueOriginalParticleEmission(NativeScriptState& state,uint32_t handle,bool enabled){
  const std::string_view name=enabled?"StartEmitters":"StopEmitters";
  const Em1LuaArgument argument{5,0,double(handle),nullptr,0};
  return state.vm && em1_lua_world_start_args(state.vm.get(),reinterpret_cast<const uint8_t*>(name.data()),name.size(),&argument,1)==EM1_LUA_OK;
}
bool QueueOriginalGearRotation(NativeScriptState& state,uint32_t handle,float speed){
  constexpr auto name="SetRotatorMaxSpeed";
  const Em1LuaArgument arguments[]={{5,0,double(handle),nullptr,0},{2,0,double(speed),nullptr,0}};
  return state.vm && em1_lua_world_start_args(state.vm.get(),reinterpret_cast<const uint8_t*>(name),18,arguments,2)==EM1_LUA_OK;
}
void TickOriginalScripts(NativeScriptState& state,double delta) {
  if(!state.vm || em1_lua_world_tick(state.vm.get(),delta)==EM1_LUA_OK)return;
  Em1LuaWorldView view{};em1_lua_world_view(state.vm.get(),&view);
  REXLOG_ERROR("[em1] original callback failed: {}",view.error_size?std::string(reinterpret_cast<const char*>(view.error),view.error_size):"Invalid simulation tick");
}
void CancelOriginalScripts(NativeScriptState& state) {
  if(state.vm)em1_lua_world_cancel(state.vm.get());
}
void UpdateOriginalScriptPlayer(NativeScriptState& state,uint32_t entity) {
  if(!state.vm || (state.player==entity && !state.player_binding_failed))return;
  const auto status=em1_lua_world_set_player(state.vm.get(),entity);
  if(status!=EM1_LUA_OK){
    if(!state.player_binding_failed)REXLOG_ERROR("[em1] native Lua player binding failed status={}; gameplay callbacks remain disabled",status);
    state.player_binding_failed=true;return;
  }
  state.player=entity;state.player_binding_failed=false;
  REXLOG_INFO("[em1] native Lua GetPlayer binding updated entity={:08X}; opaque native identity, gameplay callbacks not dispatched",entity);
}
namespace {
bool InitializeOriginalScriptModules(std::shared_ptr<RvzSource> source,std::span<const NativeScriptResource> scripts,
    const std::vector<std::string>* roots,NativeScriptState& output,std::string& error) {
  try {
    std::vector<Em1LuaWorldInput> inputs;inputs.reserve(scripts.size());
    for(const auto& script:scripts){
      Em1LuaWorldInput input{};input.bytecode=script.bytecode.data();input.bytecode_size=script.bytecode.size();
      std::copy_n(script.executable_sha256.begin(),32,input.executable_sha256);inputs.push_back(input);
    }
    std::vector<Em1LuaWorldSource> modules;
    if(roots)for(size_t i=0;i<scripts.size();++i){const auto& path=scripts[i].path;const auto key=ScriptPathKey(path);
      modules.push_back({inputs[i],reinterpret_cast<const uint8_t*>(path.data()),path.size(),uint32_t(std::find(roots->begin(),roots->end(),key)!=roots->end()),0});
    }
    NativeScriptState prepared;prepared.vm={roots?em1_lua_world_create_sources(modules.data(),modules.size()):em1_lua_world_create(inputs.data(),inputs.size()),em1_lua_world_release};
    Em1LuaWorldView view{};
    if(!prepared.vm || em1_lua_world_view(prepared.vm.get(),&view)!=EM1_LUA_OK){
      error=view.error_size?std::string(reinterpret_cast<const char*>(view.error),view.error_size):"Native script world initialization failed";return false;
    }
    prepared.scripts=view.scripts;prepared.functions=view.functions;prepared.globals=view.globals;
    // Recover the executable ABI from this same read-only mount. A local
    // analysis file or a guessed Xbox/Wii ordinal table is never a dependency.
    if(!source){error="Original script world has no source mount";return false;}
    bool events=false;
    for(const auto& file:source->Files()){
      if(!IsOriginalAnimationExecutable(file))continue;
      std::vector<uint8_t> executable;if(!source->ReadFile(file.path,executable,error))return false;
      if(em1_lua_world_load_animation_events(prepared.vm.get(),executable.data(),executable.size())==EM1_LUA_OK){
        REXLOG_INFO("[em1] original animation event ABI recovered from mounted executable '{}'; source ordinals translate by name at native dispatch",file.path);
        events=true;break;
      }
    }
    if(!events){error="Mounted source has no supported verified original animation executable";return false;}
    std::vector<uint8_t> original_globals;
    if(!source || !source->ReadFile("Apprentice/ApprenticeGlobalState.bsq",original_globals,error))return false;
    if(em1_lua_world_load_globals(prepared.vm.get(),original_globals.data(),original_globals.size())!=EM1_LUA_OK){
      em1_lua_world_view(prepared.vm.get(),&view);
      error="Original global database: "+std::string(reinterpret_cast<const char*>(view.error),view.error_size);return false;
    }
    if(em1_lua_world_global_schema(prepared.vm.get(),prepared.definition_sha256.data())!=EM1_LUA_OK){error="Global schema identity unavailable";return false;}
    Em1GlobalSummary definitions{};
    if(em1_lua_world_global_summary(prepared.vm.get(),&definitions)!=EM1_LUA_OK){error="Original global summary unavailable";return false;}
    REXLOG_INFO("[em1] authored global definitions retained: entries={} id_limit={} nonzero={} flagged={}; native engine publication pending",definitions.count,definitions.id_limit,definitions.nonzero,definitions.flagged);
    output=std::move(prepared);error.clear();return true;
  }catch(const std::exception& failure){error=failure.what();return false;}
}
}
bool InitializeOriginalScripts(std::shared_ptr<RvzSource> source,std::span<const NativeScriptResource> scripts,NativeScriptState& output,std::string& error) {
  return InitializeOriginalScriptModules(std::move(source),scripts,nullptr,output,error);
}
bool PrepareOriginalScripts(std::shared_ptr<RvzSource> source,std::string_view archive,
    std::vector<NativeScriptResource>& output,std::string& error) {
  try {
    auto pak=PakSource::Open(std::move(source),archive,error);if(!pak)return false;
    std::vector<NativeScriptResource> prepared;size_t total=0;
    for(const auto& entry:pak->Entries()) {
      auto path=entry.path;for(auto& c:path)if(c>='A'&&c<='Z')c+=char('a'-'A');
      if(!path.ends_with(".lua"))continue;
      if(prepared.size()>=512 || entry.size>1024*1024)throw std::runtime_error("EM1 script import exceeds resource limit");
      std::vector<uint8_t> original;if(!pak->ReadOriginal(entry.path,original,error))return false;
      std::unique_ptr<Em1LuaChunk,decltype(&em1_lua_chunk_release)> result(
          em1_lua_prepare_chunk(original.data(),original.size()),em1_lua_chunk_release);
      Em1LuaChunkView view{};
      if(!result || em1_lua_chunk_view(result.get(),&view)!=EM1_LUA_OK) {
        error=entry.path+": "+(view.error_size?std::string(reinterpret_cast<const char*>(view.error),view.error_size):"Native Lua preparation failed");return false;
      }
      if(!view.bytecode || !view.bytecode_size || view.bytecode_size>2*1024*1024 || total+view.bytecode_size>64*1024*1024)
        throw std::runtime_error("EM1 prepared script bytes exceed import limit");
      NativeScriptResource script;script.path=entry.path;script.origin_key=pak->OriginKey()+":"+path+":native-lua51-v1";
      script.source_origins={script.origin_key};
      script.bytecode.assign(view.bytecode,view.bytecode+view.bytecode_size);
      std::copy_n(view.original_sha256,32,script.original_sha256.begin());
      std::copy_n(view.executable_sha256,32,script.executable_sha256.begin());
      total+=script.bytecode.size();prepared.push_back(std::move(script));
    }
    output=std::move(prepared);error.clear();return true;
  }catch(const std::exception& failure){error=failure.what();return false;}
}
}
