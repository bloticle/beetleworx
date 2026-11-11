#include "console_tools.h"
#include "engine_workbench.h"
#include "engine_settings_catalog.h"
#include "native_engine.h"
#include "gameplay_mods.h"
#include "pc_settings.h"
#include "simulation.h"
#include "game_console.h"
#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <charconv>
#include <cmath>
#include <sstream>

namespace {
constexpr em2::ConsoleCommandInfo catalog[]={
 {"lua","Gameplay scripting","lua <Lua source>","Execute source in the actual game VM. Raw source needs no outer quotes. Return values and print output go to Output; protected errors are reported. The Gameplay tab has a multiline editor.","lua \"return GetAmmo('Paint')\""},
 {"lua.file","Gameplay scripting","lua.file <filename.lua>","Execute a UTF-8 Lua file in the live game VM. Relative paths use user_data/scripts; absolute paths are accepted. Uses the same protected execution, output and 16 KiB source limit as lua.","lua.file experiments.lua"},
 {"lua.api","Gameplay scripting","lua.api [search]","Search 939 native script-export names recovered from this retail build. Script helpers may add more functions.","lua.api Config"},
 {"lua.globals","Gameplay scripting","lua.globals [search]","Search the actual loaded VM globals, including Lua-defined helpers absent from the recovered native index. Prints names, types and primitive values; shows at most 100 matches, scans at most 4096 entries.","lua.globals camera"},
 {"lua.inspect","Gameplay scripting","lua.inspect <global.table.path> [search]","Inspect a live Lua table or primitive through raw entries, without invoking table metamethods. Dotted identifiers only. Results appear in Output.","lua.inspect math"},
 {"lua.cancel","Gameplay scripting","lua.cancel","Cancel queued Lua/developer requests before execution. Running code is unaffected.","lua.cancel"},
 {"game.get","Native configuration","game.get <name>","Read the game's native configuration store, separate from PC backend variables.","game.get GodModeOnPlayer"},
 {"game.set","Native configuration","game.set <name> <value>","Change a native configuration value through the game API. Returns write success and current string value. Some systems cache settings; this does not force their reinitialization.","game.set GodModeOnPlayer True"},
 {"levels","Levels","levels [search]","Find exact loadable IDs from the 205-state retail level database.","levels meanstreet"},
 {"level.load","Levels","level.load <level.state>","Request the game's clean level transition during offline gameplay. Validates the destination against the retail state catalog.","level.load meanstreet_north.visit_1"},
 {"level.restart","Levels","level.restart","Request the native restart of the current offline level.","level.restart"},
 {"level.current","Levels","level.current","Report current native level, state and interior through game script functions.","level.current"},
 {"debug","Developer gameplay","debug [feature]","Run a shipped developer action: fly camera, teleport to camera, refill, HUD/debug displays, inkwells or buddy controls. Run debug to list all actions.","debug refill"},
 {"give","Developer gameplay","give <tickets|health> <amount>","Grant E-tickets or set current player health through native gameplay functions. Authored resource caps may apply.","give tickets 500"},
 {"unlock","Developer gameplay","unlock <abilities|costumes|pins>","Run original unlock functions. Normal game autosaves may retain these changes.","unlock abilities"},
 {"engine","Discovery","engine [search]","Search commands, graphics, movement tuning and backend variables together, with current values and editability.","engine shadow"},
 {"help","Discovery","help [command or topic]","Search command names, descriptions, syntax and examples.","help animation"},
 {"commands","Discovery","commands [search]","Search the command reference. The Commands tab provides clickable examples.","commands damage"},
 {"target","Native engine","target [selected|mickey|oswald]","Choose the default actor for native entity, animation and combat commands. Movement impulses and speed follow the controlled player.","target oswald"},
 {"graphics","Rendering","graphics [setting [choice|reset]]","Read or change any graphics/display/camera setting using named choices. Changes save automatically.","graphics player_shadow 2048"},
 {"movement","Movement","movement [parameter [value|reset]] | movement reset","Read or change every sprint, dash and pound parameter. Values save automatically.","movement dash_cooldown 0"},
 {"combat","Combat","combat <paint|thinner|physical|knockback|spin> <amount> <radius> [actor]","Apply the game's native stimulus to a sphere around the target actor. Radius is in world units; the source actor is excluded.","combat spin 5 8 mickey"},
 {"entity.inspect","Native engine","entity.inspect [actor]","Read native entity identity, motion state, position and velocity.","entity.inspect oswald"},
 {"entity.components","Native engine","entity.components [actor]","List the player's native component type slots, interface addresses and vtables.","entity.components mickey"},
 {"entity.push","Native engine","entity.push <x> <y> <z> <decay> [actor]","Set native external motion for either character. Y points up, each axis accepts -500..500; decay is 0..1. Replaces the previous external impulse; does not teleport.","entity.push 0 15 0 0.9 oswald"},
 {"entity.getfloat","Native engine","entity.getfloat <property> [actor]","Read a named reflected float property if the entity exposes it.","entity.getfloat \"Max Move Speed\""},
 {"entity.setfloat","Native engine","entity.setfloat <property> <value> [actor]","Write an existing reflected float property; unavailable properties are rejected.","entity.setfloat \"Max Move Speed\" 12"},
 {"anim.snapshot","Animation","anim.snapshot [actor]","Refresh the Animation tools browser with events and raw variable values from the actual loaded graph.","anim.snapshot mickey"},
 {"anim.events","Animation","anim.events [filter] [actor]","List events accepted by the current animation graph.","anim.events Jump"},
 {"anim.vars","Animation","anim.vars [filter] [actor]","List variable names present in the current animation graph.","anim.vars Speed"},
 {"anim.event","Animation","anim.event <event> [actor]","Send an animation event. Use the Animation browser to select a real loaded event.","anim.event EVENT_Start_Jump mickey"},
 {"anim.float","Animation","anim.float <variable> <number> [actor]","Hold a float variable through controller updates until released.","anim.float VAR_Forward_Speed 12"},
 {"anim.int","Animation","anim.int <variable> <integer> [actor]","Hold an integer variable through controller updates until released.","anim.int VAR_InvisibleState 1"},
 {"anim.overrides","Animation","anim.overrides [actor]","List currently held animation overrides.","anim.overrides"},
 {"anim.release","Animation","anim.release <variable|all> [actor]","Release held animation values so the game controls them again.","anim.release all"},
 {"combat.sphere","Combat","combat.sphere <type ID> <amount> <radius> [actor]","Low-level native stimulus command. Prefer the named combat command: thinner0, paint1, physical2, knockback3, spin8.","combat.sphere 8 5 8"},
 {"timescale","Simulation","timescale <0..8>","Change offline simulation speed while retaining the native physics timestep.","timescale 0.25"},
 {"pause","Simulation","pause","Freeze offline simulation. Console and presentation remain available.","pause"},
 {"resume","Simulation","resume","Restore normal offline simulation speed.","resume"},
 {"step","Simulation","step [1..8]","Advance native simulation ticks while paused.","step 1"},
 {"player.switch","Movement","player.switch","Request a switch to the other playable character.","player.switch"},
 {"player.speed","Movement","player.speed <0..500|reset>","Override the controlled player's native maximum speed. Reset restores authored movement.","player.speed 20"},
 {"player.push","Movement","player.push <x> <y> <z> [decay 0..1]","Apply native external velocity to the controlled character. Positive Y launches upward.","player.push 0 15 0 0.9"},
 {"fps","Rendering","fps <30|40|50|60|90|120|144|165|240|uncapped>","Set the presentation frame limit.","fps 60"},
 {"settings","Legacy","settings","List numeric graphics settings. Prefer graphics for readable current values and choices.","settings"},
 {"setting","Legacy","setting <name/index> [choice index]","Legacy numeric settings command; graphics accepts readable choice names.","setting 18 1"},
 {"find","Variables","find [text]","Search registered backend variables and descriptions.","find lighting"},
 {"get","Variables","get <variable>","Read a registered backend variable.","get em2_experimental_character_swap"},
 {"set","Variables","set <variable> <value>","Change a backend variable that explicitly supports live editing.","set em2_experimental_character_swap true"},
 {"reset","Variables","reset <variable>","Restore a live backend variable's default value.","reset em2_experimental_character_swap"},
 {"writeconfig","Configuration","writeconfig","Write modified live backend variables to engine-console.cfg. Graphics and movement settings already save automatically.","writeconfig"},
 {"exec","Configuration","exec","Run engine-console.cfg from your user data directory.","exec"},
 {"echo","Discovery","echo <text>","Print a message.","echo \"Hello Wasteland\""},
 {"clear","Discovery","clear","Clear console output.","clear"},
};
std::string Normal(std::string_view s){std::string r;for(char c:s)if(c>='A'&&c<='Z')r+=char(c+32);else if((c>='a'&&c<='z')||(c>='0'&&c<='9'))r+=c;return r;}
std::string ChoiceKey(std::string_view s){std::string r;for(char c:s)if(c>='A'&&c<='Z')r+=char(c+32);else if((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='+'||c=='-'||c=='.')r+=c;return r;}
bool Match(std::string_view s,std::string_view q){return Normal(s).find(Normal(q))!=std::string::npos;}
std::string Quote(std::string_view s){std::string out="\"";for(char c:s){if(c=='"'||c=='\\')out+='\\';out+=c;}return out+'"';}
std::string Choice(const em2::EngineSettingRow& row,int i){std::string_view s=row.choices;while(i-->0)s.remove_prefix(s.find('|')+1);return std::string(s.substr(0,s.find('|')));}
const em2::EngineSettingRow* Setting(std::string_view input){const auto n=Normal(input);for(const auto& r:em2::kEngineSettings)if(n==Normal(em2::PCSettingName(r.index))||n==Normal(r.title))return &r;
 int alias=n=="hdr"?21:n=="bloom"?18:n=="lighting"?25:n=="fov"?16:n=="internalrenderresolution"?3:-1;for(const auto& r:em2::kEngineSettings)if(r.index==alias)return &r;return nullptr;}
int ChoiceIndex(const em2::EngineSettingRow& row,std::string_view input){const auto n=ChoiceKey(input);if(n.empty())return -1;int match=-1;for(int i=0;i<em2::PCSettingChoiceCount(row.index);++i){const auto c=ChoiceKey(Choice(row,i));if(c==n)return i;if(c.starts_with(n)){if(match>=0)return -1;match=i;}}return match;}
struct MovementField {const char* name;const char* title;bool em2::GameplayTuning::*flag=nullptr;float em2::GameplayTuning::*number=nullptr;float low=0,high=1;};
constexpr MovementField movement[]={
 {"sprint","Enable sprint",&em2::GameplayTuning::sprint},{"dash","Enable dash",&em2::GameplayTuning::dash},{"pound","Enable ground pound",&em2::GameplayTuning::pound},{"air_dash","Allow air dash",&em2::GameplayTuning::air_dash},
 {"keyboard_sprint_toggle","Keyboard toggle sprint",&em2::GameplayTuning::keyboard_sprint_toggle},{"controller_sprint_toggle","Controller toggle sprint",&em2::GameplayTuning::controller_sprint_toggle},{"native_animations","Native ability animations",&em2::GameplayTuning::native_animations},{"dash_spin_animation","Dash spin animation",&em2::GameplayTuning::dash_spin_animation},
 {"pound_damage","Pound damage",&em2::GameplayTuning::pound_damage},{"pound_knockback","Pound knockback",&em2::GameplayTuning::pound_knockback},
 {"sprint_multiplier","Sprint multiplier",nullptr,&em2::GameplayTuning::sprint_multiplier,1,8},{"dash_speed","Dash speed",nullptr,&em2::GameplayTuning::dash_speed,1,100},{"dash_duration","Dash duration (seconds)",nullptr,&em2::GameplayTuning::dash_duration,.05f,2},{"dash_cooldown","Dash cooldown (seconds)",nullptr,&em2::GameplayTuning::dash_cooldown,0,10},
 {"pound_speed","Pound falling speed",nullptr,&em2::GameplayTuning::pound_speed,1,100},{"pound_damage_amount","Pound damage amount",nullptr,&em2::GameplayTuning::pound_damage_amount,0,1000},{"pound_radius","Pound impact radius",nullptr,&em2::GameplayTuning::pound_radius,.1f,50}
};
const MovementField* Field(std::string_view name){for(const auto& f:movement)if(Normal(name)==Normal(f.name))return &f;return nullptr;}
std::string Value(const MovementField& f,const em2::GameplayTuning& t){return f.flag?(t.*f.flag?"on":"off"):std::to_string(t.*f.number);}
std::string Choices(const em2::EngineSettingRow& r){return std::string(em2::PCSettingName(r.index))+" = "+Choice(r,em2::PCSetting(r.index))+"\nChoices: "+r.choices;}
std::optional<float> Number(std::string_view text){float v;auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),v);if(error!=std::errc{}||end!=text.data()+text.size()||!std::isfinite(v))return {};return v;}
const char* ActorName(){const auto actor=em2::NativeConsoleTarget();return actor<0?"Selected character":actor?"Oswald":"Mickey";}
void Run(std::string command){em2::ExecuteEngineConsole(command);}
struct Form {const char* command;const char* labels;unsigned required;};
constexpr Form forms[]={
 {"lua","Lua source (or use the multiline Gameplay editor)",1},{"lua.api","Native function search",0},{"levels","Level/state search",0},
 {"lua.file","Lua filename (relative to user_data/scripts, or absolute)",1},
 {"lua.cancel","",0},{"lua.globals","Search loaded globals",0},{"lua.inspect","Dotted global table path|Entry search (optional)",1},
 {"game.get","Native configuration key",1},{"game.set","Native configuration key|String value",2},
 {"level.load","Retail level.state ID",1},{"level.restart","",0},{"level.current","",0},
 {"debug","Developer action (optional: blank lists actions)",0},{"give","Resource: tickets or health|Amount",2},{"unlock","Category: abilities, costumes, pins",1},
 {"engine","Search systems or values",0},{"help","Command or topic",0},{"commands","Search commands",0},{"target","Actor: selected, mickey, oswald",0},
 {"graphics","Setting|Choice or reset",0},{"movement","Parameter or reset|Value or reset",0},
 {"combat","Stimulus: paint, thinner, physical, knockback, spin|Amount (0..1000)|Radius (0.1..50 world units)|Actor (optional)",3},
 {"entity.inspect","Actor (optional)",0},{"entity.components","Actor (optional)",0},
 {"entity.push","X velocity (-500..500)|Y velocity, up (-500..500)|Z velocity (-500..500)|Decay per tick (0..1)|Actor (optional)",4},
 {"entity.getfloat","Reflected property name|Actor (optional)",1},{"entity.setfloat","Reflected property name|Float value|Actor (optional)",2},
 {"anim.snapshot","Actor (optional)",0},{"anim.events","Event filter (optional)|Actor (optional)",0},{"anim.vars","Variable filter (optional)|Actor (optional)",0},
 {"anim.event","Loaded event name|Actor (optional)",1},{"anim.float","Loaded variable name|Float value|Actor (optional)",2},
 {"anim.int","Loaded variable name|Integer value|Actor (optional)",2},{"anim.overrides","Actor (optional)",0},{"anim.release","Variable name or all|Actor (optional)",1},
 {"combat.sphere","Native stimulus ID (0..31)|Amount (0..1000)|Radius (0.1..50)|Actor (optional)",3},
 {"timescale","Time multiplier (0..8)",1},{"pause","",0},{"resume","",0},{"step","Ticks (1..8)",0},{"player.switch","",0},
 {"player.speed","Speed (0..500) or reset",1},{"player.push","X velocity|Y velocity, up|Z velocity|Decay (optional, 0..1)",3},
 {"fps","Frame limit or uncapped",1},{"settings","",0},{"setting","Setting name or index|Choice index (optional)",1},
 {"find","Variable search",0},{"get","Backend variable name",1},{"set","Backend variable name|Value",2},{"reset","Backend variable name",1},
 {"writeconfig","",0},{"exec","",0},{"echo","Text",0},{"clear","",0}
};
const Form* FormFor(std::string_view name){for(const auto& f:forms)if(name==f.command)return &f;return nullptr;}
std::vector<std::string> Labels(const Form& form){
 std::vector<std::string> out;std::string_view text=form.labels;
 while(!text.empty()){auto split=text.find('|');out.emplace_back(text.substr(0,split));if(split==text.npos)break;text.remove_prefix(split+1);}return out;
}
std::optional<std::string> Build(const Form& form,const std::vector<std::string>& args){
 if(args.size()>Labels(form).size())return {};
 size_t count=args.size();while(count && args[count-1].empty())--count;if(count<form.required)return {};
 std::string result=form.command;for(size_t i=0;i<count;++i){
   // Lists accept an empty filter before an explicit actor. Other commands
   // cannot skip a positional argument, even when it is optional.
   if(args[i].empty() && !(i==0 && (result=="anim.events" || result=="anim.vars")))return {};
   if(args[i].find_first_of("\r\n\t")!=std::string::npos)return {};
   result+=' ';result+=Quote(args[i]);
 }return result;
}
std::vector<std::string> FormChoices(std::string_view cmd,const std::vector<std::string>& args,size_t field){
 std::vector<std::string> before{std::string(cmd)};before.insert(before.end(),args.begin(),args.begin()+field);
 std::vector<std::string> result;
 if(auto choices=em2::ConsoleToolCompletions(before))result=std::move(*choices);
 else if(auto choices=em2::NativeEngineArgumentCompletions(cmd,unsigned(field+1),""))result=std::move(*choices);
 else if(field==0 && (cmd=="get" || cmd=="set" || cmd=="reset" || cmd=="find"))result=rex::cvar::ListFlags();
 if(cmd=="set" && field==1 && !args.empty())if(auto* info=rex::cvar::GetFlagInfo(args[0])){
   result.push_back(rex::cvar::GetFlagByName(args[0]));result.push_back(info->default_value);
   if(info->default_value=="true" || info->default_value=="false"){result.emplace_back("true");result.emplace_back("false");}
 }
 for(auto& choice:result)if(choice.starts_with('"'))if(auto tokens=em2::TokenizeEngineConsole(choice);tokens && tokens->size()==1)choice=(*tokens)[0];
 std::sort(result.begin(),result.end());result.erase(std::unique(result.begin(),result.end()),result.end());return result;
}
}
std::span<const em2::ConsoleCommandInfo> em2::ConsoleCommandCatalog(){return catalog;}
std::optional<std::string> em2::ExecuteConsoleTool(const std::vector<std::string>& args){
 if(args.empty())return {};const auto& cmd=args[0];std::ostringstream out;
 if(auto gameplay=ExecuteGameplayConsoleTool(args))return gameplay;
 if(cmd=="engine"){
   if(args.size()>2)return "Usage: engine [search]. Quote a multiword search.";
   const auto query=args.size()==2?args[1]:"";unsigned count=0;
   auto line=[&](const std::string& text){if(count++<120)out<<text<<"\n";};
   for(const auto& c:catalog)if(Match(c.name,query)||Match(c.group,query)||Match(c.description,query))line(std::string("[Command / ")+c.group+"] "+c.usage);
   for(const auto& r:kEngineSettings)if(Match(r.title,query)||Match(PCSettingName(r.index),query))line("[Graphics / saved] "+Choices(r));
   const auto tuning=GetGameplayTuning();for(const auto& f:movement)if(Match(f.name,query)||Match(f.title,query))line(std::string("[Movement / saved] ")+f.name+" = "+Value(f,tuning)+(f.flag?" (on/off)":" ("+std::to_string(f.low)+".."+std::to_string(f.high)+")"));
   if(Match("pound damage type",query))line(std::string("[Movement / saved] pound_damage_type = ")+(tuning.pound_damage_type==PoundDamageType::Spin?"spin":"physical"));
   for(const auto& name:rex::cvar::ListFlags())if(const auto* info=rex::cvar::GetFlagInfo(name);info && (Match(name,query)||Match(info->description,query)))line(std::string("[Backend / ")+(info->lifecycle==rex::cvar::Lifecycle::kHotReload?"live] ":"startup only] ")+name+" = "+rex::cvar::GetFlagByName(name)+" : "+info->description);
   out<<count<<" matches";if(count>120)out<<"; showing first 120, narrow your search";
   out<<".\nUse Commands to edit labeled fields; engine shadow / engine camera / engine animation searches across systems.";return out.str();
 }
 if(cmd=="help"||cmd=="commands"){
   if(args.size()>2)return "Usage: help [command or topic]";
   const std::string query=args.size()==2?args[1]:"";unsigned count=0;
   if(query.empty()&&cmd=="help")return "Gameplay quick start:\nlevels meanstreet -> find exact level IDs; level.load meanstreet_north.visit_1 -> native transition.\nlevel.current / level.restart -> inspect or restart this level.\nlua return GetAmmo('Paint') -> run source in the actual game VM.\nlua.file experiments.lua -> run a reusable script from user_data/scripts.\nlua.globals camera / lua.inspect math -> discover loaded functions and tables.\ndebug -> list shipped developer tools; debug fly / debug teleport_camera / debug refill.\nStart in Commands: select a system and a command, then edit labeled fields. Run executes exactly the preview shown. Tools provides direct controls.\nengine shadow / engine camera / engine animation searches commands AND settings with their current values.\ngraphics <setting> lists choices; movement <parameter> reports valid ranges.\ntarget mickey / oswald / selected chooses the native actor. entity.push can launch either character.\nUse Tools > Native entity and animation graph > Refresh loaded graph for real loaded events and values.\nTab completes. Up/Down recalls commands. Offline native tools require loaded gameplay; graphics/movement settings save automatically.";
   if(query.empty())out<<"Browse Commands or Tools in this console. help <topic> narrows the reference. Quote choices containing spaces.\n";
   for(const auto& c:catalog)if(Match(c.name,query)||Match(c.group,query)||Match(c.description,query)){++count;out<<c.usage<<"\n  "<<c.description<<"\n";if(cmd=="help")out<<"  Example: "<<c.example<<"\n";}
   if(!count)out<<"No commands match '"<<query<<"'. Try help animation, help combat or help rendering.";
   return out.str();
 }
 if(cmd=="target"){
   if(args.size()>2)return "Usage: target [selected|mickey|oswald]";
   if(args.size()==2){const auto n=Normal(args[1]);if(n!="selected"&&n!="mickey"&&n!="oswald")return "Choose selected, mickey or oswald.";SetNativeConsoleTarget(n=="selected"?-1:n=="mickey"?0:1);}
   return std::string("Native command target: ")+ActorName()+". Explicit final actor arguments override this choice.";
 }
 if(cmd=="graphics"){
   if(args.size()==1){for(const auto& r:kEngineSettings)out<<PCSettingName(r.index)<<" = "<<Choice(r,PCSetting(r.index))<<"\n";out<<"Use graphics <setting> to see its choices.";return out.str();}
   if(args.size()>3)return "Usage: graphics <setting> [choice|reset]. Quote multiword choices.";
   const auto* r=Setting(args[1]);if(!r)return "Unknown graphics setting. Run graphics to list names, or use Tools > Graphics.";
   if(args.size()==3){const auto choice=args[2]=="reset"?PCSettingDefault(r->index):ChoiceIndex(*r,args[2]);if(choice<0)return "Unknown or ambiguous choice. "+Choices(*r);if(ChangePCSetting(r->index,choice)<0)return "Setting change rejected.";}
   return Choices(*r);
 }
 if(cmd=="movement"){
   auto config=GetGameplayTuning();const GameplayTuning defaults;
   if(args.size()==2&&args[1]=="reset")return ChangeGameplayTuning(defaults)?"Restored and saved movement defaults.":"Could not save movement defaults.";
   if(args.size()==1){for(const auto& f:movement)out<<f.name<<" = "<<Value(f,config)<<"\n";out<<"pound_damage_type = "<<(config.pound_damage_type==PoundDamageType::Spin?"spin":"physical")<<"\nUse movement <parameter> <value>, or movement reset.";return out.str();}
   if(args.size()>3)return "Usage: movement <parameter> [value|reset]";
   if(Normal(args[1])=="pounddamagetype"){
     if(args.size()==3){const auto v=Normal(args[2]);if(v!="spin"&&v!="physical"&&v!="reset")return "Pound damage type choices: spin, physical, reset.";config.pound_damage_type=v=="physical"?PoundDamageType::Physical:PoundDamageType::Spin;if(!ChangeGameplayTuning(config))return "Could not save movement setting.";}
     return std::string("pound_damage_type = ")+(config.pound_damage_type==PoundDamageType::Spin?"spin":"physical");
   }
   const auto* f=Field(args[1]);if(!f)return "Unknown movement parameter. Run movement to list parameters, or use Tools > Movement.";
   if(args.size()==3){const auto n=ChoiceKey(args[2]);if(args[2]=="reset"){if(f->flag)config.*f->flag=defaults.*f->flag;else config.*f->number=defaults.*f->number;}
     else if(f->flag){if(n!="on"&&n!="off"&&n!="true"&&n!="false"&&n!="1"&&n!="0")return "Use on, off or reset.";config.*f->flag=n=="on"||n=="true"||n=="1";}
     else{const auto v=Number(args[2]);if(!v||*v<f->low||*v>f->high)return std::string(f->name)+" range: "+std::to_string(f->low)+" .. "+std::to_string(f->high);config.*f->number=*v;}
     if(!ChangeGameplayTuning(config))return "Could not save movement setting.";
   }
   return std::string(f->name)+" = "+Value(*f,config)+(f->flag?"\nChoices: on, off, reset":"\nRange: "+std::to_string(f->low)+" .. "+std::to_string(f->high)+"; reset restores the default.");
 }
 if(cmd=="combat"){
   if(args.size()!=4&&args.size()!=5)return "Usage: combat <paint|thinner|physical|knockback|spin> <amount 0..1000> <radius 0.1..50> [actor]";
   const auto n=Normal(args[1]);const int type=n=="thinner"?0:n=="paint"?1:n=="physical"?2:n=="knockback"?3:n=="spin"?8:-1;if(type<0)return "Combat types: paint, thinner, physical, knockback, spin.";
   auto native=args;native[0]="combat.sphere";native[1]=std::to_string(type);return QueueNativeEngineCommand(native);
 }
 return {};
}
std::optional<std::vector<std::string>> em2::ConsoleToolCompletions(const std::vector<std::string>& before){
 if(before.empty())return {};std::vector<std::string> out;
 if(auto gameplay=GameplayConsoleCompletions(before))return gameplay;
 if(before[0]=="help"||before[0]=="commands"){if(before.size()==1)for(const auto& c:catalog)out.emplace_back(c.name);return out;}
 if(before[0]=="target"){if(before.size()==1)out={"selected","mickey","oswald"};return out;}
 if(before[0]=="graphics"){
   if(before.size()==1)for(const auto& r:kEngineSettings)out.emplace_back(PCSettingName(r.index));
   if(before.size()==2)if(const auto* r=Setting(before[1])){out.emplace_back("reset");for(int i=0;i<PCSettingChoiceCount(r->index);++i)out.push_back(Quote(Choice(*r,i)));}return out;
 }
 if(before[0]=="movement"){
   if(before.size()==1){for(const auto& f:movement)out.emplace_back(f.name);out.emplace_back("pound_damage_type");out.emplace_back("reset");}
   if(before.size()==2){if(const auto* f=Field(before[1])){out.emplace_back("reset");if(f->flag){out.emplace_back("on");out.emplace_back("off");}}else if(before[1]=="pound_damage_type")out={"spin","physical","reset"};}return out;
 }
 if(before[0]=="combat"){if(before.size()==1)out={"paint","thinner","physical","knockback","spin"};if(before.size()==4)out={"mickey","oswald"};return out;}
 return {};
}
void em2::DrawConsoleBrowser(){
 static char query[160]{};static std::string group="All",selected="engine",loaded;
 static std::array<std::array<char,256>,5> fields{};
 ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##command-search","Find an action: animation, damage, shadow, movement...",query,sizeof(query));
 if(ImGui::BeginCombo("System",group.c_str())){if(ImGui::Selectable("All",group=="All"))group="All";std::vector<std::string_view> seen;
   for(const auto& c:catalog)if(std::find(seen.begin(),seen.end(),c.group)==seen.end()){seen.push_back(c.group);if(ImGui::Selectable(c.group,group==c.group))group=c.group;}ImGui::EndCombo();}
 const bool wide=ImGui::GetContentRegionAvail().x>=740;
 ImGui::BeginChild("command-reference",ImVec2(wide?280.f:0,wide?0.f:130.f),ImGuiChildFlags_Borders);
 unsigned matches=0;for(const auto& c:catalog){if((group!="All"&&group!=c.group)||(!Match(c.name,query)&&!Match(c.group,query)&&!Match(c.description,query)&&!Match(c.usage,query)))continue;++matches;
   ImGui::PushID(c.name);if(ImGui::Selectable(c.name,selected==c.name))selected=c.name;
   if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",c.description);ImGui::PopID();}
 if(!matches)ImGui::TextWrapped("No matching commands. Clear the search or choose All systems.");ImGui::EndChild();if(wide)ImGui::SameLine();
 ImGui::BeginChild("command-builder",ImVec2(0,0));
 const ConsoleCommandInfo* info=nullptr;for(const auto& c:catalog)if(selected==c.name)info=&c;
 if(info){const auto* form=FormFor(info->name);if(form){
   auto load_example=[&]{for(auto& field:fields)field.fill(0);if(auto tokens=TokenizeEngineConsole(info->example))for(size_t i=1;i<tokens->size()&&i<=fields.size();++i)snprintf(fields[i-1].data(),fields[i-1].size(),"%s",(*tokens)[i].c_str());};
   if(loaded!=selected){load_example();loaded=selected;}
   ImGui::SeparatorText(info->name);ImGui::TextWrapped("%s",info->description);ImGui::TextWrapped("Syntax: %s",info->usage);
   ImGui::TextWrapped("Example values are filled in below. Edit them, check the preview, then Run. Blank optional fields are omitted.");
   const auto labels=Labels(*form);std::vector<std::string> values;for(size_t i=0;i<labels.size();++i)values.emplace_back(fields[i].data());
   for(size_t i=0;i<labels.size();++i){ImGui::PushID(int(i));ImGui::TextWrapped("%s%s",labels[i].c_str(),i<form->required?" *":"");ImGui::SetNextItemWidth(-1);ImGui::InputText("##value",fields[i].data(),fields[i].size());values[i]=fields[i].data();
     const auto choices=FormChoices(info->name,values,i);if(!choices.empty() && ImGui::BeginCombo("Choose a value",values[i].c_str())){
       for(const auto& choice:choices)if(ImGui::Selectable(choice.c_str(),choice==values[i])){snprintf(fields[i].data(),fields[i].size(),"%s",choice.c_str());values[i]=choice;}
       ImGui::EndCombo();}
     if(i==0 && selected=="graphics")if(const auto* r=Setting(values[0]))ImGui::TextWrapped("Current: %s",Choice(*r,PCSetting(r->index)).c_str());
     if(i==0 && selected=="movement")if(const auto* f=Field(values[0]))ImGui::TextWrapped("Current: %s. %s",Value(*f,GetGameplayTuning()).c_str(),f->flag?"Use on / off / reset.":("Range "+std::to_string(f->low)+" .. "+std::to_string(f->high)).c_str());
     if(i==0 && (selected=="get"||selected=="set"||selected=="reset"))if(const auto* flag=rex::cvar::GetFlagInfo(values[0])){
       ImGui::TextWrapped("%s",flag->description.c_str());ImGui::TextWrapped("Current: %s; default: %s. %s",rex::cvar::GetFlagByName(values[0]).c_str(),flag->default_value.c_str(),flag->lifecycle==rex::cvar::Lifecycle::kHotReload?"Live editable.":"Startup only; cannot apply live.");}
     ImGui::PopID();}
   const auto preview=Build(*form,values);ImGui::SeparatorText("Command preview");ImGui::TextWrapped("%s",preview?preview->c_str():"Fill required fields and earlier positional arguments first.");
   ImGui::BeginDisabled(!preview);if(ImGui::Button("Run")&&preview)Run(*preview);ImGui::SameLine();if(ImGui::Button("Copy to command line")&&preview)DraftEngineConsoleCommand(*preview);ImGui::EndDisabled();
   if(ImGui::Button("Reload example values"))load_example();
 }}ImGui::EndChild();
}
void em2::DrawConsoleTools(){
 ImGui::BeginChild("engine-tools",ImVec2(0,0));
 ImGui::TextWrapped("Choose a system below for direct controls. Commands provides editable forms for every console command. Use engine <search> to find commands and settings together.");
 int actor=NativeConsoleTarget()+1;const char* targets[]={"Selected character","Mickey","Oswald"};if(ImGui::Combo("Native tool target",&actor,targets,3))SetNativeConsoleTarget(actor-1);
 ImGui::TextWrapped("Native actions run on the game thread in loaded offline gameplay. Graphics and movement tuning save automatically. Raw animation experiments do not save.");
 if(ImGui::CollapsingHeader("Simulation and controlled character",ImGuiTreeNodeFlags_DefaultOpen)){
   if(ImGui::Button("Pause world"))Run("pause");ImGui::SameLine();if(ImGui::Button("Resume world"))Run("resume");ImGui::SameLine();if(ImGui::Button("One physics tick"))Run("step 1");
   if(ImGui::Button("Quarter speed"))Run("timescale 0.25");ImGui::SameLine();if(ImGui::Button("Double speed"))Run("timescale 2");ImGui::SameLine();if(ImGui::Button("Switch character"))Run("player.switch");
   static float speed=12,push[3]={0,15,0},decay=.9f;ImGui::InputFloat("Maximum movement speed",&speed);if(ImGui::Button("Apply speed"))Run("player.speed "+std::to_string(speed));ImGui::SameLine();if(ImGui::Button("Restore authored speed"))Run("player.speed reset");
   ImGui::InputFloat3("External velocity X / Y up / Z",push);ImGui::SliderFloat("Velocity decay per tick",&decay,0,1);if(ImGui::Button("Apply external velocity"))Run("player.push "+std::to_string(push[0])+" "+std::to_string(push[1])+" "+std::to_string(push[2])+" "+std::to_string(decay));
 }
 if(ImGui::CollapsingHeader("Movement: sprint, dash and ground pound")){
   auto config=GetGameplayTuning();for(const auto& f:movement){ImGui::PushID(f.name);if(f.flag){bool v=config.*f.flag;if(ImGui::Checkbox(f.title,&v))Run(std::string("movement ")+f.name+(v?" on":" off"));}else{float v=config.*f.number;if(ImGui::InputFloat(f.title,&v,0,0,"%.3f",ImGuiInputTextFlags_EnterReturnsTrue))Run(std::string("movement ")+f.name+" "+std::to_string(v));}ImGui::PopID();}
   int type=config.pound_damage_type==PoundDamageType::Spin?0:1;if(ImGui::Combo("Pound damage type",&type,"Spin attack\0Physical\0"))Run(std::string("movement pound_damage_type ")+(type?"physical":"spin"));
   ImGui::TextDisabled("Press Enter in numeric fields to apply. Command help reports valid ranges.");if(ImGui::Button("Restore movement defaults"))Run("movement reset");
 }
 if(ImGui::CollapsingHeader("Graphics, lighting, shadows, HDR and camera"))for(const auto& r:kEngineSettings){ImGui::PushID(r.index);const auto current=Choice(r,PCSetting(r.index));if(ImGui::BeginCombo(r.title,current.c_str())){for(int i=0;i<PCSettingChoiceCount(r.index);++i){const auto choice=Choice(r,i);if(ImGui::Selectable(choice.c_str(),i==PCSetting(r.index)))Run(std::string("graphics ")+PCSettingName(r.index)+" "+Quote(choice));}ImGui::EndCombo();}ImGui::PopID();}
 if(ImGui::CollapsingHeader("Native combat sphere")){
   static int type=4;static float amount=5,radius=5;const char* names[]={"Thinner","Paint","Physical damage","Knockback","Spin attack"};const char* verbs[]={"thinner","paint","physical","knockback","spin"};
   ImGui::Combo("Stimulus",&type,names,5);ImGui::InputFloat("Amount",&amount);ImGui::InputFloat("World radius",&radius);if(ImGui::Button("Apply around target"))Run(std::string("combat ")+verbs[type]+" "+std::to_string(amount)+" "+std::to_string(radius));
 }
 if(ImGui::CollapsingHeader("Native motion: launch either character")){
   static float velocity[3]={0,15,0},decay=.9f;ImGui::InputFloat3("World velocity X / Y up / Z",velocity);ImGui::SliderFloat("Native decay per tick",&decay,0,1);
   ImGui::TextWrapped("Uses the native external-motion function for the target above, including the other character. Replaces its previous external impulse. Axis range: -500..500.");
   if(ImGui::Button("Apply to target"))Run("entity.push "+std::to_string(velocity[0])+" "+std::to_string(velocity[1])+" "+std::to_string(velocity[2])+" "+std::to_string(decay));
 }
 if(ImGui::CollapsingHeader("Native entity and animation graph")){
   if(ImGui::Button("Inspect entity"))Run("entity.inspect");ImGui::SameLine();if(ImGui::Button("List components"))Run("entity.components");ImGui::SameLine();if(ImGui::Button("Refresh loaded graph"))Run("anim.snapshot");
   const auto view=NativeAnimationSnapshot();const auto selected=NativeConsoleTarget();
   if(view && (selected<0||unsigned(selected)==view->actor)){
     ImGui::TextWrapped("%s graph snapshot: %zu events, %zu variables. Refresh after switching levels or characters. Variable types are not encoded here: choose float or integer deliberately.",view->actor?"Oswald":"Mickey",view->events.size(),view->variables.size());
     static char filter[128]{},property[128]{};static float variable=0,property_value=0;static int integer=0;static std::string event,name;
     ImGui::InputTextWithHint("##graph-search","Filter loaded events and variables",filter,sizeof(filter));
     if(ImGui::BeginCombo("Event",event.c_str())){for(const auto& e:view->events)if(Match(e,filter)&&ImGui::Selectable(e.c_str(),e==event))event=e;ImGui::EndCombo();}
     if(ImGui::Button("Send event")&&!event.empty())Run("anim.event "+Quote(event)+" "+(view->actor?"oswald":"mickey"));
     if(ImGui::BeginCombo("Variable",name.c_str())){for(const auto& v:view->variables)if(Match(v.name,filter)&&ImGui::Selectable(v.name.c_str(),v.name==name)){name=v.name;variable=v.floating;integer=v.integer;}ImGui::EndCombo();}
     ImGui::InputFloat("Float value",&variable);if(ImGui::Button("Hold float")&&!name.empty())Run("anim.float "+Quote(name)+" "+std::to_string(variable)+" "+(view->actor?"oswald":"mickey"));ImGui::InputInt("Integer value",&integer);if(ImGui::Button("Hold integer")&&!name.empty())Run("anim.int "+Quote(name)+" "+std::to_string(integer)+" "+(view->actor?"oswald":"mickey"));
     if(ImGui::TreeNode("Snapshot values (float and integer interpretations)")){
       for(const auto& v:view->variables)if(Match(v.name,filter))ImGui::TextWrapped("%s = %.6g / %d",v.name.c_str(),double(v.floating),v.integer);
       ImGui::TextWrapped("These are values at the last refresh. The graph does not expose type metadata here; the two columns interpret the same 32 bits.");ImGui::TreePop();}
     if(ImGui::Button("Release selected override")&&!name.empty())Run("anim.release "+Quote(name)+" "+(view->actor?"oswald":"mickey"));ImGui::SameLine();if(ImGui::Button("Release all overrides"))Run(std::string("anim.release all ")+(view->actor?"oswald":"mickey"));
     ImGui::InputText("Reflected float property",property,sizeof(property));ImGui::InputFloat("Property value",&property_value);if(ImGui::Button("Read property")&&*property)Run("entity.getfloat "+Quote(property));ImGui::SameLine();if(ImGui::Button("Write property")&&*property)Run("entity.setfloat "+Quote(property)+" "+std::to_string(property_value));
   }else ImGui::TextWrapped("Refresh the loaded graph during gameplay to browse actual events and variable values.");
 }
 ImGui::EndChild();
}
bool em2::TestConsoleTools(){
 if(!TestGameplayConsole())return false;
 for(const auto& command:catalog){
   const auto* form=FormFor(command.name);if(!form || Labels(*form).size()>5){REXLOG_ERROR("Missing console form: {}",command.name);return false;}
   const auto example=TokenizeEngineConsole(command.example);if(!example || example->empty() || (*example)[0]!=command.name){REXLOG_ERROR("Invalid console form example: {}",command.name);return false;}
   std::vector<std::string> args(example->begin()+1,example->end());const auto built=Build(*form,args);
   if(!built || TokenizeEngineConsole(*built)!=example){REXLOG_ERROR("Console form argument roundtrip failed: {}",command.name);return false;}
 }
 if(Build(*FormFor("combat"),{"spin","5"}) || Build(*FormFor("graphics"),{"","on"}) ||
    Build(*FormFor("echo"),{"line\nbreak"}) || !Build(*FormFor("anim.events"),{"","oswald"}))return false;
 const auto quoted=Build(*FormFor("entity.setfloat"),{"A property with \"quotes\" and \\slashes","12","oswald"});
 if(!quoted || TokenizeEngineConsole(*quoted)!=std::optional<std::vector<std::string>>({"entity.setfloat","A property with \"quotes\" and \\slashes","12","oswald"}))return false;
 const auto choices=FormChoices("graphics",{"hdr",""},1);if(std::find(choices.begin(),choices.end(),"On (Windows HDR required)")==choices.end())return false;
 const auto search=ExecuteConsoleTool({"engine","player_shadow"});if(!search || search->find("[Graphics / saved]")==std::string::npos)return false;
 const auto* shadow=Setting("player_shadow");const auto* hdr=Setting("HDR");const auto* resolution=Setting("Display resolution");
 const auto* render=Setting("Internal render resolution");if(!render || render!=Setting("3D render resolution") || ChoiceIndex(*render,"720p (1x)")!=0)return false;
 if(!shadow||!hdr||!resolution||ChoiceIndex(*shadow,"1024")!=3||ChoiceIndex(*hdr,"on")!=1||ChoiceIndex(*resolution,"1920x1080")!=2||ChoiceIndex(*resolution,"nonsense")!=-1)return false;
 if(Field("dash cooldown")!=Field("dash_cooldown")||Number("nan")||Number("1oops"))return false;
 for(const auto& c:catalog){if(!*c.description||!*c.example)return false;for(const auto& other:catalog)if(&c!=&other&&std::string_view(c.name)==other.name)return false;}
 for(const auto& row:kEngineSettings)for(int i=0;i<PCSettingChoiceCount(row.index);++i)if(ChoiceIndex(row,Choice(row,i))!=i)return false;
 for(const auto name:NativeEngineCommandNames())if(std::none_of(std::begin(catalog),std::end(catalog),[&](const auto& c){return c.name==name;}))return false;
 if(!ExecuteConsoleTool({"movement","dash_speed","nan"})||!ExecuteConsoleTool({"movement","sprint","banana"})||ExecuteConsoleTool({"unrelated"}))return false;
 const auto saved=NativeConsoleTarget();ExecuteConsoleTool({"target","oswald"});bool ok=NativeConsoleTarget()==1;ExecuteConsoleTool({"target","selected"});ok&=NativeConsoleTarget()==-1;SetNativeConsoleTarget(saved);
 if(ok)REXLOG_INFO("Console forms passed: {} command examples, quote/escape roundtrips, required/positional argument guards, named graphics choices and cross-system current-value discovery",std::size(catalog));
 return ok;
}
