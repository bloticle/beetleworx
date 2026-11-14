#include "online_room_boot.h"
#include "online_boot_prepare.h"
#include "online_address.h"
#include "online_invite.h"
#include "online_world_random.h"
#include <rex/logging.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <future>
#include <stdexcept>
#include <thread>

namespace em2::net {
namespace {
namespace fs=std::filesystem;
bool NameValid(std::string_view text){LobbyRoster roster(2);return roster.Join(1,text)==LobbyResult::Ok;}
}
struct OnlineRoomBoot::Impl {
    BootStatus status;BootPaths paths;bool host=false,cancelling=false,installation_started=false;
    std::string listen,advertised,name;uint16_t port=0;uint8_t capacity=0,verification_window=8;bool explicit_window=false;
    std::optional<Invite> invite;
    std::unique_ptr<Room> room;
    std::stop_source stop;
    std::shared_ptr<std::atomic<PreparationStep>> step=std::make_shared<std::atomic<PreparationStep>>(PreparationStep::CheckingFiles);
    std::future<PreparedOnlineBoot> preparation;
    std::future<CheckpointInstallResult> installation;
    bool Taken()const{return status.state==BootState::Taken;}
    void Drain(){
        if(preparation.valid() && preparation.wait_for(std::chrono::seconds(0))==std::future_status::ready){try{preparation.get();}catch(...){}}
        if(installation.valid() && installation.wait_for(std::chrono::seconds(0))==std::future_status::ready){try{installation.get();}catch(...){}}
        if(!preparation.valid() && !installation.valid())status.state=status.error.empty()?BootState::Cancelled:BootState::Failed;
    }
    void Fail(std::string error){
        status.error=std::move(error);status.invitation.clear();cancelling=true;stop.request_stop();
        if(room)room->Disconnect();status.state=BootState::Cancelling;Drain();
    }
};
OnlineRoomBoot::OnlineRoomBoot():impl_(std::make_unique<Impl>()){}
OnlineRoomBoot::~OnlineRoomBoot(){impl_->stop.request_stop();}
bool OnlineRoomBoot::SetGameplayVerificationWindow(uint8_t limit){
    auto& p=*impl_;
    if(p.status.state!=BootState::Idle || limit<1 || limit>8)return false;
    p.verification_window=limit;p.explicit_window=true;return true;
}
bool OnlineRoomBoot::Host(BootPaths paths,const fs::path& source,std::string listen,std::string advertised,
    uint8_t capacity,std::string name,uint16_t port,uint8_t delay,std::optional<std::array<uint8_t,16>> session_nonce){
    auto& p=*impl_;if(p.status.state!=BootState::Idle || capacity<2 || capacity>kOnlineRoomCapacity || delay>12 || !NameValid(name))return false;
    const auto local=ParseNumericAddress(listen,true),remote=ParseNumericAddress(advertised);
    if(!local || !remote || local->ipv6()!=remote->ipv6())return false;
    std::array<uint8_t,16> nonce{};
    if(session_nonce){
        nonce=*session_nonce;
        if(std::all_of(nonce.begin(),nonce.end(),[](auto value){return value==0;}))return false;
    }else if(!NewSessionNonce(nonce))return false;
    if(!EncodeInvite(Invite{advertised,port?port:uint16_t(1),nonce,0,delay,p.verification_window}))return false;
    const auto initialization=paths.startup_policy==StartupPolicy::NativeMenu?InitializationPolicy::SharedNativeV1:ConfiguredNativeInitializationPolicy();
    p.status.error.clear();
    p.paths=std::move(paths);p.host=true;p.listen=std::move(listen);p.advertised=std::move(advertised);p.name=std::move(name);p.port=port;p.capacity=capacity;
    p.status.state=BootState::Preparing;p.status.user_data=p.paths.new_user_data;
    const auto copied=p.paths;const auto step=p.step;const auto token=p.stop.get_token();
    try{p.preparation=std::async(std::launch::async,[copied,source,delay,initialization,nonce,step,token]{
        return PrepareOnlineBoot(copied,source,true,delay,initialization,nonce,step,token);
    });}catch(const std::exception& error){p.Fail(error.what());}
    return true;
}
bool OnlineRoomBoot::Join(BootPaths paths,std::string text,std::string name){
    auto& p=*impl_;if(p.status.state!=BootState::Idle || !NameValid(name))return false;
    const auto invite=DecodeInvite(text);if(!invite)return false;
    if(p.explicit_window && p.verification_window!=invite->verification_window){
        p.status.error="The invitation uses a different gameplay verification window than the explicit local override.";return false;
    }
    p.verification_window=invite->verification_window;p.status.error.clear();
    const auto initialization=paths.startup_policy==StartupPolicy::NativeMenu?InitializationPolicy::SharedNativeV1:ConfiguredNativeInitializationPolicy();
    p.paths=std::move(paths);p.invite=invite;p.name=std::move(name);p.status.state=BootState::Preparing;p.status.user_data=p.paths.new_user_data;
    const auto copied=p.paths;const auto step=p.step;const auto token=p.stop.get_token();
    try{p.preparation=std::async(std::launch::async,[copied,initialization,step,token]{
        return PrepareOnlineBoot(copied,{},false,2,initialization,{},step,token);
    });}catch(const std::exception& error){p.Fail(error.what());}
    return true;
}
void OnlineRoomBoot::Poll(){
    auto& p=*impl_;if(p.cancelling){p.Drain();return;}
    if(p.Taken() || p.status.state==BootState::Idle || p.status.state==BootState::Cancelled || p.status.state==BootState::Failed)return;
    try{
        if(p.preparation.valid()){
            if(p.preparation.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;
            auto prepared=p.preparation.get();p.room=std::make_unique<Room>();
            if(!p.room->SetGameplayVerificationWindow(p.verification_window))throw std::runtime_error("Invalid gameplay verification window");
            if(p.host){
                if(!p.room->HostWithCheckpoint(prepared.identity,p.listen,p.port,p.capacity,p.name,std::move(prepared.bundle)))throw std::runtime_error("Could not host the verified online room. Check the address and port.");
                const auto invite=EncodeInvite(Invite{p.advertised,p.room->port(),prepared.identity.session,prepared.identity.first_tick,prepared.identity.input_delay,p.verification_window});
                if(!invite)throw std::runtime_error("Could not encode the online room invitation");p.status.invitation=*invite;
            }else{
                const auto identity=IdentityForInvite(prepared.identity,*p.invite,true);
                if(!identity || !p.room->JoinWithCheckpoint(*identity,p.invite->address,p.invite->port,p.name))throw std::runtime_error("Could not join the online room with the verified game identity.");
            }
        }
        if(!p.room)return;p.room->Poll();
        if(p.room->state()==RoomState::Error || p.room->state()==RoomState::Disconnected)
            throw std::runtime_error(p.room->error().empty()?"The online room disconnected.":p.room->error());
        if(!p.host && !p.installation_started)if(auto bytes=p.room->DownloadedCheckpoint()){
            const auto expected=p.room->identity().checkpoint;const auto target=p.paths.new_user_data/"42560816";const auto token=p.stop.get_token();
            p.step->store(PreparationStep::InstallingHostSave);
            p.installation=std::async(std::launch::async,[bytes,expected,target,token]{return InstallCheckpoint(*bytes,expected,target,token);});p.installation_started=true;
        }
        if(p.installation.valid()){
            if(p.installation.wait_for(std::chrono::seconds(0))!=std::future_status::ready){p.status.state=BootState::Installing;return;}
            const auto installed=p.installation.get();
            if(!installed.installed){p.room->ConfirmCheckpointInstalled(false);throw std::runtime_error(installed.error);}
            if(!p.room->ConfirmCheckpointInstalled(true))throw std::runtime_error("Could not acknowledge the verified shared save installation.");
        }
        switch(p.room->state()){
        case RoomState::Listening:p.status.state=BootState::WaitingForPeer;break;
        case RoomState::Connecting:p.status.state=BootState::Connecting;break;
        case RoomState::Transferring:p.status.state=BootState::Transferring;break;
        case RoomState::Installing:p.status.state=BootState::Installing;break;
        case RoomState::Lobby:p.status.state=p.host || p.room->checkpoint_ready()?BootState::Ready:BootState::Installing;break;
        case RoomState::Loading:p.status.state=BootState::Ready;break;
        default:throw std::runtime_error("Unexpected online room state before native handoff.");
        }
    }catch(const std::exception& error){p.Fail(error.what());}
}
void OnlineRoomBoot::Cancel(){auto& p=*impl_;if(p.Taken() || p.status.state==BootState::Cancelled || p.status.state==BootState::Failed)return;p.Fail({});}
BootStatus OnlineRoomBoot::status()const{auto s=impl_->status;s.preparation=impl_->step->load();return s;}
RoomState OnlineRoomBoot::room_state()const{return impl_->room?impl_->room->state():RoomState::Idle;}
uint64_t OnlineRoomBoot::local_peer()const{return impl_->room?impl_->room->local_peer():0;}
LobbySnapshot OnlineRoomBoot::lobby()const{return impl_->room?impl_->room->lobby():LobbySnapshot{};}
std::vector<RoomCheckpointPeer> OnlineRoomBoot::checkpoint_progress()const{return impl_->room?impl_->room->checkpoint_peers():std::vector<RoomCheckpointPeer>{};}
LobbyResult OnlineRoomBoot::last_lobby_result()const{return impl_->room?impl_->room->last_lobby_result():LobbyResult::Ok;}
bool OnlineRoomBoot::profile_pending()const{return impl_->room && impl_->room->profile_pending();}
bool OnlineRoomBoot::Rename(std::string_view s){return impl_->room && !impl_->cancelling && impl_->room->Rename(s);}
bool OnlineRoomBoot::SetAppearance(LobbyCharacter c,uint8_t color){return impl_->room && !impl_->cancelling && impl_->room->SetAppearance(c,color);}
bool OnlineRoomBoot::SetReady(bool ready){return impl_->room && !impl_->cancelling && impl_->room->SetReady(ready);}
bool OnlineRoomBoot::CanStart()const{
    const auto& p=*impl_;if(!p.host || p.cancelling || !p.room || p.room->state()!=RoomState::Lobby)return false;
    const auto& lobby=p.room->lobby();if(lobby.locked || lobby.players.size()<2 || !std::all_of(lobby.players.begin(),lobby.players.end(),[](const auto& player){return player.ready;}))return false;
    const auto progress=p.room->checkpoint_peers();return progress.size()==lobby.players.size() && std::all_of(progress.begin(),progress.end(),[](const auto& peer){return peer.installed;});
}
bool OnlineRoomBoot::Start(){auto& p=*impl_;return CanStart() && p.room->Start();}
bool OnlineRoomBoot::StartNativeBootstrap(){auto& p=*impl_;return CanStart() && p.room->identity().initialization_policy==InitializationPolicy::SharedNativeV1 && p.room->StartNativeBootstrap();}
std::optional<RoomBootResult> OnlineRoomBoot::TakeStarted(){
    Poll();auto& p=*impl_;if(p.cancelling || p.Taken() || !p.room || p.preparation.valid() || p.installation.valid() ||
        p.room->state()!=RoomState::Loading || !p.room->lobby().locked || !p.room->checkpoint_ready())return {};
    p.status.state=BootState::Taken;p.status.invitation.clear();return RoomBootResult{std::move(p.room),p.paths.new_user_data};
}
bool TestOnlineRoomBoot(const fs::path& root){
    bool ok=true;unsigned checks=0;auto check=[&](bool value,const char* why){++checks;if(!value){REXLOG_ERROR("Online room boot: {}",why);ok=false;}return value;};
    auto write=[](const fs::path& path,std::string_view bytes){fs::create_directories(path.parent_path());std::ofstream file(path,std::ios::binary);file.write(bytes.data(),bytes.size());};
    try{
        const auto base=fs::absolute(root);fs::create_directories(base);
        write(base/"program.bin","verified-room-program");write(base/"different.bin","another-build");
        write(base/"content"/"configfiles.ini","Gravity=9.81\n");write(base/"frozen"/"save.dat",std::string(96*1024,'S'));
        write(base/"normal"/"42560816"/"save.dat","original normal campaign");
        const auto original=PackCheckpoint(base/"frozen"),normal=PackCheckpoint(base/"normal"/"42560816");
        if(!check(original.value && normal.value,"fixture original campaigns packed before coordinator work"))return false;
        auto paths=[&](std::string_view name){return BootPaths{base/"program.bin",base/"content",base/std::string(name)};};
        std::vector<std::unique_ptr<OnlineRoomBoot>> boots;
        auto pump=[&](auto done){const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(8);
            do{for(auto& boot:boots)boot->Poll();if(done())return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(std::chrono::steady_clock::now()<until);
            for(const auto& boot:boots)if(!boot->status().error.empty())REXLOG_ERROR("Room boot fixture pending: {}",boot->status().error);return false;
        };
        {
            OnlineRoomBoot invalid;
            check(!invalid.SetGameplayVerificationWindow(0) && !invalid.SetGameplayVerificationWindow(9) &&
                invalid.SetGameplayVerificationWindow(8) && invalid.SetGameplayVerificationWindow(1),
                "verification credit configured before asynchronous preparation with strict bounds");
            check(!invalid.Host(paths("zero-replay"),base/"frozen","127.0.0.1","127.0.0.1",3,"Host",0,2,std::array<uint8_t,16>{}) &&
                invalid.status().state==BootState::Idle && !fs::exists(base/"zero-replay"),"explicit zero session identity rejects before workers or files");
            check(!invalid.Join(paths("invalid-invite"),"not an invitation","Guest") && !invalid.Host(paths("invalid-name"),base/"frozen","127.0.0.1","127.0.0.1",3,"bad\nname") &&
                !invalid.Host(paths("bad-capacity"),base/"frozen","127.0.0.1","127.0.0.1",17,"Host") &&
                !invalid.Host(paths("bad-delay"),base/"frozen","127.0.0.1","127.0.0.1",3,"Host",0,13) &&
                !invalid.Host(paths("bad-address"),base/"frozen","127.0.0.1","0.0.0.0",3,"Host") &&
                invalid.status().state==BootState::Idle && !fs::exists(base/"invalid-invite"),"invalid names/invites/capacity/delay/advertised endpoints rejected before asynchronous filesystem work");
        }
        for(unsigned bad=0;bad<3;++bad){
            boots.clear();boots.push_back(std::make_unique<OnlineRoomBoot>());auto config=paths("bad-path-"+std::to_string(bad));
            if(bad==0)config.new_user_data="relative-room-root";
            else if(bad==1)config.new_user_data=base/"content"/"forbidden-room-root";
            else {config.new_user_data=base/"existing-root";fs::create_directory(config.new_user_data);}
            check(boots[0]->Host(config,base/"frozen","127.0.0.1","127.0.0.1",3,"Host") &&
                pump([&]{return boots[0]->status().state==BootState::Failed;}) && !boots[0]->status().error.empty() &&
                !fs::exists(base/"content"/"forbidden-room-root"),"shared existing preparation rejects relative/nested/existing destination without selecting normal saves");
        }
        boots.clear();boots.push_back(std::make_unique<OnlineRoomBoot>());
        std::array<uint8_t,16> replay_nonce{};replay_nonce[0]=0xA5;
        if(!check(boots[0]->Host(paths("host"),base/"frozen","127.0.0.1","127.0.0.1",3,"Host",0,0,replay_nonce) &&
            pump([&]{return boots[0]->status().state==BootState::WaitingForPeer && !boots[0]->status().invitation.empty();}),"host worker verifies assets, packs and privately installs before publishing real invitation"))return false;
        const auto invitation=boots[0]->status().invitation;const auto decoded=DecodeInvite(invitation);
        check(decoded && decoded->input_delay==0 && decoded->session==replay_nonce && decoded->verification_window==8 && !boots[0]->TakeStarted(),"default host invitation carries window8, explicit session identity and exact zero delay; waiting host cannot hand off");
        {OnlineRoomBoot mismatch;
            check(mismatch.SetGameplayVerificationWindow(4) && !mismatch.Join(paths("window-mismatch"),invitation,"Guest") &&
                mismatch.status().state==BootState::Idle && !mismatch.status().error.empty() && !fs::exists(base/"window-mismatch"),
                "explicit window mismatch rejects clearly before preparation workers or filesystem changes");}
        check(!boots[0]->SetGameplayVerificationWindow(1),"running preparation cannot change negotiated verification credit");
        for(unsigned i=1;i<3;++i){boots.push_back(std::make_unique<OnlineRoomBoot>());check((i==1 || boots[i]->SetGameplayVerificationWindow(8)) && boots[i]->Join(paths("guest-"+std::to_string(i)),invitation,"Guest "+std::to_string(i)),"fresh guest adopts invited completion credit automatically; explicit matching override remains supported");}
        if(!check(pump([&]{
            const auto progress=boots[0]->checkpoint_progress();
            return std::all_of(boots.begin(),boots.end(),[](const auto& boot){return boot->status().state==BootState::Ready;}) && progress.size()==3 &&
                std::all_of(progress.begin(),progress.end(),[](const auto& peer){return peer.installed;});
        }),"three actual TCP peers independently install and acknowledge identical isolated saves"))return false;
        const auto peers=boots[0]->checkpoint_progress();
        check(peers.size()==3 && std::all_of(peers.begin(),peers.end(),[](const auto& peer){return peer.installed;}),"host progress reports every installed peer, not just TCP connectivity");
        check(!boots[0]->CanStart() && !boots[1]->Start() && !boots[2]->TakeStarted(),"installed lobby remains unstarted until explicit readiness and host action");
        check(boots[1]->Rename("Named guest") && pump([&]{return !boots[1]->profile_pending();}) && boots[0]->lobby().players[1].name=="Named guest","rename reaches shared host roster through coordinator");
        check(boots[2]->SetAppearance(LobbyCharacter::Mickey,0) && pump([&]{return !boots[2]->profile_pending();}) && boots[2]->last_lobby_result()==LobbyResult::ColorInUse,"coordinator preserves host color uniqueness rejection");
        check(boots[2]->SetAppearance(LobbyCharacter::Mickey,2) && pump([&]{return !boots[2]->profile_pending();}) && boots[2]->last_lobby_result()==LobbyResult::Ok,"coordinator supports explicit character and available pants/name color");
        for(auto& boot:boots)check(boot->SetReady(true),"each installed peer explicitly readies");
        if(!check(pump([&]{return boots[0]->CanStart() && !boots[1]->profile_pending() && !boots[2]->profile_pending();}) && boots[0]->Start(),"all-installed/all-ready host starts frozen native Loading"))return false;
        if(!check(pump([&]{return std::all_of(boots.begin(),boots.end(),[](const auto& boot){return boot->room_state()==RoomState::Loading;});}),"Start reaches each coordinator as Loading, not native-world activation"))return false;
        std::vector<RoomBootResult> results;
        for(unsigned i=0;i<3;++i){auto result=boots[i]->TakeStarted();
            if(!check(result && result->room && result->room->state()==RoomState::Loading && result->room->gameplay_verification_window()==8 && boots[i]->status().state==BootState::Taken && !boots[i]->TakeStarted(),"frozen Loading transport and negotiated completion credit handed off exactly once"))return false;
            check(result->user_data==paths(i?"guest-"+std::to_string(i):"host").new_user_data,"handoff retains the exact private worker save root");
            const auto installed=PackCheckpoint(result->user_data/"42560816");check(installed.value && installed.value->bytes==original.value->bytes,"host and every guest have byte-identical worker-installed checkpoint trees");
            boots[i]->Cancel();results.push_back(std::move(*result));
        }
        boots.clear(); // Destroy every old coordinator before using new owners.
        std::array<uint8_t,32> world{};world[0]=0x5A;
        for(auto& result:results){RoomFrame frame;check(!result.room->TakeFrame(frame) && result.room->WorldReady(world),"destruction preserves handed-off transport; explicit fixture world assertion still required");}
        auto room_pump=[&](auto done){const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(4);do{
            for(auto& result:results)result.room->Poll();if(done())return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }while(std::chrono::steady_clock::now()<until);return false;};
        if(!check(room_pump([&]{return std::all_of(results.begin(),results.end(),[](const auto& r){return r.room->state()==RoomState::Running;});}),"new owners explicitly activate matching fixture worlds after coordinator destruction"))return false;
        for(unsigned i=0;i<3;++i){RoomMovement movement;movement.source=RoomMovementSource::NativeActionV1;movement.actor_epoch=1;movement.x=float(i)*.25f;
            check(results[i].room->SubmitLocal(0,movement),"handed-off sockets retain all three assigned input owners");}
        check(room_pump([&]{RoomFrame frame;return std::all_of(results.begin(),results.end(),[&](const auto& r){return r.room->PeekFrame(frame);});}),"handed-off sockets deliver common authoritative frame");
        RoomFrame authoritative;check(results[0].room->TakeFrame(authoritative) && authoritative.players_mask==7 && authoritative.tick==0,"three-peer frozen mask survives ownership transfer");
        for(unsigned i=1;i<3;++i){RoomFrame frame;check(results[i].room->TakeFrame(frame) && frame==authoritative,"handed-off clients receive byte-identical native command frames");}
        std::array<uint8_t,32> fixture_digest{};fixture_digest[0]=0xB0;
        for(auto& result:results)check(result.room->CompleteGameplay(authoritative.tick,fixture_digest),"handed-off fixture command processing explicitly reports completion");
        check(room_pump([&]{return std::all_of(results.begin(),results.end(),[&](const auto& result){return !result.room->gameplay_completion_pending() &&
            result.room->verified_gameplay_tick()==authoritative.tick;});}),"three handed-off owners acknowledge same fixture frame before release");
        results.clear();
        boots.push_back(std::make_unique<OnlineRoomBoot>());
        auto native_paths=[&](std::string_view name){auto value=paths(name);value.startup_policy=StartupPolicy::NativeMenu;return value;};
        if(!check(boots[0]->SetGameplayVerificationWindow(4) && boots[0]->Host(native_paths("native-host"),base/"frozen","127.0.0.1","127.0.0.1",2,"Host",0,2) &&
            pump([&]{return !boots[0]->status().invitation.empty();}),"native bootstrap coordinator hosts its verified shared checkpoint"))return false;
        boots.push_back(std::make_unique<OnlineRoomBoot>());
        if(!check(boots[1]->Join(native_paths("native-guest"),boots[0]->status().invitation,"Guest") && pump([&]{
            const auto progress=boots[0]->checkpoint_progress();return progress.size()==2 &&
                std::all_of(progress.begin(),progress.end(),[](const auto& peer){return peer.installed;}) && boots[1]->status().state==BootState::Ready;
        }),"native bootstrap waits for worker installation before lobby readiness"))return false;
        if(!check(boots[0]->SetReady(true) && boots[1]->SetReady(true) && pump([&]{return boots[0]->CanStart() && !boots[1]->profile_pending();}) &&
            boots[0]->StartNativeBootstrap() && pump([&]{return boots[1]->room_state()==RoomState::Loading;}),"native bootstrap mode is chosen at the explicit host Start boundary"))return false;
        for(auto& boot:boots){auto result=boot->TakeStarted();
            if(!check(result && result->room->gameplay_verification_window()==4 && result->room->native_bootstrap() && !result->room->WorldReady(world) && result->room->BootstrapWorldReady(world),
                "native handoff preserves raw initialization mode and cannot use direct gameplay activation"))return false;
            results.push_back(std::move(*result));
        }
        boots.clear();
        check(room_pump([&]{return std::all_of(results.begin(),results.end(),[](const auto& r){return r.room->state()==RoomState::Bootstrapping;});}),
            "shared-save native handoff reaches only synchronized raw bootstrap after matching world assertions");
        results.clear();
        // Old captured W1 invitations remain usable without an explicit guest flag.
        boots.push_back(std::make_unique<OnlineRoomBoot>());
        if(!check(boots[0]->SetGameplayVerificationWindow(1) && boots[0]->Host(paths("legacy-window-host"),base/"frozen","127.0.0.1","127.0.0.1",2,"Host") &&
            pump([&]{return !boots[0]->status().invitation.empty();}),"explicit window1 host retains old invitation format"))return false;
        const auto old_invitation=boots[0]->status().invitation;const auto old_decoded=DecodeInvite(old_invitation);
        if(!check(old_invitation.size()==62 && old_decoded && old_decoded->verification_window==1,"old invitation decodes implicit window1"))return false;
        boots.push_back(std::make_unique<OnlineRoomBoot>());
        if(!check(boots[1]->Join(paths("legacy-window-guest"),old_invitation,"Guest") && pump([&]{return boots[1]->status().state==BootState::Ready;}) &&
            boots[0]->SetReady(true) && boots[1]->SetReady(true) && pump([&]{return boots[0]->CanStart() && !boots[1]->profile_pending();}) &&
            boots[0]->Start() && pump([&]{return boots[1]->room_state()==RoomState::Loading;}),"automatic join adopts old window1 through actual save installation and start"))return false;
        for(auto& boot:boots){auto result=boot->TakeStarted();if(!check(result && result->room->gameplay_verification_window()==1,
            "old invitation window survives real frozen handoff"))return false;results.push_back(std::move(*result));}
        boots.clear();results.clear();
        boots.push_back(std::make_unique<OnlineRoomBoot>());
        check(boots[0]->Host(paths("cancelled-host"),base/"frozen","127.0.0.1","127.0.0.1",3,"Host"),"cancellable preparation starts");boots[0]->Cancel();
        check(pump([&]{return boots[0]->status().state==BootState::Cancelled;}) && !boots[0]->TakeStarted(),"cancellation safely drains immutable preparation workers without handoff");
        boots.clear();boots.push_back(std::make_unique<OnlineRoomBoot>());
        if(!check(boots[0]->Host(paths("mismatch-host"),base/"frozen","127.0.0.1","127.0.0.1",3,"Host") &&
            pump([&]{return !boots[0]->status().invitation.empty();}),"build mismatch fixture hosts shared checkpoint"))return false;
        auto bad_build=paths("mismatch-guest");bad_build.executable=base/"different.bin";
        boots.push_back(std::make_unique<OnlineRoomBoot>());check(boots[1]->Join(bad_build,boots[0]->status().invitation,"Other build") &&
            pump([&]{return boots[1]->status().state==BootState::Failed;}) && boots[0]->room_state()==RoomState::Listening,"different verified executable is rejected before guest checkpoint installation");
        boots.pop_back();boots.push_back(std::make_unique<OnlineRoomBoot>());
        check(boots[1]->Join(paths("cancelled-guest"),boots[0]->status().invitation,"Cancelled guest"),"cancellable guest preparation starts");boots[1]->Cancel();
        check(pump([&]{return boots[1]->status().state==BootState::Cancelled;}) && !boots[1]->TakeStarted(),"guest worker cancellation never enters native Loading");
        boots.pop_back();boots.push_back(std::make_unique<OnlineRoomBoot>());
        if(!check(boots[1]->Join(paths("cancelled-install-guest"),boots[0]->status().invitation,"Installing guest") &&
            pump([&]{return boots[1]->status().state==BootState::Installing;}),"guest reaches explicit shared-save installation phase"))return false;
        boots[1]->Cancel();
        check(pump([&]{return boots[1]->status().state==BootState::Cancelled && boots[0]->room_state()==RoomState::Listening;}) &&
            !boots[1]->TakeStarted(),"installation-phase cancellation drains worker and releases lobby slot without native startup");
        boots.clear();
        {OnlineRoomBoot abandoned;check(abandoned.Host(paths("destructor-cancel"),base/"frozen","127.0.0.1","127.0.0.1",3,"Host"),"destructor cancellation fixture starts immutable worker");}
        auto versioned=[&](std::string_view directory,std::string_view version,std::string_view bytes){
            const auto root=base/std::string(directory);write(root/"beetleworx.exe",bytes);
            const std::string digest(64,'0');
            write(root/"release-manifest.json","{\"format\":2,\"product\":\"Beetleworx\",\"executable\":\"beetleworx.exe\",\"version\":\""+std::string(version)+"\",\"tag\":\"v"+std::string(version)+"\",\"exe_sha256\":\""+digest+"\",\"files\":[{\"path\":\"beetleworx.exe\",\"bytes\":3,\"sha256\":\""+digest+"\"}]}");
            return root/"beetleworx.exe";
        };
        auto release_host=paths("version-host-save");release_host.executable=versioned("version-host","0.1.0-demo.3","host executable");
        auto release_guest=paths("version-guest-save");release_guest.executable=versioned("version-guest","0.1.0-demo.3","different guest executable");
        boots.push_back(std::make_unique<OnlineRoomBoot>());
        if(!check(boots[0]->Host(release_host,base/"frozen","127.0.0.1","127.0.0.1",3,"Host") &&
            pump([&]{return !boots[0]->status().invitation.empty();}),"versioned host prepares shared checkpoint"))return false;
        boots.push_back(std::make_unique<OnlineRoomBoot>());
        if(!check(boots[1]->Join(release_guest,boots[0]->status().invitation,"Same version") &&
            pump([&]{return boots[1]->status().state==BootState::Ready;}) && fs::is_regular_file(release_guest.new_user_data/"42560816"/"save.dat"),
            "matching version with different executable bytes passes real handshake and checkpoint installation"))return false;
        boots.clear();boots.push_back(std::make_unique<OnlineRoomBoot>());release_host.new_user_data=base/"different-version-host-save";
        if(!check(boots[0]->Host(release_host,base/"frozen","127.0.0.1","127.0.0.1",3,"Host") &&
            pump([&]{return !boots[0]->status().invitation.empty();}),"different-version rejection fixture hosts"))return false;
        auto different_version=paths("different-version-guest-save");different_version.executable=versioned("different-version","0.1.0-demo.4","host executable");
        boots.push_back(std::make_unique<OnlineRoomBoot>());
        if(!check(boots[1]->Join(different_version,boots[0]->status().invitation,"Other version") &&
            pump([&]{return boots[1]->status().state==BootState::Failed;}) && !fs::exists(different_version.new_user_data/"42560816") &&
            boots[0]->room_state()==RoomState::Listening,"identical executable bytes with different version rejected before checkpoint installation"))return false;
        boots.clear();
        const auto after=PackCheckpoint(base/"frozen"),normal_after=PackCheckpoint(base/"normal"/"42560816");
        check(after.value && after.value->bytes==original.value->bytes && normal_after.value && normal_after.value->bytes==normal.value->bytes,"host source and normal campaign remain unchanged through success/failure/cancel/destruction");
    }catch(const std::exception& error){check(false,error.what());}
    if(ok)REXLOG_INFO("Online room boot passed {} checks: shared verified workers, real3-peer save download/install, profiles/readiness/frozen handoff, surviving socket ownership, explicit world barrier/frame, invalid preflight/paths/build and cancellation without normal-save changes",checks);
    return ok;
}
}
