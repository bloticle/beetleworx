#include "online_content_root.h"
#include <rex/thread/mutex.h>
#include <unordered_map>
#include <rex/kernel/xam/content_manager.h>
#include <rex/logging.h>
#include <fstream>
#include <array>
#include <rex/kernel/xam/user_profile.h>

namespace em2::net {
namespace {
struct Route {std::function<bool()> packages_closed;std::filesystem::path destination;};
auto& Routes(){static std::unordered_map<const void*,Route> routes;return routes;}
using Profile=rex::kernel::xam::UserProfile;
constexpr std::array<uint32_t,3> kSaveSummaryIds={0x63E83FFF,0x63E83FFE,0x63E83FFD};
using SummarySettings=std::array<Profile::Setting*,3>;
bool ValidSummarySettings(const SummarySettings& settings){
    for(size_t i=0;i<settings.size();++i)
        if(!settings[i] || settings[i]->setting_id!=kSaveSummaryIds[i] ||
           settings[i]->type!=Profile::Setting::Type::BINARY)return false;
    return true;
}
bool ReadSummarySettings(Profile* profile,SummarySettings& summaries,std::string& error){
    if(!profile)return true;
    for(size_t i=0;i<summaries.size();++i)summaries[i]=profile->GetSetting(kSaveSummaryIds[i]);
    if(ValidSummarySettings(summaries))return true;
    error="The native profile save-summary cache is unavailable.";return false;
}
void InvalidateSummarySettings(const SummarySettings& settings){
    for(auto* setting:settings){
        // Reset both data and validity. LoadSetting leaves old bytes untouched
        // when a profile file is absent in the incoming private checkpoint.
        setting->Deserialize({});
        setting->is_set=false;
        setting->loaded_title_id=0;
    }
}
}
void RegisterNativeContentRoot(const void* manager,std::function<bool()> closed) {
    auto lock=rex::thread::global_critical_region::AcquireDirect();
    Routes().insert_or_assign(manager,Route{std::move(closed),{}});
}
void UnregisterNativeContentRoot(const void* manager) {
    auto lock=rex::thread::global_critical_region::AcquireDirect();Routes().erase(manager);
}
std::filesystem::path NativeContentRoot(const void* manager,const std::filesystem::path& original) {
    auto lock=rex::thread::global_critical_region::AcquireDirect();
    const auto it=Routes().find(manager);
    return it==Routes().end() || it->second.destination.empty()?original:it->second.destination;
}
bool SwitchNativeContentRoot(const void* manager,const std::filesystem::path& root,std::string& error,Profile* profile) {
    error.clear();std::error_code ec;
    if(!root.is_absolute() || !std::filesystem::is_directory(root/"42560816",ec)) {
        error="The prepared multiplayer save directory is missing.";return false;
    }
    const auto destination=std::filesystem::canonical(root,ec);
    if(ec){error="The prepared multiplayer save directory cannot be resolved.";return false;}
    auto lock=rex::thread::global_critical_region::AcquireDirect();
    const auto it=Routes().find(manager);
    if(it==Routes().end()){error="The native save manager is not registered.";return false;}
    if(!it->second.packages_closed || !it->second.packages_closed()) {
        error="Waiting for the game to finish using its current save.";return false;
    }
    SummarySettings summaries{};
    if(!ReadSummarySettings(profile,summaries,error))return false;
    it->second.destination=destination;
    if(profile)InvalidateSummarySettings(summaries);
    return true;
}
bool RestoreNativeContentRoot(const void* manager,std::string& error,Profile* profile){
    error.clear();
    auto lock=rex::thread::global_critical_region::AcquireDirect();
    const auto it=Routes().find(manager);
    if(it==Routes().end()){error="The native save manager is not registered.";return false;}
    if(it->second.destination.empty())return true;
    if(!it->second.packages_closed || !it->second.packages_closed()){
        error="Waiting for the game to finish using its current save.";return false;
    }
    SummarySettings summaries{};
    if(!ReadSummarySettings(profile,summaries,error))return false;
    // Reuse the manager's original root; never infer, copy or replace a save.
    it->second.destination.clear();
    if(profile)InvalidateSummarySettings(summaries);
    return true;
}
bool TestNativeContentRoot(const std::filesystem::path& fixture) {
    namespace fs=std::filesystem;
    using namespace rex::kernel::xam;
    try {
        Profile::BinarySetting slot0(kSaveSummaryIds[0],{1,2}),slot1(kSaveSummaryIds[1],{3}),slot2(kSaveSummaryIds[2],{4,5});
        SummarySettings summaries={&slot0,&slot1,&slot2};
        for(auto* setting:summaries)setting->loaded_title_id=0x42560816;
        if(!ValidSummarySettings(summaries))return false;
        auto invalid=summaries;invalid[2]=nullptr;
        if(ValidSummarySettings(invalid) || slot0.Serialize()!=std::vector<uint8_t>({1,2}))return false;
        Profile::Int32Setting wrong_type(kSaveSummaryIds[2],7);invalid[2]=&wrong_type;
        if(ValidSummarySettings(invalid))return false;
        invalid=summaries;std::swap(invalid[0],invalid[1]);
        if(ValidSummarySettings(invalid))return false;
        InvalidateSummarySettings(summaries);
        for(auto* setting:summaries)
            if(setting->is_set || setting->loaded_title_id || !setting->Serialize().empty())return false;
        const auto normal=fs::absolute(fixture/"normal"),session=fs::absolute(fixture/"session");
        fs::create_directories(normal/"42560816"/"00000001"/"NORMAL");
        fs::create_directories(session/"42560816"/"00000001"/"SESSION");
        std::ofstream(normal/"42560816"/"00000001"/"NORMAL"/"save.dat")<<"normal-save";
        ContentManager first(nullptr,normal),second(nullptr,normal);
        XCONTENT_AGGREGATE_DATA data{};data.title_id=0x42560816;data.content_type=rex::kernel::XContentType::kSavedGame;
        data.set_file_name("NORMAL");
        if(!first.ContentExists(data) || !second.ContentExists(data))return false;
        std::string error;
        if(SwitchNativeContentRoot(&first,"relative",error) || SwitchNativeContentRoot(nullptr,session,error))return false;
        bool closed=false;RegisterNativeContentRoot(&first,[&]{return closed;});
        if(SwitchNativeContentRoot(&first,session,error) || !first.ContentExists(data))return false;
        closed=true;if(!SwitchNativeContentRoot(&first,session,error))return false;
        if(first.ContentExists(data) || !second.ContentExists(data))return false;
        data.set_file_name("SESSION");
        if(!first.ContentExists(data) || second.ContentExists(data))return false;
        const auto entries=first.ListContent(0,rex::kernel::XContentType::kSavedGame,0x42560816);
        if(entries.size()!=1 || entries.front().file_name()!="SESSION")return false;
        std::ifstream input(normal/"42560816"/"00000001"/"NORMAL"/"save.dat");std::string value;input>>value;
        if(value!="normal-save")return false;
        int unregistered_manager=0;
        if(RestoreNativeContentRoot(nullptr,error) || RestoreNativeContentRoot(&unregistered_manager,error) ||
            !RestoreNativeContentRoot(&second,error) || second.ContentExists(data))return false;
        closed=false;
        if(RestoreNativeContentRoot(&first,error) || error!="Waiting for the game to finish using its current save." ||
            !first.ContentExists(data) || second.ContentExists(data))return false;
        closed=true;
        if(!RestoreNativeContentRoot(&first,error) || !error.empty() || first.ContentExists(data) || second.ContentExists(data))return false;
        data.set_file_name("NORMAL");
        if(!first.ContentExists(data) || !second.ContentExists(data))return false;
        const auto restored=first.ListContent(0,rex::kernel::XContentType::kSavedGame,0x42560816);
        if(restored.size()!=1 || restored.front().file_name()!="NORMAL")return false;
        closed=false; // Idempotent restoration does not mutate the route/cache.
        if(!RestoreNativeContentRoot(&first,error) || NativeContentRoot(&first,normal)!=normal)return false;
        closed=true;
        if(!SwitchNativeContentRoot(&first,session,error) || !RestoreNativeContentRoot(&first,error))return false;
        std::ifstream unchanged(normal/"42560816"/"00000001"/"NORMAL"/"save.dat");
        unchanged>>value;if(value!="normal-save")return false;
        UnregisterNativeContentRoot(&first);
        if(RestoreNativeContentRoot(&first,error) || NativeContentRoot(&first,normal)!=normal)return false;
        REXLOG_INFO("Native content routing passed: real ContentExists/ListContent round trips, repeated restore/re-entry, per-manager isolation, invalid/open-package guards and untouched normal save; profile reload/guest IO quiescence still require handoff validation");return true;
    }catch(const std::exception& e){REXLOG_ERROR("Native content route fixture: {}",e.what());return false;}
}
}
