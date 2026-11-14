#include "debug_level_browser.h"
#include "em1_level_entry.h"
#include "em1_vfs.h"
#include "game_console_catalog.h"
#include "game_console.h"
#include "engine_workbench.h"
#include "native_engine.h"
#include "online_runtime.h"
#include "ppc_context.h"
#include <windows.h>
#include <immintrin.h>
#include <rex/cvar.h>
#include <imgui.h>
#include <algorithm>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>

REXCVAR_DECLARE(bool,em1_probe_level_entry);
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DEFINE_STRING(em2_qa_debug_level,"","Debug","Private one-shot request through the debug level browser after offline Continue; no gameplay input");
namespace em2 { namespace {
struct Choice {std::string name,destination,search;bool em1=false,zone_only=false;};
std::string Lower(std::string_view text){std::string result(text);for(auto& c:result)if(c>='A' && c<='Z')c+='a'-'A';return result;}
bool Identifier(std::string_view text){return !text.empty() && text.size()<=120 && text.find("..") == text.npos && text.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789.")==text.npos;}
std::vector<Choice> Catalog(std::span<const std::string> originals){
  std::vector<Choice> result;std::set<std::string> seen;
  for(const auto name:kNativeLevelStates)if(seen.insert(std::string(name)).second)
    result.push_back({std::string(name),std::string(name),Lower(name),false,
      std::find(std::begin(kNativeZoneOnlyLevelStates),std::end(kNativeZoneOnlyLevelStates),name)!=std::end(kNativeZoneOnlyLevelStates)});
  for(const auto& name:originals){const auto key=Lower(name);
    if(!Identifier(key) || !key.ends_with(".level") || !seen.insert("em1:"+key).second)continue;
    result.push_back({name,"em1:"+key,key,true});
  }
  return result;
}
std::mutex request_mutex;
std::optional<std::string> pending;
bool preparing=false;
std::string last_status;
void Status(std::string text){std::lock_guard lock(request_mutex);last_status=std::move(text);}
}
std::string QueueDebugLevelLoad(std::string_view destination){
  if(net::SynchronizedSessionActive())return "Leave the online session before using debug level loading.";
  const auto name=Lower(destination);const bool original=name.starts_with("em1:");
  if(original){
    const auto descriptor=name.substr(4);auto catalog=em1::CurrentLevelCatalog();
    if(!Identifier(descriptor) || !descriptor.ends_with(".level") || !catalog ||
        std::none_of(catalog->descriptors.begin(),catalog->descriptors.end(),[&](const auto& n){return Lower(n)==descriptor;}))return "Choose a level from the mounted Epic Mickey 1 disc.";
  }else if(std::find(std::begin(kNativeLevelStates),std::end(kNativeLevelStates),name)==std::end(kNativeLevelStates))return "Choose a level from the Epic Mickey 2 catalog.";
  std::lock_guard lock(request_mutex);
  if(pending || preparing)return "A debug level load is already being prepared.";
  pending=name;last_status="Preparing "+name+"...";return last_status;
}
void TickDebugLevelLoads(PPCContext& ctx,unsigned char* base){
  static bool qa_requested=false;
  if(!qa_requested && REXCVAR_GET(em2_private_profile) && NativeConsoleWorldReady()){
    const auto requested=REXCVAR_GET(em2_qa_debug_level);if(!requested.empty()){qa_requested=true;EngineConsoleMessage("Private debug level browser: "+QueueDebugLevelLoad(requested));}
  }
  std::string destination;
  {std::lock_guard lock(request_mutex);if(!pending || preparing)return;destination=std::move(*pending);pending.reset();preparing=true;}
  struct Done {~Done(){std::lock_guard lock(request_mutex);preparing=false;}} done;
  struct Preserve {PPCContext& ctx;PPCContext saved;DWORD error=GetLastError();unsigned csr=_mm_getcsr();
    Preserve(PPCContext& c):ctx(c),saved(c){}~Preserve(){ctx=saved;SetLastError(error);_mm_setcsr(csr);}} preserve(ctx);
  try {
    if(net::SynchronizedSessionActive()){Status("Level loading cancelled: an online session started.");return;}
    std::string state=destination;
    if(destination.starts_with("em1:")){
      const auto descriptor=destination.substr(4);std::string error;
      if(!em1::PrepareDeferredRegisteredLevel(ctx,base,descriptor,error)){
        Status("Could not prepare "+descriptor+": "+error);EngineConsoleMessage("Debug level load: "+error);return;
      }
      // This is an explicit debug request to exercise original levels while
      // compatibility is being developed. It does not mark assets playable.
      REXCVAR_SET(em1_probe_level_entry,true);
      if(!em1::CanEnterRegisteredLevel(base,descriptor,error)){Status(error);EngineConsoleMessage(error);return;}
      state=em1::RegisteredLevelState(descriptor);
    }
    if(!Identifier(state))throw std::runtime_error("Invalid native level state");
    // CleanLoadLevel is also available in the title VM at the main menu.
    // Preparation/admission above runs on its initialized engine thread.
    const auto response=destination.starts_with("em1:")?QueueGameLua("CleanLoadLevel(\""+state+"\")",false):QueueRetailLevelLoad(state,false);
    Status("Requested "+destination+". "+response);EngineConsoleMessage("Debug level load: "+destination+"; "+response);
  }catch(const std::exception& e){Status(std::string("Level loading failed: ")+e.what());EngineConsoleMessage(e.what());}
}
void DrawDebugLevelBrowser(){
  static char search[160]{};static int game=0;static std::string selected;
  static std::shared_ptr<const em1::LevelCatalog> previous;
  static std::vector<Choice> choices;
  const auto original=em1::CurrentLevelCatalog();
  if(choices.empty() || previous!=original){choices=Catalog(original?std::span(original->descriptors):std::span<const std::string>{});previous=original;}
  ImGui::TextUnformatted("Choose a level");
  ImGui::SetNextItemWidth(150);ImGui::Combo("Game",&game,"Both games\0Epic Mickey 1\0Epic Mickey 2\0");
  ImGui::SameLine();ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##level-search","Search levels",search,sizeof(search));
  const auto query=Lower(search);std::vector<size_t> shown;
  for(size_t i=0;i<choices.size();++i){const auto& c=choices[i];if((game==1 && !c.em1)||(game==2 && c.em1)||c.search.find(query)==c.search.npos)continue;shown.push_back(i);}
  ImGui::TextDisabled("%zu entries%s",shown.size(),original?"":" - mount an EM1 disc in setup to include its levels");
  bool load=false;
  if(ImGui::BeginListBox("##all-levels",ImVec2(-1,std::max(100.f,ImGui::GetContentRegionAvail().y-115.f)))){
    ImGuiListClipper clipper;clipper.Begin(int(shown.size()));
    while(clipper.Step())for(int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row){const auto& c=choices[shown[size_t(row)]];
      const auto label=std::string(c.em1?"EM1  ":"EM2  ")+c.name+(c.zone_only?"  [zone state]":"");
      if(ImGui::Selectable(label.c_str(),selected==c.destination,ImGuiSelectableFlags_AllowDoubleClick)){selected=c.destination;if(ImGui::IsMouseDoubleClicked(0))load=true;}
    }ImGui::EndListBox();
  }
  bool busy;std::string status;{std::lock_guard lock(request_mutex);busy=preparing||pending.has_value();status=last_status;}
  const auto selected_choice=std::find_if(choices.begin(),choices.end(),[&](const auto& c){return c.destination==selected;});
  const bool zone_only=selected_choice!=choices.end() && selected_choice->zone_only;
  ImGui::BeginDisabled(selected.empty() || busy || net::SynchronizedSessionActive());
  load|=ImGui::Button(zone_only?"Apply zone state":"Play selected level");ImGui::EndDisabled();
  if(load && !selected.empty()){const auto result=QueueDebugLevelLoad(selected);EngineConsoleMessage(result);}
  ImGui::SameLine();if(ImGui::Button("Restart current level"))ExecuteEngineConsole("level.restart");
  if(selected.starts_with("em1:"))ImGui::TextDisabled("EM1 compatibility is in development. Assets are prepared when selected.");
  if(zone_only)ImGui::TextWrapped("This state changes existing zones. Load a full state of the same level first; applying it will keep the current level loaded.");
  if(!status.empty())ImGui::TextWrapped("%s",status.c_str());
}
bool TestDebugLevelCatalog(){
  const std::vector<std::string> originals{"meanstreet_v1.level","gv_zone_a.level","GV_ZONE_A.LEVEL","../bad.level"};
  const auto choices=Catalog(originals);if(choices.size()!=std::size(kNativeLevelStates)+2)return false;
  std::set<std::string> destinations;for(const auto& c:choices)if(!destinations.insert(c.destination).second)return false;
  for(const auto name:kNativeLevelStates)if(!destinations.contains(std::string(name)))return false;
  return destinations.contains("em1:gv_zone_a.level") && destinations.contains("em1:meanstreet_v1.level") && !destinations.contains("em1:../bad.level");
}
}
