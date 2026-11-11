#include "online_runtime.h"
#include "em1_level_entry.h"
#include <immintrin.h>
#include "engine_workbench.h"
#include "debug_level_browser.h"
#include "console_tools.h"
#include "game_console.h"
#include "native_engine.h"
#include "engine_settings_catalog.h"
#include "pc_settings.h"
#include "controls.h"
#include "players.h"
#include "simulation.h"
#include "gameplay_mods.h"
#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/input/input_system.h>
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <vector>
namespace {
enum class Panel {Closed,Settings,Console};
std::atomic<Panel> requested{Panel::Closed};
std::atomic<int> requested_page{-1};
std::atomic<rex::input::InputSystem*> input_system{nullptr};
std::atomic<bool> pad_armed{false};
std::atomic<bool> window_focused{true};
bool f1_held=false,console_held=false;
std::mutex log_mutex;
std::deque<std::string> log_lines;
std::vector<std::string> history;
std::filesystem::path directory;
int page=0,profile=0,history_index=-1;
char search[160]{},command[2048]{},variable_value[1024]{};
std::string selected_variable;
bool scroll_log=false;
std::atomic<bool> focus_command{false};
void Log(std::string line){
    if(line.size()>8192)line.resize(8192);
    std::lock_guard lock(log_mutex);log_lines.push_back(std::move(line));while(log_lines.size()>400)log_lines.pop_front();scroll_log=true;
}
std::optional<std::vector<std::string>> Tokens(std::string_view text){
    if(text.size()>8192)return {};
    std::vector<std::string> result;std::string token;bool quoted=false,started=false;
    for(size_t i=0;i<text.size();++i){const auto c=text[i];
        if(c=='"'){quoted=!quoted;started=true;continue;}
        if(quoted && c=='\\' && i+1<text.size() && (text[i+1]=='"' || text[i+1]=='\\')){token+=text[++i];started=true;continue;}
        if(!quoted && (c==' ' || c=='\t')){if(started){result.push_back(std::move(token));token={};started=false;}continue;}
        if(static_cast<unsigned char>(c)<32)return {};
        token+=c;started=true;
    }
    if(quoted)return {};if(started)result.push_back(std::move(token));return result;
}
std::optional<int> Integer(std::string_view text){int value=0;const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);if(error!=std::errc{} || end!=text.data()+text.size())return {};return value;}
std::optional<float> Scalar(std::string_view text){float value=0;const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);if(error!=std::errc{} || end!=text.data()+text.size() || !std::isfinite(value))return {};return value;}
std::string Quoted(std::string_view text){std::string result="\"";for(char c:text){if(c=='\\' || c=='"')result+='\\';result+=c;}return result+'"';}
std::string Lower(std::string_view text){std::string result(text);for(auto& c:result)if(c>='A' && c<='Z')c=char(c-'A'+'a');return result;}
bool Matches(std::string_view text,std::string_view query){return query.empty() || Lower(text).find(Lower(query))!=std::string::npos;}
int SettingIndex(std::string_view name){
    if(auto index=Integer(name);index && *index>=0 && *index<em2::kPCOptionCount)return *index;
    for(int i=0;i<em2::kPCOptionCount;++i)if(name==em2::PCSettingName(i))return i;return -1;
}
void SetPanel(Panel panel){requested.store(panel);em2::SetExternalControlCapture(panel!=Panel::Closed);focus_command=panel==Panel::Console;pad_armed=false;}
void PadNavigation(ImGuiIO& io){
    auto* input=input_system.load();rex::input::X_INPUT_STATE state{};
    if(!window_focused.load() || (input && input->GetState(0,&state)!=0))state={};
    int capture=0;em2::ControlOption(2200,-1,capture);
    if(capture){state={};pad_armed=false;}
    if(!capture && !pad_armed){
        if(uint16_t(state.gamepad.buttons) || std::abs(int16_t(state.gamepad.thumb_lx))>8000 || std::abs(int16_t(state.gamepad.thumb_ly))>8000)state={};
        else pad_armed=true;
    }
    const auto buttons=uint16_t(state.gamepad.buttons);
    io.ConfigFlags|=ImGuiConfigFlags_NavEnableGamepad;io.BackendFlags|=ImGuiBackendFlags_HasGamepad;
    const std::pair<ImGuiKey,uint16_t> keys[]={{ImGuiKey_GamepadFaceDown,0x1000},{ImGuiKey_GamepadFaceRight,0x2000},{ImGuiKey_GamepadFaceLeft,0x4000},{ImGuiKey_GamepadFaceUp,0x8000},{ImGuiKey_GamepadL1,0x100},{ImGuiKey_GamepadR1,0x200},{ImGuiKey_GamepadDpadUp,1},{ImGuiKey_GamepadDpadDown,2},{ImGuiKey_GamepadDpadLeft,4},{ImGuiKey_GamepadDpadRight,8},{ImGuiKey_GamepadStart,0x10},{ImGuiKey_GamepadBack,0x20}};
    for(const auto& [key,mask]:keys)io.AddKeyEvent(key,(buttons&mask)!=0);
    const auto x=int16_t(state.gamepad.thumb_lx),y=int16_t(state.gamepad.thumb_ly);
    auto axis=[&](ImGuiKey key,int value){const float strength=std::clamp((value-8000.f)/24767.f,0.f,1.f);io.AddKeyAnalogEvent(key,strength>0,strength);};
    axis(ImGuiKey_GamepadLStickLeft,-int(x));axis(ImGuiKey_GamepadLStickRight,x);axis(ImGuiKey_GamepadLStickUp,y);axis(ImGuiKey_GamepadLStickDown,-int(y));
}
std::string Choice(const em2::EngineSettingRow& row,int index){
    std::string_view list=row.choices;
    for(int i=0;i<index;++i){const auto split=list.find('|');if(split==list.npos)return "Unknown";list.remove_prefix(split+1);}
    return std::string(list.substr(0,list.find('|')));
}
constexpr const char* actions[]={"Jump / interact","Paint / interact","Spin attack","Thinner / interact","Camera reset","Co-op action","Thinner / electricity","Paint / electricity","Left stick click","Guardian","Pause / join","Toggle cursor","D-pad up","D-pad down","D-pad left","D-pad right","Move left","Move right","Move forward","Move backward","Camera left","Camera right","Camera up","Camera down","Switch character","Sprint (hold)","Dash","Ground pound"};
static_assert(std::size(actions)==em2::kBindingCount);
constexpr const char* pad_names[]={"A","B","X","Y","LB","RB","LT","RT","LS click","RS click","Start","Back","D-pad up","D-pad down","D-pad left","D-pad right","LS left","LS right","LS up","LS down","RS left","RS right","RS up","RS down","LB + RB"};
std::string BindingName(int code){
    if(!code)return "Unbound";
    if(code>=256 && code<281)return pad_names[code-256];
    if(code>=512 && code<=515){constexpr const char* names[]={"Wheel up","Wheel down","Wheel left","Wheel right"};return names[code-512];}
    switch(code){case VK_LBUTTON:return "Mouse left";case VK_RBUTTON:return "Mouse right";case VK_MBUTTON:return "Mouse middle";case VK_XBUTTON1:return "Mouse X1";case VK_XBUTTON2:return "Mouse X2";}
    char name[128]{};auto scan=MapVirtualKeyA(unsigned(code),MAPVK_VK_TO_VSC)<<16;
    if(code>=VK_PRIOR && code<=VK_DOWN)scan|=1u<<24;
    if(GetKeyNameTextA(LONG(scan),name,sizeof(name)))return name;
    return "Key "+std::to_string(code);
}
void DrawBindings(){
    const char* profiles[]={"Player 1 / keyboard and mouse","Player 1 / controller","Player 2 / keyboard and mouse","Player 2 / controller"};
    ImGui::SetNextItemWidth(-1);ImGui::Combo("##profile",&profile,profiles,4);
    ImGui::TextWrapped("Select an action, then press a key or controller input. Escape cancels. Controller capture starts after the pad is released. F1 and ` open settings and console.");
    int capture=0;em2::ControlOption(2200,-1,capture);
    for(int index=0;index<em2::kBindingCount;++index){if(!Matches(actions[index],search))continue;ImGui::PushID(index);int value=0;const int id=2000+profile*32+index;em2::ControlOption(id,-1,value);
        ImGui::AlignTextToFramePadding();ImGui::TextUnformatted(actions[index]);ImGui::SameLine(ImGui::GetWindowWidth()*.51f);
        const bool listening=capture==profile*32+index+1;const auto label=listening?std::string("Waiting for input..."):BindingName(value);
        if(ImGui::Button(label.c_str(),ImVec2(-56,0)))em2::ControlOption(id,-2,value);
        ImGui::SameLine();if(ImGui::SmallButton("Clear"))em2::ControlOption(id,-3,value);ImGui::PopID();
    }
    if(ImGui::Button("Restore this profile")){int unused=0;em2::ControlOption(2000+profile*32,-4,unused);}
    ImGui::SeparatorText("Camera input");
    constexpr const char* look[]={"Horizontal sensitivity","Vertical sensitivity","Invert horizontal","Invert vertical","Horizontal deadzone","Vertical deadzone","Horizontal curve","Vertical curve"};
    constexpr int counts[]={8,8,2,2,8,8,3,3};
    constexpr const char* strengths[]={"0.5x","0.6x","0.75x","0.85x","1x (default)","1.25x","1.5x","2x"};
    constexpr const char* zones[]={"Original","0%","5%","10%","15%","20%","25%","30%"};
    constexpr const char* curves[]={"Original","Precise (squared)","Fast (square root)"};
    for(int i=0;i<8;++i){int value=0;em2::ControlOption(2300+profile*8+i,-1,value);ImGui::PushID(40+i);
        if(i==2 || i==3){bool enabled=value!=0;if(ImGui::Checkbox(look[i],&enabled))em2::ControlOption(2300+profile*8+i,enabled?1:0,value);}
        else {const auto* labels=i<2?strengths:i<6?zones:curves;ImGui::SetNextItemWidth(-1);ImGui::TextUnformatted(look[i]);if(ImGui::BeginCombo("##camera-value",labels[value])){for(int choice=0;choice<counts[i];++choice)if(ImGui::Selectable(labels[choice],value==choice))em2::ControlOption(2300+profile*8+i,choice,value);ImGui::EndCombo();}}
        ImGui::PopID();
    }
}
void DrawVariables(){
    ImGui::TextWrapped("Registered engine variables. Select a variable to inspect its description and change its live value.");
    ImGui::BeginChild("variable-list",ImVec2(0,240),ImGuiChildFlags_Borders);
    for(const auto& name:rex::cvar::ListFlags()){const auto* info=rex::cvar::GetFlagInfo(name);if(!info || (!Matches(name,search) && !Matches(info->description,search)))continue;
        if(ImGui::Selectable(name.c_str(),selected_variable==name)){selected_variable=name;const auto value=rex::cvar::GetFlagByName(name);snprintf(variable_value,sizeof(variable_value),"%s",value.c_str());}}
    ImGui::EndChild();
    if(const auto* info=rex::cvar::GetFlagInfo(selected_variable)){
        ImGui::SeparatorText(selected_variable.c_str());ImGui::TextWrapped("%s",info->description.c_str());ImGui::Text("Default: %s",info->default_value.c_str());
        ImGui::InputText("Value",variable_value,sizeof(variable_value));
        ImGui::BeginDisabled(info->lifecycle!=rex::cvar::Lifecycle::kHotReload);
        if(ImGui::Button("Apply live"))em2::ExecuteEngineConsole("set "+selected_variable+" "+Quoted(variable_value));
        ImGui::SameLine();if(ImGui::Button("Restore variable"))em2::ExecuteEngineConsole("reset "+selected_variable);
        ImGui::EndDisabled();
        if(info->lifecycle!=rex::cvar::Lifecycle::kHotReload)ImGui::TextDisabled("This backend variable is not live-editable.");
    }
}
void DrawMovementMods(){
    ImGui::SeparatorText("Rebrushed-style movement / offline");
    auto config=em2::GetGameplayTuning();bool changed=false;
    changed|=ImGui::Checkbox("Sprint while held",&config.sprint);
    changed|=ImGui::SliderFloat("Sprint multiplier",&config.sprint_multiplier,1,4,"%.2fx");
    changed|=ImGui::Checkbox("Dash",&config.dash);
    changed|=ImGui::Checkbox("Allow one air dash per jump",&config.air_dash);
    changed|=ImGui::SliderFloat("Dash speed",&config.dash_speed,1,40,"%.1f m/s");
    changed|=ImGui::SliderFloat("Dash duration",&config.dash_duration,.05f,.8f,"%.2f s");
    changed|=ImGui::SliderFloat("Dash cooldown",&config.dash_cooldown,0,3,"%.2f s");
    changed|=ImGui::Checkbox("Ground-pound drop",&config.pound);
    changed|=ImGui::SliderFloat("Ground-pound speed",&config.pound_speed,1,50,"%.1f m/s");
    int damage_type=config.pound_damage_type==em2::PoundDamageType::Physical?1:0;
    if(ImGui::Combo("Ground-pound damage type",&damage_type,"Spin attack\0Physical\0")){config.pound_damage_type=damage_type?em2::PoundDamageType::Physical:em2::PoundDamageType::Spin;changed=true;}
    ImGui::TextWrapped("Keyboard defaults: Shift sprint, Ctrl dash, R ground pound while airborne. Rebind moves in Controls & Camera. Mickey uses imported ability clips; Oswald retains native movement animations. Damage uses native combat reactions.");
    if(ImGui::Button("Restore movement defaults")){config=em2::GameplayTuning{};changed=true;}
    if(changed && !em2::ChangeGameplayTuning(config))Log("Could not save gameplay-mods.ini.");
}
std::vector<std::string> ConsoleCompletions(std::string_view text,size_t cursor,int& start){
    cursor=std::min(cursor,text.size());start=0;bool quoted=false;
    for(size_t i=0;i<cursor;++i){if(quoted&&text[i]=='\\'&&i+1<cursor){++i;continue;}if(text[i]=='"')quoted=!quoted;else if(!quoted&&(text[i]==' '||text[i]=='\t'))start=int(i+1);}
    const auto before=Tokens(text.substr(0,size_t(start)));if(!before)return {};
    const auto prefix=text.substr(size_t(start),cursor-size_t(start));std::vector<std::string> choices;
    if(before->empty()){
        for(const auto& info:em2::ConsoleCommandCatalog())choices.emplace_back(info.name);
    }else if(auto tools=em2::ConsoleToolCompletions(*before))choices=std::move(*tools);
    else if(auto native=em2::NativeEngineArgumentCompletions((*before)[0],unsigned(before->size()),prefix))choices=std::move(*native);
    else if(before->size()==1 && ((*before)[0]=="get" || (*before)[0]=="set" || (*before)[0]=="reset" || (*before)[0]=="find"))choices=rex::cvar::ListFlags();
    std::erase_if(choices,[&](const auto& name){std::string_view candidate=name;if(candidate.starts_with('"')&&!prefix.starts_with('"'))candidate.remove_prefix(1);return !Lower(candidate).starts_with(Lower(prefix));});
    std::sort(choices.begin(),choices.end());choices.erase(std::unique(choices.begin(),choices.end()),choices.end());return choices;
}
int ConsoleInput(ImGuiInputTextCallbackData* data){
    if(data->EventFlag==ImGuiInputTextFlags_CallbackCharFilter && (data->EventChar=='`' || data->EventChar=='~'))return 1;
    if(data->EventFlag==ImGuiInputTextFlags_CallbackHistory){
        if(history.empty())return 0;
        if(data->EventKey==ImGuiKey_UpArrow)history_index=history_index<0?int(history.size())-1:std::max(0,history_index-1);
        else if(history_index>=0 && ++history_index>=int(history.size()))history_index=-1;
        data->DeleteChars(0,data->BufTextLen);if(history_index>=0)data->InsertChars(0,history[history_index].c_str());
    }
    if(data->EventFlag==ImGuiInputTextFlags_CallbackCompletion){
        int start;const auto choices=ConsoleCompletions(std::string_view(data->Buf,size_t(data->BufTextLen)),size_t(data->CursorPos),start);
        if(!choices.empty()){
            size_t common=0;while(common<choices.front().size() && common<choices.back().size() && choices.front()[common]==choices.back()[common])++common;
            if(choices.size()==1 || common>size_t(data->CursorPos-start)){
                int end=start;bool quoted=false;for(;end<data->BufTextLen;++end){const char c=data->Buf[end];if(quoted&&c=='\\'&&end+1<data->BufTextLen){++end;continue;}if(c=='"')quoted=!quoted;else if(!quoted&&(c==' '||c=='\t'))break;}
                const auto replacement=choices.front().substr(0,common);data->DeleteChars(start,end-start);data->InsertChars(start,replacement.c_str());
                if(choices.size()==1 && (data->CursorPos==data->BufTextLen || data->Buf[data->CursorPos]!=' '))data->InsertChars(data->CursorPos," ");
            }
            if(choices.size()>1){Log("Completions:");for(size_t i=0;i<std::min<size_t>(24,choices.size());++i)Log("  "+choices[i]);if(choices.size()>24)Log(std::to_string(choices.size()-24)+" more; type more characters to narrow the list.");}
        }
    }
    return 0;
}
void DrawConsole(){
    ImGui::TextDisabled("Enter executes  |  Tab completes  |  Up / Down history  |  ` closes");
    ImGui::BeginChild("console-content",ImVec2(0,-96));
    if(ImGui::BeginTabBar("console-tabs")){
        if(ImGui::BeginTabItem("Levels")){em2::DrawDebugLevelBrowser();ImGui::EndTabItem();}
        if(ImGui::BeginTabItem("Gameplay")){em2::DrawGameplayConsole();ImGui::EndTabItem();}
        if(ImGui::BeginTabItem("Tools")){em2::DrawConsoleTools();ImGui::EndTabItem();}
        if(ImGui::BeginTabItem("Output")){
            ImGui::BeginChild("console-output",ImVec2(0,0),ImGuiChildFlags_Borders);
            {std::lock_guard lock(log_mutex);for(const auto& line:log_lines)ImGui::TextUnformatted(line.c_str());if(scroll_log){ImGui::SetScrollHereY(1);scroll_log=false;}}
            ImGui::EndChild();ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Commands")){em2::DrawConsoleBrowser();ImGui::EndTabItem();}
        if(ImGui::BeginTabItem("Engine variables")){
            ImGui::BeginChild("console-variables",ImVec2(0,0));ImGui::InputTextWithHint("##variable-search","Search engine variables and descriptions",search,sizeof(search));DrawVariables();ImGui::EndChild();ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    if(const auto tokens=Tokens(command);tokens && !tokens->empty())for(const auto& info:em2::ConsoleCommandCatalog())if((*tokens)[0]==info.name){ImGui::TextWrapped("%s",info.usage);break;}
    {std::string latest;{std::lock_guard lock(log_mutex);if(!log_lines.empty())latest=log_lines.back();}
        const auto first=latest.substr(0,latest.find('\n'));ImGui::TextDisabled("%.*s",int(std::min<size_t>(150,first.size())),first.c_str());if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",latest.c_str());}
    if(focus_command.exchange(false))ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    if(ImGui::InputText("##command",command,sizeof(command),ImGuiInputTextFlags_EnterReturnsTrue|ImGuiInputTextFlags_CallbackCompletion|ImGuiInputTextFlags_CallbackHistory|ImGuiInputTextFlags_CallbackCharFilter,ConsoleInput)){
        if(*command){history.emplace_back(command);if(history.size()>100)history.erase(history.begin());history_index=-1;em2::ExecuteEngineConsole(command);command[0]=0;}focus_command=true;
    }
}
}
void em2::InitializeEngineWorkbench(const std::filesystem::path& path){directory=path;Log("EM2 Engine Console. Browse Commands for searchable help, or Tools for direct controls. Type help for a quick start. ` closes; game settings remain in Options.");}
void em2::DraftEngineConsoleCommand(std::string_view text){snprintf(command,sizeof(command),"%.*s",int(std::min(text.size(),sizeof(command)-1)),text.data());focus_command=true;}
std::optional<std::vector<std::string>> em2::TokenizeEngineConsole(std::string_view text){return Tokens(text);}
void em2::SetWorkbenchInputSystem(rex::input::InputSystem* input){input_system.store(input);}
void em2::ShowEngineSettings(){Log("Open Options > Settings & Mods, or Options > Controls & Camera. These pages use the native game UI.");}
void em2::ShowEngineConsole(){SetPanel(Panel::Console);}
void em2::EngineConsoleMessage(const std::string& message){
    if(em1::RegisteredLevelEntryProbeRequested()){
        const auto error=GetLastError();const auto csr=_mm_getcsr();
        REXLOG_INFO("[em1] full-entry console: {}",message);
        SetLastError(error);_mm_setcsr(csr);
    }
    Log(message);
}
bool em2::EngineWorkbenchKey(unsigned key,bool down){
    auto* held=key==VK_OEM_3?&console_held:nullptr;
    if(held){const bool pressed=down && !*held;*held=down;int capture=0;ControlOption(2200,-1,capture);if(pressed && !capture){SetPanel(requested.load()==Panel::Console?Panel::Closed:Panel::Console);}return true;}
    if(down && key==VK_ESCAPE && requested.load()!=Panel::Closed){int capture=0;ControlOption(2200,-1,capture);if(!capture){SetPanel(Panel::Closed);return true;}}
    return false;
}
void em2::ResetEngineWorkbenchInput(){f1_held=console_held=false;pad_armed=false;}
void em2::SetEngineWorkbenchFocused(bool focused){window_focused.store(focused);ResetEngineWorkbenchInput();}
bool em2::EngineWorkbenchOption(int id,int command,int& result){if(id!=2500)return false;if(command==1)ShowEngineSettings();else if(command==2)ShowEngineConsole();else if(command==3){requested_page=5;ShowEngineSettings();}else if(command==0)SetPanel(Panel::Closed);result=int(requested.load());return true;}
void em2::ExecuteEngineConsole(std::string_view line){
    // Lua takes the unmodified remainder: normal tokenization would remove
    // Lua's own string quotes. A single outer quoted argument is accepted for
    // the generated command forms, with the regular escaping rules.
    if(const auto source=GameLuaConsoleSource(line)){Log("> lua "+*source);Log(QueueGameLua(*source));return;}
    const auto parsed=Tokens(line);if(!parsed){Log("Error: malformed quotes, control character, or command too long.");return;}const auto& args=*parsed;if(args.empty())return;Log("> "+std::string(line));
    try{
        const auto& cmd=args[0];
        if(cmd=="level.load" && args.size()==2 && Lower(args[1]).starts_with("em1:")){Log(QueueDebugLevelLoad(args[1]));return;}
        if(auto response=ExecuteConsoleTool(args)){Log(*response);return;}
        if(auto response=QueueNativeEngineCommand(args)){Log(*response);return;}
        if(cmd=="lua.file"){
            if(args.size()!=2){Log("Usage: lua.file <filename.lua>. Relative paths use "+(directory/"scripts").string());return;}
            const auto requested_file=std::filesystem::path(std::u8string(args[1].begin(),args[1].end()));
            const auto file=requested_file.is_absolute()?requested_file:directory/"scripts"/requested_file;
            std::ifstream in(file,std::ios::binary);if(!in){Log("Cannot open Lua file: "+file.string());return;}
            std::array<char,16388> source{};in.read(source.data(),source.size());
            if(in.bad()){Log("Failed reading Lua file: "+file.string());return;}
            std::string_view text(source.data(),size_t(in.gcount()));
            if(text.starts_with("\xEF\xBB\xBF"))text.remove_prefix(3);
            Log("Lua file: "+file.string());Log(QueueGameLua(text));return;
        }
        if(cmd=="clear"){std::lock_guard lock(log_mutex);log_lines.clear();return;}
        if(cmd=="echo"){std::string text;for(size_t i=1;i<args.size();++i){if(i>1)text+=' ';text+=args[i];}Log(text);return;}
        if(cmd=="find"){unsigned count=0;for(const auto& name:rex::cvar::ListFlags()){const auto* info=rex::cvar::GetFlagInfo(name);if(args.size()<2 || Matches(name,args[1]) || (info && Matches(info->description,args[1]))){Log(name+" = "+rex::cvar::GetFlagByName(name));++count;}}Log(std::to_string(count)+" variables");return;}
        if(cmd=="get" && args.size()==2){if(!rex::cvar::GetFlagInfo(args[1])){Log("Unknown variable: "+args[1]);return;}Log(args[1]+" = "+rex::cvar::GetFlagByName(args[1]));return;}
        if((cmd=="set" && args.size()==3) || (cmd=="reset" && args.size()==2)){
            const auto* info=rex::cvar::GetFlagInfo(args[1]);if(!info){Log("Unknown variable: "+args[1]);return;}
            if(info->lifecycle!=rex::cvar::Lifecycle::kHotReload){Log("Variable cannot be changed live: "+args[1]);return;}
            const auto value=cmd=="reset"?info->default_value:args[2];if(!rex::cvar::SetFlagByName(args[1],value)){Log("Rejected value for "+args[1]);return;}Log(args[1]+" = "+rex::cvar::GetFlagByName(args[1]));return;
        }
        if(cmd=="settings"){for(int i=0;i<kPCOptionCount;++i)Log(std::to_string(i)+" "+PCSettingName(i)+" = "+std::to_string(PCSetting(i)));return;}
        if(cmd=="setting" && (args.size()==2 || args.size()==3)){
            const auto index=SettingIndex(args[1]);if(index<0){Log("Unknown setting: "+args[1]);return;}if(args.size()==3){auto value=Integer(args[2]);if(!value || ChangePCSetting(index,*value)<0){Log("Choice must be 0.."+std::to_string(PCSettingChoiceCount(index)-1));return;}}Log(std::string(PCSettingName(index))+" = "+std::to_string(PCSetting(index)));return;
        }
        if(cmd=="fps" && args.size()==2){constexpr int caps[]={30,40,50,60,90,120,144,165,240,0};const auto cap=args[1]=="uncapped"?std::optional<int>(0):Integer(args[1]);if(cap)for(int i=0;i<10;++i)if(caps[i]==*cap){ChangePCSetting(2,i);Log("Frame limit requested: "+args[1]);return;}Log("Use 30,40,50,60,90,120,144,165,240 or uncapped.");return;}
        if(cmd=="player.switch" && args.size()==1){RequestCharacterSwap();Log("Character switch requested.");return;}
        if(cmd=="player.speed" && args.size()==2){const auto speed=args[1]=="reset"?std::optional<float>(-1):Scalar(args[1]);if(!speed || !SetPlayerSpeedOverride(*speed)){Log("Use an offline speed of 0..500 or reset.");return;}Log("Native player speed override: "+args[1]);return;}
        if(cmd=="player.push" && (args.size()==4 || args.size()==5)){const auto x=Scalar(args[1]),y=Scalar(args[2]),z=Scalar(args[3]),decay=args.size()==5?Scalar(args[4]):std::optional<float>(.9f);if(!x || !y || !z || !decay || !QueuePlayerImpulse(*x,*y,*z,*decay)){Log("Use offline velocity components -500..500 and decay 0..1; at most 32 requests can wait.");return;}Log("Queued native external velocity. Y is vertical; decay applies each simulation tick.");return;}
        if((cmd=="timescale" && args.size()==2) || ((cmd=="pause" || cmd=="resume") && args.size()==1)){
            const auto scale=cmd=="timescale"?Scalar(args[1]):std::optional<float>(cmd=="pause"?0.f:1.f);
            const auto value=scale && *scale>=0 && *scale<=8?std::optional<int>(int(std::lround(*scale*100))):std::optional<int>{};
            if(!value || !SetSimulationSpeedPercent(*value)){Log("Use an offline time multiplier between 0 and 8.");return;}Log("Simulation speed: "+std::to_string(*value)+"%");return;
        }
        if(cmd=="step" && args.size()<=2){const auto count=args.size()==2?Integer(args[1]):std::optional<int>(1);if(!count || *count<1 || !RequestSimulationSteps(unsigned(*count))){Log("Pause offline first; step accepts 1..8 queued ticks.");return;}Log("Queued "+std::to_string(*count)+" native physics tick(s).");return;}
        if(cmd=="writeconfig" && args.size()==1){
            if(directory.empty()){Log("No user directory.");return;}std::ofstream out(directory/"engine-console.cfg");
            for(const auto& name:rex::cvar::ListModifiedFlags()){const auto* info=rex::cvar::GetFlagInfo(name);if(!info || info->lifecycle!=rex::cvar::Lifecycle::kHotReload)continue;auto value=rex::cvar::GetFlagByName(name);if(value.find_first_of("\r\n")!=value.npos)continue;std::string escaped;for(char c:value){if(c=='\\' || c=='"')escaped+='\\';escaped+=c;}out<<"set "<<name<<" \""<<escaped<<"\"\n";}Log(out.good()?"Wrote engine-console.cfg":"Could not write config.");return;
        }
        if(cmd=="exec" && args.size()==1){static thread_local bool reading=false;if(reading){Log("Nested exec is not supported.");return;}std::ifstream in(directory/"engine-console.cfg");if(!in){Log("No engine-console.cfg found.");return;}reading=true;struct Reset{bool& value;~Reset(){value=false;}}reset{reading};std::string entry;for(unsigned i=0;i<1024 && std::getline(in,entry);++i)if(!entry.empty() && !entry.starts_with('#'))ExecuteEngineConsole(entry);return;}
        for(const auto& info:ConsoleCommandCatalog())if(cmd==info.name){Log(std::string("Usage: ")+info.usage+"\nExample: "+info.example);return;}
        Log("Unknown command '"+cmd+"'. Search the Commands tab or type help <topic>.");
    }catch(const std::exception& e){Log(std::string("Command failed: ")+e.what());}
}
void em2::DrawEngineWorkbench(ImGuiIO& io){
    static bool was_open=false;
    auto mode=requested.load();if(mode==Panel::Closed){if(was_open){for(int key=ImGuiKey_GamepadStart;key<=ImGuiKey_GamepadRStickDown;++key)io.AddKeyEvent(ImGuiKey(key),false);}was_open=false;return;}
    was_open=true;
    if(const auto selected=requested_page.exchange(-1);selected>=0)page=selected;
    PadNavigation(io);
    if(ImGui::IsKeyPressed(ImGuiKey_GamepadStart)){SetPanel(Panel::Closed);return;}
    io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
    const auto width=std::max(220.f,std::min(1180.f,io.DisplaySize.x-32)),height=std::max(150.f,std::min(850.f,io.DisplaySize.y-32));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x*.5f,io.DisplaySize.y*.5f),ImGuiCond_Always,ImVec2(.5f,.5f));ImGui::SetNextWindowSize(ImVec2(width,height),ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,14);ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,6);ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(20,18));
    ImGui::PushStyleColor(ImGuiCol_WindowBg,ImVec4(.035f,.045f,.07f,.985f));ImGui::PushStyleColor(ImGuiCol_Header,ImVec4(.16f,.26f,.38f,1));ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.12f,.19f,.28f,1));ImGui::PushStyleColor(ImGuiCol_CheckMark,ImVec4(.94f,.69f,.25f,1));
    bool open=true;
    if(ImGui::Begin(mode==Panel::Console?"EM2 / ENGINE CONSOLE":"EM2 / INTERNAL WIDGET TEST",&open,ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoResize)){
        ImGui::TextColored(ImVec4(.94f,.72f,.32f,1),"%s",mode==Panel::Console?"ENGINE CONSOLE":"SETTINGS");
        if(width>=850)ImGui::SameLine();ImGui::TextDisabled("  ` console  /  Esc close  /  Game settings: Options");ImGui::Separator();
        if(mode==Panel::Console)DrawConsole();else{
            constexpr const char* pages[]={"Display & performance","Rendering & lighting","HDR calibration","Camera","Gameplay","Controls & bindings","Engine variables","Console"};
            const float sidebar=width<700?140.f:215.f;
            ImGui::BeginChild("navigation",ImVec2(sidebar,0));
            constexpr const char* compact_pages[]={"Display","Rendering","HDR","Camera","Gameplay","Controls","Engine variables","Console"};
            for(int i=0;i<8;++i){if(ImGui::Selectable(width<700?compact_pages[i]:pages[i],page==i,0,ImVec2(0,36))){if(i==7)ShowEngineConsole();else page=i;}}
            ImGui::Spacing();ImGui::TextDisabled("Changes apply live.");ImGui::TextWrapped("Settings save automatically. Console experiments remain live until reset.");ImGui::EndChild();ImGui::SameLine();
            ImGui::BeginChild("settings-body",ImVec2(0,0));ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##search","Search settings, bindings or engine variables",search,sizeof(search));ImGui::SeparatorText(pages[page]);
            if(page==5)DrawBindings();else if(page==6)DrawVariables();else{
                if(page==0)ImGui::TextWrapped("Display resolution sets the window/output size. 3D render resolution controls the game's pixel workload. For upscaling, choose a lower 3D resolution than the display resolution.");
                for(const auto& row:kEngineSettings){if((*search && !Matches(row.title,search) && !Matches(PCSettingName(row.index),search)) || (!*search && row.page!=page))continue;ImGui::PushID(row.index);ImGui::TextUnformatted(row.title);ImGui::SetNextItemWidth(-46);const auto current=Choice(row,PCSetting(row.index));
                    if(ImGui::BeginCombo("##value",current.c_str())){for(int choice=0;choice<PCSettingChoiceCount(row.index);++choice){const auto label=Choice(row,choice);if(ImGui::Selectable(label.c_str(),PCSetting(row.index)==choice))ChangePCSetting(row.index,choice);}ImGui::EndCombo();}ImGui::SameLine();if(ImGui::SmallButton("Reset"))ChangePCSetting(row.index,PCSettingDefault(row.index));ImGui::Spacing();ImGui::PopID();}
                if(page==4){DrawMovementMods();bool swapping=rex::cvar::GetFlagByName("em2_experimental_character_swap")=="true";if(ImGui::Checkbox("Instant character switching",&swapping))rex::cvar::SetFlagByName("em2_experimental_character_swap",swapping?"true":"false");if(ImGui::Button("Switch character now"))RequestCharacterSwap();ImGui::SeparatorText("World simulation / offline");const bool synchronized=net::SynchronizedSessionActive();ImGui::BeginDisabled(synchronized);int speed=SimulationSpeedPercent();if(ImGui::SliderInt("Speed (%)",&speed,0,800))SetSimulationSpeedPercent(speed);if(ImGui::Button("Pause"))SetSimulationSpeedPercent(0);ImGui::SameLine();if(ImGui::Button("Normal speed"))SetSimulationSpeedPercent(100);ImGui::SameLine();ImGui::BeginDisabled(SimulationSpeedPercent()!=0);if(ImGui::Button("Advance one tick"))RequestSimulationSteps(1);ImGui::EndDisabled();ImGui::EndDisabled();if(synchronized)ImGui::TextWrapped("Online sessions use normal simulation speed from loading through co-op. Your offline speed is preserved.");ImGui::TextWrapped("Speed changes the number of native physics ticks, keeping their timestep intact. Console: timescale 0.25, pause, step, resume.");}
            }
            ImGui::EndChild();
        }
    }
    ImGui::End();ImGui::PopStyleColor(4);ImGui::PopStyleVar(3);if(!open)SetPanel(Panel::Closed);
}
bool em2::TestEngineWorkbench(){
    if(!TestDebugLevelCatalog())return false;
    if(!TestConsoleTools())return false;
    const auto tokens=Tokens("set example \"quoted value \\\"test\\\"\"");if(!tokens || tokens->size()!=3 || (*tokens)[2]!="quoted value \"test\"")return false;
    const std::string quoted_value="path\\and \"quotes\"";const auto quoted=Tokens("set x "+Quoted(quoted_value));if(!quoted || quoted->size()!=3 || (*quoted)[2]!=quoted_value)return false;
    ImGuiInputTextCallbackData filter{};filter.EventFlag=ImGuiInputTextFlags_CallbackCharFilter;filter.EventChar='`';if(ConsoleInput(&filter)!=1)return false;filter.EventChar='a';if(ConsoleInput(&filter)!=0)return false;
    if(Tokens("set x \"unterminated") || Tokens(std::string(8193,'x')) || Integer("2abc") || Integer("999999999999999999") || Scalar("nan") || Scalar("inf") || Scalar("1.0oops") || Scalar("0.25")!=std::optional<float>(.25f))return false;
    if(SettingIndex("resolution")!=0 || SettingIndex("-1")!=-1 || SettingIndex(std::to_string(kPCOptionCount))!=-1 || SettingIndex("screen_upscaler")!=kPCScreenUpscaler || SettingIndex("upscale_sharpness")!=kPCUpscaleSharpness)return false;
    for(const auto& row:kEngineSettings){if(row.index<0 || row.index>=kPCOptionCount)return false;int count=1;for(char c:std::string_view(row.choices))count+=c=='|';if(count!=PCSettingChoiceCount(row.index))return false;}
    const auto previous_panel=requested.load();const int previous_page=page;
    SetPanel(Panel::Closed);ResetEngineWorkbenchInput();
    if(EngineWorkbenchKey(VK_F1,true) || requested.load()!=Panel::Closed)return false;
    EngineWorkbenchKey(VK_OEM_3,true);if(requested.load()!=Panel::Console)return false;
    EngineWorkbenchKey(VK_OEM_3,true);if(requested.load()!=Panel::Console)return false;
    ResetEngineWorkbenchInput();EngineWorkbenchKey(VK_OEM_3,true);if(requested.load()!=Panel::Closed)return false;
    ResetEngineWorkbenchInput();
    auto* previous_context=ImGui::GetCurrentContext();auto* context=ImGui::CreateContext();
    auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;io.DeltaTime=1.f/60;io.Fonts->Build();
    auto complete=[](const char* text,int cursor=-1){
        std::array<char,256> buffer{};const auto length=std::strlen(text);std::memcpy(buffer.data(),text,length+1);
        ImGuiInputTextCallbackData data{};data.Ctx=ImGui::GetCurrentContext();data.EventFlag=ImGuiInputTextFlags_CallbackCompletion;data.Buf=buffer.data();data.BufSize=int(buffer.size());data.BufTextLen=int(length);
        data.CursorPos=data.SelectionStart=data.SelectionEnd=cursor<0?data.BufTextLen:cursor;ConsoleInput(&data);return std::string(buffer.data());
    };
    int completion_start=0;const std::string numeric="anim.float VAR_Forward_Speed 1";
    bool drawn=complete("anim.o")=="anim.overrides " && complete("anim.e")=="anim.event" &&
        complete("  anim.o")=="  anim.overrides " && complete("anim.o oswald",6)=="anim.overrides oswald" &&
        complete("anim.overrides oswald",6)=="anim.overrides oswald" &&
        ConsoleCompletions(numeric,numeric.size(),completion_start).empty() &&
        complete("graphics hdr on")=="graphics hdr \"On (Windows HDR required)\" " &&
        complete("graphics bloom \"Smooth s")=="graphics bloom \"Smooth sampling\" " &&
        complete("movement controller_sprint_t")=="movement controller_sprint_toggle " &&
        complete("lua CleanLoadL")=="lua CleanLoadLevel( " && complete("level.load meanstreet_north.visit_1")=="level.load meanstreet_north.visit_1 ";
    for(const auto size:{ImVec2(640,480),ImVec2(1280,720),ImVec2(3840,2160)})for(int selected=0;selected<8;++selected){
        io.DisplaySize=size;page=selected==7?0:selected;SetPanel(selected==7?Panel::Console:Panel::Settings);
        ImGui::NewFrame();DrawEngineWorkbench(io);ImGui::Render();const auto* draw=ImGui::GetDrawData();drawn&=draw && draw->TotalVtxCount>0;
    }
    for(const auto size:{ImVec2(640,480),ImVec2(1280,720),ImVec2(3840,2160)})for(bool tools:{false,true}){
        io.DisplaySize=size;ImGui::NewFrame();ImGui::SetNextWindowSize(ImVec2(size.x-40,size.y-40));ImGui::Begin("Console browser fixture");
        if(tools)DrawConsoleTools();else DrawConsoleBrowser();ImGui::End();ImGui::Render();drawn&=ImGui::GetDrawData()->TotalVtxCount>0;
    }
    SetPanel(Panel::Closed);ImGui::NewFrame();DrawEngineWorkbench(io);ImGui::Render();
    ImGui::DestroyContext(context);ImGui::SetCurrentContext(previous_context);page=previous_page;SetPanel(previous_panel);
    if(!drawn)return false;
    return true;
}
