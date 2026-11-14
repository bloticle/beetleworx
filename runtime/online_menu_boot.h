#pragma once
#include "online_room_boot.h"
#include "online_invite.h"
namespace em2::net {
enum class NativeLobbyStartBlock {None,MissingSecondPlayer,UnsupportedCharacters};
struct NativeMenuReady {
    std::unique_ptr<Session> session;
    std::unique_ptr<Room> room;
    std::filesystem::path user_data;
    const Identity& identity()const{return room?room->identity():session->identity();}
};
// Native UI migration adapter. The Room mode uses the actual larger-player
// coordinator and local peer identity; the legacy path stays selectable for
// comparison until authored reload/gameplay acceptance is established.
class OnlineMenuBoot {
    bool rooms_,host_=false;
    OnlineBoot legacy_;
    OnlineRoomBoot room_;
public:
    explicit OnlineMenuBoot(bool rooms):rooms_(rooms){}
    bool uses_room()const{return rooms_;}
    bool Host(BootPaths paths,const std::filesystem::path& source,std::string listen,std::string advertised,uint16_t port,uint8_t delay,std::optional<std::array<uint8_t,16>> nonce){
        host_=true;
        return rooms_?room_.Host(std::move(paths),source,std::move(listen),std::move(advertised),16,"Player 1",port,delay,nonce):legacy_.Host(std::move(paths),source,std::move(listen),std::move(advertised),port,delay,nonce);
    }
    bool Join(BootPaths paths,std::string invitation){
        host_=false;if(!rooms_)return legacy_.Join(std::move(paths),std::move(invitation));
        // Room names are host-enforced unique. A fixed "Player" prevents the
        // second guest from joining before they can even reach the name editor.
        std::array<uint8_t,16> nonce{};if(!NewSessionNonce(nonce))return false;
        constexpr char hex[]="0123456789ABCDEF";std::string name="Player ";
        for(unsigned i=0;i<4;++i){name+=hex[nonce[i]>>4];name+=hex[nonce[i]&15];}
        return room_.Join(std::move(paths),std::move(invitation),std::move(name));
    }
    void Poll(){
        if(!rooms_){legacy_.Poll();return;}room_.Poll();
        // The authored host row is Start, rather than a separate Ready vote.
        // Keep host consent through that explicit Start action, but supply the
        // host's installed-save vote after profile changes invalidate readiness.
        if(host_ && room_.status().state==BootState::Ready && !room_.profile_pending()){
            const auto roster=room_.lobby();for(const auto& p:roster.players)
                if(p.peer==room_.local_peer() && !p.ready && !roster.locked)room_.SetReady(true);
        }
    }
    void Cancel(){if(rooms_)room_.Cancel();else legacy_.Cancel();}
    BootStatus status()const{return rooms_?room_.status():legacy_.status();}
    LobbySnapshot lobby()const{return rooms_?room_.lobby():legacy_.lobby();}
    uint64_t local_peer()const{return rooms_?room_.local_peer():(host_?1:2);}
    bool RenameLobbyPlayer(std::string_view s){return rooms_?room_.Rename(s):legacy_.RenameLobbyPlayer(s);}
    bool SetLobbyReady(bool v){return rooms_?room_.SetReady(v):legacy_.SetLobbyReady(v);}
    bool lobby_request_pending()const{return rooms_?room_.profile_pending():legacy_.lobby_request_pending();}
    LobbyResult last_lobby_result()const{return rooms_?room_.last_lobby_result():legacy_.last_lobby_result();}
    bool ArmLobbyStart(){return rooms_?room_.SetReady(true):legacy_.ArmLobbyStart();}
    NativeLobbyStartBlock native_start_block()const{
        if(!rooms_)return NativeLobbyStartBlock::None;
        const auto roster=room_.lobby();
        if(roster.players.size()<2 || roster.players[0].peer!=1 || roster.players[1].peer!=2)return NativeLobbyStartBlock::MissingSecondPlayer;
        if(roster.players[0].character!=LobbyCharacter::Mickey || roster.players[1].character!=LobbyCharacter::Oswald)return NativeLobbyStartBlock::UnsupportedCharacters;
        return NativeLobbyStartBlock::None;
    }
    bool CanRequestLobbyStart()const{return rooms_?(native_start_block()==NativeLobbyStartBlock::None && room_.CanStart()):legacy_.CanRequestLobbyStart();}
    bool RequestLobbyStart(){return rooms_?(CanRequestLobbyStart() && room_.StartNativeBootstrap()):legacy_.RequestLobbyStart();}
    bool LobbyStartConfirmed()const{return rooms_?room_.room_state()==RoomState::Loading:legacy_.LobbyStartConfirmed();}
    bool SetAppearance(LobbyCharacter c,uint8_t color){return rooms_ && room_.SetAppearance(c,color);}
    std::optional<NativeMenuReady> TakeReady(){
        if(rooms_){auto r=room_.TakeStarted();if(r)return NativeMenuReady{{},std::move(r->room),std::move(r->user_data)};}
        else {auto r=legacy_.TakeReady();if(r)return NativeMenuReady{std::move(r->session),{},std::move(r->user_data)};}
        return {};
    }
};
bool TestOnlineMenuBoot(const std::filesystem::path& fixture);
}
