#include "online_menu_boot.h"
#include "online_checkpoint.h"
#include <rex/logging.h>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace em2::net {
bool TestOnlineMenuBoot(const std::filesystem::path& fixture){
    unsigned checks=0;
    auto check=[&](bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);};
    try{
        const auto root=std::filesystem::absolute(fixture);
        auto write=[](const auto& file,std::string_view bytes){std::filesystem::create_directories(file.parent_path());std::ofstream out(file,std::ios::binary);out.write(bytes.data(),bytes.size());if(!out)throw std::runtime_error("Menu fixture write failed");};
        write(root/"program.bin","room-menu-test");write(root/"content"/"configfiles.ini","gravity=9.81\n");write(root/"source"/"save.dat",std::string(32768,'S'));
        const auto before=PackCheckpoint(root/"source");check(bool(before.value),"Source fixture packs");
        std::vector<std::unique_ptr<OnlineMenuBoot>> peers;
        auto paths=[&](unsigned peer){return BootPaths{root/"program.bin",root/"content",root/("user-"+std::to_string(peer)),StartupPolicy::NativeMenu};};
        auto pump=[&](auto done){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(8);
            do{for(auto& p:peers)p->Poll();if(done())return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(std::chrono::steady_clock::now()<end);
            for(const auto& p:peers){const auto s=p->status();REXLOG_ERROR("Native Room menu fixture pending: phase {} local {} players {} profile pending {} result {} error '{}'",int(s.state),p->local_peer(),p->lobby().players.size(),p->lobby_request_pending(),int(p->last_lobby_result()),s.error);}return false;};
        peers.push_back(std::make_unique<OnlineMenuBoot>(true));
        check(peers[0]->Host(paths(1),root/"source","127.0.0.1","127.0.0.1",0,2,{}),"Room menu host starts");
        check(pump([&]{return !peers[0]->status().invitation.empty();}),"Room menu host publishes invitation");
        const auto invite=peers[0]->status().invitation;
        for(unsigned i=1;i<3;++i){peers.push_back(std::make_unique<OnlineMenuBoot>(true));check(peers.back()->Join(paths(i+1),invite),"Room menu guest joins");}
        check(pump([&]{return std::all_of(peers.begin(),peers.end(),[](const auto& p){return p->status().state==BootState::Ready && p->lobby().players.size()==3;});}),"All menu peers install shared save and see three players");
        // Concurrent verification workers need not finish in launcher order.
        std::sort(peers.begin(),peers.end(),[](const auto& a,const auto& b){return a->local_peer()<b->local_peer();});
        for(unsigned i=0;i<3;++i)check(peers[i]->local_peer()==i+1 && peers[i]->lobby().capacity==16,"Native menu keeps actual peer identity and Room capacity");
        check(!peers[0]->CanRequestLobbyStart() && !peers[0]->TakeReady(),"Unready guests cannot launch or hand off");
        check(peers[2]->SetAppearance(LobbyCharacter::Mickey,2),"Extra menu player selects Mickey");
        check(pump([&]{return !peers[2]->lobby_request_pending() && peers[0]->lobby().players[2].character==LobbyCharacter::Mickey;}),"Host acknowledges extra character choice");
        check(peers[2]->SetAppearance(LobbyCharacter::Mickey,0),"Conflicting color request reaches authority");
        check(pump([&]{return !peers[2]->lobby_request_pending() && peers[2]->last_lobby_result()==LobbyResult::ColorInUse;}),"Host rejects duplicate color");
        check(peers[0]->lobby().players[2].color==2,"Rejected color does not replace current choice");
        check(peers[2]->RenameLobbyPlayer("Third player"),"Third peer edits its own name");
        check(pump([&]{return !peers[2]->lobby_request_pending() && peers[0]->lobby().players[2].name=="Third player";}),"Third name does not overwrite player two");
        check(peers[1]->SetAppearance(LobbyCharacter::Mickey,1),"Unsupported native retail choice reaches normal Room authority");
        check(pump([&]{return !peers[1]->lobby_request_pending() && peers[0]->lobby().players[1].character==LobbyCharacter::Mickey;}),"Room profile update is acknowledged before native capability gate");
        check(peers[1]->ArmLobbyStart() && peers[2]->ArmLobbyStart(),"Unsupported roster may still have ordinary Room ready votes");
        check(pump([&]{const auto roster=peers[0]->lobby();return std::all_of(roster.players.begin(),roster.players.end(),[](const auto& p){return p.ready;});}),"Unsupported roster is otherwise fully ready");
        check(peers[0]->native_start_block()==NativeLobbyStartBlock::UnsupportedCharacters && !peers[0]->CanRequestLobbyStart() && !peers[0]->RequestLobbyStart() && !peers[0]->TakeReady(),"Native menu rejects unsupported characters before freezing or save handoff");
        check(peers[1]->SetAppearance(LobbyCharacter::Oswald,1),"Second player restores native profile");
        check(pump([&]{return !peers[1]->lobby_request_pending() && peers[0]->native_start_block()==NativeLobbyStartBlock::None;}),"Restored profiles clear capability block");
        peers[1]->Cancel();
        check(pump([&]{return peers[1]->status().state==BootState::Cancelled && peers[0]->lobby().players.size()==2 && peers[2]->lobby().players.size()==2;}),"Second player leaves while third remains connected");
        check(peers[2]->ArmLobbyStart(),"Remaining third player can acknowledge readiness");
        check(pump([&]{const auto roster=peers[0]->lobby();return !peers[2]->lobby_request_pending() && std::all_of(roster.players.begin(),roster.players.end(),[](const auto& p){return p.ready;});}),"Roster with a peer gap is otherwise ready");
        check(peers[0]->native_start_block()==NativeLobbyStartBlock::MissingSecondPlayer && !peers[0]->CanRequestLobbyStart() && !peers[0]->RequestLobbyStart() && !peers[0]->TakeReady(),"Missing native player two cannot produce failed save handoff");
        peers[1]=std::make_unique<OnlineMenuBoot>(true);
        check(peers[1]->Join(paths(4),invite),"Replacement player joins same lobby without replacing its source save");
        check(pump([&]{return std::all_of(peers.begin(),peers.end(),[](const auto& p){return p->status().state==BootState::Ready && p->lobby().players.size()==3;});}),"Replacement installs shared save and restores complete roster");
        check(peers[1]->local_peer()==2 && peers[0]->native_start_block()==NativeLobbyStartBlock::None && peers[0]->lobby().players[2].name=="Third player", "Replacement fills native slot without renaming or displacing third player");
        check(!peers[0]->lobby().players[1].ready && !peers[0]->CanRequestLobbyStart(),"Replacement cannot inherit departed player's ready vote");
        check(peers[1]->ArmLobbyStart() && peers[2]->ArmLobbyStart(),"Guest Ready uses real Room votes");
        check(pump([&]{return peers[0]->CanRequestLobbyStart();}),"Installed complete roster becomes startable");
        check(peers[0]->RequestLobbyStart(),"Host requests native Room bootstrap");
        check(pump([&]{return std::all_of(peers.begin(),peers.end(),[](const auto& p){return p->LobbyStartConfirmed();});}),"All menu clients confirm frozen native startup");
        std::vector<NativeMenuReady> owned;
        for(unsigned i=0;i<3;++i){auto result=peers[i]->TakeReady();check(result && result->room && !result->session && result->room->local_peer()==i+1 && result->room->native_bootstrap() && result->room->state()==RoomState::Loading,"Native menu returns correct exclusive Room owner");
            check(!peers[i]->TakeReady(),"Frozen Room handed off once");owned.push_back(std::move(*result));}
        peers.clear();check(std::all_of(owned.begin(),owned.end(),[](const auto& r){return r.room->lobby().players.size()==3 && r.room->lobby().locked;}),"Menu coordinator destruction retains frozen transports");
        const auto after=PackCheckpoint(root/"source");check(after.value && after.value->bytes==before.value->bytes,"Native menu preparation preserves source campaign");
        REXLOG_INFO("Native Room menu adapter passed {} real TCP/save/profile/color/readiness/exclusive-handoff checks; authored native reload and gameplay acceptance separate",checks);return true;
    }catch(const std::exception& e){REXLOG_ERROR("Native Room menu adapter test: {}",e.what());return false;}
}
}
