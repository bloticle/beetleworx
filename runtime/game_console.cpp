#include "em1_vfs.h"
#include "em1_level_entry.h"
#include "game_console.h"
#include "game_console_catalog.h"
#include "engine_workbench.h"
#include "native_engine.h"
#include "online_runtime.h"
#include "online_native_menu.h"
#include "online_room_probe.h"
#include "online_room.h"
#include "online.h"
#include "simulation.h"
#include "players.h"
#include "ppc_config.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <windows.h>
#include <imgui.h>
#include <immintrin.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>

PPC_EXTERN_FUNC(__imp__sub_8271C108);
PPC_EXTERN_FUNC(__imp__sub_8271A360);
PPC_EXTERN_FUNC(__imp__sub_826F6C20);
PPC_EXTERN_FUNC(__imp__sub_826F71F8);
PPC_EXTERN_FUNC(__imp__sub_82705AF8);
REXCVAR_DEFINE_BOOL(em2_verify_game_console_vm,false,"Debug","Run pure arithmetic/string/error/stack console probes in the real game VM; no gameplay actions");
REXCVAR_DEFINE_UINT32(em2_native_lobby_qa_restart_tick,0xFFFFFFFFu,"Debug","Private validated loopback QA only: invoke the original RestartCurrentLevel at this agreed native script-scheduler tick; maximum disables");
REXCVAR_DEFINE_BOOL(em2_native_lobby_qa_cutscene_cycle,false,"Debug","Private native scheduler fixture: balanced original RestrictCutSceneInput/UnrestrictCutSceneInput calls; never forces a load or changes a context directly");
REXCVAR_DEFINE_BOOL(em2_native_lobby_qa_checkpoint_reload,false,"Debug","Private validated loopback QA: compare native checkpoint-memory restoration with bare level restart");
REXCVAR_DEFINE_UINT32(em2_native_lobby_qa_damage_player,0,"Debug","Private native lobby scheduler fixture only: damage retail player 1 or 2 (3 means both, 4 means all three private test humans) at the requested restart tick; zero disables");
REXCVAR_DECLARE(uint32_t,em2_probe_extra_damage_peer);
REXCVAR_DEFINE_UINT32(em2_native_lobby_qa_restart_count,1,"Debug","Private validated loopback QA only: one through four scheduled original reloads");
REXCVAR_DEFINE_UINT32(em2_native_lobby_qa_restart_interval,700,"Debug","Private validated loopback QA only: agreed ticks between scheduled reloads");
REXCVAR_DEFINE_STRING(em2_native_lobby_qa_level_target,"","Debug","Private validated loopback QA only: known retail state for the original TransitionToLevel helper");
REXCVAR_DEFINE_STRING(em2_native_lobby_qa_level_route,"","Debug","Private validated loopback QA only: pipe-separated known retail states, one destination per scheduled original transition");
REXCVAR_DEFINE_BOOL(em2_native_lobby_qa_clean_level_load,false,"Debug","Private loopback comparison only: use destructive debug CleanLoadLevel instead of the normal campaign transition");

REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(std::string,em2_qa_debug_level);

namespace {
constexpr size_t kMaxSource=16384;
struct Request {std::string source;bool needs_world=false;uint64_t world_version=0;std::string original_level;};
std::mutex queue_mutex;std::deque<Request> requests;
std::atomic<bool> work_pending{false};
std::atomic<bool> vm_seen{false};
thread_local unsigned protected_depth=0;
thread_local bool executing=false;
thread_local uint64_t print_count=0;
bool Readable(uint8_t* base,uint32_t address,size_t size){
 if(!base || address<4096 || !size || uint64_t(address)+size>0x100000000ull)return false;
 MEMORY_BASIC_INFORMATION region{};const auto host=reinterpret_cast<uintptr_t>(PPC_RAW_ADDR(address));
 return VirtualQuery(PPC_RAW_ADDR(address),&region,sizeof(region)) && region.State==MEM_COMMIT && !(region.Protect&(PAGE_NOACCESS|PAGE_GUARD)) && host+size<=reinterpret_cast<uintptr_t>(region.BaseAddress)+region.RegionSize;
}
std::string Lower(std::string_view s){std::string r(s);for(auto& c:r)if(c>='A'&&c<='Z')c+=32;return r;}
bool Match(std::string_view s,std::string_view query){return Lower(s).find(Lower(query))!=std::string::npos;}
std::string Quote(std::string_view source){std::string r="\"";for(char c:source){if(c=='"'||c=='\\')r+='\\';r+=c;}return r+'"';}
bool ValidSource(std::string_view source){return !source.empty() && source.size()<=kMaxSource && source.find('\0')==source.npos;}
// Traverse raw table entries only: discovery must not invoke __index/__tostring.
std::optional<std::string> InspectionSource(std::string_view path,std::string_view filter){
 std::string resolve="local t=_G or getfenv(0); ";
 if(!path.empty()){
   size_t start=0;
   while(start<path.size()){
     auto end=path.find('.',start);if(end==path.npos)end=path.size();
     const auto part=path.substr(start,end-start);
     if(part.empty() || part.size()>128)return {};
     for(size_t i=0;i<part.size();++i){const char c=part[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||(i && c>='0'&&c<='9')))return {};}
     resolve+="if type(t)~='table' then error('Path crosses a non-table value') end; t=rawget(t,"+Quote(part)+"); ";
     if(end==path.size())break;start=end+1;if(start==path.size())return {};
   }
 }
 return resolve+"if type(t)~='table' then return type(t),t end; "
   "local q=string.lower("+Quote(filter)+"); local k=nil; local scanned=0; local shown=0; "
   "while scanned<4096 do local v; k,v=next(t,k); if k==nil then break end; scanned=scanned+1; "
   "if type(k)=='string' and string.find(string.lower(k),q,1,true) then "
   "shown=shown+1; if shown<=100 then local kind=type(v); "
   "if kind=='number' or kind=='boolean' or kind=='string' then print(k,kind,string.sub(tostring(v),1,256)) else print(k,kind) end end end end; "
   "print('Matches',shown,'Entries scanned',scanned,'Scan limited',scanned==4096); return shown,scanned,scanned==4096";
}
std::string Value(uint8_t* base,uint32_t slot){
 if(!Readable(base,slot,8))return "<unreadable>";
 const auto word=PPC_LOAD_U32(slot),type=PPC_LOAD_U32(slot+4)&15;
 if(type==0)return "nil";if(type==1)return word?"true":"false";
 if(type==2)return std::to_string(std::bit_cast<int32_t>(word));
 if(type==3){std::ostringstream out;out<<std::bit_cast<float>(word);return out.str();}
 if(type==4 && Readable(base,word,12)){
   const auto length=PPC_LOAD_U32(word+4)&0x3fffffff;const auto count=std::min<uint32_t>(length,1024);
   if(Readable(base,word+12,count?count:1))return std::string(reinterpret_cast<char*>(PPC_RAW_ADDR(word+12)),count)+(length>count?"...":"");
 }
 std::ostringstream out;out<<"<VM type "<<type<<" @0x"<<std::hex<<word<<">";return out.str();
}
struct Result {bool attempted=false,stack_ok=false;int status=-1;std::vector<std::string> values;};
Result Evaluate(PPCContext& ctx,uint8_t* base,uint32_t state,std::string_view source,uint32_t environment=0){
 Result result;
 if(!ValidSource(source) || !Readable(base,state,128) || ctx.r1.u32<65536)return result;
 const auto storage=PPC_LOAD_U32(state+48),top=PPC_LOAD_U32(state+36),bottom=PPC_LOAD_U32(state+40),global=PPC_LOAD_U32(state+8);
 if(top<storage || bottom<storage || bottom>top || (top-storage)%8 || (bottom-storage)%8 || top-storage>8*1024*1024 ||
    !Readable(base,global,704) || !Readable(base,ctx.r1.u32-32768,32768))return result;
 // HKS closure environments are separate from the console compiler's globals.
 // Root the optional table before compilation can collect or relocate anything.
 // 44 is stack_last (also checked by the original call path 826F6920).
 if(environment && (!Readable(base,environment,24) || uint64_t(top)+24>PPC_LOAD_U32(state+44) || !Readable(base,top,24)))return result;
 const auto saved=ctx;const unsigned csr=_mm_getcsr();const auto top_offset=top-storage,base_offset=bottom-storage;
 const auto call_storage=PPC_LOAD_U32(state+12),call_current=PPC_LOAD_U32(state+20);
 // Retail pcall saves this as a signed index. The empty call stack uses -1.
 const auto call_delta=int64_t(call_current)-call_storage;
 if(call_delta%16 || call_delta < -65536 || call_delta>16*1024*1024)return result;
 struct Restore {PPCContext& ctx;PPCContext saved;unsigned csr;bool old;~Restore(){ctx=saved;_mm_setcsr(csr);executing=old;}} restore{ctx,saved,csr,executing};
 executing=true;ctx.r1.u64=saved.r1.u32-32768;
 const uint32_t text=ctx.r1.u32+512,name=ctx.r1.u32+512+uint32_t(kMaxSource)+16;
 std::memcpy(PPC_RAW_ADDR(text),source.data(),source.size());PPC_STORE_U8(text+uint32_t(source.size()),0);std::memcpy(PPC_RAW_ADDR(name),"=EM2Console",12);
 result.attempted=true;
 if(environment){PPC_STORE_U32(top,environment);PPC_STORE_U32(top+4,5);PPC_STORE_U32(state+36,top+8);}
 // Exact loadstring adapter: compiler settings are global_State+680. The
 // retail compiler and pcall both catch their own VM errors/longjmps.
 ctx.r3.u64=state;ctx.r4.u64=global+680;ctx.r5.u64=text;ctx.r6.u64=source.size();ctx.r7.u64=name;
 __imp__sub_8271A360(ctx,base);result.status=ctx.r3.s32;
 if(!result.status){
   if(environment){
     const auto relocated=PPC_LOAD_U32(state+48),argument=PPC_LOAD_U32(state+36);
     const uint64_t root=uint64_t(relocated)+top_offset;
     if(root>UINT32_MAX || !Readable(base,uint32_t(root),8) || uint64_t(argument)+8>PPC_LOAD_U32(state+44) || !Readable(base,argument,8))return result;
     PPC_STORE_U32(argument,PPC_LOAD_U32(uint32_t(root)));PPC_STORE_U32(argument+4,5);PPC_STORE_U32(state+36,argument+8);
   }
   ctx.r3.u64=state;ctx.r4.u64=environment?1:0;ctx.r5.s64=-1;ctx.r6.u64=0;__imp__sub_826F6C20(ctx,base);result.status=ctx.r3.s32;
 }
 // The VM may have relocated its stack. Never restore the old raw pointer.
 const auto relocated=PPC_LOAD_U32(state+48),after=PPC_LOAD_U32(state+36);
 const uint64_t restore_top=uint64_t(relocated)+top_offset,restore_base=uint64_t(relocated)+base_offset,result_top=restore_top+(environment?8:0);
 if(result_top>UINT32_MAX || restore_base>UINT32_MAX || after<result_top || (after-result_top)%8 ||
    !Readable(base,relocated,size_t(std::max<uint64_t>(after-relocated,8))))return result;
 const auto count=(after-result_top)/8;
 for(unsigned i=0;i<std::min<unsigned>(count,16);++i)result.values.push_back(Value(base,uint32_t(result_top)+i*8));
 if(count>16)result.values.push_back("... "+std::to_string(count-16)+" more results");
 const auto after_call_storage=PPC_LOAD_U32(state+12),after_call_current=PPC_LOAD_U32(state+20);
 result.stack_ok=PPC_LOAD_U32(state+40)==restore_base && int64_t(after_call_current)-after_call_storage==call_delta;
 PPC_STORE_U32(state+36,uint32_t(restore_top));return result;
}
struct Action {const char* name;const char* description;const char* source;};
constexpr Action actions[]={
 {"god_on","Enable the native GodModeOnPlayer configuration.","return SetConfigVariableString(nil,'GodModeOnPlayer','True'), GetConfigVariableBool('GodModeOnPlayer')"},
 {"god_off","Disable the native GodModeOnPlayer configuration.","return SetConfigVariableString(nil,'GodModeOnPlayer','False'), GetConfigVariableBool('GodModeOnPlayer')"},
 {"fly","Toggle the original developer fly camera; close the console to control it.","FlyCamera(GetPlayer())"},
 {"teleport_camera","Teleport the player to the developer camera.","TeleportToCamera(GetPlayer())"},
 {"refill","Refill current player health and paint/thinner to their authored maxima.","SetHealth(GetPlayer(),GetPropertyInt(GetPlayer(),'MaxHealth',0)); SetAmmo('Paint',GetAmmoMax('Paint')); SetAmmo('Thinner',GetAmmoMax('Thinner'))"},
 {"hud","Toggle the game's HUD.","ToggleHudVisibility()"},
 {"text","Toggle the native debug text display.","ToggleDebugText()"},
 {"misc","Toggle native miscellaneous debug information.","ToggleMiscDebugInfo()"},
 {"cutscene_input","Release a scripted cutscene input restriction.","UnrestrictCutSceneInput()"},
 {"invisible","Apply the original debug invisible inkwell action.","DEBUG_SetPlayer_InkWell('Invisible')"},
 {"indelible","Apply the original debug indelible inkwell action.","DEBUG_SetPlayer_InkWell('Indelible')"},
 {"bring_oswald","Move the buddy to the controlled player.","MoveToEntity(GetPlayer2OrAI(),GetPlayer())"},
 {"goto_oswald","Move the controlled player to the buddy.","MoveToEntity(GetPlayer(),GetPlayer2OrAI())"},
 {"buddy_follow","Resume Oswald's scripted follow behavior.","OswaldStartFollowAction()"},
 {"buddy_stop","Stop Oswald's scripted follow behavior.","OswaldStopFollowAction()"},
};
const Action* FindAction(std::string_view name){for(const auto& a:actions)if(name==a.name)return &a;return nullptr;}
std::string_view Signature(std::string_view name){const auto it=std::lower_bound(std::begin(em2::kNativeScriptExports),std::end(em2::kNativeScriptExports),name);return it!=std::end(em2::kNativeScriptExports)&&*it==name?em2::kNativeScriptSignatures[it-std::begin(em2::kNativeScriptExports)]:std::string_view{};}
std::optional<std::string> Level(std::string_view input){const auto name=Lower(input);for(auto entry:em2::kNativeLevelStates)if(entry==name)return name;return {};}
void Report(const Result& result){
 if(!result.attempted){em2::EngineConsoleMessage("Lua VM not ready at this boundary; no source executed.");return;}
 std::string line=result.status?"Lua error ("+std::to_string(result.status)+"): ":"Lua result: ";
 if(result.values.empty())line+=result.status?"no error value returned":"completed (no return values)";
 for(size_t i=0;i<result.values.size();++i){if(i)line+=" | ";line+=result.values[i];}
 if(REXCVAR_GET(em2_private_profile) && !REXCVAR_GET(em2_qa_debug_level).empty())REXLOG_INFO("Private level browser VM result: {}",line);
 em2::EngineConsoleMessage(line);
 if(!result.stack_ok)REXLOG_ERROR("Console Lua stack did not return to its caller base; further execution disabled");
}
std::atomic<bool> faulted{false};
// The retail Mean Street startup requests interior streaming, then teleports
// before that asynchronous request completes. Install a fixed compatibility
// wrapper when that module finishes defining its functions. This is not a
// console request: every peer executes the same original level-load wait.
// Do not retain lua_State/closure pointers across loads.
uint32_t MeanStreetModuleEnvironment(uint8_t* base,uint32_t state,uint32_t arguments){
 struct HostState{DWORD error=GetLastError();unsigned csr=_mm_getcsr();~HostState(){SetLastError(error);_mm_setcsr(csr);}}host;
 if(arguments>64 || !Readable(base,state,128))return 0;
 const auto top=PPC_LOAD_U32(state+36);
 const uint32_t bytes=(arguments+1)*8;
 if(top<bytes || !Readable(base,top-bytes,8) || (PPC_LOAD_U32(top-bytes+4)&15)!=9)return 0;
 const auto closure=PPC_LOAD_U32(top-bytes);
 if(!Readable(base,closure,16))return 0;
 const auto proto=PPC_LOAD_U32(closure+8);
 if(!Readable(base,proto,44))return 0;
 const auto location=PPC_LOAD_U32(proto+40);
 if(!Readable(base,location,28) || PPC_LOAD_U32(location)>1)return 0;
 const auto source=PPC_LOAD_U32(location+24);
 if(!Readable(base,source,12))return 0;
 const auto length=PPC_LOAD_U32(source+4)&0x3fffffff;
 if(!length || length>192 || !Readable(base,source+12,length))return 0;
 auto name=Lower(std::string_view(reinterpret_cast<const char*>(PPC_RAW_ADDR(source+12)),length));
 for(auto& c:name)if(c=='\\')c='/';
 if(name.starts_with('@'))name.erase(0,1);
 if(name!="environments/_shared/scripts/meanstreet_north.lua")return 0;
 // Original getfenv helper 826D7558 reads a Lua closure's environment at +12.
 const auto environment=PPC_LOAD_U32(closure+12);
 return Readable(base,environment,24)?environment:0;
}
void InstallMeanStreetLoadWait(PPCContext& ctx,uint8_t* base,uint32_t state,uint32_t environment){
 if(faulted)return;
 struct HostState{DWORD error=GetLastError();unsigned csr=_mm_getcsr();~HostState(){SetLastError(error);_mm_setcsr(csr);}}host;
 constexpr std::string_view source=R"lua(
local env=...
local original=rawget(env,'MSTN_AlwaysSetup')
local installed=rawget(env,'__beetleworx_msn_load_wait')
if type(original)~='function' or original==installed then return false,type(original),env==_G end
local function ready(...)
  WaitForLevelToLoad(false)
  return original(...)
end
rawset(env,'MSTN_AlwaysSetup',ready)
rawset(env,'__beetleworx_msn_load_wait',ready)
return true,type(original),env==_G
)lua";
 const auto result=Evaluate(ctx,base,state,source,environment);
 REXLOG_INFO("Mean Street interior startup wait: attempted {}, status {}, stack {}, installed {}",result.attempted,result.status,result.stack_ok,
   !result.values.empty() && result.values[0]=="true");
 if(result.values.size()==3)REXLOG_INFO("Mean Street startup scope: function type {}, global environment {}",result.values[1],result.values[2]);
 if(!result.attempted || result.status || !result.stack_ok){
   REXLOG_ERROR("Mean Street compatibility installation failed; original VM diagnostics retained, unsafe online continuation rejected");
   if(em2::net::SynchronizedSessionActive())em2::net::GameplayInputGate().ObservationFailed();
 }
 if(result.attempted && !result.stack_ok)faulted=true;
}

void Service(PPCContext& ctx,uint8_t* base,uint32_t state){
 if(executing || faulted)return;
 if(!vm_seen.load(std::memory_order_relaxed))vm_seen.store(true,std::memory_order_relaxed);
 if(!work_pending.load(std::memory_order_acquire) && !REXCVAR_GET(em2_verify_game_console_vm))return;
 if(em2::net::SynchronizedSessionActive()){
   std::lock_guard lock(queue_mutex);requests.clear();work_pending.store(false,std::memory_order_release);return;
 }
 if(REXCVAR_GET(em2_verify_game_console_vm)){
   static unsigned phase=0,attempts=0;static auto last=std::chrono::steady_clock::time_point{};
   const auto now=std::chrono::steady_clock::now();
   if(phase<8 && attempts<100 && now-last>std::chrono::milliseconds(200)){
     last=now;++attempts;
     const char* probes[]={"return 6*7, 'console vm', true, nil, type(CleanLoadLevel), type(FlyCamera)","local invalid =", "error('EM2 expected console runtime error')", "print('EM2 console print capture',7); return (function(x) return x*2 end)(21)",
       "local k='__EM2ConsolePrivateProbe'; local old=GetConfigVariableString(k); assert(SetConfigVariableString(nil,k,'True')); local a=GetConfigVariableBool(k); assert(SetConfigVariableString(nil,k,'False')); local b=GetConfigVariableBool(k); assert(SetConfigVariableString(nil,k,old)); return a,b,GetConfigVariableString(k)==old",
       "local t=_G or getfenv(0); return type(rawget(t,'CleanLoadLevel')),type(next),type(string.find),type(string.lower)"};
     const auto saved=ctx;const auto csr=_mm_getcsr();const auto prints=print_count;const auto source=phase==6?*InspectionSource("","CleanLoadLevel"):phase==7?
       "local _G={fixture=setmetatable({answer=42,nested={}}, {__index=function() error('unexpected __index') end, __tostring=function() error('unexpected __tostring') end})}; "+*InspectionSource("fixture",""):std::string(probes[phase]);
     const auto result=Evaluate(ctx,base,state,source);
     bool ok=result.attempted && result.stack_ok && !std::memcmp(&ctx,&saved,sizeof(ctx)) && csr==_mm_getcsr();
     if(phase==0){ok&=result.status==0 && result.values==std::vector<std::string>{"42","console vm","true","nil","function","function"};}
     else if(phase==1 || phase==2)ok&=result.status!=0 && !result.values.empty();
     else if(phase==3)ok&=result.status==0 && result.values==std::vector<std::string>{"42"} && print_count==prints+1;
     else if(phase==4)ok&=result.status==0 && result.values==std::vector<std::string>{"true","false","true"};
     else if(phase==5)ok&=result.status==0 && result.values==std::vector<std::string>{"function","function","function","function"};
     else if(phase==6)ok&=result.status==0 && result.values.size()==3 && result.values[0]=="1" && result.values[2]=="false" && print_count==prints+2;
     else ok&=result.status==0 && result.values==std::vector<std::string>{"2","2","false"} && print_count==prints+3;
     if(phase==3 && ok)REXLOG_INFO("Game console real-VM print capture passed");
     if(ok){REXLOG_INFO("Game console real-VM probe {} passed: retail compiler/pcall, relocated stack top/base, full caller context and MXCSR",phase);++phase;}
     else {REXLOG_WARN("Game console real-VM probe {} not accepted: attempted {} status {} stack {}",phase,result.attempted,result.status,result.stack_ok);Report(result);}
     if(result.attempted && !result.stack_ok){faulted=true;return;}
   }
 }
 Request request;
 {std::lock_guard lock(queue_mutex);if(requests.empty())return;request=std::move(requests.front());requests.pop_front();work_pending.store(!requests.empty(),std::memory_order_release);}
 const auto world=em2::NativeConsoleWorldState();
 if(request.needs_world && (!world.ready || world.version!=request.world_version)){em2::EngineConsoleMessage("The gameplay world changed or unloaded before this action could run; nothing was executed. Submit it again in the intended level.");return;}
 if(!request.original_level.empty()){std::string error;if(!em2::em1::CanEnterRegisteredLevel(base,request.original_level,error)){em2::EngineConsoleMessage(error);return;}}
 const auto result=Evaluate(ctx,base,state,request.source);Report(result);
 if(result.attempted && !result.stack_ok)faulted=true;
}
}

void em2::TickPrivateNativeRestart(PPCContext& ctx,uint8_t* base){
 net::TickPrivateNativeTaskPolicy(ctx,base);
 if(REXCVAR_GET(em2_native_lobby_qa_damage_player)!=4 && net::TickPrivateNativeExtraDamage(ctx,base))return;
 const auto requested=REXCVAR_GET(em2_native_lobby_qa_restart_tick);
 const auto count=REXCVAR_GET(em2_native_lobby_qa_restart_count);
 const auto interval=REXCVAR_GET(em2_native_lobby_qa_restart_interval);
 if(requested==0xFFFFFFFFu || !count || count>4 || interval<600 ||
    uint64_t(requested)+uint64_t(count-1)*interval>=0xFFFFFFFFu)return;
 struct HostState{DWORD error=GetLastError();unsigned csr=_mm_getcsr();~HostState(){SetLastError(error);_mm_setcsr(csr);}}host;
 static thread_local unsigned sent=0;
 const auto scheduled=uint64_t(requested)+uint64_t(sent)*interval;
 uint64_t tick=0;
 if(sent>=count || executing || faulted || protected_depth || ctx.lr!=0x829ED694 || !CurrentAgreedSimulationTick(tick) || tick!=scheduled ||
    !net::NativeOnlineMenuQaActive() || !NativeCoopReady(base))return;
 const auto damage_player=REXCVAR_GET(em2_native_lobby_qa_damage_player);
 const bool cutscene=REXCVAR_GET(em2_native_lobby_qa_cutscene_cycle);
 const auto requested_level=REXCVAR_GET(em2_native_lobby_qa_level_target);
 const auto route=REXCVAR_GET(em2_native_lobby_qa_level_route);
 auto level=requested_level.empty()?std::optional<std::string>{}:Level(requested_level);
 if(!route.empty()){
   std::vector<std::string> targets;size_t begin=0;
   for(;;){
     const auto end=route.find('|',begin);
     const auto entry=Level(std::string_view(route).substr(begin,end==std::string::npos?end:end-begin));
     if(!entry || targets.size()>=4){REXLOG_ERROR("Native lobby QA rejected: route contains an unknown retail state or exceeds four transitions");return;}
     targets.push_back(*entry);if(end==std::string::npos)break;begin=end+1;
   }
   if(!requested_level.empty() || targets.size()!=count){REXLOG_ERROR("Native lobby QA rejected: route must match the scheduled count without a separate target");return;}
   level=targets[sent];
 }
 if(!requested_level.empty() && !level){
   REXLOG_ERROR("Native lobby QA rejected: unknown retail target state");return;
 }
 if(damage_player && (damage_player>4 || level || !route.empty() || count!=1 || Signature("Damage")!="ScriptEntityHandle hEntity, float fAmount")){
   REXLOG_ERROR("Native lobby QA rejected: damage fixture requires one scheduled retail player and no manual level route");return;
 }
 if(damage_player==4 && (REXCVAR_GET(em2_probe_extra_damage_peer)!=3 || !net::PrivateNativeMenuRoom() || net::PrivateNativeMenuRoom()->lobby().players.size()!=3)){
   REXLOG_ERROR("Native lobby QA rejected: whole-party damage requires exactly three private humans and extra peer 3");return;
 }
 if(cutscene && (damage_player || REXCVAR_GET(em2_probe_extra_damage_peer) || level || !route.empty() || count!=2 ||
     REXCVAR_GET(em2_native_lobby_qa_checkpoint_reload) || !net::PrivateNativeMenuRoom() || net::PrivateNativeMenuRoom()->lobby().players.size()!=3)){
   REXLOG_ERROR("Native lobby QA rejected: cutscene cycle requires three private humans, exactly two balanced native calls and no other fixture");return;
 }
 // 82732AB8 publishes the reusable native BScript at 833DA5E0. Retail
 // 82732634 reads its +4 lua_State before calling Lua. Borrow it freshly at
 // the original world coroutine-scheduler boundary, with no active Lua call.
 // Never keep a VM pointer across world loads or invoke arbitrary source.
 ++sent;
 uint32_t state=0;
 if(Readable(base,0x833DA5E0,4)){
   const auto script=PPC_LOAD_U32(0x833DA5E0);
   if(Readable(base,script,8))state=PPC_LOAD_U32(script+4);
 }
 REXLOG_INFO("Native lobby QA restart executing: tick {}",tick);
 const bool checkpoint=REXCVAR_GET(em2_native_lobby_qa_checkpoint_reload);
 const bool clean=REXCVAR_GET(em2_native_lobby_qa_clean_level_load);
 REXLOG_INFO("Native lobby QA reload method: {}",cutscene?"original-cutscene-cycle":damage_player?"original-damage":level?(clean?"original-level":"original-transition"):checkpoint?"checkpoint-memory":"level-only");
 if(level)REXLOG_INFO("Native lobby QA level target: {}",*level);
 const auto source=cutscene?std::string(sent==1?"RestrictCutSceneInput()":"UnrestrictCutSceneInput()"):damage_player>=3?std::string("local a=GetPlayer(); local b=GetPlayer2(); local x=GetHealth(a); local y=GetHealth(b); Damage(a,1000); Damage(b,1000); return x,y,GetHealth(a),GetHealth(b)"):damage_player?std::string("local p=")+(damage_player==1?"GetPlayer()":"GetPlayer2()")+"; local before=GetHealth(p); Damage(p,1000); return before,GetHealth(p)":level?std::string(clean?"CleanLoadLevel(":"TransitionToLevel(")+Quote(*level)+")":std::string(checkpoint?"LoadGameFromMemory()":"RestartCurrentLevel()");
 if(damage_player)REXLOG_INFO("Native lobby QA damage requested: tick {}, retail player {}, amount 1000; original Damage Lua export, no forced reload",tick,damage_player);
 const auto result=Evaluate(ctx,base,state,source);
 if(cutscene)REXLOG_INFO("Private native cutscene cycle: tick {} phase {} attempted {} status {} stack {}; original Lua export, no direct context/state writes",tick,sent==1?"restricted":"released",result.attempted,result.status,result.stack_ok);
 REXLOG_INFO("Native lobby QA restart requested: tick {}, attempted {}, status {}, stack {}",tick,result.attempted,result.status,result.stack_ok);
 if(damage_player>=3 && result.values.size()==4)REXLOG_INFO("Native lobby QA pair damage health: before {}/{}, after {}/{}; original Damage and GetHealth, extra humans untouched",result.values[0],result.values[1],result.values[2],result.values[3]);
 if(damage_player && result.values.size()==2)REXLOG_INFO("Native lobby QA damage health: player {}, before {}, after {}; native GetHealth",damage_player,result.values[0],result.values[1]);
 if(!result.attempted || result.status || !result.stack_ok){Report(result);net::GameplayInputGate().ObservationFailed();}
 else if(damage_player==4)net::TickPrivateNativeExtraDamage(ctx,base);
}
std::string em2::QueueGameLua(std::string_view source,bool needs_world){
 if(!ValidSource(source))return "Lua source must contain 1..16384 bytes and no embedded NUL.";
 if(net::SynchronizedSessionActive())return "Lua and developer gameplay actions require an offline session.";
 if(faulted)return "Lua console disabled after a VM stack-integrity failure. See the log.";
 const auto world=NativeConsoleWorldState();
 if(needs_world && !world.ready)return "Enter offline gameplay before using this action.";
 std::lock_guard lock(queue_mutex);if(requests.size()>=8)return "Lua request queue is full (8).";
 if(net::SynchronizedSessionActive())return "Lua and developer gameplay actions require an offline session.";
 requests.push_back({std::string(source),needs_world,world.version});
 work_pending.store(true,std::memory_order_release);
 return "Queued Lua for the next protected game-VM boundary. Results/errors appear in Output.";
}
std::string em2::QueueRetailLevelLoad(std::string_view state,bool needs_world){
 const auto name=Level(state);if(!name)return "Unknown retail level state. Use levels [search]; names are validated against the 205-state database.";
 const bool zone_only=std::find(std::begin(kNativeZoneOnlyLevelStates),std::end(kNativeZoneOnlyLevelStates),*name)!=std::end(kNativeZoneOnlyLevelStates);
 if(!zone_only)return QueueGameLua("CleanLoadLevel("+Quote(*name)+")",needs_world);
 // Check the ACTUAL native level in the protected VM, not thread-local loader
 // diagnostics on the UI thread. The normal queue also fences world changes.
 // CleanLoadLevel would discard the geometry this incremental state needs.
 const auto root=name->substr(0,name->find('.'));
 const auto source="if LevelManager_GetCurrentLevelName() ~= "+Quote(root)+" then error("+Quote("Load a full state of "+root+" before applying this zone state")+") end; LevelManager_SetCurrentState("+Quote(*name)+")";
 return QueueGameLua(source,true);
}
unsigned em2::CancelQueuedGameLua(){std::lock_guard lock(queue_mutex);const auto count=unsigned(requests.size());requests.clear();work_pending.store(false,std::memory_order_release);return count;}
unsigned em2::PendingGameLua(){std::lock_guard lock(queue_mutex);return unsigned(requests.size());}
std::optional<std::string> em2::GameLuaConsoleSource(std::string_view raw){
 while(!raw.empty()&&(raw.front()==' '||raw.front()=='\t'))raw.remove_prefix(1);
 if(raw!="lua" && !raw.starts_with("lua ") && !raw.starts_with("lua\t"))return {};
 raw.remove_prefix(3);while(!raw.empty()&&(raw.front()==' '||raw.front()=='\t'))raw.remove_prefix(1);
 if(raw.starts_with('"'))if(const auto one=TokenizeEngineConsole(raw);one && one->size()==1)return (*one)[0];
 return std::string(raw);
}
std::optional<std::string> em2::ExecuteGameplayConsoleTool(const std::vector<std::string>& args){
 if(args.empty())return {};const auto& cmd=args[0];std::ostringstream out;
 if(cmd=="lua.cancel"){if(args.size()!=1)return "Usage: lua.cancel";return "Cancelled "+std::to_string(CancelQueuedGameLua())+" queued Lua requests. Running code is unaffected.";}
 if(cmd=="lua.globals" || cmd=="lua.inspect"){
   if((cmd=="lua.globals" && args.size()>2) || (cmd=="lua.inspect" && (args.size()<2 || args.size()>3)))return "Usage: lua.globals [search] | lua.inspect <global.table.path> [search]";
   const auto source=InspectionSource(cmd=="lua.inspect"?args[1]:"",cmd=="lua.inspect"?(args.size()==3?args[2]:""):(args.size()==2?args[1]:""));
   if(!source)return "Use a dotted global name such as math or LevelManager; each component must be a Lua identifier.";
   return QueueGameLua(*source);
 }
 if(cmd=="levels" || cmd=="lua.api"){
   if(args.size()>2)return "Use levels [search] or lua.api [search].";
   const auto query=args.size()==2?args[1]:"";unsigned count=0;
   if(cmd=="levels"){
     for(auto name:kNativeLevelStates)if(Match(name,query)){out<<name;
       if(std::find(std::begin(kNativeZoneOnlyLevelStates),std::end(kNativeZoneOnlyLevelStates),name)!=std::end(kNativeZoneOnlyLevelStates))out<<" [zone state; requires loaded level]";
       out<<'\n';++count;}
     if(auto catalog=em1::CurrentLevelCatalog())for(const auto& name:catalog->descriptors)
       if(Match(name,query)){out<<"em1:"<<name<<" [full-level activation pending]\n";++count;}
   }
   else for(auto name:kNativeScriptExports)if(Match(name,query)){if(count<100)out<<name<<"("<<Signature(name)<<")\n";++count;}
   out<<count<<" matches. ";out<<(cmd=="levels"?"Use level.load <exact name>, or the Gameplay tab.":"Recovered native exports; use Lua to call functions. Narrow the search if more than 100 match. Script helpers may exist beyond this index.");return out.str();
 }
 if(cmd=="level.load"){
   if(args.size()!=2)return "Usage: level.load <level.state>. Run levels [search] to find exact IDs.";
   if(Lower(args[1]).starts_with("em1:")){
     const auto descriptor=Lower(args[1]);const auto state=em1::RegisteredLevelState(descriptor);
     if(state.empty())return "This original level is not registered in the running native registry.";
     if(net::SynchronizedSessionActive())return "Original level loading currently requires offline gameplay.";
     const auto world=NativeConsoleWorldState();if(!world.ready)return "Enter offline gameplay before changing levels.";
     std::lock_guard lock(queue_mutex);if(requests.size()>=8)return "The game command queue is full.";
     requests.push_back({"CleanLoadLevel("+Quote(state)+")",true,world.version,descriptor});work_pending.store(true,std::memory_order_release);
     return "Queued original .level entry through the native level loader. Admission failures appear in Output.";
   }
   const auto name=Level(args[1]);if(!name)return "Unknown retail level state. Use levels [search]; names are validated against the 205-state database.";
   return QueueRetailLevelLoad(*name,true);
 }
 if(cmd=="game.get" || cmd=="game.set"){
   if(args.size()!=(cmd=="game.get"?2u:3u))return "Usage: game.get <native config key> | game.set <key> <value>";
   if(args[1].empty() || args[1].size()>128)return "Native configuration keys must contain 1..128 characters.";
   const auto key=Quote(args[1]);
   return QueueGameLua(cmd=="game.get"?"return GetConfigVariableString("+key+")":"return SetConfigVariableString(nil,"+key+","+Quote(args[2])+"), GetConfigVariableString("+key+")");
 }
 if(cmd=="level.restart" || cmd=="level.current"){
   if(args.size()!=1)return "This command takes no arguments.";
   return QueueGameLua(cmd=="level.restart"?"LoadGameFromMemory()":"return LevelManager_GetCurrentLevelName(), LevelManager_GetCurrentState(), LevelManager_GetCurrentInterior()",true);
 }
 if(cmd=="debug"){
   if(args.size()==1){for(const auto& a:actions)out<<a.name<<" : "<<a.description<<'\n';return out.str();}
   if(args.size()!=2)return "Use debug <feature>. Run debug to list available native developer actions.";
   if(const auto* action=FindAction(args[1]))return QueueGameLua(action->source,true);
   return "Unknown developer action. Run debug to list actions.";
 }
 if(cmd=="unlock"){
   if(args.size()!=2)return "Usage: unlock <abilities|costumes|pins>";
   const auto code=args[1]=="abilities"?"UnlockAllAbilities()":args[1]=="costumes"?"UnlockAllCostumes()":args[1]=="pins"?"UnlockAllPinBuffs()":nullptr;
   return code?QueueGameLua(code,true):"Choose abilities, costumes or pins.";
 }
 if(cmd=="give"){
   if(args.size()!=3)return "Usage: give <tickets|health> <amount>";
   int amount;const auto [end,error]=std::from_chars(args[2].data(),args[2].data()+args[2].size(),amount);
   if(error!=std::errc{} || end!=args[2].data()+args[2].size() || amount<0 || amount>100000)return "Amount must be a whole number from 0 to 100000.";
   if(args[1]=="tickets")return QueueGameLua("AddCurrency('ETicket',"+std::to_string(amount)+")",true);
   if(args[1]=="health" && amount<=1000)return QueueGameLua("SetHealth(GetPlayer(),"+std::to_string(amount)+")",true);
   return "Choose tickets (0..100000) or health (0..1000). Authored resource caps may still apply.";
 }
 return {};
}
std::optional<std::vector<std::string>> em2::GameplayConsoleCompletions(const std::vector<std::string>& before){
 if(before.empty())return {};std::vector<std::string> values;
 if(before[0]=="lua"){for(auto name:kNativeScriptExports)values.emplace_back(std::string(name)+"(");for(auto name:{"print(","return","type(","pairs(","ipairs("})values.emplace_back(name);return values;}
 if(before.size()==1){
   if(before[0]=="lua.inspect")values={"math","string","table","package"};
   else if(before[0]=="level.load")for(auto name:kNativeLevelStates)values.emplace_back(name);
   else if(before[0]=="debug")for(const auto& action:actions)values.emplace_back(action.name);
   else if(before[0]=="unlock")values={"abilities","costumes","pins"};
   else if(before[0]=="give")values={"tickets","health"};
   else if(before[0]=="lua.api")for(auto name:kNativeScriptExports)values.emplace_back(name);
   else return {};
   return values;
 }return {};
}
void em2::DrawGameplayConsole(){
 ImGui::BeginChild("gameplay-actions",ImVec2(0,0));
 ImGui::TextWrapped("Native gameplay and developer actions execute in EM2's real Lua VM. Player actions use the original GetPlayer() target. Results and protected errors appear in Output. Gameplay actions require an offline loaded level; normal game autosaves may preserve progress changes.");
 if(ImGui::CollapsingHeader("Load a level",ImGuiTreeNodeFlags_DefaultOpen)){
   static char search[128]{};static std::string selected="meanstreet_north.visit_1";
   ImGui::InputTextWithHint("##level-filter","Search EM2 states or EM1 .level files",search,sizeof(search));
   if(ImGui::BeginCombo("Destination",selected.c_str())){for(auto name:kNativeLevelStates)if(Match(name,search)&&ImGui::Selectable(std::string(name).c_str(),name==selected))selected=name;ImGui::EndCombo();}
   if(ImGui::Button("Load selected level"))ExecuteEngineConsole("level.load "+selected);
   ImGui::SameLine();if(ImGui::Button("Reload checkpoint"))ExecuteEngineConsole("level.restart");
   if(ImGui::Button("Report current level/state/interior"))ExecuteEngineConsole("level.current");
   if(auto catalog=em1::CurrentLevelCatalog()) {
     ImGui::Separator();ImGui::TextUnformatted("Epic Mickey 1 — original .level files");
     if(!catalog->error.empty())ImGui::TextWrapped("Level directory: %s",catalog->error.c_str());
     static std::string selected_em1;
     if(ImGui::BeginCombo("EM1 destination",selected_em1.empty()?"Choose a .level file":selected_em1.c_str())) {
       for(const auto& name:catalog->descriptors)
         if(Match(name,search) && ImGui::Selectable(name.c_str(),name==selected_em1))selected_em1=name;
       ImGui::EndCombo();
     }
     const bool registered=!selected_em1.empty() && !em1::RegisteredLevelState(selected_em1).empty();
     ImGui::BeginDisabled(!registered);
     if(ImGui::Button("Load original .level"))ExecuteEngineConsole("level.load em1:"+selected_em1);
     ImGui::EndDisabled();
     ImGui::TextWrapped("%zu original descriptors. Selected entry: %s",catalog->descriptors.size(),registered?"registered in native loader":"not registered in this session");
   }

 }
 if(ImGui::CollapsingHeader("Developer gameplay tools",ImGuiTreeNodeFlags_DefaultOpen)){
   for(const auto& action:actions){ImGui::PushID(action.name);if(ImGui::Button(action.name))ExecuteEngineConsole(std::string("debug ")+action.name);ImGui::SameLine();ImGui::TextWrapped("%s",action.description);ImGui::PopID();}
   if(ImGui::Button("Unlock abilities"))ExecuteEngineConsole("unlock abilities");ImGui::SameLine();if(ImGui::Button("Unlock costumes"))ExecuteEngineConsole("unlock costumes");ImGui::SameLine();if(ImGui::Button("Unlock pin buffs"))ExecuteEngineConsole("unlock pins");
   static int tickets=500;ImGui::InputInt("Ticket amount",&tickets);if(ImGui::Button("Give tickets"))ExecuteEngineConsole("give tickets "+std::to_string(tickets));
 }
 if(ImGui::CollapsingHeader("Lua editor",ImGuiTreeNodeFlags_DefaultOpen)){
   static char source[kMaxSource+1]="return LevelManager_GetCurrentLevelName(), LevelManager_GetCurrentState(), LevelManager_GetCurrentInterior()";
   ImGui::TextWrapped("This runs in the game's VM and shares its globals. Enter Lua directly; no outer quoting. Use return for results or print for console output. NiFixedString parameters take Lua strings; int/float take numbers; entity handles can come from GetPlayer() or GetPlayerOrAI(PLAYER_TWO).");
   if(ImGui::BeginCombo("Example snippet","Choose a gameplay example (does not execute)")){
     struct Example {const char* title;const char* code;};
     static constexpr Example examples[]={
       {"Inspect current level / state / interior","return LevelManager_GetCurrentLevelName(), LevelManager_GetCurrentState(), LevelManager_GetCurrentInterior()"},
       {"Inspect paint and thinner","return GetAmmo('Paint'), GetAmmoMax('Paint'), GetAmmo('Thinner'), GetAmmoMax('Thinner')"},
       {"Inspect god-mode setting","return GetConfigVariableBool('GodModeOnPlayer')"},
       {"Fly camera","FlyCamera(GetPlayer())"},
       {"Bring Oswald to the player","MoveToEntity(GetPlayer2OrAI(),GetPlayer())"}
     };
     for(const auto& example:examples)if(ImGui::Selectable(example.title))snprintf(source,sizeof(source),"%s",example.code);
     ImGui::EndCombo();
   }
   ImGui::InputTextMultiline("##lua-source",source,sizeof(source),ImVec2(-1,180),ImGuiInputTextFlags_AllowTabInput);
   if(ImGui::Button("Run Lua"))EngineConsoleMessage(QueueGameLua(source));
   ImGui::SameLine();ImGui::TextDisabled("%s",vm_seen?"Game VM observed":"Waiting for a game VM boundary");
   if(ImGui::Button("Cancel queued Lua"))EngineConsoleMessage("Cancelled "+std::to_string(CancelQueuedGameLua())+" queued requests.");ImGui::SameLine();ImGui::TextDisabled("%u queued",PendingGameLua());
   static char script_file[512]="experiments.lua";
   ImGui::InputTextWithHint("Lua file","Relative to user_data/scripts, or an absolute path",script_file,sizeof(script_file));
   if(ImGui::Button("Run Lua file"))ExecuteEngineConsole("lua.file "+Quote(script_file));
   if(ImGui::Button("Browse loaded globals"))ExecuteEngineConsole("lua.globals");
   ImGui::SameLine();ImGui::TextDisabled("Live VM functions and tables, including loaded Lua helpers");
   static char filter[128]{};ImGui::InputTextWithHint("##api-search","Search 939 recovered native function names",filter,sizeof(filter));
   if(*filter){unsigned shown=0;for(auto name:kNativeScriptExports)if(Match(name,filter)&&shown++<60){if(ImGui::Selectable(std::string(name).c_str()))snprintf(source,sizeof(source),"-- Arguments: %.*s\n%.*s()",int(Signature(name).size()),Signature(name).data(),int(name.size()),name.data());if(ImGui::IsItemHovered())ImGui::SetTooltip("%.*s(%.*s)",int(name.size()),name.data(),int(Signature(name).size()),Signature(name).data());}ImGui::TextDisabled("%u matches; first 60 shown. Select a name to start a call with its parameter declaration.",shown);}
 }
 ImGui::EndChild();
}
bool em2::TestGameplayConsole(){
 if(!Level("MeanStreet_North.Visit_1") || Level("../bad") || !Level("empty_level") || std::size(kNativeLevelStates)!=205 || std::size(kNativeScriptExports)!=939)return false;
 if(ValidSource({}) || ValidSource(std::string(kMaxSource+1,'x')) || ValidSource(std::string_view("a\0b",3)))return false;
 if(!ValidSource("local x=1\nreturn x+2") || !FindAction("fly") || !FindAction("refill") || FindAction("unknown"))return false;
 if(GameLuaConsoleSource("lua print(\"hello world\")")!=std::optional<std::string>("print(\"hello world\")") ||
    GameLuaConsoleSource("  lua \"return \\\"hello world\\\"\"")!=std::optional<std::string>("return \"hello world\"") || GameLuaConsoleSource("lua.api Config"))return false;
 if(Signature("TransitionToLevel")!="NiFixedString levelName" || Signature("CleanLoadLevel")!="NiFixedString levelName" || Signature("SetConfigVariableString")!="ScriptEntityHandle hIgnoredEntity, NiFixedString name, NiFixedString value")return false;
 if(!InspectionSource("math","") || !InspectionSource("LevelManager.State","level") || InspectionSource("math..sin","") || InspectionSource("math.","") || InspectionSource("math;error()","") || InspectionSource("1bad","") || InspectionSource("math.__index()",""))return false;
 if(ExecuteGameplayConsoleTool({"unrelated"}) || !ExecuteGameplayConsoleTool({"give","tickets","nan"}) || !ExecuteGameplayConsoleTool({"level.load","bad"}))return false;
 const auto choices=GameplayConsoleCompletions({"level.load"});if(!choices || choices->size()!=205)return false;
 if(PendingGameLua()!=0)return false;
 for(unsigned i=0;i<8;++i)if(!QueueGameLua("return 1").starts_with("Queued Lua"))return false;
 if(QueueGameLua("return 2").find("full")==std::string::npos || CancelQueuedGameLua()!=8 || PendingGameLua()!=0)return false;
 auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));if(!base)return false;
 struct Release {uint8_t* p;unsigned csr=_mm_getcsr();~Release(){_mm_setcsr(csr);VirtualFree(p,0,MEM_RELEASE);}} release{base};
 if(!VirtualAlloc(base,65536,MEM_COMMIT,PAGE_READWRITE) || !VirtualAlloc(base+0xE0000000,65536,MEM_COMMIT,PAGE_READWRITE))return false;
 PPC_STORE_U32(0x4000,0xE0000100);PPC_STORE_U32(0x4004,4);PPC_STORE_U32(0xE0000104,5);
 std::memcpy(PPC_RAW_ADDR(0xE000010C),"hello",5);std::memcpy(base+0xE000010C,"wrong",5);
 if(Value(base,0x4000)!="hello")return false;
 PPC_STORE_U32(0x4000,std::bit_cast<uint32_t>(42.f));PPC_STORE_U32(0x4004,3);if(Value(base,0x4000)!="42")return false;
 PPC_STORE_U32(0x4000,1);PPC_STORE_U32(0x4004,1);if(Value(base,0x4000)!="true")return false;
 // A module guard must reject ordinary level functions, malformed stacks and
 // hostile source lengths without touching guest data. Exercise SDK E-view
 // addresses as well as low-address fixture memory.
 PPC_STORE_U32(0x5000+36,0x5108);PPC_STORE_U32(0x5100,0x5200);PPC_STORE_U32(0x5104,9);
 PPC_STORE_U32(0x5208,0x5300);PPC_STORE_U32(0x520C,0x5500);PPC_STORE_U32(0x5328,0x5400);PPC_STORE_U32(0x5400,0);
 PPC_STORE_U32(0x5418,0xE0000200);
 constexpr char module_name[]="@Environments/_shared/Scripts/MeanStreet_North.lua";
 PPC_STORE_U32(0xE0000204,sizeof(module_name)-1);std::memcpy(PPC_RAW_ADDR(0xE000020C),module_name,sizeof(module_name));
 const auto fixture=std::vector<uint8_t>(base+0x5000,base+0x5500);
 const auto csr=_mm_getcsr();SetLastError(ERROR_BUSY);
 if(MeanStreetModuleEnvironment(base,0x5000,0)!=0x5500 || GetLastError()!=ERROR_BUSY || _mm_getcsr()!=csr ||
    std::memcmp(base+0x5000,fixture.data(),fixture.size()))return false;
 if(MeanStreetModuleEnvironment(base,0x5000,65) || MeanStreetModuleEnvironment(base,0xFFFFFFF8,0))return false;
 PPC_STORE_U32(0x5400,45);if(MeanStreetModuleEnvironment(base,0x5000,0))return false;PPC_STORE_U32(0x5400,0);
 PPC_STORE_U32(0xE0000204,0x3FFFFFFF);if(MeanStreetModuleEnvironment(base,0x5000,0))return false;
 PPC_STORE_U32(0xE0000204,sizeof(module_name)-1);PPC_STORE_U32(0x5104,8);if(MeanStreetModuleEnvironment(base,0x5000,0))return false;
 PPC_STORE_U32(0x5104,9);PPC_STORE_U32(0x5418,0xFFFFFFF8);if(MeanStreetModuleEnvironment(base,0x5000,0))return false;
 REXLOG_INFO("Mean Street module guard passed: root-only matching, SDK E-view source, bounded arguments/lengths, unreadable pointers and unchanged guest/host state");
 REXLOG_INFO("Gameplay console checks passed: 205 retail states, 939 native exports, recovered parameter declarations, raw/quoted/multiline source, developer recipes, selector guards and SDK E-view string offset");return true;
}

// Openfile executes module chunks through the original unprotected lua_call.
// Only a normal return can reach installation; original VM errors still unwind.
PPC_FUNC_IMPL(sub_826F71F8){
 const auto state=ctx.r3.u32;
 const auto environment=faulted?0:MeanStreetModuleEnvironment(base,state,ctx.r4.u32);
 __imp__sub_826F71F8(ctx,base);
 if(environment)InstallMeanStreetLoadWait(ctx,base,state,environment);
}
PPC_FUNC_IMPL(sub_8271C108){
 const auto state=ctx.r3.u32;
 struct Depth {Depth(){++protected_depth;}~Depth(){--protected_depth;}} depth;
 __imp__sub_8271C108(ctx,base);
 // Borrow the current owner/thread only after a successful outermost protected
 // operation. No retained lua_State survives a world/VM destruction boundary.
 if(protected_depth==1 && ctx.r3.s32==0)Service(ctx,base,state);
}
PPC_FUNC_IMPL(sub_82705AF8){
 if(executing && Readable(base,ctx.r3.u32,128)){
   const auto csr=_mm_getcsr();++print_count;
   const auto state=ctx.r3.u32,top=PPC_LOAD_U32(state+36),bottom=PPC_LOAD_U32(state+40);std::string line="Lua print: ";
   if(top>=bottom && top-bottom<=4096 && (top-bottom)%8==0)for(auto p=bottom;p<top;p+=8){if(p!=bottom)line+=' ';line+=Value(base,p);}
   em2::EngineConsoleMessage(line);
   _mm_setcsr(csr);
 }
 __imp__sub_82705AF8(ctx,base);
}
