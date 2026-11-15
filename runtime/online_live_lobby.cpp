#include "online_live_lobby.h"
#include <windows.h>
#include <bcrypt.h>
#include <map>
#include <thread>
#include <algorithm>

namespace em2::net {
namespace {
bool Ticket(RejoinTicket& ticket){return BCryptGenRandom(nullptr,ticket.data(),ULONG(ticket.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0 && std::any_of(ticket.begin(),ticket.end(),[](auto x){return x!=0;});}
bool Same(const RejoinTicket& a,const RejoinTicket& b){uint8_t difference=0,any=0;for(unsigned i=0;i<a.size();++i){difference|=a[i]^b[i];any|=a[i];}return !difference && any;}
}
struct LiveLobby::Impl {
    struct Member {
        struct Proof {RollbackScope scope;uint64_t tick=0,revision=0;};
        uint64_t id=0;RejoinTicket ticket{};LivePresence presence=LivePresence::PendingSnapshot;
        bool avatar=false;Clock::time_point expires{};std::optional<Proof> staged;
        uint64_t departure_revision=0,departure_world=0;
    };
    uint8_t capacity;std::chrono::seconds grace;LobbyRoster roster;
    std::map<unsigned,Member> members;uint64_t next_id=1;
    bool opened=false;std::thread::id owner;
    std::optional<RollbackSnapshot> latest;std::shared_ptr<const RollbackBytes> packet;
    Impl(uint8_t c,std::chrono::seconds g):capacity(c),grace(g),roster(c){}
    ~Impl(){for(auto& [slot,m]:members)SecureZeroMemory(m.ticket.data(),m.ticket.size());}
    bool Owner()const{return owner==std::this_thread::get_id();}
    bool Available()const{return opened && Owner();}
    LiveAdmission Admission(unsigned slot,const Member& m)const{return {uint8_t(slot),m.id,m.ticket};}
    LiveLobbyResult Depart(unsigned slot){
        if(!Available())return LiveLobbyResult::Closed;if(slot==1)return LiveLobbyResult::Invalid;
        const auto found=members.find(slot);if(found==members.end())return LiveLobbyResult::UnknownPlayer;
        auto& m=found->second;if(m.presence==LivePresence::Leaving)return LiveLobbyResult::Accepted;
        m.presence=LivePresence::Leaving;m.ticket={};m.staged.reset();m.departure_revision=latest->scope.roster_revision;m.departure_world=latest->scope.world_epoch;
        return LiveLobbyResult::Accepted;
    }
};
LiveLobby::LiveLobby(uint8_t capacity,std::chrono::seconds grace):impl_(std::make_unique<Impl>(std::clamp<uint8_t>(capacity,2,16),std::clamp(grace,std::chrono::seconds(1),std::chrono::seconds(600)))){}
LiveLobby::~LiveLobby()=default;
LiveLobbyResult LiveLobby::Open(RollbackSession& host,std::string_view name)try{
    auto& s=*impl_;if(s.opened)return LiveLobbyResult::WrongState;
    auto packet=host.ExportAuthoritative();if(!packet)return LiveLobbyResult::Invalid;
    auto snapshot=DecodeRollbackSnapshot(*packet,16*1024*1024);
    // Existing local companions remain native world state; remote admission
    // starts from one actual host, rather than inventing online participants.
    if(!snapshot || snapshot->scope.players_mask!=1 || s.next_id==UINT64_MAX)return LiveLobbyResult::Invalid;
    LobbyRoster roster(s.capacity);if(roster.Join(1,name)!=LobbyResult::Ok)return LiveLobbyResult::Invalid;
    auto shared=std::make_shared<const RollbackBytes>(std::move(*packet));
    Impl::Member host_member;host_member.id=s.next_id;host_member.avatar=true;host_member.presence=LivePresence::Active;
    std::map<unsigned,Impl::Member> members;members.emplace(1,std::move(host_member));
    s.owner=std::this_thread::get_id();s.latest=std::move(snapshot);s.packet=std::move(shared);
    s.roster=std::move(roster);s.members=std::move(members);++s.next_id;s.opened=true;return LiveLobbyResult::Accepted;
}catch(...){return LiveLobbyResult::Invalid;}
LiveLobbyResult LiveLobby::Publish(std::span<const uint8_t> bytes)try{
    auto& s=*impl_;if(!s.Available())return LiveLobbyResult::Closed;
    auto snapshot=DecodeRollbackSnapshot(bytes,16*1024*1024);if(!snapshot)return LiveLobbyResult::Invalid;
    const auto& old=*s.latest;const auto next=snapshot->scope;
    if(next.session!=old.scope.session || next.schema!=old.scope.schema || next.world_epoch<old.scope.world_epoch || !(next.players_mask&1))return LiveLobbyResult::Invalid;
    if(next.world_epoch==old.scope.world_epoch && (next.roster_revision<old.scope.roster_revision ||
       (next.roster_revision==old.scope.roster_revision && (snapshot->revision<old.revision || snapshot->tick<old.tick))))return LiveLobbyResult::StaleSnapshot;
    if(next.world_epoch==old.scope.world_epoch && next.roster_revision==old.scope.roster_revision && next.players_mask!=old.scope.players_mask)return LiveLobbyResult::StaleSnapshot;
    for(unsigned slot=1;slot<=16;++slot)if((next.players_mask&(1u<<(slot-1))) && !s.members.contains(slot))return LiveLobbyResult::UnknownPlayer;
    const bool changed=next!=old.scope;
    auto copy=std::make_shared<const RollbackBytes>(bytes.begin(),bytes.end());s.latest=std::move(snapshot);s.packet=std::move(copy);
    const auto deadline=Clock::now()+std::chrono::seconds(30);
    for(auto& [slot,m]:s.members){
        if(next.players_mask&(1u<<(slot-1)))m.avatar=true;
        if(changed && slot!=1){m.staged.reset();if(m.presence==LivePresence::Active){m.presence=LivePresence::PendingSnapshot;m.expires=deadline;}}
    }
    return LiveLobbyResult::Accepted;
}catch(...){return LiveLobbyResult::Invalid;}
LiveLobbyResult LiveLobby::Join(std::string_view name,LiveAdmission& out)try{
    out={};auto& s=*impl_;if(!s.Available())return LiveLobbyResult::Closed;
    unsigned slot=2;for(;slot<=s.capacity && s.members.contains(slot);++slot){}
    if(slot>s.capacity)return LiveLobbyResult::Full;if(s.next_id==UINT64_MAX)return LiveLobbyResult::Invalid;
    Impl::Member member;if(!Ticket(member.ticket))return LiveLobbyResult::Invalid;
    auto roster=s.roster;const auto result=roster.Join(slot,name);if(result!=LobbyResult::Ok)return result==LobbyResult::Full?LiveLobbyResult::Full:LiveLobbyResult::Invalid;
    member.id=s.next_id;member.expires=Clock::now()+std::chrono::seconds(30);
    const auto admission=s.Admission(slot,member);s.members.emplace(slot,std::move(member));
    s.roster=std::move(roster);++s.next_id;out=admission;return LiveLobbyResult::Accepted;
}catch(...){out={};return LiveLobbyResult::Invalid;}
LiveLobbyResult LiveLobby::Drop(unsigned slot,Clock::time_point now){
    auto& s=*impl_;if(!s.Available())return LiveLobbyResult::Closed;if(slot==1)return LiveLobbyResult::Invalid;
    const auto found=s.members.find(slot);if(found==s.members.end())return LiveLobbyResult::UnknownPlayer;
    auto& m=found->second;if(m.presence==LivePresence::Leaving)return LiveLobbyResult::WrongState;
    if(m.presence==LivePresence::Reconnecting)return LiveLobbyResult::Accepted; // Repeated drop cannot extend the deadline.
    m.presence=LivePresence::Reconnecting;m.expires=now+s.grace;m.staged.reset();return LiveLobbyResult::Accepted;
}
LiveLobbyResult LiveLobby::Resume(unsigned slot,const RejoinTicket& ticket,Clock::time_point now,LiveAdmission& out){
    out={};auto& s=*impl_;if(!s.Available())return LiveLobbyResult::Closed;
    const auto found=s.members.find(slot);if(found==s.members.end() || slot==1)return LiveLobbyResult::UnknownPlayer;
    auto& m=found->second;if(m.presence!=LivePresence::Reconnecting || !Same(ticket,m.ticket))return LiveLobbyResult::WrongState;
    if(now>=m.expires){s.Depart(slot);return LiveLobbyResult::Expired;}
    RejoinTicket next;if(!Ticket(next))return LiveLobbyResult::Invalid;
    m.ticket=next;m.presence=LivePresence::PendingSnapshot;m.expires=now+std::chrono::seconds(30);m.staged.reset();out=s.Admission(slot,m);return LiveLobbyResult::Accepted;
}
std::optional<LiveJoinSnapshot> LiveLobby::SnapshotFor(unsigned slot){
    auto& s=*impl_;if(!s.Available())return {};
    auto found=s.members.find(slot);if(found==s.members.end() || found->second.presence!=LivePresence::PendingSnapshot || !(s.latest->scope.players_mask&(1u<<(slot-1))))return {};
    auto& m=found->second;m.avatar=true;m.staged=Impl::Member::Proof{s.latest->scope,s.latest->tick,s.latest->revision};return LiveJoinSnapshot{s.Admission(slot,m),s.packet};
}
LiveLobbyResult LiveLobby::Acknowledge(unsigned slot,const RejoinTicket& ticket,const RollbackScope& scope,uint64_t tick,uint64_t revision){
    auto& s=*impl_;if(!s.Available())return LiveLobbyResult::Closed;
    const auto found=s.members.find(slot);if(found==s.members.end())return LiveLobbyResult::UnknownPlayer;
    auto& m=found->second;if(m.presence!=LivePresence::PendingSnapshot || !m.staged || !Same(ticket,m.ticket))return LiveLobbyResult::WrongState;
    if(m.staged->scope!=scope || scope!=s.latest->scope || tick!=m.staged->tick || revision!=m.staged->revision)return LiveLobbyResult::StaleSnapshot;
    m.presence=LivePresence::Active;m.staged.reset();return LiveLobbyResult::Accepted;
}
LiveLobbyResult LiveLobby::Leave(unsigned slot){return impl_->Depart(slot);}
LiveLobbyResult LiveLobby::Kick(unsigned slot){return impl_->Depart(slot);}
LiveLobbyResult LiveLobby::CommitDeparture(unsigned slot){
    auto& s=*impl_;if(!s.Available())return LiveLobbyResult::Closed;
    const auto found=s.members.find(slot);if(found==s.members.end())return LiveLobbyResult::UnknownPlayer;
    const auto& m=found->second;if(m.presence!=LivePresence::Leaving)return LiveLobbyResult::WrongState;
    if((s.latest->scope.players_mask&(1u<<(slot-1))) || (m.avatar && s.latest->scope.world_epoch==m.departure_world && s.latest->scope.roster_revision<=m.departure_revision))return LiveLobbyResult::StaleSnapshot;
    s.roster.Leave(slot);s.members.erase(found);return LiveLobbyResult::Accepted;
}
void LiveLobby::Expire(Clock::time_point now){auto& s=*impl_;if(!s.Available())return;for(auto& [slot,m]:s.members)if((m.presence==LivePresence::Reconnecting || m.presence==LivePresence::PendingSnapshot) && now>=m.expires)s.Depart(slot);}
LiveLobbyResult LiveLobby::SetAppearance(unsigned slot,LobbyCharacter character,uint8_t color){
    auto& s=*impl_;if(!s.Available())return LiveLobbyResult::Closed;
    const auto found=s.members.find(slot);if(found==s.members.end())return LiveLobbyResult::UnknownPlayer;
    if(found->second.presence!=LivePresence::PendingSnapshot && found->second.presence!=LivePresence::Active)return LiveLobbyResult::WrongState;
    const auto* old=s.roster.Find(slot);if(found->second.avatar && old && old->character!=character)return LiveLobbyResult::WrongState;
    const auto result=s.roster.SetAppearance(slot,character,color);return result==LobbyResult::Ok?LiveLobbyResult::Accepted:result==LobbyResult::ColorInUse?LiveLobbyResult::ColorInUse:LiveLobbyResult::Invalid;
}
std::vector<LiveParticipant> LiveLobby::players()const{
    const auto& s=*impl_;std::vector<LiveParticipant> out;if(!s.Available())return out;
    for(const auto& p:s.roster.players()){const auto& m=s.members.at(unsigned(p.peer));out.push_back({p,m.id,m.presence});}return out;
}
LiveLobbyResult LiveLobby::PrepareHostFrame(RollbackSession& host){
    auto& s=*impl_;if(!s.Available())return LiveLobbyResult::Closed;
    const auto scope=host.scope();if(scope!=s.latest->scope)return LiveLobbyResult::StaleSnapshot;
    for(unsigned slot=1;slot<=16;++slot)if(scope.players_mask&(1u<<(slot-1))){
        const auto member=s.members.find(slot);if(member==s.members.end())return LiveLobbyResult::UnknownPlayer;
        const auto result=host.SetPeerConnected(slot,member->second.presence==LivePresence::Active);
        if(result!=RollbackResult::Accepted && result!=RollbackResult::Duplicate)return LiveLobbyResult::Invalid;
    }
    return LiveLobbyResult::Accepted;
}
bool LiveLobby::open()const{return impl_->Available();}
void LiveLobby::Close(){auto& s=*impl_;if(!s.Available())return;for(auto& [slot,m]:s.members)SecureZeroMemory(m.ticket.data(),m.ticket.size());s.members.clear();s.packet.reset();s.latest.reset();s.roster=LobbyRoster(s.capacity);s.opened=false;}
}
