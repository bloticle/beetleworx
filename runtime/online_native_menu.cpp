#include "online_name_input.h"
#include "online_internet_relay.h"
#include "online_discord.h"
#include "demo_support.h"
#include "controls.h"
#include "online_native_menu.h"
#include "online_runtime.h"
#include "online_room_leave.h"
#include "online_native_handoff.h"
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
#include <atomic>
#include <cstring>
#include <charconv>
#include <fstream>
#include <future>
#include <immintrin.h>
#include <mutex>
#include <stdexcept>
#include <windows.h>

REXCVAR_DEFINE_BOOL(em2_open_native_online_menu,false,"Debug","Open the authored native online page on the title menu (no gameplay input)");
REXCVAR_DEFINE_STRING(em2_native_lobby_qa_role,"","Debug","Private native-menu loopback test role: host or client");
REXCVAR_DEFINE_INT32(em2_native_lobby_qa_players,2,"Debug","Private native Room menu: wait for exactly 2..16 verified participants before Ready");
REXCVAR_DEFINE_STRING(em2_native_lobby_qa_invitation_file,"","Debug","Private native-menu loopback invitation file");
REXCVAR_DEFINE_STRING(em2_native_lobby_qa_replay_invitation_file,"","Debug","Host-only private native-menu replay: reuse a prior loopback invitation nonce, with freshly verified build/content/save");
REXCVAR_DEFINE_BOOL(em2_native_lobby_qa_reenter,false,"Debug","Private validated native lobby QA: start one fresh lobby after verified title recovery");
REXCVAR_DECLARE(bool,em2_demo_build);
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DEFINE_BOOL(em2_native_room_lobby,false,"Online","Experimental native menu using the larger-player Room bootstrap and gameplay owner");
REXCVAR_DEFINE_STRING(em2_online_relay_url,"","Online","Hosted HTTPS relay origin; supplied by the distribution after deployment");
namespace em2::net {
namespace {
bool OnlineMenuAvailable(){return !Em1DemoSession() && (!PublicDemoBuild() || !REXCVAR_GET(em2_online_relay_url).empty());}
std::atomic<Room*> private_menu_room{nullptr};
std::atomic<unsigned> qa_reentries{0};
std::string previous_qa_invitation;
bool FrontendLevel(std::string_view level){return level=="mainmenu.start" || level.starts_with("mainmenu.");}
struct QaConfig {int role=0;std::filesystem::path invitation;std::optional<std::array<uint8_t,16>> replay_nonce;unsigned players=2;};
QaConfig LoadQaConfig(){
    namespace fs=std::filesystem;
    const auto role=REXCVAR_GET(em2_native_lobby_qa_role);
    if(role.empty())return {};
    try{
        auto* runtime=rex::Runtime::instance();if(!runtime)return {};
        const auto players=REXCVAR_GET(em2_native_lobby_qa_players);
        if(players<2 || players>16 || (players>2 && !REXCVAR_GET(em2_native_room_lobby)))throw std::runtime_error("invalid participant count");
        if(role!="host" && role!="client"){
            unsigned index=0;const auto suffix=std::string_view(role).substr(std::min<size_t>(7,role.size()));
            const auto parsed=std::from_chars(suffix.data(),suffix.data()+suffix.size(),index);
            if(!role.starts_with("client-") || parsed.ec!=std::errc{} || parsed.ptr!=suffix.data()+suffix.size() || index<3 || index>unsigned(players) || suffix!=std::to_string(index))throw std::runtime_error("invalid role");
        }
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
        std::optional<std::array<uint8_t,16>> nonce;
        const auto replay=REXCVAR_GET(em2_native_lobby_qa_replay_invitation_file);
        if(!replay.empty()){
            const fs::path source(replay);const auto folder=fs::canonical(source.parent_path());
            if(role!="host" || !source.is_absolute() || source.filename()!=L"native-menu-invitation.txt" ||
               folder.parent_path()!=analysis || !folder.filename().string().starts_with("native-lobby-") || folder==root ||
               fs::is_symlink(source) || fs::file_size(source)>256)throw std::runtime_error("invalid replay invitation source");
            std::ifstream saved(source,std::ios::binary);std::string text((std::istreambuf_iterator<char>(saved)),{});
            const auto old=DecodeInvite(text);if(!saved || !old || old->address!="127.0.0.1")throw std::runtime_error("invalid replay invitation");
            nonce=old->session;
            REXLOG_INFO("Native lobby QA reusing captured nonce from {}; compatibility identities remain freshly measured",source.string());
        }
        REXLOG_INFO("Native lobby QA enabled: {}, private user data {}, loopback only; discovery/router/clipboard disabled",role,user.string());
        return {role=="host"?1:2,invitation,nonce,unsigned(players)};
    }catch(const std::exception& error){REXLOG_ERROR("Native lobby QA rejected: {}",error.what());return {};}
}
const QaConfig& Qa(){static const auto configuration=LoadQaConfig();return configuration;}
bool QaReentryPageRequested(){
    return !PublicDemoBuild() && rex::Runtime::instance() && REXCVAR_GET(em2_private_profile) &&
        REXCVAR_GET(em2_native_lobby_qa_reenter) && Qa().role!=0 && qa_reentries.load()==0 &&
        NativeOnlineTitleRestoreStatus()!=NativeTitleRestoreState::Failed;
}
bool PublishQaInvitation(std::string_view invitation){
    const auto parsed=DecodeInvite(invitation);
    if(Qa().role!=1 || !parsed || parsed->address!="127.0.0.1")return false;
    auto temporary=Qa().invitation;temporary+=L".tmp";
    std::ofstream output(temporary,std::ios::binary|std::ios::trunc);output.write(invitation.data(),std::streamsize(invitation.size()));output.close();
    if(!output)return false;
    return MoveFileExW(temporary.c_str(),Qa().invitation.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
}
std::optional<std::string> ReadQaInvitation(){
    if(Qa().role!=2)return {};
    std::error_code error;const auto size=std::filesystem::file_size(Qa().invitation,error);if(error || !size || size>256)return {};
    std::string code(size,'\0');std::ifstream input(Qa().invitation,std::ios::binary);input.read(code.data(),std::streamsize(code.size()));
    const auto invitation=DecodeInvite(code);
    if(!input || !invitation || invitation->address!="127.0.0.1")return {};
    if(NativeOnlineTitleRestoreStatus()==NativeTitleRestoreState::Ready && code==previous_qa_invitation)return {};
    previous_qa_invitation=code;
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
struct RosterView {
    struct Row {std::string name,details;uint32_t rgb=0;};
    std::array<Row,4> rows;
    size_t page=0,pages=1,total=0;
};
RosterView MakeRosterView(const LobbySnapshot& roster,size_t page,uint64_t local){
    RosterView view;view.total=roster.players.size();view.pages=std::max<size_t>(1,(view.total+3)/4);view.page=page%view.pages;
    for(size_t row=0;row<4 && view.page*4+row<view.total;++row){
        const auto& player=roster.players[view.page*4+row];auto& out=view.rows[row];
        out.name=player.name+(player.peer==local?" (you)":"");
        out.details=player.name+"\n"+(player.character==LobbyCharacter::Mickey?"Mickey | ":"Oswald | ");
        if(player.color<kLobbyColors.size()){out.details+=kLobbyColors[player.color].name;out.rgb=kLobbyColors[player.color].rgba>>8;}
        out.details+=player.peer==1?" | Host":player.ready?" | Ready":" | Not ready";
    }
    return view;
}
bool HasShareableInvite(const BootStatus& status,bool room=false){
    return !status.invitation.empty() &&
        (status.state==BootState::Preparing || status.state==BootState::WaitingForPeer || (room && status.state==BootState::Ready));
}
struct Controller {
    std::mutex mutex;
    std::unique_ptr<OnlineMenuBoot> boot;
    std::shared_ptr<InternetRelay> internet;
    std::vector<std::shared_ptr<InternetRelay>> closing_internet;
    void CollectInternet(){std::erase_if(closing_internet,[](const auto& relay){return relay->finished();});}
    void RetireInternet(){
        if(!internet)return;internet->Stop();
        ClearDiscordParty(internet->status().code);
        if(!internet->finished())closing_internet.push_back(internet);
        internet.reset();
    }
    std::optional<BootPaths> internet_join_paths;
    std::string discord_join_code;
    bool InternetMode()const{return !NativeOnlineMenuQaActive() && !REXCVAR_GET(em2_online_relay_url).empty();}
    std::string ReleaseVersion()const{
        std::array<wchar_t,32768> path{};const auto size=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));
        if(!size || size>=path.size())throw std::runtime_error("Executable path unavailable");
        auto version=em2::OnlineReleaseVersion(std::filesystem::path(path.data()).parent_path());
        return version.empty()?"0.0.0-dev":version;
    }
    std::unique_ptr<RouterMapping> router;
    LANBrowser browser;LANAdvertiser advertiser;
    size_t selected=0;
    size_t roster_page=0;
    RosterView roster_view;
    HostCampaignCatalog campaigns;
    size_t selected_campaign=0;
    std::stop_source campaign_cancel;
    std::future<HostCampaignCatalog> campaign_scan;
    bool host=false,start=false,start_requested=false,handoff_available=false,try_router=false,lan_discovery=false;
    bool qa_appearance_reported=false;
    std::string error,feedback,advertised,reported_error,message_readback;
    ~Controller(){campaign_cancel.request_stop();}
    bool Busy()const{
        if(internet && !internet->status().failed)return true;
        if(!boot)return false;
        const auto state=boot->status().state;
        return state!=BootState::Idle && state!=BootState::Cancelled && state!=BootState::Failed;
    }
    void Find(){
        if(InternetMode()){feedback="Host an internet game, or copy your friend's room code and select Join copied room code.";return;}
        if(NativeOnlineMenuQaActive() || QaReentryPageRequested()){feedback="Private loopback test: LAN discovery is disabled.";return;}
        if(boot && (boot->status().state==BootState::Failed || boot->status().state==BootState::Cancelled))boot.reset();
        error.clear();browser.Start(LocalLANDiscoveryTargets());selected=0;feedback="Searching your LAN for a host...";
    }
    void Poll(){
        CollectInternet();
        if(campaign_scan.valid() && campaign_scan.wait_for(std::chrono::seconds(0))==std::future_status::ready){
            try{campaigns=campaign_scan.get();}catch(const std::exception& exception){campaigns.error=exception.what();}
            if(selected_campaign>=campaigns.campaigns.size())selected_campaign=0;
        }
        browser.Poll();if(selected>=browser.games().size())selected=0;
        if(!browser.scanning() && feedback=="Searching your LAN for a host...")
            feedback=browser.games().empty()?"No LAN hosts found. Ask your friend to host with LAN discovery enabled or send you an invitation.":"Select Join LAN game. Left / Right selects a different host.";
        if(!boot)return;
        if(internet){
            const auto relay=internet->status();
            if(relay.failed){error=relay.error;boot->Cancel();RetireInternet();internet_join_paths.reset();return;}
            if(relay.ready && internet_join_paths){
                auto paths=std::move(*internet_join_paths);internet_join_paths.reset();
                if(!boot->Join(std::move(paths),relay.invitation)){
                    error="Could not join this internet room. Ask the host for a fresh code.";
                    boot->Cancel();RetireInternet();internet_join_paths.reset();return;
                }
            }
        }
        boot->Poll();const auto status=boot->status();
        if(internet && (status.state==BootState::WaitingForPeer || status.state==BootState::Ready)){
            const auto relay=internet->status();if(relay.ready){
                const auto roster=boot->lobby();
                internet->SetJoinable(!roster.locked);
                SetDiscordParty(relay.code,unsigned(std::max<size_t>(1,roster.players.size())),roster.capacity>=2?roster.capacity:16,!roster.locked);
            }
        }
        if(host && InternetMode() && !internet && HasShareableInvite(status,boot->uses_room())){
            try{
                internet=std::make_shared<InternetRelay>();
                internet->Host(REXCVAR_GET(em2_online_relay_url),ReleaseVersion(),status.invitation);
            }catch(const std::exception& exception){error=exception.what();return;}
        }
        if(status.state==BootState::Failed || status.state==BootState::Cancelled)
        {
            if(!status.error.empty())error=status.error;
            RetireInternet();internet_join_paths.reset();
            router.reset();
            start=false;
            start_requested=false;
        }
        if(status.state==BootState::Ready && handoff_available){
            bool qa_profile_ready=true;
            if(boot->uses_room() && NativeOnlineMenuQaActive()){
                const auto roster=boot->lobby();const auto* local=LocalPlayer(roster);
                const auto wanted=[&](const LobbyPlayer& player){return std::pair{
                    player.peer==1 || (player.peer>2 && (player.peer&1))?LobbyCharacter::Mickey:LobbyCharacter::Oswald,
                    uint8_t(Qa().players==2?player.peer+1:player.peer-1)};};
                qa_profile_ready=roster.players.size()==Qa().players && std::all_of(roster.players.begin(),roster.players.end(),[&](const auto& player){const auto [character,color]=wanted(player);return player.character==character && player.color==color;});
                if(!qa_profile_ready && local && !boot->lobby_request_pending() &&
                   (Qa().players>2?roster.players.size()==Qa().players:host || (roster.players.size()==2 && roster.players[0].color==2))){
                    const auto [character,color]=wanted(*local);
                    if(local->character!=character || local->color!=color)boot->SetAppearance(character,color);
                }
                if(qa_profile_ready && !qa_appearance_reported){qa_appearance_reported=true;
                    REXLOG_INFO("Native Room menu QA appearance accepted: peer {} color {}; real host-authorized profile before Ready",boot->local_peer(),local?local->color:255);}
            }
            if(!host && NativeOnlineMenuQaActive() && qa_profile_ready)boot->ArmLobbyStart();
            if(!start && boot->LobbyStartConfirmed()){
                start=true;
                REXLOG_INFO("Native online lobby start confirmed for {}",host?"host":"client");
            }
        }
        if(host && HasShareableInvite(status,boot->uses_room())){
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
                if(lan_discovery){
                    if(advertised!=status.invitation){advertised=status.invitation;if(auto invite=DecodeInvite(advertised))advertiser.Start(*invite);}
                    advertiser.Poll();
                }else advertiser.Stop();
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
        if(!HasShareableInvite(status,boot->uses_room()))return;
        if(internet){
            const auto relay=internet->status();
            if(!relay.ready){feedback="Connecting the internet lobby. Please wait.";return;}
            if(!CopyInvitation(relay.code)){feedback="Could not copy the room code. Try again.";return;}
            feedback="Internet room code copied. Send it to your friends; they select Join copied room code.";return;
        }
        const auto mapped=MappedInvite();
        const auto& code=mapped?*mapped:status.invitation;
        if(!CopyInvitation(code)){feedback="No invitation could be copied. Try again.";return;}
        feedback=mapped?"Internet invitation copied.":"LAN invitation copied.";
    }
    int State()const{
        if(!error.empty())return 9;
        if(internet && !internet->status().ready && !internet->status().failed)return 3;
        if(NativeOnlineStatus().state==NativeSessionState::Failed)return 9;
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
        const bool rooms=boot?boot->uses_room():REXCVAR_GET(em2_native_room_lobby);
        if(!error.empty())return error;
        if(internet){const auto relay=internet->status();
            if(relay.failed)return relay.error;
            if(!relay.ready)return "Connecting the internet lobby. No router setup is needed.";
            if(host && boot && boot->status().state==BootState::WaitingForPeer)
                return "Internet room: "+relay.code+". Copy the room code or invite friends through Discord.\n"+DiscordConnectionStatus();
        }
        const auto native=NativeOnlineStatus();
        if(native.state==NativeSessionState::Failed)return OnlineFailureAdvice(native.error,true);
        switch(State()){
        case 1:
            if(!host)return "Checking your game files before connecting...";
            if(!feedback.empty())return feedback+" Checking game files...";
            if(boot){switch(boot->status().preparation){
                case PreparationStep::PackingSave:return "Preparing a separate online copy of your current save...";
                case PreparationStep::InstallingHostSave:return "Finishing online save setup. Your normal save stays separate.";
                default:break;
            }}
            return "Checking your game files. The first check can take several minutes. Your room code appears when the lobby is ready.";
        case 2:
            if(!feedback.empty())return feedback+(rooms?" Waiting for players.":" Waiting for player two.");
            if(router){
                const auto& mapping=router->status();
                if(mapping.state==RouterState::Active)return "Internet invitation ready. Copy it and send it to your friend.";
                if(mapping.state==RouterState::Failed)return mapping.error+" Friends on your LAN can still join by invitation.";
                return "Requesting a temporary Internet port. LAN invitations are available now.";
            }
            if(lan_discovery && !advertiser.error().empty())
                return "LAN discovery is unavailable. Copy the invitation and share it with your friend.";
            if(rooms)return lan_discovery?"Waiting for players. Friends on your LAN can find this game.":"Waiting for players. Copy the invitation and share it with your friends.";
            return lan_discovery?"Waiting for player two. Friends on your LAN can find this game.":
                "Waiting for player two. Copy the invitation and share it with your friend.";
        case 3:return "Connecting and checking that your game builds and files match...";
        case 4:return "Transferring the host save into a separate online session...";
        case 5:return "Installing the shared session save. Your normal save is kept separate.";
        case 6:
            if(!handoff_available)return "Connected. Starting online gameplay is temporarily unavailable. Cancel to return.";
            if(start_requested)return "Waiting for the other player to confirm the start...";
            if(!host){
                const auto roster=boot->lobby();
                const auto player=LocalPlayer(roster);
                return player && player->ready?"Ready. Waiting for the host to start.":"Select Ready when you want the host to start.";
            }
            if(boot->native_start_block()==NativeLobbyStartBlock::MissingSecondPlayer)return "Waiting for another player to join before starting.";
            if(boot->native_start_block()==NativeLobbyStartBlock::UnsupportedCharacters)return "This build needs Mickey as host and Oswald as player two.";
            if(boot->CanRequestLobbyStart())return rooms?"Everyone is ready. Select Start online game.":"Both players are ready. Select Start online game.";
            return rooms?"Connected. Waiting for players to be ready...":"Connected. Waiting for the other player to be ready...";
        case 7:return "Preparing the shared game...";
        case 8:return "Cancelling file operations...";
        case 9:return OnlineFailureAdvice(boot?boot->status().error:std::string{},false);
        default:if(!feedback.empty())return feedback;
            if(InternetMode())return "Host your current save online, or copy a friend's eight-character room code and select Join copied room code. Saved online games continues previous online progress.";
            return rooms?"Host your save, paste a friend's invitation, or find a game on your LAN.":"Two-player online co-op. Host your save, paste a friend's invitation, or find a game on your LAN.";
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
        if(campaign_scan.valid())return "Checking previous online saves...";
        if(!campaigns.error.empty())return campaigns.error;
        if(campaigns.campaigns.empty())return "No previous online saves. Select Host my save online to start from your current single-player save. Online progress is saved separately.";
        return campaigns.campaigns[selected_campaign].label+"\nContinue creates a new room from this online save. Your single-player save and the previous online save stay separate."+
            (campaigns.truncated?" More archives exist than this list can display.":"");
    }
    void Begin(bool hosting,std::string invitation={},std::optional<std::filesystem::path> retained={}){
        if(Busy())return;
        CollectInternet();
        if(closing_internet.size()>=8){error="Previous connections are still closing. Please try again in a moment.";return;}
        name_input.End();name_input_state=0;
        router.reset();RetireInternet();internet_join_paths.reset();
        error.clear();feedback.clear();start=false;start_requested=false;
        qa_appearance_reported=false;
        roster_page=0;roster_view={};
        const auto level=em2::CurrentNativeLoadLevel();auto* runtime=rex::Runtime::instance();
        if(NativeOnlineStatus().state==NativeSessionState::Failed){
            error="Restart Beetleworx to begin a new online session from the shared save.";return;
        }
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
            paths.content_identity_cache=user/"online-content-identity.bin";
            boot=std::make_unique<OnlineMenuBoot>(REXCVAR_GET(em2_native_room_lobby) || InternetMode());host=hosting;
            if(boot->uses_room())try_router=false;
            if(hosting){
                const auto source=ResolveCampaignSaveSource(retained?*retained/"42560816":user/"42560816");
                if(!source.root){error=source.error;boot.reset();return;}
                if(retained)paths.retained_host_normal_root=user;
                const auto loopback=NativeOnlineMenuQaActive() || InternetMode();
                const auto address=loopback?std::string("127.0.0.1"):PreferredOnlineHostAddress();
                if(address.empty() || !boot->Host(paths,*source.root,loopback?"127.0.0.1":"0.0.0.0",address,0,2,NativeOnlineTitleRestoreStatus()==NativeTitleRestoreState::Ready?std::nullopt:Qa().replay_nonce))error="No usable local IPv4 address was found. Check your network connection.";
            }else if(InternetMode()){
                invitation.erase(std::remove_if(invitation.begin(),invitation.end(),[](unsigned char c){return c==' ' || c=='\r' || c=='\n' || c=='\t';}),invitation.end());
                for(auto& c:invitation)if(c>='a' && c<='z')c=char(c-'a'+'A');
                if(invitation.size()!=8){error="Copy the host's eight-character internet room code and select Join copied room code.";return;}
                internet_join_paths=paths;internet=std::make_shared<InternetRelay>();
                internet->Join(REXCVAR_GET(em2_online_relay_url),ReleaseVersion(),std::move(invitation));
            }else if(!boot->Join(paths,std::move(invitation)))error="Paste the host's current invitation, or use Find LAN games.";
        }catch(const std::exception&){error="Could not prepare the online session folder. Check disk space and permissions.";}
    }
    LobbyNameInput name_input;int name_input_state=0;
    std::array<bool,256> name_keys{};
    std::string name_readback;unsigned cursor_readback=0,anchor_readback=0;
    void FinishNameInput(bool commit){
        if(!name_input.active())return;
        const auto name=name_input.text();name_input.End();name_input_state=commit?2:3;
        if(commit && (!ProfileEditable() || !boot->RenameLobbyPlayer(name)))feedback="Name could not be saved. Try again.";
    }
    const LobbyPlayer* LocalPlayer(const LobbySnapshot& roster)const{
        const auto peer=boot?boot->local_peer():0;
        for(const auto& player:roster.players)if(player.peer==peer)return &player;
        return nullptr;
    }
    bool ProfileEditable()const{return boot && State()==6 && !boot->lobby().locked && !boot->lobby_request_pending();}
    std::string RosterText()const{
        if(!boot)return {};
        const auto roster=boot->lobby();std::string text;
        for(const auto& player:roster.players){
            if(!text.empty())text+='\n';
            text+=player.name; text+=player.peer==boot->local_peer()?" (you)":"";
            if(boot->uses_room()){text+=player.character==LobbyCharacter::Mickey?" - Mickey, ":" - Oswald, ";text+=kLobbyColors[player.color].name;}
            text+=player.peer==1?" - Host":(player.ready?" - Ready":" - Not ready");
        }
        return text;
    }
    void Cancel(){name_input.End();name_input_state=0;campaign_cancel.request_stop();if(boot)boot->Cancel();RetireInternet();internet_join_paths.reset();router.reset();start=false;start_requested=false;error.clear();feedback.clear();advertiser.Stop();browser.Stop();advertised.clear();}
};
Controller& Menu(){static Controller controller;return controller;}
}
bool NativeOnlineMenuOption(int id,int choice,int& result){
    if(id<2700 || id>2745 || (id>2716 && id<2720))return false;
    if(id==2743){result=choice==1?(!REXCVAR_GET(em2_online_relay_url).empty() && !Em1DemoSession()):OnlineMenuAvailable();return true;}
    struct Restore{unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Restore(){_mm_setcsr(csr);SetLastError(error);}}restore;
    if(!OnlineMenuAvailable()) {
        // Keep public networking unavailable until the distribution supplies
        // its deployed relay endpoint. EM1 preview remains offline.
        result=(id==2710 || id==2713)?ByteAt("Online multiplayer is unavailable in this demo.",choice):0;
        return true;
    }
    auto& menu=Menu();std::lock_guard lock(menu.mutex);result=0;
    if(id==2745){
        const bool available=!NativeOnlineMenuQaActive() && menu.InternetMode() && menu.host && menu.internet && menu.internet->status().ready && CanInviteDiscordFriends();
        if(choice==1 && available){
            result=InviteDiscordFriends();
            menu.feedback=result?"Opening Discord's invite screen...":"Discord invitations are not ready. Use Copy room code instead.";
        }else if(choice==0)result=available;
        return true;
    }
    if(id==2744){
        if(menu.InternetMode() && !menu.Busy()){
            if(auto code=TakeDiscordJoinCode())menu.discord_join_code=std::move(*code);
            result=!menu.discord_join_code.empty();
        }
        return true;
    }
    if(id==2700){
        switch(choice){
        case 0:REXLOG_INFO("Authored native Online Multiplayer page opened");if(!menu.Busy()){
            menu.error.clear();menu.feedback.clear();
            if(!menu.discord_join_code.empty()){auto code=std::move(menu.discord_join_code);menu.discord_join_code.clear();menu.Begin(false,std::move(code));}
            else menu.Find();
        }break;
        case 1:REXLOG_INFO("Native GFx online requested Host");menu.Begin(true);break;
        case 2:if(NativeOnlineMenuQaActive())break;else if(auto invite=ClipboardInvitation())menu.Begin(false,*invite);else menu.error="Copy your friend's invitation first, then select Join copied room code.";break;
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
            if(REXCVAR_GET(em2_native_room_lobby)){menu.feedback="Automatic router mapping is not yet available for Room lobbies.";break;}
            if(!menu.Busy() && !NativeOnlineMenuQaActive()){
                menu.try_router=!menu.try_router;
                menu.error.clear();
                menu.feedback=menu.try_router
                    ?"Hosting will ask a compatible router for a temporary Internet port."
                    :"Automatic Internet port mapping is off. LAN hosting is available.";
            }
            break;
        case 16:
            if(!menu.Busy() && !NativeOnlineMenuQaActive()){
                menu.lan_discovery=!menu.lan_discovery;
                menu.error.clear();
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
    else if(id==2705)result=REXCVAR_GET(em2_open_native_online_menu) || NativeOnlineMenuQaActive() || QaReentryPageRequested();
    else if(id==2706)result=menu.handoff_available && menu.boot && menu.boot->CanRequestLobbyStart();
    else if(id==2707)result=NativeOnlineMenuQaActive()?Qa().role:0;
    else if(id==2708)result=int(menu.campaigns.campaigns.size());
    else if(id==2709)result=int(menu.selected_campaign);
    else if(id==2710){if(choice==0)menu.message_readback=menu.Message();result=ByteAt(menu.message_readback,choice);}
    else if(id==2711)result=menu.internet?ByteAt(menu.internet->status().code,choice):menu.boot?ByteAt(menu.boot->status().invitation,choice):0;
    else if(id==2712)result=menu.selected<menu.browser.games().size()?ByteAt(menu.browser.games()[menu.selected].address,choice):0;
    else if(id==2713)result=ByteAt(menu.CampaignMessage(),choice);
    else if(id==2714)result=menu.campaign_scan.valid();
    else if(id==2715)result=choice==0?int(menu.try_router):int(menu.MappedInvite().has_value());
    else if(id==2716)result=menu.lan_discovery;
    else if(id==2720)result=menu.ProfileEditable();
    else if(id==2721){if(menu.boot){const auto roster=menu.boot->lobby();if(const auto player=menu.LocalPlayer(roster))result=player->ready;}}
    else if(id==2722){if(menu.boot){const auto roster=menu.boot->lobby();if(const auto player=menu.LocalPlayer(roster))result=ByteAt(player->name,choice);}}
    else if(id==2723)result=ByteAt(menu.RosterText(),choice);
    else if(id==2724)result=0; // Reserved; text comes through the window character path.
    else if(id==2725){
        if(menu.ProfileEditable() && !menu.host){
            const auto roster=menu.boot->lobby();if(const auto player=menu.LocalPlayer(roster))
                result=player->ready?menu.boot->SetLobbyReady(false):menu.boot->ArmLobbyStart();
        }
    }
    else if(id==2726){
        std::string text;
        if(menu.boot && menu.boot->lobby_request_pending())text="Updating lobby profile...";
        else if(menu.boot && menu.boot->last_lobby_result()==LobbyResult::InvalidName)text="Use 1-24 characters for your name, without control characters.";
        else if(menu.boot && menu.boot->last_lobby_result()==LobbyResult::Locked)text="The host has started the game. Your profile is locked.";
        else if(menu.boot && menu.boot->last_lobby_result()==LobbyResult::ColorInUse)text="That color was selected by another player. Choose another color.";
        result=ByteAt(text,choice);
    }
    else if(id==2727){
        if(choice==1 && menu.ProfileEditable()){
            const auto roster=menu.boot->lobby();if(const auto player=menu.LocalPlayer(roster)){
                if(menu.name_input.Begin(player->name)){menu.name_input_state=1;em2::ClearControlKeys();}
            }
        }else if(choice==2)menu.FinishNameInput(true);
        else if(choice==0){menu.name_input.End();menu.name_input_state=0;}
        result=menu.name_input_state;
    }
    else if(id==2728){
        if(choice==0){menu.name_readback=menu.name_input.text();menu.cursor_readback=menu.name_input.cursor_utf16();menu.anchor_readback=menu.name_input.anchor_utf16();}
        result=ByteAt(menu.name_readback,choice);
    }
    else if(id==2729)result=int(menu.cursor_readback);
    else if(id==2730)result=int(menu.anchor_readback);
    else if(id==2731)result=menu.boot && menu.boot->uses_room();
    else if(id>=2732 && id<=2737 && menu.boot && menu.boot->uses_room()){
        const auto roster=menu.boot->lobby();const auto* player=menu.LocalPlayer(roster);
        if(id==2736)result=roster.capacity;
        else if(player){
            if(id==2732)result=int(player->character);
            else if(id==2733)result=ByteAt(kLobbyColors[player->color].name,choice);
            else if(id==2737)result=menu.ProfileEditable() && player->peer>2;
            else if(choice==0 && menu.ProfileEditable()){
                if(id==2734 && player->peer>2)result=menu.boot->SetAppearance(player->character==LobbyCharacter::Mickey?LobbyCharacter::Oswald:LobbyCharacter::Mickey,player->color);
                else if(id==2735){
                    for(unsigned step=1;step<kLobbyColors.size();++step){const auto color=uint8_t((player->color+step)%kLobbyColors.size());
                        if(std::none_of(roster.players.begin(),roster.players.end(),[&](const auto& p){return p.peer!=player->peer && p.color==color;})){
                            result=menu.boot->SetAppearance(player->character,color);break;
                        }
                    }
                }
            }
        }
    }
    else if(id>=2738 && id<=2742 && menu.boot && menu.boot->uses_room()){
        if(id==2741 || (id==2742 && choice>=0 && choice<=2)){
            const auto roster=menu.boot->lobby();const auto pages=std::max<size_t>(1,(roster.players.size()+3)/4);
            if(id==2742){
                if(choice==0)menu.roster_page=(menu.roster_page+1)%pages;
                else if(choice==1)menu.roster_page=(menu.roster_page+pages-1)%pages;
                else for(size_t i=0;i<roster.players.size();++i)if(roster.players[i].peer==menu.boot->local_peer())menu.roster_page=i/4;
            }
            menu.roster_view=MakeRosterView(roster,menu.roster_page,menu.boot->local_peer());menu.roster_page=menu.roster_view.page;
            if(id==2741)result=int(choice==0?menu.roster_view.page+1:choice==1?menu.roster_view.pages:choice==2?menu.roster_view.total:0);
        }else if((id==2738 || id==2740) && choice>=0 && choice<512){
            const auto& row=menu.roster_view.rows[size_t(choice)/128];result=ByteAt(id==2738?row.name:row.details,choice%128);
        }else if(id==2739 && choice>=0 && choice<4)result=int(menu.roster_view.rows[size_t(choice)].rgb);
    }
    return true;
}
bool NativeOnlineNameKey(unsigned key,bool down,bool shift,bool control){
    if(!OnlineMenuAvailable())return false;
    auto& menu=Menu();std::lock_guard lock(menu.mutex);
    if(key<menu.name_keys.size() && menu.name_keys[key]){
        if(!down)menu.name_keys[key]=false;
        // Repeated downs still edit while active, but cannot escape ownership
        // when Return/Escape has already ended the editor.
        if(!down || !menu.name_input.active())return true;
    }
    if(!menu.name_input.active())return false;
    if(down){
        if(key<menu.name_keys.size())menu.name_keys[key]=true;
        if(key==VK_RETURN)menu.FinishNameInput(true);
        else if(key==VK_ESCAPE)menu.FinishNameInput(false);
        else if(control && key=='V'){
            if(OpenClipboard(nullptr)){
                if(auto data=GetClipboardData(CF_UNICODETEXT)){
                    const auto bytes=GlobalSize(data);
                    if(bytes && bytes<=8192)if(const auto text=static_cast<const wchar_t*>(GlobalLock(data))){
                        for(size_t i=0;i<bytes/sizeof(wchar_t) && text[i];++i)menu.name_input.Character(uint16_t(text[i]));
                        GlobalUnlock(data);
                    }
                }
                CloseClipboard();
            }
        }else menu.name_input.Key(key,shift,control);
    }
    return true;
}
bool NativeOnlineNameCharacter(uint32_t unit){
    if(!OnlineMenuAvailable())return false;
    auto& menu=Menu();std::lock_guard lock(menu.mutex);return menu.name_input.Character(unit);
}
void RestoreNativeOnlineNameFocus(){
    auto& menu=Menu();std::lock_guard lock(menu.mutex);
    for(unsigned key=0;key<menu.name_keys.size();++key)
        if(menu.name_keys[key] && !(GetAsyncKeyState(int(key))&0x8000))menu.name_keys[key]=false;
}
void CancelNativeOnlineNameInput(){auto& menu=Menu();std::lock_guard lock(menu.mutex);menu.FinishNameInput(false);}
std::optional<NativeMenuReady> TakeNativeOnlineMenuReady(){
    if(!OnlineMenuAvailable())return {};
    auto& menu=Menu();std::lock_guard lock(menu.mutex);
    if(!menu.start || !menu.boot || menu.boot->status().state!=BootState::Ready)return {};
    auto result=menu.boot->TakeReady();
    if(result){
        if(result->room && menu.internet){
            const auto relay=menu.internet->status();const auto& roster=result->room->lobby();
            menu.internet->SetJoinable(false);
            SetDiscordParty(relay.code,unsigned(roster.players.size()),roster.capacity,false);
            result->room->KeepInternetRelay(menu.internet);
        }
        if(result->room && menu.boot->uses_room() && REXCVAR_GET(em2_private_profile) && NativeOnlineMenuQaActive())
            private_menu_room.store(result->room.get(),std::memory_order_release);
        menu.advertiser.Stop();
        menu.browser.Stop();
        if(result->session && menu.router && menu.router->status().state==RouterState::Active)
            result->session->KeepRouterMapping(menu.router);
        menu.router.reset();
    }
    return result;
}
void CompleteNativeOnlineMenuHandoff(){
    auto& menu=Menu();std::lock_guard lock(menu.mutex);
    if(!menu.boot || menu.boot->status().state!=BootState::Taken)return;
    menu.boot.reset();
    menu.internet.reset();menu.internet_join_paths.reset();
    menu.start=false;
    menu.start_requested=false;
    menu.advertised.clear();
    menu.feedback.clear();
    REXLOG_INFO("Native online menu released completed preparation; live session retained");
}
void SetNativeOnlineMenuHandoffAvailable(bool available){if(!OnlineMenuAvailable())return;auto& menu=Menu();std::lock_guard lock(menu.mutex);menu.handoff_available=available;}
void FailNativeOnlineMenuHandoff(std::string reason){
    private_menu_room.store(nullptr,std::memory_order_release);
    auto& menu=Menu();std::lock_guard lock(menu.mutex);
    // TakeReady already moved the transport out. Cancelling a Taken boot is
    // intentionally a no-op; retaining it here would make Busy reject every
    // later Host/Join retry even for a safe pre-route validation failure.
    if(menu.boot){if(menu.boot->status().state==BootState::Taken)menu.boot.reset();else menu.boot->Cancel();}
    menu.error=std::move(reason);menu.start=false;menu.start_requested=false;
}
void CancelNativeOnlineMenu(){private_menu_room.store(nullptr,std::memory_order_release);auto& menu=Menu();std::lock_guard lock(menu.mutex);menu.Cancel();}
bool NativeOnlineMenuQaActive(){
    if(PublicDemoBuild() || !rex::Runtime::instance() || Qa().role==0 || NativeRoomTitleLeaveRenderRetired())return false;
    const auto state=NativeOnlineTitleRestoreStatus();
    return state==NativeTitleRestoreState::Idle || (state==NativeTitleRestoreState::Ready &&
        REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_native_lobby_qa_reenter) && qa_reentries.load()==0);
}
uint32_t NativeOnlineMenuHandoffDiagnostic(){
    auto& menu=Menu();std::lock_guard lock(menu.mutex);
    return uint32_t(menu.start) | (uint32_t(bool(menu.boot))<<1) |
        (uint32_t(menu.boot?unsigned(menu.boot->status().state):255)<<8) |
        (uint32_t(menu.boot && menu.boot->LobbyStartConfirmed())<<16);
}
void RecordNativeOnlineMenuReentry(){
    if(NativeOnlineMenuQaActive() && NativeOnlineTitleRestoreStatus()==NativeTitleRestoreState::Ready)
        qa_reentries.store(1);
}
Room* PrivateNativeMenuRoom(){return REXCVAR_GET(em2_private_profile) && NativeOnlineMenuQaActive()?private_menu_room.load(std::memory_order_acquire):nullptr;}
void ForgetPrivateNativeMenuRoom(const Room* room)noexcept{auto* expected=const_cast<Room*>(room);private_menu_room.compare_exchange_strong(expected,nullptr,std::memory_order_acq_rel);}
bool TestNativeOnlineMenu(){
    bool ok=FrontendLevel("mainmenu.start") && !FrontendLevel("tcy_autotopia_visit1.sectiona_first") &&
        !FrontendLevel("notmainmenu.start") && ByteAt("ABC",0)==65 && ByteAt("ABC",3)==0 && ByteAt("ABC",-1)==0 &&
        ByteAt(std::string_view("\xC3\xA9",2),0)==195;
    {
        LobbySnapshot roster;roster.capacity=16;
        for(unsigned i=0;i<16;++i)roster.players.push_back({i+1,"Player "+std::to_string(i+1),LobbyCharacter(i%2),uint8_t(i),i%2==0});
        // Maximum-size UTF-8 name: each four-byte code point remains intact in
        // native row readback, rather than truncating a combined roster string.
        roster.players.back().name.clear();for(unsigned i=0;i<24;++i)roster.players.back().name+="\xF0\x9F\x8E\xAE";
        for(size_t page=0;page<4;++page){const auto view=MakeRosterView(roster,page,16);
            ok=view.pages==4 && view.total==16 && view.page==page && ok;
            for(size_t row=0;row<4;++row){const auto index=page*4+row;
                ok=view.rows[row].name==roster.players[index].name+(index==15?" (you)":"") && view.rows[row].rgb==(kLobbyColors[index].rgba>>8) &&
                    view.rows[row].name.size()<128 && view.rows[row].details.size()<128 && ok;
            }
        }
        ok=MakeRosterView(roster,4,1).page==0 && ok;
        roster.players.resize(3);const auto shrunk=MakeRosterView(roster,3,1);
        ok=shrunk.page==0 && shrunk.pages==1 && shrunk.rows[3].name.empty() && shrunk.rows[0].name=="Player 1 (you)" && ok;
        BootStatus ready;ready.state=BootState::Ready;ready.invitation="fixture invitation";
        ok=HasShareableInvite(ready,true) && !HasShareableInvite(ready,false) && ok;
    }
    {
        const bool previous=REXCVAR_GET(em2_demo_build);
        struct RestoreDemo {bool value;~RestoreDemo(){REXCVAR_SET(em2_demo_build,value);}} restore{previous};
        REXCVAR_SET(em2_demo_build,true);
        for(int id=2700;id<=2742;++id)for(int choice=-3;choice<=255;++choice) {
            if(id>2716 && id<2720)continue;
            int result=-99;
            ok=NativeOnlineMenuOption(id,choice,result) &&
                result==((id==2710 || id==2713)?ByteAt("Online multiplayer is unavailable in this demo.",choice):0) && ok;
        }
        int result=99;ok=!NativeOnlineMenuOption(2699,0,result) && !TakeNativeOnlineMenuReady() && !NativeOnlineMenuQaActive() && ok;
        ok=!ShowOnlineFrontend({},{},{}) && ok;
        REXCVAR_SET(em2_demo_build,false);
        ok=!NativeOnlineMenuOption(2719,0,result) && ok;
        for(unsigned key:{unsigned(VK_ESCAPE),unsigned(VK_RETURN)}){
            auto& menu=Menu();menu.name_input.Begin("Name");
            ok=NativeOnlineNameKey(key,true) && !menu.name_input.active() &&
                NativeOnlineNameKey(key,true) && NativeOnlineNameKey(key,false) &&
                !NativeOnlineNameKey(key,true) && ok;
            menu.name_input_state=0;menu.feedback.clear();
        }
        {
            auto& menu=Menu();menu.name_input.Begin("Name");
            ok=NativeOnlineNameKey(VK_LBUTTON,true) && menu.name_input.active() && ok;
            menu.FinishNameInput(false);
            ok=NativeOnlineNameKey(VK_LBUTTON,true) && NativeOnlineNameKey(VK_LBUTTON,false) &&
                !NativeOnlineNameKey(VK_LBUTTON,true) && ok;
            menu.name_input_state=0;
        }
        for(int choice:{-1,65,-2})ok=NativeOnlineMenuOption(2724,choice,result) && result==0 && ok;
        ok=NativeOnlineMenuOption(2724,-3,result) && result==0 && ok;
        ok=NativeOnlineMenuOption(2725,0,result) && result==0 && ok;
        ok=NativeOnlineMenuOption(2716,-1,result) && result==0 && ok;
        ok=NativeOnlineMenuOption(2700,16,result) && NativeOnlineMenuOption(2716,-1,result) && result==1 && ok;
        ok=NativeOnlineMenuOption(2700,16,result) && NativeOnlineMenuOption(2716,-1,result) && result==0 && ok;
    }
    if(!ok)REXLOG_ERROR("Native online menu boundary/text bounds test failed");return ok;
}
}
