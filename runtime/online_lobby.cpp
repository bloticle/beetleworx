#include "online_lobby.h"
#include <algorithm>
#include <rex/logging.h>

namespace em2::net {
namespace {
bool PlayerName(std::string_view input,std::string& output) {
    while(!input.empty() && input.front()==' ')input.remove_prefix(1);
    while(!input.empty() && input.back()==' ')input.remove_suffix(1);
    if(input.empty() || input.size()>96)return false;
    unsigned count=0;
    for(size_t i=0;i<input.size();) {
        const auto first=static_cast<uint8_t>(input[i++]);uint32_t code=first;unsigned extra=0;
        if(first>=0xC2 && first<=0xDF){code=first&31;extra=1;}
        else if(first>=0xE0 && first<=0xEF){code=first&15;extra=2;}
        else if(first>=0xF0 && first<=0xF4){code=first&7;extra=3;}
        else if(first>=0x80)return false;
        const unsigned length=extra+1;
        while(extra--) {
            if(i>=input.size())return false;
            auto next=static_cast<uint8_t>(input[i++]);if((next&0xC0)!=0x80)return false;
            code=(code<<6)|(next&63);
        }
        if((length==2 && code<0x80) || (length==3 && code<0x800) ||
            (length==4 && code<0x10000) || code>0x10FFFF ||
            (code>=0xD800 && code<=0xDFFF) || code<32 || (code>=127 && code<=159) ||
            code==0x2028 || code==0x2029 || ++count>24)return false;
    }
    output.assign(input);return true;
}
}
LobbyRoster::LobbyRoster(uint8_t capacity):capacity_(std::min<uint8_t>(capacity,uint8_t(kLobbyColors.size()))){}
const LobbyPlayer* LobbyRoster::Find(uint64_t peer) const {
    const auto it=std::find_if(players_.begin(),players_.end(),[&](const auto& p){return p.peer==peer;});
    return it==players_.end()?nullptr:&*it;
}
void LobbyRoster::Changed(bool invalidate) {
    ++revision_;if(invalidate)for(auto& player:players_)player.ready=false;
}
LobbyResult LobbyRoster::Join(uint64_t peer,std::string_view name) {
    if(locked_)return LobbyResult::Locked;
    if(!peer || Find(peer))return LobbyResult::DuplicatePeer;
    if(players_.size()>=capacity_)return LobbyResult::Full;
    std::string normalized;if(!PlayerName(name,normalized))return LobbyResult::InvalidName;
    uint8_t color=0;
    while(std::any_of(players_.begin(),players_.end(),[&](const auto& p){return p.color==color;}))++color;
    const auto character=players_.empty()?LobbyCharacter::Mickey:LobbyCharacter::Oswald;
    players_.push_back({peer,std::move(normalized),character,color,false});Changed(true);
    return LobbyResult::Ok;
}
LobbyResult LobbyRoster::Leave(uint64_t peer) {
    // A disconnected peer must always be removable, even during game startup.
    const auto it=std::find_if(players_.begin(),players_.end(),[&](const auto& p){return p.peer==peer;});
    if(it==players_.end())return LobbyResult::UnknownPeer;
    players_.erase(it);locked_=false;Changed(true);return LobbyResult::Ok;
}
LobbyResult LobbyRoster::Rename(uint64_t peer,std::string_view name) {
    if(locked_)return LobbyResult::Locked;
    auto player=const_cast<LobbyPlayer*>(Find(peer));if(!player)return LobbyResult::UnknownPeer;
    std::string normalized;if(!PlayerName(name,normalized))return LobbyResult::InvalidName;
    if(normalized!=player->name){player->name=std::move(normalized);Changed(false);}
    return LobbyResult::Ok;
}
LobbyResult LobbyRoster::SetAppearance(uint64_t peer,LobbyCharacter character,uint8_t color) {
    if(locked_)return LobbyResult::Locked;
    auto player=const_cast<LobbyPlayer*>(Find(peer));if(!player)return LobbyResult::UnknownPeer;
    if(character!=LobbyCharacter::Mickey && character!=LobbyCharacter::Oswald)return LobbyResult::InvalidCharacter;
    if(color>=kLobbyColors.size())return LobbyResult::InvalidColor;
    if(std::any_of(players_.begin(),players_.end(),[&](const auto& p){return p.peer!=peer && p.color==color;}))return LobbyResult::ColorInUse;
    if(character!=player->character || color!=player->color){player->character=character;player->color=color;Changed(true);}
    return LobbyResult::Ok;
}
LobbyResult LobbyRoster::SetReady(uint64_t peer,bool ready) {
    if(locked_)return LobbyResult::Locked;
    auto player=const_cast<LobbyPlayer*>(Find(peer));if(!player)return LobbyResult::UnknownPeer;
    if(player->ready!=ready){player->ready=ready;Changed(false);}return LobbyResult::Ok;
}
LobbyResult LobbyRoster::Start() {
    if(locked_)return LobbyResult::Locked;
    if(players_.size()<2 || !std::all_of(players_.begin(),players_.end(),[](const auto& p){return p.ready;}))return LobbyResult::NotReady;
    locked_=true;Changed(false);return LobbyResult::Ok;
}
void LobbyRoster::Unlock(){if(locked_){locked_=false;Changed(true);}}

bool TestOnlineLobby() {
    bool ok=true;auto check=[&](bool value,const char* message){if(!value){REXLOG_ERROR("Online lobby: {}",message);ok=false;}};
    LobbyRoster lobby(4);
    check(lobby.Join(1,"  Mickey fan  ")==LobbyResult::Ok && lobby.Find(1)->name=="Mickey fan","name normalization");
    check(lobby.Join(2,"Oswald fan")==LobbyResult::Ok && lobby.Join(3,"Third")==LobbyResult::Ok,"three distinct players join");
    check(lobby.Find(1)->character==LobbyCharacter::Mickey && lobby.Find(1)->color==0 &&
        lobby.Find(2)->character==LobbyCharacter::Oswald && lobby.Find(2)->color==1 &&
        lobby.Find(3)->character==LobbyCharacter::Oswald && lobby.Find(3)->color==2,"red Mickey, blue Oswald, green Oswald defaults");
    auto revision=lobby.revision();
    check(lobby.SetAppearance(3,LobbyCharacter::Mickey,0)==LobbyResult::ColorInUse && lobby.revision()==revision,"duplicate color rejected atomically");
    check(lobby.SetAppearance(3,LobbyCharacter::Mickey,7)==LobbyResult::Ok,"duplicate character permitted with distinct color");
    lobby.SetReady(1,true);lobby.SetReady(2,true);lobby.SetReady(3,true);
    lobby.SetAppearance(3,LobbyCharacter::Oswald,2);
    check(lobby.Start()==LobbyResult::NotReady && !lobby.Find(1)->ready,"appearance change invalidates ready agreement");
    lobby.SetReady(1,true);lobby.SetReady(2,true);lobby.SetReady(3,true);
    check(lobby.Start()==LobbyResult::Ok && lobby.locked() && lobby.Join(4,"Fourth")==LobbyResult::Locked &&
        lobby.SetAppearance(1,LobbyCharacter::Oswald,4)==LobbyResult::Locked,"start freezes agreed roster");
    check(lobby.Leave(2)==LobbyResult::Ok && !lobby.locked() && !lobby.Find(1)->ready,"disconnect unlocks and invalidates readiness");
    check(lobby.Join(4,"Fourth")==LobbyResult::Ok && lobby.Find(4)->color==1,"disconnected color reusable without recoloring others");
    check(lobby.Join(5,"Fifth")==LobbyResult::Ok && lobby.Join(6,"Sixth")==LobbyResult::Full,"gameplay capacity enforced");
    revision=lobby.revision();
    check(lobby.Rename(1,"\xC0\xAF")==LobbyResult::InvalidName && lobby.Rename(1,"bad\nname")==LobbyResult::InvalidName &&
        lobby.Rename(1,std::string(25,'a'))==LobbyResult::InvalidName && lobby.revision()==revision,"malformed UTF-8/control/length requests do not mutate roster");
    check(lobby.Rename(1,"\xE7\x8E\xA9\xE5\xAE\xB6")==LobbyResult::Ok,"valid international player names");
    check(lobby.SetAppearance(1,static_cast<LobbyCharacter>(99),3)==LobbyResult::InvalidCharacter &&
        lobby.SetAppearance(1,LobbyCharacter::Mickey,255)==LobbyResult::InvalidColor &&
        lobby.SetReady(99,true)==LobbyResult::UnknownPeer,"invalid enum/color/peer rejected");
    LobbyRoster maximum(255);for(uint64_t peer=1;peer<=16;++peer)check(maximum.Join(peer,"Player")==LobbyResult::Ok,"all sixteen palette entries unique");
    check(maximum.capacity()==16 && maximum.Join(17,"Extra")==LobbyResult::Full,"palette bounds cap roster capacity");
    for(uint64_t peer=1;peer<=16;++peer){
        const auto player=maximum.Find(peer);check(player && player->color==peer-1,"sixteen players receive distinct stable colors");
        const auto revision=maximum.revision();
        check(maximum.SetAppearance(peer,LobbyCharacter::Mickey,uint8_t(peer%16))==LobbyResult::ColorInUse && maximum.revision()==revision,
            "every occupied color is rejected without changing appearance or revision");
    }
    check(maximum.Leave(9)==LobbyResult::Ok && maximum.Join(17,"Replacement")==LobbyResult::Ok && maximum.Find(17)->color==8,
        "released extended-palette color is reused without recoloring other players");
    if(ok)REXLOG_INFO("Online lobby roster passed: unique colors, character choices, capacity, readiness/start locking, disconnect recovery and UTF-8 names (model only; native actors/replication require integration)");
    return ok;
}
}
