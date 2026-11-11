#include "online_address.h"
#include "online_room.h"
#include <rex/logging.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <thread>

namespace em2::net {
namespace {
using Clock=std::chrono::steady_clock;
using Bytes=std::vector<uint8_t>;
constexpr size_t Window=64,MaxPacket=8192,MaxQueued=256*1024;
constexpr auto Timeout=std::chrono::seconds(15);
constexpr auto StartupTimeout=std::chrono::seconds(180);
enum class Wire:uint8_t {Hello=1,Welcome,Profile,Snapshot,Start,Input,Frame,Ping,Pong,ProfileReply,WorldReady,Activate};
bool Winsock(){
    struct Startup {bool ok=false;Startup(){WSADATA data{};ok=WSAStartup(MAKEWORD(2,2),&data)==0;}~Startup(){if(ok)WSACleanup();}};
    static Startup startup;return startup.ok;
}
void Close(SOCKET& socket){if(socket!=INVALID_SOCKET){closesocket(socket);socket=INVALID_SOCKET;}}
void U8(Bytes& b,uint8_t n){b.push_back(n);}
void U64(Bytes& b,uint64_t n,unsigned count=8){for(unsigned i=count;i;--i)b.push_back(uint8_t(n>>(8*(i-1))));}
void Text(Bytes& b,std::string_view s){U8(b,uint8_t(s.size()));b.insert(b.end(),s.begin(),s.end());}
struct Reader {
    std::span<const uint8_t> bytes;size_t at=0;bool good=true;
    uint64_t Number(unsigned count){if(!good || count>bytes.size()-at){good=false;return 0;}uint64_t n=0;while(count--)n=(n<<8)|bytes[at++];return n;}
    uint8_t Byte(){return uint8_t(Number(1));}
    std::string String(){const auto n=Byte();if(!good || n>bytes.size()-at){good=false;return {};}std::string s(reinterpret_cast<const char*>(bytes.data()+at),n);at+=n;return s;}
    bool Done()const{return good && at==bytes.size();}
};
Bytes Packet(Wire kind){return {'B','W','R','M',1,uint8_t(kind)};}
void WriteIdentity(Bytes& b,const Identity& id){
    for(const auto* p:{&id.build,&id.content,&id.checkpoint})b.insert(b.end(),p->begin(),p->end());
    b.insert(b.end(),id.session.begin(),id.session.end());U64(b,id.first_tick);U8(b,id.input_delay);
    U8(b,uint8_t(id.startup_policy));U8(b,uint8_t(id.initialization_policy));
}
Identity ReadIdentity(Reader& r){
    Identity id;for(auto* p:{&id.build,&id.content,&id.checkpoint})for(auto& n:*p)n=r.Byte();
    for(auto& n:id.session)n=r.Byte();id.first_tick=r.Number(8);id.input_delay=r.Byte();
    id.startup_policy=StartupPolicy(r.Byte());id.initialization_policy=InitializationPolicy(r.Byte());return id;
}
bool ValidIdentity(const Identity& id){
    const auto nonzero=[](const auto& bytes){return std::any_of(bytes.begin(),bytes.end(),[](auto n){return n!=0;});};
    return id.input_delay<=12 && id.first_tick<=UINT64_MAX-Window && uint8_t(id.startup_policy)<=1 &&
        uint8_t(id.initialization_policy)<=1 && nonzero(id.session) && nonzero(id.build) && nonzero(id.content) && nonzero(id.checkpoint);
}
bool Name(std::string_view text,std::string& normalized){
    LobbyRoster check(2);if(check.Join(1,text)!=LobbyResult::Ok)return false;normalized=check.players()[0].name;return true;
}
bool UniqueName(const LobbySnapshot& lobby,uint64_t except,std::string_view name){
    return std::none_of(lobby.players.begin(),lobby.players.end(),[&](const auto& p){return p.peer!=except && p.name==name;});
}
bool ValidCommand(const RoomCommand& c){return c.actor_epoch==1 && ValidRoomMovement(c);}
RoomCommand Neutral(){RoomCommand c;c.actor_epoch=1;c.source=RoomMovementSource::NativeActionV1;return c;}
void WriteCommand(Bytes& b,const RoomCommand& c){
    for(float n:{c.x,c.y,c.forward_x,c.forward_z})U64(b,std::bit_cast<uint32_t>(n),4);
    U64(b,c.actor_epoch);U8(b,uint8_t(c.source));
    const auto& p=c.pad;U64(b,p.buttons,2);U8(b,p.left_trigger);U8(b,p.right_trigger);
    for(auto n:{p.left_x,p.left_y,p.right_x,p.right_y})U64(b,uint16_t(n),2);
}
RoomCommand ReadCommand(Reader& r){
    RoomCommand c;c.x=std::bit_cast<float>(uint32_t(r.Number(4)));c.y=std::bit_cast<float>(uint32_t(r.Number(4)));
    c.forward_x=std::bit_cast<float>(uint32_t(r.Number(4)));c.forward_z=std::bit_cast<float>(uint32_t(r.Number(4)));
    c.actor_epoch=r.Number(8);c.source=RoomMovementSource(r.Byte());
    auto& p=c.pad;p.buttons=uint16_t(r.Number(2));p.left_trigger=r.Byte();p.right_trigger=r.Byte();
    for(auto* n:{&p.left_x,&p.left_y,&p.right_x,&p.right_y})*n=std::bit_cast<int16_t>(uint16_t(r.Number(2)));return c;
}
LobbySnapshot Snapshot(const LobbyRoster& roster){
    LobbySnapshot result{roster.revision(),roster.capacity(),roster.locked(),roster.players()};
    std::sort(result.players.begin(),result.players.end(),[](const auto& a,const auto& b){return a.peer<b.peer;});return result;
}
void WriteRoster(Bytes& b,const LobbySnapshot& s){
    U64(b,s.revision);U8(b,s.capacity);U8(b,s.locked);U8(b,uint8_t(s.players.size()));
    for(const auto& p:s.players){U64(b,p.peer);Text(b,p.name);U8(b,uint8_t(p.character));U8(b,p.color);U8(b,p.ready);}
}
std::optional<LobbySnapshot> ReadRoster(Reader& r){
    LobbySnapshot s;s.revision=r.Number(8);s.capacity=r.Byte();const auto locked=r.Byte();s.locked=locked!=0;const auto count=r.Byte();
    if(locked>1 || s.capacity<2 || s.capacity>16 || !count || count>s.capacity || (s.locked && count<2))return {};
    uint64_t previous=0;std::array<bool,16> colors{};
    for(unsigned i=0;i<count;++i){
        LobbyPlayer p;p.peer=r.Number(8);p.name=r.String();p.character=LobbyCharacter(r.Byte());p.color=r.Byte();const auto ready=r.Byte();p.ready=ready!=0;
        std::string normalized;
        if(!r.good || !p.peer || p.peer>s.capacity || p.peer<=previous || (i==0 && p.peer!=1) || uint8_t(p.character)>1 || p.color>=16 || colors[p.color] || ready>1 ||
           !Name(p.name,normalized) || normalized!=p.name || !UniqueName(s,0,p.name) || (s.locked && !p.ready))return {};
        previous=p.peer;colors[p.color]=true;s.players.push_back(std::move(p));
    }
    return s;
}
struct Connection {
    SOCKET socket=INVALID_SOCKET;bool connecting=false,welcomed=false,wire_exhausted=false;
    uint64_t peer=0,wire_next=0;uint32_t profile_next=1;
    Clock::time_point received=Clock::now(),pinged=received;
    Bytes incoming,outgoing;size_t sent=0;
    ~Connection(){Close(socket);}
    bool Queue(const Bytes& body){
        if(body.size()>MaxPacket || outgoing.size()-sent+body.size()+4>MaxQueued)return false;
        if(sent){outgoing.erase(outgoing.begin(),outgoing.begin()+sent);sent=0;}
        U64(outgoing,body.size(),4);outgoing.insert(outgoing.end(),body.begin(),body.end());return true;
    }
    bool Flush(){
        while(sent<outgoing.size()){
            const int n=send(socket,reinterpret_cast<const char*>(outgoing.data()+sent),int(outgoing.size()-sent),0);
            if(n>0){sent+=size_t(n);continue;}
            if(n==SOCKET_ERROR && WSAGetLastError()==WSAEWOULDBLOCK)return true;return false;
        }
        outgoing.clear();sent=0;return true;
    }
};
bool Nonblocking(SOCKET socket){u_long one=1;const BOOL no_delay=TRUE;return ioctlsocket(socket,FIONBIO,&one)==0 && setsockopt(socket,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&no_delay),sizeof(no_delay))==0;}
}

struct Room::Impl {
    RoomState state=RoomState::Idle;bool host=false;SOCKET listener=INVALID_SOCKET;uint16_t bound_port=0;
    Identity id{};LobbyRoster roster{2};LobbySnapshot lobby;uint64_t peer=0;
    std::vector<std::unique_ptr<Connection>> connections;
    std::string error,join_name;LobbyResult profile_result=LobbyResult::Ok;
    uint32_t profile_sequence=0,pending_profile=0;
    uint64_t submit_next=0,frame_next=0,consume_next=0;
    bool submit_exhausted=false,frame_exhausted=false,consume_exhausted=false;
    std::map<uint64_t,std::map<uint64_t,RoomCommand>> inputs;
    std::map<uint64_t,RoomCommand> echoes;
    std::deque<RoomFrame> frames;
    Clock::time_point progress=Clock::now();
    Clock::time_point loading_started{};
    std::optional<std::array<uint8_t,32>> local_world;
    std::array<std::optional<std::array<uint8_t,32>>,kOnlineRoomCapacity> worlds;
    ~Impl(){Close(listener);}
    bool Active()const{return state==RoomState::Listening || state==RoomState::Connecting || state==RoomState::Lobby || state==RoomState::Loading || state==RoomState::Running;}
    void Fail(std::string why){error=std::move(why);state=RoomState::Error;Close(listener);connections.clear();inputs.clear();echoes.clear();frames.clear();}
    bool Queue(Connection& c,const Bytes& p){if(c.Queue(p))return true;Fail("Room send queue exceeded its bounded capacity");return false;}
    bool Broadcast(const Bytes& p){for(auto& c:connections)if(c->welcomed && !c->Queue(p)){Fail("Room broadcast queue full");return false;}return true;}
    bool Publish(){
        lobby=Snapshot(roster);auto p=Packet(Wire::Snapshot);U64(p,0,4);U8(p,0);WriteRoster(p,lobby);
        return Broadcast(p);
    }
    void BeginLoading(Clock::time_point now){state=RoomState::Loading;loading_started=now;Close(listener);}
    void BeginRunning(Clock::time_point now){
        state=RoomState::Running;submit_next=id.first_tick+id.input_delay;frame_next=consume_next=id.first_tick;
        progress=now;Close(listener);
        for(auto& c:connections){c->wire_next=submit_next;c->received=c->pinged=now;}
        for(uint64_t t=id.first_tick;t<submit_next;++t){
            if(host)for(const auto& p:lobby.players)inputs[t][p.peer]=Neutral();
            else echoes[t]=Neutral();
        }
    }
    uint16_t PlayerMask()const{uint16_t mask=0;for(const auto& p:lobby.players)mask|=uint16_t(1u<<(p.peer-1));return mask;}
    bool ActivateIfReady(Clock::time_point now){
        if(!host || state!=RoomState::Loading || !local_world)return true;
        for(const auto& p:lobby.players){
            const auto& token=worlds[p.peer-1];if(token && *token!=*local_world){Fail("Online peers loaded different native worlds");return false;}
        }
        for(const auto& p:lobby.players)if(!worlds[p.peer-1])return true;
        auto packet=Packet(Wire::Activate);packet.insert(packet.end(),local_world->begin(),local_world->end());
        U64(packet,lobby.revision);U64(packet,PlayerMask(),2);U64(packet,id.first_tick);
        if(!Broadcast(packet))return false;BeginRunning(now);return true;
    }
    bool CompleteFrames(Clock::time_point now){
        while(!frame_exhausted && frames.size()<Window){
            const auto found=inputs.find(frame_next);if(found==inputs.end() || found->second.size()!=lobby.players.size())break;
            RoomFrame frame;frame.tick=frame_next;
            auto p=Packet(Wire::Frame);U64(p,frame.tick);U8(p,uint8_t(lobby.players.size()));
            for(const auto& player:lobby.players){const auto command=found->second.find(player.peer);if(command==found->second.end())return true;
                frame.players_mask|=uint16_t(1u<<(player.peer-1));frame.players[player.peer-1]=command->second;U64(p,player.peer);WriteCommand(p,command->second);}
            if(!Broadcast(p))return false;frames.push_back(std::move(frame));inputs.erase(found);
            if(frame_next==UINT64_MAX)frame_exhausted=true;else ++frame_next;progress=now;
        }
        return true;
    }
    LobbyResult Apply(uint64_t who,uint8_t action,std::string_view name,LobbyCharacter character,uint8_t color,bool ready){
        if(action==1){std::string normalized;if(!Name(name,normalized))return LobbyResult::InvalidName;
            if(!UniqueName(Snapshot(roster),who,normalized))return LobbyResult::InvalidName;return roster.Rename(who,normalized);}
        if(action==2)return roster.SetAppearance(who,character,color);
        return roster.SetReady(who,ready);
    }
    bool Profile(uint8_t action,std::string_view name={},LobbyCharacter character=LobbyCharacter::Mickey,uint8_t color=0,bool ready=false){
        if(state!=RoomState::Lobby && state!=RoomState::Listening)return false;
        if(host){profile_result=Apply(peer,action,name,character,color,ready);if(!Publish())return false;return profile_result==LobbyResult::Ok;}
        if(pending_profile || profile_sequence==UINT32_MAX || connections.empty())return false;
        auto p=Packet(Wire::Profile);U64(p,profile_sequence+1,4);U8(p,action);
        if(action==1){std::string normalized;if(!Name(name,normalized))return false;Text(p,normalized);}
        else if(action==2){U8(p,uint8_t(character));U8(p,color);}else U8(p,ready);
        if(!Queue(*connections.front(),p))return false;pending_profile=++profile_sequence;return true;
    }
    // False means a protocol violation. Unadmitted connections are rejected
    // individually; a violation by an admitted participant stops every peer.
    bool Message(Connection& c,std::span<const uint8_t> bytes,Clock::time_point now){
        Reader r{bytes};if(r.Number(4)!=0x4257524D || r.Byte()!=1)return false;const auto kind=Wire(r.Byte());
        if(kind==Wire::Hello){
            if(!host || c.welcomed || (state!=RoomState::Listening && state!=RoomState::Lobby))return false;
            const auto identity=ReadIdentity(r);auto name=r.String();std::string normalized;
            if(!r.Done() || identity!=id || !Name(name,normalized) || !UniqueName(Snapshot(roster),0,normalized) || roster.players().size()>=roster.capacity())return false;
            uint64_t assigned=2;while(assigned<=roster.capacity() && roster.Find(assigned))++assigned;
            if(assigned>roster.capacity() || roster.Join(assigned,normalized)!=LobbyResult::Ok)return false;
            c.peer=assigned;c.welcomed=true;state=RoomState::Lobby;
            auto welcome=Packet(Wire::Welcome);WriteIdentity(welcome,id);U64(welcome,assigned);
            if(!c.Queue(welcome)){Fail("Room welcome queue full");return true;}return Publish();
        }
        if(kind==Wire::Welcome){
            if(host || c.welcomed || state!=RoomState::Connecting)return false;
            const auto identity=ReadIdentity(r);const auto assigned=r.Number(8);
            if(!r.Done() || identity!=id || assigned<2 || assigned>kOnlineRoomCapacity)return false;
            peer=assigned;c.peer=1;c.welcomed=true;state=RoomState::Lobby;return true;
        }
        if(!c.welcomed)return false;
        if(kind==Wire::Ping || kind==Wire::Pong){
            if(!r.Done())return false;if(kind==Wire::Ping){auto p=Packet(Wire::Pong);return c.Queue(p);}return true;
        }
        if(kind==Wire::Profile){
            if(!host || (state!=RoomState::Lobby && state!=RoomState::Loading && state!=RoomState::Running))return false;
            const auto sequence=uint32_t(r.Number(4));const auto action=r.Byte();std::string name;LobbyCharacter character{};uint8_t color=0,ready=0;
            if(action==1)name=r.String();else if(action==2){character=LobbyCharacter(r.Byte());color=r.Byte();}else if(action==3)ready=r.Byte();else return false;
            if(!r.Done() || ready>1 || sequence!=c.profile_next || !sequence)return false;
            if(c.profile_next==UINT32_MAX)c.profile_next=0;else ++c.profile_next;
            // Requests already in flight when Start freezes the roster receive
            // a normal rejection. They cannot mutate the running room or turn
            // a legitimate Ready/Start race into a protocol disconnect.
            const auto result=state!=RoomState::Lobby?LobbyResult::Locked:Apply(c.peer,action,name,character,color,ready!=0);
            if(state==RoomState::Lobby && !Publish())return true;
            auto reply=Packet(Wire::ProfileReply);U64(reply,sequence,4);U8(reply,uint8_t(result));U64(reply,lobby.revision);
            return c.Queue(reply);
        }
        if(kind==Wire::ProfileReply){
            if(host || (state!=RoomState::Lobby && state!=RoomState::Loading && state!=RoomState::Running))return false;
            const auto sequence=uint32_t(r.Number(4));const auto result=r.Byte();const auto revision=r.Number(8);
            if(!r.Done() || !pending_profile || sequence!=pending_profile || revision!=lobby.revision || result>uint8_t(LobbyResult::NotReady))return false;
            pending_profile=0;profile_result=LobbyResult(result);return true;
        }
        if(kind==Wire::Snapshot){
            if(host || (state!=RoomState::Lobby && state!=RoomState::Connecting))return false;
            const auto ack=uint32_t(r.Number(4));const auto result=r.Byte();auto snapshot=ReadRoster(r);
            if(!r.Done() || !snapshot || snapshot->locked || ack || result ||
                snapshot->revision<lobby.revision || (snapshot->revision==lobby.revision && *snapshot!=lobby) ||
                std::none_of(snapshot->players.begin(),snapshot->players.end(),[&](const auto& p){return p.peer==peer;}))return false;
            lobby=std::move(*snapshot);return true;
        }
        if(kind==Wire::Start){
            if(host || state!=RoomState::Lobby)return false;
            auto snapshot=ReadRoster(r);if(!r.Done() || !snapshot || !snapshot->locked || snapshot->players.size()<2 || snapshot->capacity!=lobby.capacity || snapshot->players.size()!=lobby.players.size())return false;
            auto expected=lobby;expected.locked=true;expected.revision=snapshot->revision;
            if(*snapshot!=expected || snapshot->revision<=lobby.revision)return false;
            lobby=std::move(*snapshot);BeginLoading(now);return true;
        }
        if(kind==Wire::WorldReady){
            if(!host || (state!=RoomState::Loading && state!=RoomState::Running))return false;
            std::array<uint8_t,32> token{};for(auto& b:token)b=r.Byte();
            if(!r.Done() || !std::any_of(token.begin(),token.end(),[](auto b){return b!=0;}))return false;
            auto& previous=worlds[c.peer-1];if(previous && *previous!=token)return false;
            if(state==RoomState::Running)return previous && *previous==token;
            previous=token;return ActivateIfReady(now);
        }
        if(kind==Wire::Activate){
            if(host || state!=RoomState::Loading || !local_world)return false;
            std::array<uint8_t,32> token{};for(auto& b:token)b=r.Byte();
            const auto revision=r.Number(8),mask=r.Number(2),tick=r.Number(8);
            if(!r.Done() || token!=*local_world || revision!=lobby.revision || mask!=PlayerMask() || tick!=id.first_tick)return false;
            BeginRunning(now);return true;
        }
        if(kind==Wire::Input){
            if(!host || state!=RoomState::Running)return false;
            const auto tick=r.Number(8);const auto command=ReadCommand(r);
            if(!r.Done() || !ValidCommand(command) || c.wire_exhausted || tick!=c.wire_next || tick<consume_next || tick-consume_next>=Window || inputs[tick].contains(c.peer))return false;
            inputs[tick][c.peer]=command;if(c.wire_next==UINT64_MAX)c.wire_exhausted=true;else ++c.wire_next;return true;
        }
        if(kind==Wire::Frame){
            if(host || state!=RoomState::Running || frame_exhausted || frames.size()>=Window)return false;
            RoomFrame frame;frame.tick=r.Number(8);const auto count=r.Byte();
            if(frame.tick!=frame_next || count!=lobby.players.size())return false;
            for(unsigned i=0;i<count;++i){const auto entry_peer=r.Number(8);const auto command=ReadCommand(r);
                if(entry_peer!=lobby.players[i].peer || !ValidCommand(command))return false;
                frame.players_mask|=uint16_t(1u<<(entry_peer-1));frame.players[entry_peer-1]=command;}
            const auto echo=echoes.find(frame.tick);
            if(!r.Done() || echo==echoes.end() || !(frame.players_mask&(1u<<(peer-1))) || frame.players[peer-1]!=echo->second)return false;
            echoes.erase(echo);frames.push_back(std::move(frame));if(frame_next==UINT64_MAX)frame_exhausted=true;else ++frame_next;progress=now;return true;
        }
        return false;
    }
};

Room::Room():impl_(std::make_unique<Impl>()){}
Room::~Room()=default;
bool Room::Host(const Identity& id,std::string_view address,uint16_t port,uint8_t capacity,std::string_view name){
    auto& p=*impl_;std::string normalized;const auto endpoint=ParseNumericAddress(address,true);
    if(p.state!=RoomState::Idle || !Winsock() || !endpoint || !ValidIdentity(id) || capacity<2 || capacity>16 || !Name(name,normalized))return false;
    auto bound=*endpoint;bound.Port(port);p.listener=socket(bound.storage.ss_family,SOCK_STREAM,IPPROTO_TCP);
    if(p.listener==INVALID_SOCKET)return false;
    if(!Nonblocking(p.listener) || bind(p.listener,reinterpret_cast<const sockaddr*>(&bound.storage),bound.size)!=0 || listen(p.listener,16)!=0){Close(p.listener);return false;}
    int size=sizeof(bound.storage);if(getsockname(p.listener,reinterpret_cast<sockaddr*>(&bound.storage),&size)!=0){Close(p.listener);return false;}
    p.id=id;p.host=true;p.peer=1;p.bound_port=bound.Port();p.roster=LobbyRoster(capacity);p.roster.Join(1,normalized);p.lobby=Snapshot(p.roster);p.state=RoomState::Listening;return true;
}
bool Room::Join(const Identity& id,std::string_view address,uint16_t port,std::string_view name){
    auto& p=*impl_;std::string normalized;auto endpoint=ParseNumericAddress(address);
    if(p.state!=RoomState::Idle || !Winsock() || !endpoint || !port || !ValidIdentity(id) || !Name(name,normalized))return false;
    endpoint->Port(port);auto c=std::make_unique<Connection>();c->socket=socket(endpoint->storage.ss_family,SOCK_STREAM,IPPROTO_TCP);
    if(c->socket==INVALID_SOCKET || !Nonblocking(c->socket))return false;
    const auto result=connect(c->socket,reinterpret_cast<const sockaddr*>(&endpoint->storage),endpoint->size);
    if(result==SOCKET_ERROR && WSAGetLastError()!=WSAEWOULDBLOCK)return false;
    c->connecting=result!=0;p.id=id;p.join_name=normalized;p.state=RoomState::Connecting;
    auto hello=Packet(Wire::Hello);WriteIdentity(hello,id);Text(hello,normalized);if(!c->Queue(hello))return false;
    p.connections.push_back(std::move(c));return true;
}
void Room::Poll(Clock::time_point now){
    auto& p=*impl_;if(!p.Active())return;
    if(p.state==RoomState::Loading && now-p.loading_started>StartupTimeout){p.Fail("Online native world startup exceeded its 180-second deadline");return;}
    if(p.host && p.listener!=INVALID_SOCKET){
        // Bounded work per poll, including unauthenticated sockets.
        for(unsigned i=0;i<16;++i){auto socket=accept(p.listener,nullptr,nullptr);if(socket==INVALID_SOCKET)break;
            if(p.connections.size()>=15 || !Nonblocking(socket)){Close(socket);continue;}
            auto c=std::make_unique<Connection>();c->socket=socket;c->received=c->pinged=now;p.connections.push_back(std::move(c));}
    }
    for(size_t i=0;i<p.connections.size();){
        auto& c=*p.connections[i];bool alive=true;
        if(c.connecting){
            fd_set write,except;FD_ZERO(&write);FD_ZERO(&except);FD_SET(c.socket,&write);FD_SET(c.socket,&except);timeval zero{};
            const auto ready=select(0,nullptr,&write,&except,&zero);
            if(ready==SOCKET_ERROR)alive=false;
            else if(ready){int error=0,size=sizeof(error);if(getsockopt(c.socket,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&size)!=0 || error)alive=false;else c.connecting=false;}
        }
        if(alive && !c.connecting){
            alive=c.Flush();
            for(unsigned reads=0;alive && reads<16;++reads){
                std::array<char,4096> buffer{};const auto n=recv(c.socket,buffer.data(),int(buffer.size()),0);
                if(n>0){if(c.incoming.size()+size_t(n)>MaxPacket*8){alive=false;break;}c.incoming.insert(c.incoming.end(),buffer.begin(),buffer.begin()+n);}
                else {if(n==0 || WSAGetLastError()!=WSAEWOULDBLOCK)alive=false;break;}
            }
            unsigned packets=0;
            while(alive && c.incoming.size()>=4 && packets++<128){
                Reader length{std::span(c.incoming).first(4)};const auto count=length.Number(4);
                if(count<6 || count>MaxPacket){alive=false;break;}if(c.incoming.size()<count+4)break;
                Bytes message(c.incoming.begin()+4,c.incoming.begin()+4+size_t(count));c.incoming.erase(c.incoming.begin(),c.incoming.begin()+4+size_t(count));
                const bool valid=p.Message(c,message,now);if(!p.Active())return;if(!valid){alive=false;break;}c.received=now;
            }
            if(alive && c.welcomed && now-c.pinged>=std::chrono::seconds(2)){auto ping=Packet(Wire::Ping);alive=c.Queue(ping);c.pinged=now;}
            if(alive)alive=c.Flush();
        }
        if(now-c.received>(p.state==RoomState::Loading?StartupTimeout:Timeout))alive=false;
        if(!alive){
            if(!p.host || (c.welcomed && (p.state==RoomState::Loading || p.state==RoomState::Running))){p.Fail("Online room participant disconnected, timed out, or sent invalid data; simulation stopped");return;}
            const auto departed=c.welcomed?c.peer:0;
            p.connections.erase(p.connections.begin()+i);
            if(departed){
                if(p.roster.Leave(departed)!=LobbyResult::Ok){p.Fail("Room departure did not match its authoritative roster");return;}
                p.state=p.roster.players().size()==1?RoomState::Listening:RoomState::Lobby;
                if(!p.Publish())return;
            }
            continue;
        }
        ++i;
    }
    if(p.state==RoomState::Running){
        if(p.host && !p.CompleteFrames(now))return;
        if(now-p.progress>Timeout)p.Fail("Online room is missing contiguous input; simulation stopped");
    }
}
bool Room::Rename(std::string_view name){return impl_->Profile(1,name);}
bool Room::SetAppearance(LobbyCharacter c,uint8_t color){return impl_->Profile(2,{},c,color);}
bool Room::SetReady(bool ready){return impl_->Profile(3,{},LobbyCharacter::Mickey,0,ready);}
bool Room::Start(){
    auto& p=*impl_;if(!p.host || p.state!=RoomState::Lobby || p.roster.players().size()<2)return false;
    if(p.roster.Start()!=LobbyResult::Ok)return false;p.lobby=Snapshot(p.roster);
    // Pending unadmitted TCP sockets do not become gameplay participants.
    for(size_t i=0;i<p.connections.size();)if(!p.connections[i]->welcomed)p.connections.erase(p.connections.begin()+i);else ++i;
    auto start=Packet(Wire::Start);WriteRoster(start,p.lobby);if(!p.Broadcast(start))return false;p.BeginLoading(Clock::now());return true;
}
bool Room::WorldReady(const std::array<uint8_t,32>& token){
    auto& p=*impl_;
    if(p.state!=RoomState::Loading && p.state!=RoomState::Running)return false;
    if(p.local_world)return *p.local_world==token;
    if(p.state!=RoomState::Loading || !std::any_of(token.begin(),token.end(),[](auto b){return b!=0;}))return false;
    const auto now=Clock::now();if(now-p.loading_started>StartupTimeout){p.Fail("Online native world startup exceeded its 180-second deadline");return false;}
    p.local_world=token;
    if(p.host){p.worlds[p.peer-1]=token;return p.ActivateIfReady(now);}
    auto packet=Packet(Wire::WorldReady);packet.insert(packet.end(),token.begin(),token.end());return p.Queue(*p.connections.front(),packet);
}
bool Room::SubmitLocal(uint64_t tick,const RoomCommand& command){
    auto& p=*impl_;if(p.state!=RoomState::Running || p.submit_exhausted || tick!=p.submit_next || tick<p.consume_next || tick-p.consume_next>=Window || !ValidCommand(command))return false;
    if(p.host)p.inputs[tick][p.peer]=command;
    else {auto packet=Packet(Wire::Input);U64(packet,tick);WriteCommand(packet,command);if(!p.Queue(*p.connections.front(),packet))return false;p.echoes[tick]=command;}
    if(p.submit_next==UINT64_MAX)p.submit_exhausted=true;else ++p.submit_next;return true;
}
bool Room::PeekFrame(RoomFrame& frame)const{
    const auto& p=*impl_;
    if(p.state!=RoomState::Running || p.consume_exhausted || p.frames.empty() || p.frames.front().tick!=p.consume_next)return false;
    frame=p.frames.front();return true;
}
bool Room::TakeFrame(RoomFrame& frame){
    auto& p=*impl_;if(p.state!=RoomState::Running || p.consume_exhausted || p.frames.empty())return false;
    if(p.frames.front().tick!=p.consume_next){p.Fail("Room frame consumption lost contiguous ordering");return false;}
    frame=std::move(p.frames.front());p.frames.pop_front();if(p.consume_next==UINT64_MAX)p.consume_exhausted=true;else ++p.consume_next;return true;
}
void Room::Disconnect(){auto& p=*impl_;if(p.state==RoomState::Disconnected)return;Close(p.listener);p.connections.clear();p.inputs.clear();p.echoes.clear();p.frames.clear();p.state=RoomState::Disconnected;}
RoomState Room::state()const{return impl_->state;}
uint16_t Room::port()const{return impl_->bound_port;}
uint64_t Room::local_peer()const{return impl_->peer;}
uint64_t Room::next_tick()const{return impl_->consume_next;}
uint64_t Room::next_submit_tick()const{return impl_->submit_next;}
const Identity& Room::identity()const{return impl_->id;}
const LobbySnapshot& Room::lobby()const{return impl_->lobby;}
LobbyResult Room::last_lobby_result()const{return impl_->profile_result;}
bool Room::profile_pending()const{return impl_->pending_profile!=0;}
const std::string& Room::error()const{return impl_->error;}

bool TestOnlineRoom(){
    // Network tests use only ephemeral numeric loopback ports. No game assets,
    // save paths, actor creation, GUI or separate executable are involved.
    bool ok=true;unsigned checks=0;auto check=[&](bool value,const char* why){++checks;if(!value){REXLOG_ERROR("Online room: {}",why);ok=false;}return value;};
    Identity id;id.session[0]=7;id.build[0]=1;id.content[0]=2;id.checkpoint[0]=3;id.first_tick=100;id.input_delay=2;
    std::array<uint8_t,32> world{};world[0]=9;
    std::vector<std::unique_ptr<Room>> rooms;
    auto pump=[&](auto done){const auto deadline=Clock::now()+std::chrono::seconds(5);do{for(auto& r:rooms)r->Poll();if(done())return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(Clock::now()<deadline);return false;};
    auto setup=[&](unsigned count){rooms.clear();rooms.push_back(std::make_unique<Room>());if(!rooms[0]->Host(id,"127.0.0.1",0,uint8_t(count),"Host"))return false;
        for(unsigned i=1;i<count;++i){rooms.push_back(std::make_unique<Room>());if(!rooms.back()->Join(id,"127.0.0.1",rooms[0]->port(),"Guest "+std::to_string(i)))return false;}
        return pump([&]{return std::all_of(rooms.begin(),rooms.end(),[&](const auto& r){return r->lobby().players.size()==count;});});};
    if(!check(setup(16),"sixteen real simultaneous loopback participants handshake and receive the host roster"))return false;
    for(unsigned i=0;i<16;++i)check(rooms[i]->local_peer()==i+1,"peer IDs assigned by host connection, never client supplied");
    check(!rooms[0]->Start(),"unready roster cannot start");
    check(rooms[1]->SetAppearance(LobbyCharacter::Mickey,0) && pump([&]{return !rooms[1]->profile_pending();}) && rooms[1]->last_lobby_result()==LobbyResult::ColorInUse,"host rejects duplicate colors transactionally");
    check(rooms[1]->Rename("Host") && pump([&]{return !rooms[1]->profile_pending();}) && rooms[1]->last_lobby_result()==LobbyResult::InvalidName,"host rejects duplicate names");
    check(rooms[1]->Rename("Unicode \xE7\x8E\xA9\xE5\xAE\xB6") && pump([&]{return !rooms[1]->profile_pending();}) && rooms[1]->last_lobby_result()==LobbyResult::Ok,"bounded Unicode rename reaches authority");
    for(auto& r:rooms)check(r->SetReady(true),"each participant explicitly readies");
    check(pump([&]{return std::all_of(rooms[0]->lobby().players.begin(),rooms[0]->lobby().players.end(),[](const auto& p){return p.ready;}) && std::none_of(rooms.begin(),rooms.end(),[](const auto& r){return r->profile_pending();});}),"readiness acknowledged by all clients");
    check(rooms[0]->Start() && pump([&]{return std::all_of(rooms.begin(),rooms.end(),[](const auto& r){return r->state()==RoomState::Loading;});}),"host freezes complete ready roster and enters native Loading, never automatic Running");
    for(auto& r:rooms){RoomFrame frame;check(!r->SubmitLocal(102,Neutral()) && !r->PeekFrame(frame) && !r->TakeFrame(frame) && r->impl_->frames.empty() && r->impl_->inputs.empty(),"no commands, neutral seeds or frames admitted before world barrier");}
    for(unsigned i=0;i<15;++i)check(rooms[i]->WorldReady(world),"caller explicitly asserts loaded native world for each ready participant");
    check(pump([&]{return std::count_if(rooms[0]->impl_->worlds.begin(),rooms[0]->impl_->worlds.end(),[](const auto& token){return bool(token);})==15;}),"fifteen native-world assertions reach authority, final guest still loading");
    const auto slow=Clock::now()+std::chrono::seconds(20);for(unsigned i=0;i<15;++i)rooms[i]->Poll(slow);
    check(std::all_of(rooms.begin(),rooms.end(),[](const auto& r){return r->state()==RoomState::Loading && r->impl_->frames.empty();}),"blocked native loader beyond runtime15-second timeout keeps startup barrier closed without disconnect");
    auto different=world;++different[0];
    check(rooms[0]->WorldReady(world) && !rooms[0]->WorldReady(different) && !rooms[15]->WorldReady({}),"same caller assertion idempotent; changed and empty tokens rejected");
    check(rooms[15]->WorldReady(world) && pump([&]{return std::all_of(rooms.begin(),rooms.end(),[](const auto& r){return r->state()==RoomState::Running;});}),"only final matching world assertion activates all sixteen peers");
    check(rooms[0]->WorldReady(world) && rooms[15]->WorldReady(world),"same readiness remains idempotent after activation");
    check(!rooms[1]->SetReady(false) && !rooms[0]->Rename("Changed"),"locked roster cannot mutate after start");
    for(uint64_t t=100;t<102;++t){check(pump([&]{return std::all_of(rooms.begin(),rooms.end(),[&](const auto& r){return !r->impl_->frames.empty();});}),"neutral delayed frame delivered to every peer");
        for(auto& r:rooms){RoomFrame f,again;
            check(r->PeekFrame(f) && r->PeekFrame(again) && f==again && r->next_tick()==t,"repeated preflight inspection never consumes the agreed frame");
            check(r->TakeFrame(f) && f.tick==t && f.players_mask==0xFFFF && std::all_of(f.players.begin(),f.players.end(),[](const auto& p){return p==Neutral();}),"identical full-roster neutral delay frame");}}
    std::array<RoomCommand,16> commands;commands.fill(Neutral());for(unsigned i=0;i<16;++i){
        commands[i].x=float(i)/16;commands[i].y=-float(i)/16;
        commands[i].forward_x=std::sin(float(i)*.25f);commands[i].forward_z=std::cos(float(i)*.25f);
        commands[i].pad.buttons=uint16_t(i);commands[i].pad.left_trigger=uint8_t(i*5);commands[i].pad.right_y=int16_t(-int(i)*101);
    }
    for(unsigned i=0;i<15;++i)check(rooms[i]->SubmitLocal(102,commands[i]),"native command queued once by its owning connection");
    for(unsigned i=0;i<5;++i)for(auto& r:rooms)r->Poll();
    check(std::all_of(rooms.begin(),rooms.end(),[](const auto& r){RoomFrame f;return !r->TakeFrame(f);}),"no tick emitted while even one connected peer input is missing");
    check(!rooms[0]->SubmitLocal(102,commands[0]) && !rooms[0]->SubmitLocal(104,commands[0]),"duplicate and skipped local ticks rejected");
    auto invalid=commands[15];invalid.x=std::numeric_limits<float>::quiet_NaN();check(!rooms[15]->SubmitLocal(102,invalid),"nonfinite native movement rejected before wire");
    invalid=commands[15];invalid.forward_x=1;invalid.forward_z=1;check(!rooms[15]->SubmitLocal(102,invalid),"nonorthogonal view basis rejected");
    check(rooms[15]->SubmitLocal(102,commands[15]) && pump([&]{return std::all_of(rooms.begin(),rooms.end(),[](const auto& r){return !r->impl_->frames.empty();});}),"last input completes one contiguous authoritative frame");
    RoomFrame baseline;check(rooms[0]->TakeFrame(baseline) && baseline.tick==102 && baseline.players_mask==0xFFFF,"host consumes the same network-authoritative frame");
    for(unsigned i=1;i<16;++i){RoomFrame f;check(rooms[i]->TakeFrame(f) && f==baseline,"all fifteen clients receive identical peer-associated commands and own-input echo");}
    for(unsigned i=0;i<16;++i)check(baseline.players[i]==commands[i],"commands remain associated with host-assigned peer IDs");
    for(uint64_t tick=103;tick<107;++tick)for(unsigned i=0;i<16;++i)check(rooms[i]->SubmitLocal(tick,commands[i]),"bounded pipelined contiguous input accepted");
    check(pump([&]{return std::all_of(rooms.begin(),rooms.end(),[](const auto& r){return r->impl_->frames.size()==4;});}),"four complete queued ticks arrive without gaps");
    for(uint64_t tick=103;tick<107;++tick){RoomFrame expected;check(rooms[0]->TakeFrame(expected) && expected.tick==tick,"host consumes pipelined ticks in order");
        for(unsigned i=1;i<16;++i){RoomFrame actual;check(rooms[i]->TakeFrame(actual) && actual==expected,"every client consumes the same contiguous pipelined sequence");}}
    Room late;const auto late_connect=late.Join(id,"127.0.0.1",rooms[0]->port(),"Late guest");
    if(late_connect)late.Poll(Clock::now()+Timeout+std::chrono::seconds(1));
    check(!late_connect || late.state()==RoomState::Error,"closed listener cannot admit late participants");
    rooms[15]->Disconnect();check(pump([&]{return rooms[0]->state()==RoomState::Error;}) && pump([&]{return rooms[1]->state()==RoomState::Error;}),"one disconnect stops host and remaining clients, never silent continuation");
    // Each malicious admitted client gets its own isolated two-peer room.
    auto loading=[&]{if(!setup(2))return false;rooms[0]->SetReady(true);rooms[1]->SetReady(true);
        if(!pump([&]{return !rooms[1]->profile_pending() && rooms[0]->lobby().players[1].ready;}))return false;
        return rooms[0]->Start() && pump([&]{return rooms[1]->state()==RoomState::Loading;});};
    auto activate=[&]{for(auto& r:rooms)if(!r->WorldReady(world))return false;
        return pump([&]{return std::all_of(rooms.begin(),rooms.end(),[](const auto& r){return r->state()==RoomState::Running;});});};
    auto running=[&]{return loading() && activate();};
    if(!check(setup(2),"unsolicited WorldReady fixture connects lobby"))return false;
    auto premature=Packet(Wire::WorldReady);premature.insert(premature.end(),world.begin(),world.end());rooms[1]->impl_->connections.front()->Queue(premature);
    check(pump([&]{return rooms[1]->state()==RoomState::Error && rooms[0]->state()==RoomState::Listening;}),"native-world assertion before frozen Start is rejected without activating or destroying host lobby");
    if(!check(loading(),"input-before-world-barrier fixture enters Loading"))return false;
    premature=Packet(Wire::Input);U64(premature,id.first_tick+id.input_delay);WriteCommand(premature,Neutral());rooms[1]->impl_->connections.front()->Queue(premature);
    check(pump([&]{return rooms[0]->state()==RoomState::Error;}),"remote movement before all native world assertions is a protocol violation, never buffered for simulation");
    if(!check(loading(),"mismatched loaded-world fixture enters Loading"))return false;
    check(rooms[0]->WorldReady(world) && rooms[1]->WorldReady(different) &&
        pump([&]{return rooms[0]->state()==RoomState::Error;}),"different canonical native world token stops startup instead of activating");
    if(!check(loading(),"heartbeat-only startup deadline fixture enters Loading"))return false;
    const auto startup=Clock::now();
    for(unsigned seconds=10;seconds<=170;seconds+=10)for(auto& r:rooms)r->Poll(startup+std::chrono::seconds(seconds));
    check(rooms[0]->state()==RoomState::Loading && rooms[1]->state()==RoomState::Loading,"heartbeats keep slow loading connection alive within absolute startup bound");
    rooms[0]->Poll(startup+StartupTimeout+std::chrono::seconds(1));
    check(rooms[0]->state()==RoomState::Error && rooms[0]->error().find("180-second")!=std::string::npos,"heartbeat traffic cannot extend missing WorldReady beyond absolute180-second deadline");
    if(!check(loading(),"client-forged activation fixture enters Loading"))return false;
    auto forged_activation=Packet(Wire::Activate);forged_activation.insert(forged_activation.end(),world.begin(),world.end());
    U64(forged_activation,rooms[0]->lobby().revision);U64(forged_activation,3,2);U64(forged_activation,id.first_tick);
    rooms[1]->impl_->connections.front()->Queue(forged_activation);
    check(pump([&]{return rooms[0]->state()==RoomState::Error;}),"guest cannot activate the frozen room even with valid token and roster metadata");
    if(!check(loading(),"unsolicited activation fixture enters Loading"))return false;
    forged_activation=Packet(Wire::Activate);forged_activation.insert(forged_activation.end(),world.begin(),world.end());
    U64(forged_activation,rooms[0]->lobby().revision);U64(forged_activation,3,2);U64(forged_activation,id.first_tick);
    rooms[0]->impl_->connections.front()->Queue(forged_activation);
    check(pump([&]{return rooms[1]->state()==RoomState::Error;}) && rooms[0]->state()==RoomState::Loading,"client rejects activation before its native caller asserted loaded-world readiness");
    if(!check(loading(),"repeated-invalid WorldReady fixture enters Loading"))return false;
    check(rooms[1]->WorldReady(world) && pump([&]{return bool(rooms[0]->impl_->worlds[1]);}),"first native world token is bound to authenticated guest socket");
    auto repeated=Packet(Wire::WorldReady);repeated.insert(repeated.end(),different.begin(),different.end());rooms[1]->impl_->connections.front()->Queue(repeated);
    check(pump([&]{return rooms[0]->state()==RoomState::Error;}),"guest cannot replace its earlier world assertion with another token");
    for(unsigned edit=0;edit<2;++edit){
        if(!check(setup(2),"in-flight profile race connects real TCP peers"))return false;
        rooms[0]->SetReady(true);rooms[1]->SetReady(true);
        if(!check(pump([&]{return !rooms[1]->profile_pending() && rooms[0]->lobby().players[1].ready;}),"race fixture has acknowledged ready agreement"))return false;
        check(edit==0?rooms[1]->Rename("In-flight rename"):rooms[1]->SetReady(false),"client queues profile before host start without polling it to authority");
        check(rooms[1]->profile_pending() && rooms[0]->Start(),"host freezes last accepted ready roster while profile request remains in flight");
        const auto frozen=rooms[0]->lobby();
        check(pump([&]{return rooms[1]->state()==RoomState::Loading && !rooms[1]->profile_pending();}) &&
            rooms[0]->state()==RoomState::Loading && rooms[1]->last_lobby_result()==LobbyResult::Locked &&
            rooms[0]->lobby()==frozen && rooms[1]->lobby()==frozen,
            "queued rename/unready receives Locked after Start without disconnect or frozen roster mutation");
        check(activate(),"in-flight rejected profile does not prevent explicit world activation");
    }
    if(!check(running(),"malformed-wire fixture starts fresh real room"))return false;
    if(rooms[1]->state()==RoomState::Running){auto spoof=Packet(Wire::Input);U64(spoof,102);U64(spoof,1);WriteCommand(spoof,Neutral());rooms[1]->impl_->connections.front()->Queue(spoof);
        check(pump([&]{return rooms[0]->state()==RoomState::Error;}),"extra spoofed peer field is rejected by exact wire shape");}
    if(!check(running(),"stale-wire fixture starts fresh room"))return false;
    if(rooms[1]->state()==RoomState::Running){auto stale=Packet(Wire::Input);U64(stale,101);WriteCommand(stale,Neutral());rooms[1]->impl_->connections.front()->Queue(stale);
        check(pump([&]{return rooms[0]->state()==RoomState::Error;}),"stale remote tick closes room");}
    if(!check(running(),"future-window fixture starts fresh room"))return false;
    if(rooms[1]->state()==RoomState::Running){auto future=Packet(Wire::Input);U64(future,100+Window);WriteCommand(future,Neutral());rooms[1]->impl_->connections.front()->Queue(future);
        check(pump([&]{return rooms[0]->state()==RoomState::Error;}),"out-of-window remote input rejected");}
    if(!check(running(),"own-echo fixture starts fresh room"))return false;
    if(rooms[1]->state()==RoomState::Running){
        // Alter authority's echo while preserving a syntactically valid frame.
        auto changed=Neutral();changed.x=.5f;
        rooms[1]->SubmitLocal(102,Neutral());
        auto forged=Packet(Wire::Frame);U64(forged,102);U8(forged,2);U64(forged,1);WriteCommand(forged,Neutral());U64(forged,2);WriteCommand(forged,changed);
        rooms[0]->impl_->connections.front()->Queue(forged);
        check(pump([&]{return rooms[1]->state()==RoomState::Error;}) && rooms[0]->state()==RoomState::Running,"client itself refuses authoritative frame with altered local-input echo");
    }
    for(unsigned component=0;component<4;++component){
        if(!check(running(),"signed-zero own-echo fixture starts fresh real room"))return false;
        auto original=Neutral();if(component==3){original.forward_x=1;original.forward_z=0;}
        auto changed=original;
        if(component==0)changed.x=-0.0f;else if(component==1)changed.y=-0.0f;
        else if(component==2)changed.forward_x=-0.0f;else changed.forward_z=-0.0f;
        check(rooms[1]->SubmitLocal(102,original),"client submits native command containing a positive zero component");
        auto forged=Packet(Wire::Frame);U64(forged,102);U8(forged,2);U64(forged,1);WriteCommand(forged,Neutral());U64(forged,2);WriteCommand(forged,changed);
        rooms[0]->impl_->connections.front()->Queue(forged);
        check(pump([&]{return rooms[1]->state()==RoomState::Error;}) && rooms[0]->state()==RoomState::Running,
            "own-input echo rejects changed sign bits in x/y/forward X/Z without relying on host disconnect");
    }
    if(!check(running(),"timeout fixture starts fresh room"))return false;
    if(rooms[0]->state()==RoomState::Running){for(auto& r:rooms)r->Poll(Clock::now()+Timeout+std::chrono::seconds(1));check(rooms[0]->state()==RoomState::Error,"missing input/heartbeat has bounded timeout");}
    for(unsigned field=0;field<4;++field){
        rooms.clear();rooms.push_back(std::make_unique<Room>());check(rooms[0]->Host(id,"127.0.0.1",0,2,"Host"),"identity mismatch fixture hosts");
        auto wrong=id;if(field==0)++wrong.build[0];else if(field==1)++wrong.content[0];else if(field==2)++wrong.checkpoint[0];else ++wrong.session[0];
        rooms.push_back(std::make_unique<Room>());check(rooms[1]->Join(wrong,"127.0.0.1",rooms[0]->port(),"Guest"),"mismatching identity connects for rejection");
        check(pump([&]{return rooms[1]->state()==RoomState::Error;}) && rooms[0]->state()==RoomState::Listening && rooms[0]->lobby().players.size()==1,"build/content/checkpoint/session mismatch rejected before roster admission");
    }
    for(unsigned field=0;field<4;++field){
        auto missing=id;if(field==0)missing.build={};else if(field==1)missing.content={};else if(field==2)missing.checkpoint={};else missing.session={};
        Room host,guest;check(!host.Host(missing,"127.0.0.1",0,2,"Host") && !guest.Join(missing,"127.0.0.1",1,"Guest"),
            "missing build/content/checkpoint/session fingerprint rejected before socket setup");
    }
    check(setup(2),"malformed frame-length fixture connects");
    if(rooms[1]->state()==RoomState::Lobby){
        auto& c=*rooms[1]->impl_->connections.front();c.outgoing.clear();c.sent=0;U64(c.outgoing,MaxPacket+1,4);
        check(pump([&]{return rooms[1]->state()==RoomState::Error && rooms[0]->lobby().players.size()==1;}) && rooms[0]->state()==RoomState::Listening,
            "oversized packet declaration evicts malformed lobby participant while host keeps listening");
    }
    // Reuse the same host and actual capacity-2 room for more than sixteen
    // cumulative joins. Logical slots/colors are reusable before start only.
    rooms.clear();rooms.push_back(std::make_unique<Room>());
    if(!check(rooms[0]->Host(id,"127.0.0.1",0,2,"Host"),"persistent leave/rejoin room hosts"))return false;
    for(unsigned visit=0;visit<20;++visit){
        rooms.push_back(std::make_unique<Room>());
        if(!check(rooms[1]->Join(id,"127.0.0.1",rooms[0]->port(),"Returning guest") &&
            pump([&]{return rooms[1]->state()==RoomState::Lobby && rooms[1]->lobby().players.size()==2;}),"guest rejoins persistent TCP lobby"))return false;
        check(rooms[1]->local_peer()==2 && rooms[1]->lobby().players[1].color==1,"lowest free peer slot and blue color reused without cumulative exhaustion");
        rooms[0]->SetReady(true);rooms[1]->SetReady(true);
        if(!check(pump([&]{return !rooms[1]->profile_pending() && rooms[0]->lobby().players[1].ready;}),"returning guest acknowledges readiness"))return false;
        rooms[1]->Disconnect();
        check(pump([&]{return rooms[0]->state()==RoomState::Listening && rooms[0]->lobby().players.size()==1;}) &&
            !rooms[0]->lobby().locked && !rooms[0]->lobby().players[0].ready,"lobby departure preserves host, unlocks roster and invalidates previous readiness");
        rooms.pop_back();
    }
    if(!check(running(),"native-source fixture connects"))return false;
    if(rooms[1]->state()==RoomState::Running){
        auto raw=Neutral();raw.source=RoomMovementSource::UnconvertedPad;check(!rooms[1]->SubmitLocal(102,raw),"raw pad movement cannot masquerade as interpreted native movement");
        raw=Neutral();raw.actor_epoch=2;check(!rooms[1]->SubmitLocal(102,raw),"client cannot change frozen native actor incarnation");
        raw=Neutral();raw.forward_z=.999f;check(!rooms[1]->SubmitLocal(102,raw),"near-unit basis outside native 1e-4 tolerance rejected by transport too");
    }
    for(unsigned malformed=0;malformed<4;++malformed){
        if(!check(running(),"remote movement validation fixture connects"))return false;
        if(rooms[1]->state()!=RoomState::Running)continue;
        auto command=Neutral();if(malformed==0)command.actor_epoch=2;
        else if(malformed==1)command.source=RoomMovementSource::UnconvertedPad;
        else command.forward_z=malformed==2?0:.999f;
        auto packet=Packet(Wire::Input);U64(packet,102);WriteCommand(packet,command);rooms[1]->impl_->connections.front()->Queue(packet);
        check(pump([&]{return rooms[0]->state()==RoomState::Error;}),"host independently rejects remote changed epoch, raw source, invalid and near-unit basis");
    }
    for(unsigned missing=0;missing<3;++missing){
        auto unverified=id;if(missing==0)unverified.build={};else if(missing==1)unverified.content={};else unverified.checkpoint={};
        Room rejected;check(!rejected.Host(unverified,"127.0.0.1",0,2,"Host") &&
            !rejected.Join(unverified,"127.0.0.1",12345,"Guest"),"missing build/content/checkpoint identity rejected before sockets");
    }
    {
        auto imprecise=Neutral();imprecise.forward_z=.999f;
        check(!ValidCommand(imprecise),"wire basis tolerance matches the native coordinator");
    }
    const auto saved=id;id.first_tick=0;id.input_delay=0;
    if(!check(running(),"zero-delay tick-zero room starts"))return false;
    if(rooms[1]->state()==RoomState::Running){
        check(rooms[0]->SubmitLocal(0,Neutral()) && rooms[1]->SubmitLocal(0,Neutral()) &&
            pump([&]{return !rooms[0]->impl_->frames.empty() && !rooms[1]->impl_->frames.empty();}),"tick zero is a valid contiguous input, not an exhausted sentinel");
        RoomFrame a,b;check(rooms[0]->TakeFrame(a) && rooms[1]->TakeFrame(b) && a==b && a.tick==0 && a.players_mask==3,"shared fixed frame preserves both active bits and absent slots");
    }
    id=saved;
    if(ok)REXLOG_INFO("Online room passed {} checks: real16-peer TCP, authoritative roster/ready, native movement basis, contiguous all-peer input, own echo, malformed/spoof/stale/window rejection and disconnect/timeout stop",checks);
    return ok;
}
}
