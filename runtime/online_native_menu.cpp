#include "online_native_menu.h"
#include "online_runtime.h"
#include "online_frontend.h"
#include "online_checkpoint.h"
#include "online_campaigns.h"
#include "online_discovery.h"
#include "online_invite.h"
#include "online_notice.h"
#include "online_router.h"
#include "demo_setup.h"
#include "allocation_trace.h"
#include <rex/runtime.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <future>
#include <immintrin.h>
#include <mutex>
#include <stdexcept>
#include <windows.h>

REXCVAR_DEFINE_BOOL(em2_open_native_online_menu,false,"Debug","Open the authored native online page on the title menu (no gameplay input)");
REXCVAR_DEFINE_STRING(em2_native_lobby_qa_role,"","Debug","Private native-menu loopback test role: host or client");
REXCVAR_DEFINE_STRING(em2_native_lobby_qa_invitation_file,"","Debug","Private native-menu loopback invitation file");
REXCVAR_DECLARE(bool,em2_demo_build);
namespace em2::net {
namespace {
bool FrontendLevel(std::string_view level){return level=="mainmenu.start" || level.starts_with("mainmenu.");}
struct QaConfig {int role=0;std::filesystem::path invitation;};
QaConfig LoadQaConfig(){
    namespace fs=std::filesystem;
    const auto role=REXCVAR_GET(em2_native_lobby_qa_role);
    if(role.empty())return {};
    try{
        auto* runtime=rex::Runtime::instance();if(!runtime)return {};
        if(role!="host" && role!="client")throw std::runtime_error("invalid role");
        std::array<wchar_t,32768> executable{};const auto size=GetModuleFileNameW(nullptr,executable.data(),DWORD(executable.size()));
        if(!size || size>=executable.size())throw std::runtime_error("executable path");
        const auto workspace=fs::canonical(fs::path(executable.data()).parent_path().parent_path().parent_path());
        const auto analysis=fs::canonical(workspace/"analysis"),user=fs::canonical(runtime->user_data_root());
        const auto folder=user.parent_path(),root=folder.parent_path();
        if(user.filename()!=L"user_data" || folder.filename()!=fs::path(role) || root.parent_path()!=analysis ||
            !root.filename().string().starts_with("native-lobby-"))throw std::runtime_error("not a private native-lobby analysis root");
        const auto marker=user/".em2-native-lobby-qa";
        std::ifstream input(marker,std::ios::binary);std::string signature;std::getline(input,signature);
        if(signature!="EM2-native-menu-loopback-QA-v1" || fs::is_symlink(marker))throw std::runtime_error("private marker missing");
        const auto invitation=fs::path(REXCVAR_GET(em2_native_lobby_qa_invitation_file));
        if(!invitation.is_absolute() || invitation.filename()!=L"native-menu-invitation.txt" ||
            fs::canonical(invitation.parent_path())!=root || fs::is_symlink(invitation))throw std::runtime_error("invitation outside private pair");
        REXLOG_INFO("Native lobby QA enabled: {}, private user data {}, loopback only; discovery/router/clipboard disabled",role,user.string());
        return {role=="host"?1:2,invitation};
    }catch(const std::exception& error){REXLOG_ERROR("Native lobby QA rejected: {}",error.what());return {};}
}
const QaConfig& Qa(){static const auto configuration=LoadQaConfig();return configuration;}
bool PublishQaInvitation(std::string_view invitation){
    const auto parsed=DecodeInvite(invitation);
    if(Qa().role!=1 || !parsed || parsed->address!="127.0.0.1")return false;
    auto temporary=Qa().invitation;temporary+=L".tmp";
    std::ofstream output(temporary,std::ios::binary|std::ios::trunc);output.write(invitation.data(),std::streamsize(invitation.size()));output.close();
    if(!output)return false;
    std::error_code error;std::filesystem::rename(temporary,Qa().invitation,error);return !error;
}
std::optional<std::string> ReadQaInvitation(){
    if(Qa().role!=2)return {};
    std::error_code error;const auto size=std::filesystem::file_size(Qa().invitation,error);if(error || !size || size>256)return {};
    std::string code(size,'\0');std::ifstream input(Qa().invitation,std::ios::binary);input.read(code.data(),std::streamsize(code.size()));
    const auto invitation=DecodeInvite(code);
    if(!input || !invitation || invitation->address!="127.0.0.1")return {};
    return code;
}
std::optional<std::string> ClipboardInvitation(){
    if(!OpenClipboard(nullptr))return {};
    struct Close{~Close(){CloseClipboard();}}close;
    auto data=GetClipboardData(CF_UNICODETEXT);if(!data)return {};
    const auto size=GlobalSize(data);if(size<2 || size>8192)return {};
    const auto* text=static_cast<const wchar_t*>(GlobalLock(data));if(!text)return {};
    struct Unlock{HANDLE data;~Unlock(){GlobalUnlock(data);}}unlock{data};
    std::string result;const auto count=size/sizeof(wchar_t);
    for(size_t i=0;i<count;++i){if(!text[i])return result;if(text[i]>127)return {};result.push_back(char(text[i]));}
    return {};
}
bool CopyInvitation(std::string_view invitation){
    if(invitation.empty() || invitation.size()>256 || !OpenClipboard(nullptr))return false;
    struct Close{~Close(){CloseClipboard();}}close;
    auto memory=GlobalAlloc(GMEM_MOVEABLE,(invitation.size()+1)*sizeof(wchar_t));if(!memory)return false;
    auto* text=static_cast<wchar_t*>(GlobalLock(memory));if(!text){GlobalFree(memory);return false;}
    for(size_t i=0;i<invitation.size();++i)text[i]=uint8_t(invitation[i]);text[invitation.size()]=0;GlobalUnlock(memory);
    if(!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT,memory)){GlobalFree(memory);return false;}
    return true;
}
int ByteAt(std::string_view text,int index){return index>=0 && size_t(index)<text.size()?uint8_t(text[size_t(index)]):0;}
bool HasShareableInvite(const BootStatus& status){
    return !status.invitation.empty() &&
        (status.state==BootState::Preparing || status.state==BootState::WaitingForPeer);
}
struct Controller {
    std::mutex mutex;
    std::unique_ptr<OnlineBoot> boot;
    std::unique_ptr<RouterMapping> router;
    LANBrowser browser;LANAdvertiser advertiser;
    size_t selected=0;
    HostCampaignCatalog campaigns;
    size_t selected_campaign=0;
    std::stop_source campaign_cancel;
    std::future<HostCampaignCatalog> campaign_scan;
    bool host=false,start=false,start_requested=false,handoff_available=false,try_router=false;
    std::string error,feedback,advertised,reported_error;
    ~Controller(){campaign_cancel.request_stop();}
    bool Busy()const{
        if(!boot)return false;
        const auto state=boot->status().state;
        return state!=BootState::Idle && state!=BootState::Cancelled && state!=BootState::Failed;
    }
    void Find(){
        if(NativeOnlineMenuQaActive()){feedback="Private loopback test: LAN discovery is disabled.";return;}
        if(boot && (boot->status().state==BootState::Failed || boot->status().state==BootState::Cancelled))boot.reset();
        error.clear();browser.Start(LocalLANDiscoveryTargets());selected=0;feedback="Searching your LAN for a host...";
    }
    void Poll(){
        if(campaign_scan.valid() && campaign_scan.wait_for(std::chrono::seconds(0))==std::future_status::ready){
            try{campaigns=campaign_scan.get();}catch(const std::exception& exception){campaigns.error=exception.what();}
            if(selected_campaign>=campaigns.campaigns.size())selected_campaign=0;
        }
        browser.Poll();if(selected>=browser.games().size())selected=0;
        if(!browser.scanning() && feedback=="Searching your LAN for a host...")
            feedback=browser.games().empty()?"No LAN hosts found. Ask your friend to select Host, then find games again.":"Select Join LAN game. Left / Right selects a different host.";
        if(!boot)return;
        boot->Poll();const auto status=boot->status();
        if(status.state==BootState::Failed || status.state==BootState::Cancelled)
        {
            router.reset();
            start=false;
            start_requested=false;
        }
        if(status.state==BootState::Ready && handoff_available){
            if(!host)boot->ArmLobbyStart();
            if(!start && boot->LobbyStartConfirmed()){
                start=true;
                REXLOG_INFO("Native online lobby start confirmed for {}",host?"host":"client");
            }
        }
        if(host && HasShareableInvite(status)){
            if(NativeOnlineMenuQaActive()){
                if(advertised!=status.invitation && PublishQaInvitation(status.invitation)){
                    advertised=status.invitation;REXLOG_INFO("Native lobby QA host published private loopback invitation");
                }
            }else{
                if(host && try_router && !router){
                    if(const auto invite=DecodeInvite(status.invitation)){
                        router=std::make_unique<RouterMapping>();
                        router->Start(invite->address,invite->port);
                    }
                }
                if(advertised!=status.invitation){advertised=status.invitation;if(auto invite=DecodeInvite(advertised))advertiser.Start(*invite);}
                advertiser.Poll();
            }
        }else advertiser.Stop();
        if(router)router->Poll();
    }
    std::optional<std::string> MappedInvite()const{
        if(!router || !boot)return {};
        return InternetInvitation(boot->status().invitation,router->status());
    }
    void CopyHostInvite(){
        if(!host || !boot)return;
        const auto status=boot->status();
        if(!HasShareableInvite(status))return;
        const auto mapped=MappedInvite();
        const auto& code=mapped?*mapped:status.invitation;
        if(!CopyInvitation(code)){feedback="No invitation could be copied. Try again.";return;}
        feedback=mapped?"Internet invitation copied.":"LAN invitation copied.";
    }
    int State()const{
        if(!error.empty())return 9;
        if(start)return 7;
        if(!boot)return 0;
        switch(boot->status().state){
        case BootState::Preparing:return 1;case BootState::WaitingForPeer:return 2;
        case BootState::Connecting:return 3;case BootState::Transferring:return 4;
        case BootState::Installing:return 5;case BootState::Ready:return 6;
        case BootState::Cancelling:return 8;case BootState::Failed:return 9;
        default:return 0;
        }
    }
    std::string Message()const{
        if(!error.empty())return error;
        switch(State()){
        case 1:
            if(!host)return "Checking your game files before connecting...";
            if(!feedback.empty())return feedback+" Checking game files...";
            return "Checking game files and preparing the host save. Your friend can join now.";
        case 2:
            if(!feedback.empty())return feedback+" Waiting for player two.";
            if(router){
                const auto& mapping=router->status();
                if(mapping.state==RouterState::Active)return "Internet invitation ready. Copy it and send it to your friend.";
                if(mapping.state==RouterState::Failed)return mapping.error+" LAN hosting is still available.";
                return "Requesting a temporary Internet port. Friends on your LAN can join now.";
            }
            return "Waiting for player two. Friends on your LAN can find this game.";
        case 3:return "Connecting and checking that your game builds and files match...";
        case 4:return "Transferring the host save into a separate online session...";
        case 5:return "Installing the shared session save. Your normal save is kept separate.";
        case 6:
            if(!handoff_available)return "Connected. Starting online gameplay is temporarily unavailable. Cancel to return.";
            if(start_requested)return "Waiting for the other player to confirm the start...";
            if(!host)return "Connected. The host will start the game.";
            if(boot->CanRequestLobbyStart())return "Both players are ready. Select Start online game.";
            return "Connected. Waiting for the other player to be ready...";
        case 7:return "Preparing the shared game...";
        case 8:return "Cancelling file operations...";
        case 9:return OnlineFailureAdvice(boot?boot->status().error:std::string{},false);
        default:if(!feedback.empty())return feedback;
            return "Two-player online co-op. Host your save, paste a friend's invitation, or find a game on your LAN.";
        }
    }
    void ScanCampaigns(){
        if(Busy() || campaign_scan.valid())return;
        auto* runtime=rex::Runtime::instance();if(!runtime)return;
        campaigns={};selected_campaign=0;campaign_cancel=std::stop_source{};
        const auto normal=runtime->user_data_root();const auto token=campaign_cancel.get_token();
        campaign_scan=std::async(std::launch::async,[normal,token]{return FindRetainedHostCampaigns(normal,token);});
    }
    std::string CampaignMessage()const{
        if(campaign_scan.valid())return "Checking retained online campaigns...";
        if(!campaigns.error.empty())return campaigns.error;
        if(campaigns.campaigns.empty())return "No retained host campaigns yet. Host your offline campaign first. Online progress is saved separately and never replaces your offline campaign.";
        return campaigns.campaigns[selected_campaign].label+"\nResume creates a new online session from this campaign. Your offline save and the retained source are kept separate."+
            (campaigns.truncated?" More archives exist than this list can display.":"");
    }
    void Begin(bool hosting,std::string invitation={},std::optional<std::filesystem::path> retained={}){
        if(Busy())return;
        router.reset();
        error.clear();feedback.clear();start=false;start_requested=false;
        const auto level=em2::CurrentNativeLoadLevel();auto* runtime=rex::Runtime::instance();
        if(!runtime || !FrontendLevel(level.data()) || SynchronizedSessionActive()){
            error="Return to the title menu before starting an online session.";return;
        }
        try{
            const auto user=runtime->user_data_root();
            const auto parent=std::filesystem::absolute(user.parent_path()/"online_sessions");
            std::filesystem::create_directories(parent);
            const auto target=parent/("native-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
            std::array<wchar_t,32768> executable{};const auto length=GetModuleFileNameW(nullptr,executable.data(),DWORD(executable.size()));
            if(!length || length>=executable.size())throw std::runtime_error("Executable path unavailable");
            BootPaths paths{std::filesystem::path(executable.data()),runtime->game_data_root(),target,StartupPolicy::NativeMenu};
            boot=std::make_unique<OnlineBoot>();host=hosting;
            if(hosting){
                const auto source=ResolveCampaignSaveSource(retained?*retained/"42560816":user/"42560816");
                if(!source.root){error=source.error;boot.reset();return;}
                if(retained)paths.retained_host_normal_root=user;
                const auto address=NativeOnlineMenuQaActive()?std::string("127.0.0.1"):PreferredOnlineHostAddress();
                if(address.empty() || !boot->Host(paths,*source.root,NativeOnlineMenuQaActive()?"127.0.0.1":"0.0.0.0",address))error="No usable local IPv4 address was found. Check your network connection.";
            }else if(!boot->Join(paths,std::move(invitation)))error="Paste the host's current invitation, or use Find LAN games.";
        }catch(const std::exception&){error="Could not prepare the online session folder. Check disk space and permissions.";}
    }
    void Cancel(){campaign_cancel.request_stop();if(boot)boot->Cancel();router.reset();start=false;start_requested=false;error.clear();feedback.clear();advertiser.Stop();browser.Stop();advertised.clear();}
};
Controller& Menu(){static Controller controller;return controller;}
}
bool NativeOnlineMenuOption(int id,int choice,int& result){
    if(id<2700 || id>2715)return false;
    struct Restore{unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Restore(){_mm_setcsr(csr);SetLastError(error);}}restore;
    if(PublicDemoBuild()) {
        // Stale authored pages and diagnostic auto-open flags cannot create a
        // browser, host socket, save transfer or ready handoff in the demo.
        result=(id==2710 || id==2713)?ByteAt("Online multiplayer is unavailable in this demo.",choice):0;
        return true;
    }
    auto& menu=Menu();std::lock_guard lock(menu.mutex);result=0;
    if(id==2700){
        switch(choice){
        case 0:REXLOG_INFO("Authored native Online Multiplayer page opened");if(!menu.Busy()){menu.error.clear();menu.feedback.clear();menu.Find();}break;
        case 1:REXLOG_INFO("Native GFx online requested Host");menu.Begin(true);break;
        case 2:if(NativeOnlineMenuQaActive())break;else if(auto invite=ClipboardInvitation())menu.Begin(false,*invite);else menu.error="Copy your friend's invitation first, then select Join copied invitation.";break;
        case 3:if(!menu.Busy())menu.Find();break;
        case 4:if(menu.selected<menu.browser.games().size())menu.Begin(false,menu.browser.games()[menu.selected].invitation);else menu.feedback="No LAN game selected. Ask your friend to select Host, then find games again.";break;
        case 5:if(!NativeOnlineMenuQaActive())menu.CopyHostInvite();break;
        case 6:menu.Cancel();break;
        case 7:
            if(menu.handoff_available && menu.boot && menu.boot->RequestLobbyStart())
                menu.start_requested=true;
            break;
        case 8:case 9:if(!menu.browser.games().empty())menu.selected=(menu.selected+menu.browser.games().size()+(choice==8?1:-1))%menu.browser.games().size();break;
        case 10:if(NativeOnlineMenuQaActive() && Qa().role==2 && !menu.Busy())if(auto invitation=ReadQaInvitation()){
            REXLOG_INFO("Native GFx online requested Join through private loopback invitation");menu.Begin(false,*invitation);
        }break;
        case 11:menu.ScanCampaigns();break;
        case 12:if(!menu.Busy() && !menu.campaign_scan.valid() && menu.selected_campaign<menu.campaigns.campaigns.size()){
            REXLOG_INFO("Native GFx online requested Resume retained host campaign");
            menu.Begin(true,{},menu.campaigns.campaigns[menu.selected_campaign].user_data);
        }break;
        case 13:case 14:if(!menu.campaigns.campaigns.empty())menu.selected_campaign=(menu.selected_campaign+menu.campaigns.campaigns.size()+(choice==14?1:-1))%menu.campaigns.campaigns.size();break;
        case 15:
            if(!menu.Busy() && !NativeOnlineMenuQaActive()){
                menu.try_router=!menu.try_router;
                menu.error.clear();
                menu.feedback=menu.try_router
                    ?"Hosting will ask a compatible router for a temporary Internet port."
                    :"Automatic Internet port mapping is off. LAN hosting is available.";
            }
            break;
        default:break;
        }
        result=menu.State();return true;
    }
    if(id==2701){
        menu.Poll();result=menu.State();
        if(result==9){const auto message=menu.Message();if(message!=menu.reported_error){menu.reported_error=message;REXLOG_ERROR("Native online menu preparation failed: {}",message);}}
    }
    else if(id==2702)result=menu.host;
    else if(id==2703)result=int(menu.browser.games().size());
    else if(id==2704)result=int(menu.selected);
    else if(id==2705)result=REXCVAR_GET(em2_open_native_online_menu) || NativeOnlineMenuQaActive();
    else if(id==2706)result=menu.handoff_available && menu.boot && menu.boot->CanRequestLobbyStart();
    else if(id==2707)result=NativeOnlineMenuQaActive()?Qa().role:0;
    else if(id==2708)result=int(menu.campaigns.campaigns.size());
    else if(id==2709)result=int(menu.selected_campaign);
    else if(id==2710)result=ByteAt(menu.Message(),choice);
    else if(id==2711)result=menu.boot?ByteAt(menu.boot->status().invitation,choice):0;
    else if(id==2712)result=menu.selected<menu.browser.games().size()?ByteAt(menu.browser.games()[menu.selected].address,choice):0;
    else if(id==2713)result=ByteAt(menu.CampaignMessage(),choice);
    else if(id==2714)result=menu.campaign_scan.valid();
    else if(id==2715)result=choice==0?int(menu.try_router):int(menu.MappedInvite().has_value());
    return true;
}
std::optional<BootResult> TakeNativeOnlineMenuReady(){
    if(PublicDemoBuild())return {};
    auto& menu=Menu();std::lock_guard lock(menu.mutex);
    if(!menu.start || !menu.boot || menu.boot->status().state!=BootState::Ready)return {};
    auto result=menu.boot->TakeReady();
    if(result){
        menu.advertiser.Stop();
        menu.browser.Stop();
        if(menu.router && menu.router->status().state==RouterState::Active)
            result->session->KeepRouterMapping(menu.router);
        menu.router.reset();
    }
    return result;
}
void SetNativeOnlineMenuHandoffAvailable(bool available){if(PublicDemoBuild())return;auto& menu=Menu();std::lock_guard lock(menu.mutex);menu.handoff_available=available;}
void FailNativeOnlineMenuHandoff(std::string reason){
    auto& menu=Menu();std::lock_guard lock(menu.mutex);
    // TakeReady already moved the transport out. Cancelling a Taken boot is
    // intentionally a no-op; retaining it here would make Busy reject every
    // later Host/Join retry even for a safe pre-route validation failure.
    if(menu.boot){if(menu.boot->status().state==BootState::Taken)menu.boot.reset();else menu.boot->Cancel();}
    menu.error=std::move(reason);menu.start=false;menu.start_requested=false;
}
void CancelNativeOnlineMenu(){auto& menu=Menu();std::lock_guard lock(menu.mutex);menu.Cancel();}
bool NativeOnlineMenuQaActive(){return !PublicDemoBuild() && rex::Runtime::instance() && Qa().role!=0;}
bool TestNativeOnlineMenu(){
    bool ok=FrontendLevel("mainmenu.start") && !FrontendLevel("tcy_autotopia_visit1.sectiona_first") &&
        !FrontendLevel("notmainmenu.start") && ByteAt("ABC",0)==65 && ByteAt("ABC",3)==0 && ByteAt("ABC",-1)==0 &&
        ByteAt(std::string_view("\xC3\xA9",2),0)==195;
    {
        const bool previous=REXCVAR_GET(em2_demo_build);
        struct RestoreDemo {bool value;~RestoreDemo(){REXCVAR_SET(em2_demo_build,value);}} restore{previous};
        REXCVAR_SET(em2_demo_build,true);
        for(int id=2700;id<=2715;++id)for(int choice=-1;choice<=15;++choice) {
            int result=-99;
            ok=NativeOnlineMenuOption(id,choice,result) &&
                result==((id==2710 || id==2713)?ByteAt("Online multiplayer is unavailable in this demo.",choice):0) && ok;
        }
        int result=99;ok=!NativeOnlineMenuOption(2699,0,result) && !TakeNativeOnlineMenuReady() && !NativeOnlineMenuQaActive() && ok;
        ok=!ShowOnlineFrontend({},{},{}) && ok;
    }
    if(!ok)REXLOG_ERROR("Native online menu boundary/text bounds test failed");return ok;
}
}
