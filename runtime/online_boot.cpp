#include "online_address.h"
#include "online_boot.h"
#include "online_discovery.h"
#include "online_checkpoint.h"
#include "online_campaigns.h"
#include "online_world_random.h"
#include <rex/cvar.h>
#include "online_identity.h"
#include "online_invite.h"
#include <windows.h>
#include <rex/logging.h>
#include <atomic>
#include <fstream>
#include <future>
#include <stdexcept>
#include <thread>

REXCVAR_DECLARE(bool,em2_online_world_random);
namespace em2::net {
namespace {
namespace fs=std::filesystem;
struct Prepared {Identity identity;std::shared_ptr<const CheckpointBundle> bundle;};
void Require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
bool Within(const fs::path& child,const fs::path& root) {
    auto c=child.begin();for(auto r=root.begin();r!=root.end();++r,++c) {
        if(c==child.end() || _wcsicmp(c->c_str(),r->c_str()))return false;
    }return true;
}
Prepared Prepare(const BootPaths& paths,const fs::path& source,bool host,uint8_t delay,InitializationPolicy initialization,
                 const std::array<uint8_t,16>& session_nonce,
                 const std::shared_ptr<std::atomic<PreparationStep>>& step,std::stop_token stop) {
    Require(paths.executable.is_absolute() && paths.content.is_absolute() && paths.new_user_data.is_absolute(),"Online boot paths must be absolute");
    Require(!paths.new_user_data.filename().empty() && paths.new_user_data.filename()!="." && paths.new_user_data.filename()!="..","Online save directory must have a new name");
    const auto parent=fs::canonical(paths.new_user_data.parent_path());
    const auto destination=parent/paths.new_user_data.filename();
    Require(!fs::exists(fs::symlink_status(destination)),"Online save directory already exists");
    Require(!Within(destination,fs::canonical(paths.content)),"Online saves cannot be installed inside game content");
    if(host)Require(source.is_absolute() && !Within(destination,fs::canonical(source)),"Online save directory cannot be inside the frozen checkpoint");
    if(!paths.retained_host_normal_root.empty()){
        Require(host && paths.startup_policy==StartupPolicy::NativeMenu,"Retained campaigns require native host startup");
        const auto retained=ValidateRetainedHostCampaign(paths.retained_host_normal_root,source.parent_path(),stop);
        if(!retained.root)throw std::runtime_error(retained.error);
        Require(fs::canonical(source)==*retained.root,"The selected retained campaign changed while preparing it");
    }
    step->store(PreparationStep::CheckingFiles);
    const auto started=std::chrono::steady_clock::now();
    const auto program=FingerprintProgram(paths.executable,paths.content,stop,paths.content_identity_cache);
    if(!program.value)throw std::runtime_error(program.error);
    REXLOG_INFO("Online {} game files verified in {:.2f} seconds ({} bytes, content digest reused {})",host?"host":"guest",
        std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count(),program.value->bytes,program.value->reused_content_digest);
    Prepared prepared;prepared.identity.build=program.value->build;prepared.identity.content=program.value->content;prepared.identity.input_delay=delay;
    prepared.identity.startup_policy=paths.startup_policy;prepared.identity.initialization_policy=initialization;
    if(host) {
        step->store(PreparationStep::PackingSave);auto packed=PackCheckpoint(source,stop);
        if(!packed.value)throw std::runtime_error(packed.error);
        prepared.bundle=std::make_shared<CheckpointBundle>(std::move(*packed.value));prepared.identity.checkpoint=prepared.bundle->identity;
        prepared.identity.session=session_nonce;
    }
    Require(!stop.stop_requested(),"Online preparation cancelled");
    // CREATE_NEW semantics: never adopt or replace an existing save directory.
    Require(CreateDirectoryW(destination.c_str(),nullptr)!=0,"Cannot create a new online save directory");
    try {
        if(host) {
            step->store(PreparationStep::InstallingHostSave);
            const auto installed=InstallCheckpoint(prepared.bundle->bytes,prepared.bundle->identity,destination/"42560816",stop);
            if(!installed.installed)throw std::runtime_error(installed.error);
        }
        Require(!stop.stop_requested(),"Online preparation cancelled");return prepared;
    }catch(...) {
        // Remove only an empty directory created by this operation. Completed
        // session snapshots remain isolated and are never treated as main saves.
        std::error_code error;if(fs::is_empty(destination,error) && !error)fs::remove(destination,error);throw;
    }
}
}
struct OnlineBoot::Impl {
    BootStatus status;BootPaths paths;bool host=false,cancelling=false;
    std::string listen,advertised;uint16_t port=0;uint8_t delay=2;
    std::optional<Invite> invitation;std::unique_ptr<Session> session;
    std::stop_source stop;
    std::shared_ptr<std::atomic<PreparationStep>> step=std::make_shared<std::atomic<PreparationStep>>(PreparationStep::CheckingFiles);
    std::future<Prepared> preparation;std::future<CheckpointInstallResult> installation;
    bool installation_started=false;
    void Fail(std::string error) {
        status.error=std::move(error);status.invitation.clear();cancelling=true;stop.request_stop();
        if(session)session->Disconnect();status.state=BootState::Cancelling;
    }
    void DrainCancellation() {
        if(preparation.valid() && preparation.wait_for(std::chrono::seconds(0))==std::future_status::ready){try{preparation.get();}catch(...){}}
        if(installation.valid() && installation.wait_for(std::chrono::seconds(0))==std::future_status::ready){try{installation.get();}catch(...){}}
        if(!preparation.valid() && !installation.valid())status.state=status.error.empty()?BootState::Cancelled:BootState::Failed;
    }
};
OnlineBoot::OnlineBoot():impl_(std::make_unique<Impl>()){}
OnlineBoot::~OnlineBoot(){impl_->stop.request_stop();}
bool OnlineBoot::Host(BootPaths paths,const fs::path& source,std::string listen,std::string advertised,uint16_t port,uint8_t delay) {
    auto& p=*impl_;if(p.status.state!=BootState::Idle || delay>12)return false;
    // Validate the advertised endpoint before any expensive work; bind may use
    // wildcard while the invitation must name a usable numeric unicast address.
    Invite check{advertised,port?port:uint16_t(1)};check.session[0]=1;
    if(!EncodeInvite(check))return false;
    const auto bound=ParseNumericAddress(listen,true),target=ParseNumericAddress(advertised);
    if(!bound || !target || bound->ipv6()!=target->ipv6())return false;
    std::array<uint8_t,16> nonce{};
    if(!NewSessionNonce(nonce))return false;
    p.host=true;p.paths=std::move(paths);p.listen=std::move(listen);p.advertised=std::move(advertised);p.port=port;p.delay=delay;
    p.status.state=BootState::Preparing;p.status.user_data=p.paths.new_user_data;
    if(p.paths.startup_policy==StartupPolicy::NativeMenu){
        p.session=std::make_unique<Session>();
        if(!p.session->ReserveHostListener(p.listen,p.port)){
            p.Fail(p.session->error());
            p.DrainCancellation();
            return true;
        }
        const auto code=EncodeInvite(Invite{p.advertised,p.session->port(),nonce,0,delay});
        if(!code){
            p.Fail("Cannot encode online invitation");
            p.DrainCancellation();
            return true;
        }
        p.status.invitation=*code;
    }
    const auto copied=p.paths;const auto step=p.step;const auto token=p.stop.get_token();const auto initialization=ConfiguredNativeInitializationPolicy();
    try{
        p.preparation=std::async(std::launch::async,[copied,source,delay,initialization,nonce,step,token]{
            return Prepare(copied,source,true,delay,initialization,nonce,step,token);
        });
    }catch(const std::exception& error){
        p.Fail(error.what());
        p.DrainCancellation();
    }
    return true;
}
bool OnlineBoot::Join(BootPaths paths,std::string text) {
    auto& p=*impl_;if(p.status.state!=BootState::Idle)return false;
    const auto invite=DecodeInvite(text);if(!invite)return false;
    p.host=false;p.paths=std::move(paths);p.invitation=invite;p.status.state=BootState::Preparing;p.status.user_data=p.paths.new_user_data;
    const auto copied=p.paths;const auto step=p.step;const auto token=p.stop.get_token();const auto initialization=ConfiguredNativeInitializationPolicy();
    p.preparation=std::async(std::launch::async,[copied,initialization,step,token]{return Prepare(copied,{},false,2,initialization,{},step,token);});return true;
}
void OnlineBoot::Cancel() {
    auto& p=*impl_;if(p.status.state==BootState::Taken || p.status.state==BootState::Cancelled || p.status.state==BootState::Failed)return;
    p.Fail({});p.DrainCancellation();
}
BootStatus OnlineBoot::status()const{auto result=impl_->status;result.preparation=impl_->step->load();return result;}
void OnlineBoot::Poll() {
    auto& p=*impl_;
    if(p.cancelling){p.DrainCancellation();return;}
    if(p.status.state==BootState::Taken || p.status.state==BootState::Idle)return;
    try {
        if(p.preparation.valid()) {
            if(p.preparation.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;
            auto prepared=p.preparation.get();
            const bool reserved=bool(p.session);
            if(!reserved)p.session=std::make_unique<Session>();
            bool opened=false;
            if(p.host) {
                opened=reserved?p.session->StartReservedHost(prepared.identity,std::move(prepared.bundle)):
                    p.session->HostWithCheckpoint(prepared.identity,p.listen,p.port,std::move(prepared.bundle));
                if(opened && !reserved){const Invite invite{p.advertised,p.session->port(),prepared.identity.session,prepared.identity.first_tick,p.delay};
                    const auto code=EncodeInvite(invite);Require(bool(code),"Cannot encode online invitation");p.status.invitation=*code;}
            } else {
                const auto identity=IdentityForInvite(prepared.identity,*p.invitation,true);Require(bool(identity),"Cannot apply online invitation");
                opened=p.session->JoinWithCheckpoint(*identity,p.invitation->address,p.invitation->port);
            }
            if(!opened)throw std::runtime_error(p.session->error());
        }
        if(!p.session)return;
        p.session->Poll();
        if(p.session->state()==State::Error || p.session->state()==State::Disconnected)throw std::runtime_error(p.session->error().empty()?"Online peer disconnected":p.session->error());
        if(!p.host && !p.installation_started)if(auto bytes=p.session->DownloadedCheckpoint()) {
            const auto expected=p.session->identity().checkpoint;const auto target=p.paths.new_user_data/"42560816";const auto token=p.stop.get_token();
            p.installation=std::async(std::launch::async,[bytes,expected,target,token]{return InstallCheckpoint(*bytes,expected,target,token);});p.installation_started=true;
        }
        if(p.installation.valid()) {
            if(p.installation.wait_for(std::chrono::seconds(0))!=std::future_status::ready){p.status.state=BootState::Installing;return;}
            const auto installed=p.installation.get();
            if(!installed.installed){p.session->ConfirmCheckpointInstalled(false);throw std::runtime_error(installed.error);}
            Require(p.session->ConfirmCheckpointInstalled(true),"Cannot acknowledge online checkpoint installation");
        }
        switch(p.session->state()) {
            case State::Listening:p.status.state=BootState::WaitingForPeer;break;
            case State::Connecting:case State::Handshaking:p.status.state=BootState::Connecting;break;
            case State::Transferring:p.status.state=BootState::Transferring;break;
            case State::Ready:p.status.state=BootState::Ready;break;
            default:break;
        }
    }catch(const std::exception& error){p.Fail(error.what());p.DrainCancellation();}
}
bool OnlineBoot::ArmLobbyStart(){return impl_->session && impl_->session->ArmLobbyStart();}
bool OnlineBoot::CanRequestLobbyStart()const{return impl_->session && impl_->session->CanRequestLobbyStart();}
bool OnlineBoot::RequestLobbyStart(){return impl_->session && impl_->session->RequestLobbyStart();}
bool OnlineBoot::LobbyStartConfirmed()const{return impl_->session && impl_->session->LobbyStartConfirmed();}
std::optional<BootResult> OnlineBoot::TakeReady() {
    Poll();auto& p=*impl_;if(p.status.state!=BootState::Ready)return {};
    if(p.paths.startup_policy==StartupPolicy::NativeMenu && !LobbyStartConfirmed())return {};
    p.status.state=BootState::Taken;return BootResult{std::move(p.session),p.paths.new_user_data};
}
bool TestOnlineBoot(const fs::path& root) {
    auto check=[](bool ok,const char* text){if(!ok)REXLOG_ERROR("Online boot test: {}",text);return ok;};
    auto write=[](const fs::path& path,std::string_view data){fs::create_directories(path.parent_path());std::ofstream file(path,std::ios::binary);file.write(data.data(),data.size());};
    try {
        const auto base=fs::absolute(root);fs::create_directories(base);
        write(base/"program.bin","program");write(base/"content"/"configfiles.ini","Gravity=9.81\n");write(base/"frozen"/"save.dat","native checkpoint");
        auto paths=[&](std::string_view name){return BootPaths{base/"program.bin",base/"content",base/std::string(name)};};
        auto pump=[&](OnlineBoot& host,OnlineBoot* client,auto done) {
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(4);
            do{host.Poll();if(client)client->Poll();if(done())return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(std::chrono::steady_clock::now()<deadline);
            REXLOG_ERROR("Online boot timeout: host '{}' client '{}'",host.status().error,client?client->status().error:"");return false;
        };
        {
            OnlineBoot invalid_retained;auto retained_paths=paths("invalid-retained");
            retained_paths.startup_policy=StartupPolicy::NativeMenu;
            retained_paths.retained_host_normal_root=base/"normal";fs::create_directories(retained_paths.retained_host_normal_root);
            if(!check(invalid_retained.Host(retained_paths,base/"frozen","127.0.0.1","127.0.0.1") &&
                pump(invalid_retained,nullptr,[&]{return invalid_retained.status().state==BootState::Failed;}) &&
                !invalid_retained.status().error.empty() && !fs::exists(retained_paths.new_user_data),
                "retained source is revalidated on worker before fingerprint or destination creation"))return false;
        }
        std::optional<BootResult> host_result,client_result;
        {
            OnlineBoot host,client;
            if(!check(!client.Join(paths("invalid"),"invalid invitation") && client.status().state==BootState::Idle &&
                !host.Host(paths("bad-address"),base/"frozen","invalid","127.0.0.1"),"invalid invitations/endpoints rejected before I/O"))return false;
            if(!check(!host.Host(paths("bad-delay"),base/"frozen","127.0.0.1","127.0.0.1",0,13) &&
                host.status().state==BootState::Idle && !fs::exists(base/"bad-delay"),"unsupported input buffer rejected before save I/O"))return false;
            if(!check(host.Host(paths("host"),base/"frozen","127.0.0.1","127.0.0.1",0,0) && host.status().state==BootState::Preparing &&
                !host.Host(paths("other"),base/"frozen","127.0.0.1","127.0.0.1"),"asynchronous preparation cannot be silently replaced"))return false;
            if(!check(pump(host,nullptr,[&]{return host.status().state==BootState::WaitingForPeer;}),"host verifies installation and exposes a real invitation"))return false;
            const auto code=host.status().invitation;
            const auto direct=DecodeInvite(code);if(!check(direct && direct->input_delay==0,"host invitation carries zero-tick input buffer"))return false;
            auto public_invite=*direct;public_invite.address="203.0.113.19";
            LANAdvertiser advertisement;LANBrowser browser;
            if(!check(advertisement.Start(public_invite,"127.0.0.1",0) && browser.Start({{"127.0.0.1",advertisement.port()}}),"boot host starts loopback-only LAN discovery"))return false;
            if(!check(pump(host,nullptr,[&]{browser.Poll();advertisement.Poll();browser.Poll();return !browser.games().empty();}),"boot host discovered through actual UDP challenge/response"))return false;
            const auto lan_code=browser.games().front().invitation;advertisement.Stop();browser.Stop();
            if(!check(DecodeInvite(lan_code)==direct && client.Join(paths("client"),lan_code) && !client.TakeReady(),"LAN result starts actual fresh-client save preparation with original identity/timing"))return false;
            if(!check(pump(host,&client,[&]{return host.status().state==BootState::Ready && client.status().state==BootState::Ready;}),"asynchronous host/client save preparation and transfer reach mutual readiness"))return false;
            const auto original=PackCheckpoint(base/"frozen"),host_save=PackCheckpoint(base/"host"/"42560816"),client_save=PackCheckpoint(base/"client"/"42560816");
            if(!check(original.value && host_save.value && client_save.value && original.value->bytes==host_save.value->bytes &&
                original.value->bytes==client_save.value->bytes,"both isolated runtimes receive the exact frozen save without source changes"))return false;
            host_result=host.TakeReady();client_result=client.TakeReady();
            if(!check(host_result && client_result && !host.TakeReady() && !client.TakeReady(),"ready sessions transfer ownership exactly once"))return false;
            host.Cancel();client.Cancel();
        }
        if(!check(host_result->session->state()==State::Ready && client_result->session->state()==State::Ready &&
            host_result->session->identity().input_delay==0 && client_result->session->identity().input_delay==0 &&
            host_result->user_data==base/"host" && client_result->user_data==base/"client","coordinator destruction preserves handed-off sessions and agreed zero-tick buffer"))return false;
        Frame a{},b{};bool ha=false,cb=false;
        if(!check(host_result->session->SubmitLocal(0,Pad{0x1000}) && client_result->session->SubmitLocal(0,Pad{0x2000}),"handed-off sockets accept gameplay input"))return false;
        const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(std::chrono::steady_clock::now()<end && (!ha||!cb)) {
            host_result->session->Poll();client_result->session->Poll();
            if(!ha)ha=host_result->session->TakeFrame(a);if(!cb)cb=client_result->session->TakeFrame(b);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if(!check(ha && cb && a==b,"same retained TCP sessions carry agreed frames after boot owners are destroyed"))return false;
        host_result.reset();client_result.reset();
        {
            struct RestorePreference{bool value=REXCVAR_GET(em2_online_world_random);~RestorePreference(){REXCVAR_SET(em2_online_world_random,value);}}restore_preference;
            auto native_host=paths("native-policy-host"),native_client=paths("native-policy-client");
            native_host.startup_policy=native_client.startup_policy=StartupPolicy::NativeMenu;
            OnlineBoot host,client;
            REXCVAR_SET(em2_online_world_random,true);
            if(!check(host.Host(native_host,base/"frozen","127.0.0.1","127.0.0.1",0,0),"native-policy host begins"))return false;
            REXCVAR_SET(em2_online_world_random,false);
            const auto early=host.status();
            if(!check(early.state==BootState::Preparing && DecodeInvite(early.invitation),
                "native host publishes an invitation while its verification is pending"))return false;
            REXCVAR_SET(em2_online_world_random,true);
            if(!check(client.Join(native_client,early.invitation),"native-policy client begins"))return false;
            REXCVAR_SET(em2_online_world_random,false);
            if(!check(pump(client,nullptr,[&]{return client.status().state==BootState::Connecting;}) &&
                host.status().state==BootState::Preparing && !client.TakeReady(),
                "the early listener lets the guest connect before the host admits the verified session"))return false;
            if(!check(pump(host,&client,[&]{return host.status().state==BootState::Ready && client.status().state==BootState::Ready;}),
                "captured native policies survive console changes during real preparation and checkpoint transfer"))return false;
            if(!check(host.status().invitation==early.invitation,"host preparation retains the original port and invitation"))return false;
            if(!check(!host.TakeReady() && !client.TakeReady() && !host.RequestLobbyStart() &&
                !client.RequestLobbyStart() && !host.ArmLobbyStart(),"native lobby cannot start before the client is ready"))return false;
            if(!check(client.ArmLobbyStart() && client.ArmLobbyStart() &&
                pump(host,&client,[&]{return host.CanRequestLobbyStart();}),"client readiness reaches the host once"))return false;
            if(!check(!host.LobbyStartConfirmed() && !client.LobbyStartConfirmed() &&
                host.RequestLobbyStart() && !host.RequestLobbyStart() && !host.TakeReady(),
                "only the host can request start and host waits for acknowledgement"))return false;
            if(!check(pump(host,&client,[&]{return host.LobbyStartConfirmed() && client.LobbyStartConfirmed();}),
                "one host action confirms both native handoffs over the original TCP connection"))return false;
            auto h=host.TakeReady(),c=client.TakeReady();
            if(!check(h && c && h->session->identity().startup_policy==StartupPolicy::NativeMenu &&
                c->session->identity().startup_policy==StartupPolicy::NativeMenu &&
                h->session->identity().initialization_policy==InitializationPolicy::SharedNativeV1 &&
                c->session->identity().initialization_policy==InitializationPolicy::SharedNativeV1,
                "ready native sessions retain their original startup and initialization policies"))return false;
        }
        {
            OnlineBoot host6,client6;
            if(!check(!host6.Host(paths("wrong-family6"),base/"frozen","0.0.0.0","::1") &&
                !host6.Host(paths("wrong-family4"),base/"frozen","::","127.0.0.1") &&
                host6.status().state==BootState::Idle && !fs::exists(base/"wrong-family6") && !fs::exists(base/"wrong-family4"),
                "mismatched listen/advertised families rejected before save I/O"))return false;
            if(!check(host6.Host(paths("host6"),base/"frozen","::","::1",0,12) &&
                pump(host6,nullptr,[&]{return host6.status().state==BootState::WaitingForPeer;}),"IPv6 boot publishes invitation from wildcard listener"))return false;
            const auto invite6=DecodeInvite(host6.status().invitation);
            if(!check(invite6 && invite6->address=="::1" && invite6->input_delay==12 && host6.status().invitation.size()==81 &&
                client6.Join(paths("client6"),host6.status().invitation) &&
                pump(host6,&client6,[&]{return host6.status().state==BootState::Ready && client6.status().state==BootState::Ready;}),
                "IPv6 invitation drives checkpoint transfer and installation through actual boot owners"))return false;
            const auto original=PackCheckpoint(base/"frozen"),received=PackCheckpoint(base/"client6"/"42560816");
            if(!check(original.value && received.value && original.value->bytes==received.value->bytes,"IPv6 client installed exact isolated checkpoint"))return false;
            host_result=host6.TakeReady();client_result=client6.TakeReady();
            if(!check(host_result && client_result && host_result->session->identity().input_delay==12 &&
                client_result->session->identity().input_delay==12,"IPv6 ready owners hand off sockets with the agreed twelve-tick input buffer"))return false;
        }
        Frame six_a{},six_b{};bool six_host=false,six_client=false;
        if(!check(host_result->session->SubmitLocal(0,Pad{0x1000}) && client_result->session->SubmitLocal(0,Pad{0x2000}),"IPv6 sockets remain usable after boot owner destruction"))return false;
        const auto six_end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(std::chrono::steady_clock::now()<six_end && (!six_host || !six_client)) {
            host_result->session->Poll();client_result->session->Poll();
            if(!six_host)six_host=host_result->session->TakeFrame(six_a);
            if(!six_client)six_client=client_result->session->TakeFrame(six_b);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if(!check(six_host && six_client && six_a==six_b,"IPv6 handed-off sessions carry agreed gameplay input"))return false;
        host_result.reset();client_result.reset();
        OnlineBoot cancelled;
        if(!check(cancelled.Host(paths("cancelled"),base/"frozen","127.0.0.1","127.0.0.1"),"cancellation fixture starts"))return false;
        cancelled.Cancel();if(!check(pump(cancelled,nullptr,[&]{return cancelled.status().state==BootState::Cancelled;}) && !cancelled.TakeReady(),"cancellation drains workers without publishing a runnable session"))return false;
        write(base/"existing"/"sentinel","keep");OnlineBoot existing;
        if(!check(existing.Host(paths("existing"),base/"frozen","127.0.0.1","127.0.0.1") &&
            pump(existing,nullptr,[&]{return existing.status().state==BootState::Failed;}) && fs::file_size(base/"existing"/"sentinel")==4,
            "existing user-data directories are preserved"))return false;
        OnlineBoot inside;
        if(!check(inside.Host(paths("content/new-save"),base/"frozen","127.0.0.1","127.0.0.1") &&
            pump(inside,nullptr,[&]{return inside.status().state==BootState::Failed;}) && !fs::exists(base/"content"/"new-save"),"session saves cannot contaminate game content"))return false;
        OnlineBoot different_host,different_client;write(base/"different-content"/"configfiles.ini","Gravity=1.62\n");
        if(!check(different_host.Host(paths("second-host"),base/"frozen","127.0.0.1","127.0.0.1") &&
            pump(different_host,nullptr,[&]{return different_host.status().state==BootState::WaitingForPeer;}),"content mismatch host starts"))return false;
        auto mismatched=paths("second-client");mismatched.content=base/"different-content";
        if(!check(different_client.Join(mismatched,different_host.status().invitation) &&
            pump(different_host,&different_client,[&]{return different_host.status().state==BootState::Failed || different_client.status().state==BootState::Failed;}) &&
            !fs::exists(base/"second-client"/"42560816"),"independently hashed gameplay content mismatch prevents save installation"))return false;
        REXLOG_INFO("Online boot tests passed: worker identities, isolated snapshots, fresh-client TCP transfer, zero/twelve-tick buffer negotiation over IPv4/LAN/IPv6, invalid buffer rejection, ownership handoff, cancellation, path preservation and content mismatch");return true;
    }catch(const std::exception& error){REXLOG_ERROR("Online boot test exception: {}",error.what());return false;}
}
}
