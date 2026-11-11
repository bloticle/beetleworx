#include "online_address.h"
#include "online_room.h"
#include "online_checkpoint.h"
#include <rex/logging.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
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
constexpr auto TransferTimeout=std::chrono::seconds(120);
enum class Wire:uint8_t {Hello=1,Welcome,Profile,Snapshot,Start,Input,Frame,Ping,Pong,ProfileReply,WorldReady,Activate,
    CheckpointBegin,CheckpointChunk,CheckpointEnd,CheckpointProgress,CheckpointInstalled,CheckpointReady,CheckpointAck,
    BootstrapWorldReady,BootstrapBegin,BootstrapPad,BootstrapFrame,BootstrapComplete,BootstrapContinue,BootstrapActivate,GameplayComplete,GameplayVerified};
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
Bytes Packet(Wire kind){return {'B','W','R','M',6,uint8_t(kind)};}
using Hash=std::array<uint8_t,32>;
bool Nonzero(const Hash& hash){return std::any_of(hash.begin(),hash.end(),[](auto n){return n!=0;});}
void WritePad(Bytes& bytes,const Pad& pad){
    U64(bytes,pad.buttons,2);U8(bytes,pad.left_trigger);U8(bytes,pad.right_trigger);
    for(auto n:{pad.left_x,pad.left_y,pad.right_x,pad.right_y})U64(bytes,uint16_t(n),2);
}
Pad ReadPad(Reader& reader){
    Pad pad;pad.buttons=uint16_t(reader.Number(2));pad.left_trigger=reader.Byte();pad.right_trigger=reader.Byte();
    for(auto* n:{&pad.left_x,&pad.left_y,&pad.right_x,&pad.right_y})*n=std::bit_cast<int16_t>(uint16_t(reader.Number(2)));return pad;
}
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
bool CompatibleCheckpoint(Identity a,Identity b){a.checkpoint={};b.checkpoint={};return a==b;}
bool BundleHeader(std::span<const uint8_t> bytes,const std::array<uint8_t,32>& digest){
    if(bytes.size()<52 || bytes.size()>kMaxCheckpointBundle)return false;
    Reader r{bytes};if(r.Number(4)!=0x454D3243 || r.Number(2)!=1 || r.Number(2)!=0)return false;
    const auto count=r.Number(4),total=r.Number(8);if(!count || count>128 || total>32*1024*1024)return false;
    for(auto n:digest)if(r.Byte()!=n)return false;return r.good;
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
    U64(b,c.jump_button_mask,2);U8(b,c.jump_count);
    for(unsigned i=0;i<c.jump_count && i<kMaxRoomJumpEvents;++i){U64(b,c.jump_events[i].sequence);U8(b,uint8_t(c.jump_events[i].edge));}
}
RoomCommand ReadCommand(Reader& r){
    RoomCommand c;c.x=std::bit_cast<float>(uint32_t(r.Number(4)));c.y=std::bit_cast<float>(uint32_t(r.Number(4)));
    c.forward_x=std::bit_cast<float>(uint32_t(r.Number(4)));c.forward_z=std::bit_cast<float>(uint32_t(r.Number(4)));
    c.actor_epoch=r.Number(8);c.source=RoomMovementSource(r.Byte());
    auto& p=c.pad;p.buttons=uint16_t(r.Number(2));p.left_trigger=r.Byte();p.right_trigger=r.Byte();
    for(auto* n:{&p.left_x,&p.left_y,&p.right_x,&p.right_y})*n=std::bit_cast<int16_t>(uint16_t(r.Number(2)));
    c.jump_button_mask=uint16_t(r.Number(2));c.jump_count=r.Byte();
    if(c.jump_count>kMaxRoomJumpEvents){r.good=false;return c;}
    for(unsigned i=0;i<c.jump_count;++i){c.jump_events[i].sequence=r.Number(8);c.jump_events[i].edge=RoomJumpEdge(r.Byte());}
    return c;
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
    enum class Transfer {None,Sending,AwaitInstalled,AwaitAck,Complete};
    Transfer transfer=Transfer::None;size_t transfer_sent=0,transfer_acked=0;
    Clock::time_point transfer_progress=received;
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
    std::array<RoomJumpStream,kOnlineRoomCapacity> submitted_jumps{},received_jumps{};
    // Updated only by completed-world consensus, never received input/frames.
    Clock::time_point progress=Clock::now();
    uint64_t gameplay_tick=0;
    bool gameplay_exhausted=false;
    std::optional<uint64_t> gameplay_pending,gameplay_verified;
    uint8_t gameplay_window=1;
    struct GameplayRound {std::optional<Hash> local;std::array<std::optional<Hash>,kOnlineRoomCapacity> proofs{};};
    std::map<uint64_t,GameplayRound> gameplay_rounds;
    std::array<uint64_t,kOnlineRoomCapacity> gameplay_proof_next{};
    std::array<bool,kOnlineRoomCapacity> gameplay_proof_exhausted{};
    size_t LocalUnverified()const{size_t n=0;for(const auto& [tick,round]:gameplay_rounds)n+=round.local.has_value();return n;}
    bool ProofCredit(uint64_t tick)const{return !gameplay_exhausted && tick>=gameplay_tick && tick-gameplay_tick<gameplay_window;}
    bool FrameBlocked()const{return gameplay_pending.has_value() || gameplay_exhausted || LocalUnverified()>=gameplay_window;}
    Clock::time_point loading_started{};
    bool checkpoint_mode=false,checkpoint_end=false,install_reported=false,checkpoint_confirmed=false;
    size_t checkpoint_size=0;
    std::shared_ptr<const CheckpointBundle> offered;
    std::shared_ptr<Bytes> downloaded;
    std::optional<std::array<uint8_t,32>> local_world;
    std::array<std::optional<std::array<uint8_t,32>>,kOnlineRoomCapacity> worlds;
    struct BootstrapProof {bool ready=false;Hash digest{};};
    bool native_bootstrap=false,raw_consumed=false;
    uint64_t raw_tick=0;
    std::array<std::optional<Pad>,kOnlineRoomCapacity> raw_pads;
    std::array<std::optional<BootstrapProof>,kOnlineRoomCapacity> raw_proofs;
    std::optional<RoomBootstrapFrame> raw_frame;
    std::optional<BootstrapProof> local_proof;
    std::optional<Pad> local_raw_pad;
    ~Impl(){Close(listener);}
    bool Active()const{return state==RoomState::Listening || state==RoomState::Connecting || state==RoomState::Transferring || state==RoomState::Installing || state==RoomState::Lobby || state==RoomState::Loading || state==RoomState::Bootstrapping || state==RoomState::Running;}
    void Fail(std::string why){error=std::move(why);state=RoomState::Error;Close(listener);connections.clear();inputs.clear();echoes.clear();frames.clear();}
    bool Queue(Connection& c,const Bytes& p){if(c.Queue(p))return true;Fail("Room send queue exceeded its bounded capacity");return false;}
    bool Broadcast(const Bytes& p){for(auto& c:connections)if(c->welcomed && !c->Queue(p)){Fail("Room broadcast queue full");return false;}return true;}
    bool Publish(){
        lobby=Snapshot(roster);auto p=Packet(Wire::Snapshot);U64(p,0,4);U8(p,0);WriteRoster(p,lobby);
        return Broadcast(p);
    }
    bool PumpCheckpoint(Connection& c){
        if(!host || !checkpoint_mode || c.transfer!=Connection::Transfer::Sending)return true;
        if(!offered)return false;
        for(unsigned i=0;i<16 && c.transfer_sent<offered->bytes.size();++i){
            const auto count=std::min<size_t>(1024,offered->bytes.size()-c.transfer_sent);
            if(c.outgoing.size()-c.sent+count+32>MaxQueued)break;
            auto packet=Packet(Wire::CheckpointChunk);U64(packet,c.transfer_sent,4);U64(packet,count,2);
            packet.insert(packet.end(),offered->bytes.begin()+c.transfer_sent,offered->bytes.begin()+c.transfer_sent+count);
            if(!c.Queue(packet))return false;c.transfer_sent+=count;
        }
        if(c.transfer_sent==offered->bytes.size() && c.outgoing.size()-c.sent+16<MaxQueued){
            auto packet=Packet(Wire::CheckpointEnd);U64(packet,c.transfer_sent,4);
            if(!c.Queue(packet))return false;c.transfer=Connection::Transfer::AwaitInstalled;
        }
        return true;
    }
    void BeginLoading(Clock::time_point now){state=RoomState::Loading;loading_started=now;Close(listener);}
    void ResetRawRound(uint64_t tick){raw_tick=tick;raw_pads={};raw_proofs={};raw_frame.reset();local_proof.reset();local_raw_pad.reset();raw_consumed=false;}
    void BeginRaw(){state=RoomState::Bootstrapping;ResetRawRound(id.first_tick);}
    void BeginRunning(Clock::time_point now,uint64_t first){
        state=RoomState::Running;submit_next=first+id.input_delay;frame_next=consume_next=first;
        raw_pads={};raw_proofs={};raw_frame.reset();local_proof.reset();local_raw_pad.reset();raw_consumed=false;
        progress=now;gameplay_tick=first;gameplay_exhausted=false;
        gameplay_pending.reset();gameplay_verified.reset();gameplay_rounds.clear();gameplay_proof_next.fill(first);gameplay_proof_exhausted={};Close(listener);
        for(auto& c:connections){c->wire_next=submit_next;c->received=c->pinged=now;}
        for(uint64_t t=first;t<submit_next;++t){
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
        auto packet=Packet(native_bootstrap?Wire::BootstrapBegin:Wire::Activate);packet.insert(packet.end(),local_world->begin(),local_world->end());
        U64(packet,lobby.revision);U64(packet,PlayerMask(),2);U64(packet,id.first_tick);
        if(!Broadcast(packet))return false;if(native_bootstrap)BeginRaw();else BeginRunning(now,id.first_tick);return true;
    }
    bool EmitRawFrame(){
        if(!host || state!=RoomState::Bootstrapping || raw_frame)return true;
        for(const auto& player:lobby.players)if(!raw_pads[player.peer-1])return true;
        RoomBootstrapFrame frame;frame.tick=raw_tick;frame.players_mask=PlayerMask();
        auto packet=Packet(Wire::BootstrapFrame);U64(packet,raw_tick);U64(packet,frame.players_mask,2);
        for(const auto& player:lobby.players){frame.players[player.peer-1]=*raw_pads[player.peer-1];WritePad(packet,frame.players[player.peer-1]);}
        if(!Broadcast(packet))return false;raw_frame=frame;return true;
    }
    bool AdvanceRaw(Clock::time_point now){
        if(!host || state!=RoomState::Bootstrapping || !raw_frame || !raw_consumed || !local_proof)return true;
        if(now-loading_started>StartupTimeout){Fail("Online native bootstrap exceeded its 180-second startup deadline");return false;}
        for(const auto& player:lobby.players)if(!raw_proofs[player.peer-1])return true;
        bool ready=true;const auto digest=local_proof->digest;
        for(const auto& player:lobby.players){const auto& proof=*raw_proofs[player.peer-1];
            if(proof.digest!=digest){Fail("Native bootstrap completed worlds diverged");return false;}ready&=proof.ready;}
        if(raw_tick==UINT64_MAX || (ready && raw_tick+1>UINT64_MAX-id.input_delay)){Fail("Native bootstrap tick range exhausted");return false;}
        const auto next=raw_tick+1;
        auto packet=Packet(ready?Wire::BootstrapActivate:Wire::BootstrapContinue);
        packet.insert(packet.end(),local_world->begin(),local_world->end());U64(packet,lobby.revision);U64(packet,PlayerMask(),2);
        U64(packet,raw_tick);U64(packet,next);packet.insert(packet.end(),digest.begin(),digest.end());
        if(!Broadcast(packet))return false;if(ready)BeginRunning(now,next);else ResetRawRound(next);return true;
    }
    void WriteGameplayProof(Bytes& packet,uint64_t tick,const Hash& digest)const{
        WriteIdentity(packet,id);packet.insert(packet.end(),local_world->begin(),local_world->end());
        U64(packet,lobby.revision);U64(packet,PlayerMask(),2);U64(packet,tick);packet.insert(packet.end(),digest.begin(),digest.end());
    }
    bool ReadGameplayProof(Reader& reader,uint64_t& tick,Hash& digest)const{
        const auto identity=ReadIdentity(reader);Hash token{};for(auto& value:token)value=reader.Byte();
        const auto revision=reader.Number(8),mask=reader.Number(2);tick=reader.Number(8);for(auto& value:digest)value=reader.Byte();
        return reader.Done() && local_world && identity==id && token==*local_world && revision==lobby.revision && mask==PlayerMask() && Nonzero(digest);
    }
    void GameplayVerified(uint64_t tick,Clock::time_point now){
        gameplay_verified=tick;gameplay_rounds.erase(tick);progress=now;
        if(tick==UINT64_MAX)gameplay_exhausted=true;else gameplay_tick=tick+1;
    }
    bool AdvanceGameplay(Clock::time_point now){
        if(!host || state!=RoomState::Running)return true;
        while(!gameplay_exhausted){const auto found=gameplay_rounds.find(gameplay_tick);
            if(found==gameplay_rounds.end() || !found->second.local)return true;
            const auto& round=found->second;
            for(const auto& player:lobby.players)if(!round.proofs[player.peer-1])return true;
            const auto digest=*round.local;
            for(const auto& player:lobby.players)if(*round.proofs[player.peer-1]!=digest){Fail("Completed native gameplay worlds diverged");return false;}
            const auto tick=gameplay_tick;auto packet=Packet(Wire::GameplayVerified);WriteGameplayProof(packet,tick,digest);
            if(!Broadcast(packet))return false;GameplayVerified(tick,now);
        }
        return true;
    }
    bool CompleteFrames(Clock::time_point){
        while(!frame_exhausted && frames.size()<Window){
            const auto found=inputs.find(frame_next);if(found==inputs.end() || found->second.size()!=lobby.players.size())break;
            RoomFrame frame;frame.tick=frame_next;
            auto p=Packet(Wire::Frame);U64(p,frame.tick);U8(p,uint8_t(lobby.players.size()));
            for(const auto& player:lobby.players){const auto command=found->second.find(player.peer);if(command==found->second.end())return true;
                frame.players_mask|=uint16_t(1u<<(player.peer-1));frame.players[player.peer-1]=command->second;U64(p,player.peer);WriteCommand(p,command->second);}
            if(!Broadcast(p))return false;frames.push_back(std::move(frame));inputs.erase(found);
            if(frame_next==UINT64_MAX)frame_exhausted=true;else ++frame_next;
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
        Reader r{bytes};if(r.Number(4)!=0x4257524D || r.Byte()!=6)return false;const auto kind=Wire(r.Byte());
        if(kind==Wire::Hello){
            if(!host || c.welcomed || (state!=RoomState::Listening && state!=RoomState::Lobby))return false;
            const auto identity=ReadIdentity(r);const auto mode=r.Byte(),window=r.Byte();auto name=r.String();std::string normalized;
            if(!r.Done() || window!=gameplay_window || mode!=uint8_t(checkpoint_mode) || (checkpoint_mode?!CompatibleCheckpoint(identity,id):identity!=id) || !Name(name,normalized) || !UniqueName(Snapshot(roster),0,normalized) || roster.players().size()>=roster.capacity())return false;
            uint64_t assigned=2;while(assigned<=roster.capacity() && roster.Find(assigned))++assigned;
            if(assigned>roster.capacity() || roster.Join(assigned,normalized)!=LobbyResult::Ok)return false;
            c.peer=assigned;c.welcomed=true;state=RoomState::Lobby;
            auto welcome=Packet(Wire::Welcome);WriteIdentity(welcome,id);U64(welcome,assigned);U8(welcome,uint8_t(checkpoint_mode));U8(welcome,gameplay_window);
            if(!c.Queue(welcome)){Fail("Room welcome queue full");return true;}
            if(!Publish())return true;
            if(checkpoint_mode){
                if(!offered)return false;
                auto begin=Packet(Wire::CheckpointBegin);U64(begin,offered->bytes.size(),4);begin.insert(begin.end(),id.checkpoint.begin(),id.checkpoint.end());
                if(!c.Queue(begin))return false;c.transfer=Connection::Transfer::Sending;c.transfer_progress=now;
            }
            return true;
        }
        if(kind==Wire::Welcome){
            if(host || c.welcomed || state!=RoomState::Connecting)return false;
            const auto identity=ReadIdentity(r);const auto assigned=r.Number(8);const auto mode=r.Byte(),window=r.Byte();
            if(!r.Done() || window!=gameplay_window || mode!=uint8_t(checkpoint_mode) || !ValidIdentity(identity) || (checkpoint_mode?!CompatibleCheckpoint(identity,id):identity!=id) || assigned<2 || assigned>kOnlineRoomCapacity)return false;
            id=identity;peer=assigned;c.peer=1;c.welcomed=true;
            state=checkpoint_mode?RoomState::Transferring:RoomState::Lobby;
            if(checkpoint_mode){c.transfer=Connection::Transfer::Sending;c.transfer_progress=now;}return true;
        }
        if(!c.welcomed)return false;
        if(kind==Wire::Ping || kind==Wire::Pong){
            if(!r.Done())return false;if(kind==Wire::Ping){auto p=Packet(Wire::Pong);return c.Queue(p);}return true;
        }
        if(kind>=Wire::CheckpointBegin && kind<=Wire::CheckpointAck){
            if(!checkpoint_mode || state==RoomState::Loading || state==RoomState::Bootstrapping || state==RoomState::Running)return false;
            if(kind==Wire::CheckpointBegin){
                if(host || state!=RoomState::Transferring || checkpoint_size || c.transfer!=Connection::Transfer::Sending)return false;
                const auto size=r.Number(4);std::array<uint8_t,32> digest{};for(auto& b:digest)b=r.Byte();
                if(!r.Done() || size<52 || size>kMaxCheckpointBundle || digest!=id.checkpoint)return false;
                checkpoint_size=size;downloaded=std::make_shared<Bytes>();downloaded->reserve(size);c.transfer_progress=now;return true;
            }
            if(kind==Wire::CheckpointChunk){
                const auto offset=r.Number(4),count=r.Number(2);
                if(host || !downloaded || checkpoint_end || c.transfer!=Connection::Transfer::Sending || !count || count>1024 ||
                    offset!=downloaded->size() || count>checkpoint_size-downloaded->size() || !r.good || r.bytes.size()-r.at!=count)return false;
                downloaded->insert(downloaded->end(),r.bytes.begin()+r.at,r.bytes.end());c.transfer_progress=now;
                auto progress=Packet(Wire::CheckpointProgress);U64(progress,downloaded->size(),4);return c.Queue(progress);
            }
            if(kind==Wire::CheckpointEnd){
                const auto size=r.Number(4);
                if(host || !r.Done() || !downloaded || checkpoint_end || size!=checkpoint_size || downloaded->size()!=size || !BundleHeader(*downloaded,id.checkpoint))return false;
                checkpoint_end=true;state=RoomState::Installing;c.transfer=Connection::Transfer::AwaitInstalled;c.transfer_progress=now;return true;
            }
            if(kind==Wire::CheckpointProgress){
                const auto count=r.Number(4);
                if(!host || !r.Done() || (c.transfer!=Connection::Transfer::Sending && c.transfer!=Connection::Transfer::AwaitInstalled) || count<c.transfer_acked || count>c.transfer_sent)return false;
                if(count>c.transfer_acked){c.transfer_acked=count;c.transfer_progress=now;}return true;
            }
            std::array<uint8_t,32> digest{};for(auto& b:digest)b=r.Byte();if(!r.Done() || digest!=id.checkpoint)return false;
            if(kind==Wire::CheckpointInstalled){
                if(!host || c.transfer!=Connection::Transfer::AwaitInstalled || !offered || c.transfer_acked!=offered->bytes.size())return false;
                auto ready=Packet(Wire::CheckpointReady);ready.insert(ready.end(),digest.begin(),digest.end());
                if(!c.Queue(ready))return false;c.transfer=Connection::Transfer::AwaitAck;c.transfer_progress=now;return true;
            }
            if(kind==Wire::CheckpointReady){
                if(host || state!=RoomState::Installing || !install_reported || c.transfer!=Connection::Transfer::AwaitAck)return false;
                auto ack=Packet(Wire::CheckpointAck);ack.insert(ack.end(),digest.begin(),digest.end());if(!c.Queue(ack))return false;
                c.transfer=Connection::Transfer::Complete;checkpoint_confirmed=true;state=RoomState::Lobby;downloaded.reset();return true;
            }
            if(kind==Wire::CheckpointAck){
                if(!host || c.transfer!=Connection::Transfer::AwaitAck)return false;
                c.transfer=Connection::Transfer::Complete;c.transfer_progress=now;return true;
            }
            return false;
        }
        if(kind==Wire::Profile){
            if(!host || (state!=RoomState::Lobby && state!=RoomState::Loading && state!=RoomState::Bootstrapping && state!=RoomState::Running))return false;
            const auto sequence=uint32_t(r.Number(4));const auto action=r.Byte();std::string name;LobbyCharacter character{};uint8_t color=0,ready=0;
            if(action==1)name=r.String();else if(action==2){character=LobbyCharacter(r.Byte());color=r.Byte();}else if(action==3)ready=r.Byte();else return false;
            if(!r.Done() || ready>1 || sequence!=c.profile_next || !sequence)return false;
            if(c.profile_next==UINT32_MAX)c.profile_next=0;else ++c.profile_next;
            // Requests already in flight when Start freezes the roster receive
            // a normal rejection. They cannot mutate the running room or turn
            // a legitimate Ready/Start race into a protocol disconnect.
            const auto result=state!=RoomState::Lobby?LobbyResult::Locked:
                (checkpoint_mode && action==3 && ready && c.transfer!=Connection::Transfer::Complete)?LobbyResult::NotReady:Apply(c.peer,action,name,character,color,ready!=0);
            if(state==RoomState::Lobby && !Publish())return true;
            auto reply=Packet(Wire::ProfileReply);U64(reply,sequence,4);U8(reply,uint8_t(result));U64(reply,lobby.revision);
            return c.Queue(reply);
        }
        if(kind==Wire::ProfileReply){
            if(host || (state!=RoomState::Lobby && state!=RoomState::Loading && state!=RoomState::Bootstrapping && state!=RoomState::Running))return false;
            const auto sequence=uint32_t(r.Number(4));const auto result=r.Byte();const auto revision=r.Number(8);
            if(!r.Done() || !pending_profile || sequence!=pending_profile || revision!=lobby.revision || result>uint8_t(LobbyResult::NotReady))return false;
            pending_profile=0;profile_result=LobbyResult(result);return true;
        }
        if(kind==Wire::Snapshot){
            if(host || (state!=RoomState::Lobby && state!=RoomState::Connecting && state!=RoomState::Transferring && state!=RoomState::Installing))return false;
            const auto ack=uint32_t(r.Number(4));const auto result=r.Byte();auto snapshot=ReadRoster(r);
            if(!r.Done() || !snapshot || snapshot->locked || ack || result ||
                snapshot->revision<lobby.revision || (snapshot->revision==lobby.revision && *snapshot!=lobby) ||
                std::none_of(snapshot->players.begin(),snapshot->players.end(),[&](const auto& p){return p.peer==peer;}))return false;
            lobby=std::move(*snapshot);return true;
        }
        if(kind==Wire::Start){
            if(host || state!=RoomState::Lobby)return false;
            const auto mode=r.Byte();if(mode>1)return false;
            auto snapshot=ReadRoster(r);if(!r.Done() || !snapshot || !snapshot->locked || snapshot->players.size()<2 || snapshot->capacity!=lobby.capacity || snapshot->players.size()!=lobby.players.size())return false;
            auto expected=lobby;expected.locked=true;expected.revision=snapshot->revision;
            if(*snapshot!=expected || snapshot->revision<=lobby.revision)return false;
            lobby=std::move(*snapshot);native_bootstrap=mode!=0;BeginLoading(now);return true;
        }
        if(kind==Wire::WorldReady || kind==Wire::BootstrapWorldReady){
            if(!host || native_bootstrap!=(kind==Wire::BootstrapWorldReady) ||
                (state!=RoomState::Loading && state!=RoomState::Bootstrapping && state!=RoomState::Running))return false;
            std::array<uint8_t,32> token{};for(auto& b:token)b=r.Byte();
            if(!r.Done() || !std::any_of(token.begin(),token.end(),[](auto b){return b!=0;}))return false;
            auto& previous=worlds[c.peer-1];if(previous && *previous!=token)return false;
            if(state!=RoomState::Loading)return previous && *previous==token;
            previous=token;return ActivateIfReady(now);
        }
        if(kind==Wire::Activate || kind==Wire::BootstrapBegin){
            if(host || state!=RoomState::Loading || !local_world || native_bootstrap!=(kind==Wire::BootstrapBegin))return false;
            std::array<uint8_t,32> token{};for(auto& b:token)b=r.Byte();
            const auto revision=r.Number(8),mask=r.Number(2),tick=r.Number(8);
            if(!r.Done() || token!=*local_world || revision!=lobby.revision || mask!=PlayerMask() || tick!=id.first_tick)return false;
            if(native_bootstrap)BeginRaw();else BeginRunning(now,id.first_tick);return true;
        }
        if(kind==Wire::BootstrapPad){
            if(!host || state!=RoomState::Bootstrapping || !native_bootstrap || raw_frame)return false;
            const auto tick=r.Number(8);const auto pad=ReadPad(r);
            if(!r.Done() || tick!=raw_tick || raw_pads[c.peer-1])return false;raw_pads[c.peer-1]=pad;return true;
        }
        if(kind==Wire::BootstrapFrame){
            if(host || state!=RoomState::Bootstrapping || !native_bootstrap || raw_frame || !local_raw_pad)return false;
            RoomBootstrapFrame frame;frame.tick=r.Number(8);frame.players_mask=uint16_t(r.Number(2));
            if(frame.tick!=raw_tick || frame.players_mask!=PlayerMask())return false;
            for(const auto& player:lobby.players)frame.players[player.peer-1]=ReadPad(r);
            if(!r.Done() || frame.players[peer-1]!=*local_raw_pad)return false;raw_frame=frame;return true;
        }
        if(kind==Wire::BootstrapComplete){
            if(!host || state!=RoomState::Bootstrapping || !raw_frame || raw_proofs[c.peer-1])return false;
            const auto tick=r.Number(8);const auto ready=r.Byte();BootstrapProof proof;proof.ready=ready!=0;for(auto& b:proof.digest)b=r.Byte();
            if(!r.Done() || ready>1 || tick!=raw_tick || !Nonzero(proof.digest))return false;
            raw_proofs[c.peer-1]=proof;return AdvanceRaw(now);
        }
        if(kind==Wire::BootstrapContinue || kind==Wire::BootstrapActivate){
            if(host || state!=RoomState::Bootstrapping || !raw_consumed || !local_proof || !local_world)return false;
            Hash token{},digest{};for(auto& b:token)b=r.Byte();const auto revision=r.Number(8),mask=r.Number(2),completed=r.Number(8),next=r.Number(8);for(auto& b:digest)b=r.Byte();
            const bool activate=kind==Wire::BootstrapActivate;
            if(!r.Done() || token!=*local_world || revision!=lobby.revision || mask!=PlayerMask() || completed!=raw_tick || completed==UINT64_MAX ||
                next!=completed+1 || digest!=local_proof->digest || (activate && (!local_proof->ready || next>UINT64_MAX-id.input_delay)))return false;
            if(activate)BeginRunning(now,next);else ResetRawRound(next);return true;
        }
        if(kind==Wire::GameplayComplete){
            if(!host || state!=RoomState::Running || gameplay_exhausted || !c.peer || c.peer>kOnlineRoomCapacity || gameplay_proof_exhausted[c.peer-1])return false;
            uint64_t tick=0;Hash digest{};
            if(!ReadGameplayProof(r,tick,digest) || !ProofCredit(tick) || tick!=gameplay_proof_next[c.peer-1] || (!frame_exhausted && frame_next<=tick))return false;
            auto& round=gameplay_rounds[tick];if(round.proofs[c.peer-1])return false;round.proofs[c.peer-1]=digest;
            if(tick==UINT64_MAX)gameplay_proof_exhausted[c.peer-1]=true;else gameplay_proof_next[c.peer-1]=tick+1;
            return AdvanceGameplay(now);
        }
        if(kind==Wire::GameplayVerified){
            if(host || state!=RoomState::Running || gameplay_exhausted)return false;
            uint64_t tick=0;Hash digest{};
            if(!ReadGameplayProof(r,tick,digest) || tick!=gameplay_tick)return false;
            const auto round=gameplay_rounds.find(tick);
            if(round==gameplay_rounds.end() || !round->second.local || digest!=*round->second.local)return false;
            GameplayVerified(tick,now);return true;
        }
        if(kind==Wire::Input){
            if(!host || state!=RoomState::Running)return false;
            const auto tick=r.Number(8);const auto command=ReadCommand(r);
            if(!r.Done() || !ValidCommand(command) || c.wire_exhausted || tick!=c.wire_next || tick<consume_next || tick-consume_next>=Window || inputs[tick].contains(c.peer))return false;
            auto jump_state=submitted_jumps[c.peer-1];if(!AdvanceRoomJumpStream(jump_state,command))return false;
            submitted_jumps[c.peer-1]=jump_state;
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
            auto jump_states=received_jumps;
            for(const auto& player:lobby.players)if(!AdvanceRoomJumpStream(jump_states[player.peer-1],frame.players[player.peer-1]))return false;
            received_jumps=jump_states;
            echoes.erase(echo);frames.push_back(std::move(frame));if(frame_next==UINT64_MAX)frame_exhausted=true;else ++frame_next;return true;
        }
        return false;
    }
};

Room::Room():impl_(std::make_unique<Impl>()){}
Room::~Room()=default;
bool Room::SetGameplayVerificationWindow(uint8_t window){auto& p=*impl_;if(p.state!=RoomState::Idle || window<1 || window>8)return false;p.gameplay_window=window;return true;}
uint8_t Room::gameplay_verification_window()const{return impl_->gameplay_window;}
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
    return JoinInternal(id,address,port,name,false);
}
bool Room::HostWithCheckpoint(const Identity& id,std::string_view address,uint16_t port,uint8_t capacity,std::string_view name,std::shared_ptr<const CheckpointBundle> bundle){
    if(!bundle || bundle->identity!=id.checkpoint || !BundleHeader(bundle->bytes,id.checkpoint))return false;
    if(!Host(id,address,port,capacity,name))return false;
    impl_->checkpoint_mode=true;impl_->offered=std::move(bundle);return true;
}
bool Room::JoinWithCheckpoint(const Identity& id,std::string_view address,uint16_t port,std::string_view name){
    return JoinInternal(id,address,port,name,true);
}
bool Room::JoinInternal(const Identity& id,std::string_view address,uint16_t port,std::string_view name,bool checkpoint){
    auto& p=*impl_;std::string normalized;auto endpoint=ParseNumericAddress(address);
    auto check=id;if(checkpoint)check.checkpoint[0]=1;
    if(p.state!=RoomState::Idle || !Winsock() || !endpoint || !port || !ValidIdentity(check) || !Name(name,normalized))return false;
    endpoint->Port(port);auto c=std::make_unique<Connection>();c->socket=socket(endpoint->storage.ss_family,SOCK_STREAM,IPPROTO_TCP);
    if(c->socket==INVALID_SOCKET || !Nonblocking(c->socket))return false;
    const auto result=connect(c->socket,reinterpret_cast<const sockaddr*>(&endpoint->storage),endpoint->size);
    if(result==SOCKET_ERROR && WSAGetLastError()!=WSAEWOULDBLOCK)return false;
    c->connecting=result!=0;p.id=id;p.join_name=normalized;p.state=RoomState::Connecting;p.checkpoint_mode=checkpoint;
    auto hello=Packet(Wire::Hello);WriteIdentity(hello,id);U8(hello,uint8_t(checkpoint));U8(hello,p.gameplay_window);Text(hello,normalized);if(!c->Queue(hello))return false;
    p.connections.push_back(std::move(c));return true;
}
std::shared_ptr<const std::vector<uint8_t>> Room::DownloadedCheckpoint()const{
    const auto& p=*impl_;return !p.host && p.state==RoomState::Installing && p.checkpoint_end && !p.install_reported?p.downloaded:nullptr;
}
bool Room::ConfirmCheckpointInstalled(bool success){
    auto& p=*impl_;if(!DownloadedCheckpoint() || p.connections.size()!=1)return false;
    if(!success){p.Fail("The shared checkpoint could not be installed or verified; normal saves were not selected");return true;}
    auto& c=*p.connections.front();auto packet=Packet(Wire::CheckpointInstalled);packet.insert(packet.end(),p.id.checkpoint.begin(),p.id.checkpoint.end());
    if(!p.Queue(c,packet))return false;p.install_reported=true;c.transfer=Connection::Transfer::AwaitAck;c.transfer_progress=Clock::now();return true;
}
bool Room::checkpoint_ready()const{
    const auto& p=*impl_;return p.checkpoint_mode?(p.host || p.checkpoint_confirmed):(p.state==RoomState::Lobby || p.state==RoomState::Loading || p.state==RoomState::Bootstrapping || p.state==RoomState::Running);
}
std::vector<RoomCheckpointPeer> Room::checkpoint_peers()const{
    const auto& p=*impl_;std::vector<RoomCheckpointPeer> result;
    for(const auto& player:p.lobby.players){
        RoomCheckpointPeer info;info.peer=player.peer;
        if(!p.checkpoint_mode)info.installed=true;
        else if(p.host){
            info.total=p.offered?p.offered->bytes.size():0;
            if(player.peer==1){info.installed=true;info.received=info.total;}
            else for(const auto& c:p.connections)if(c->peer==player.peer){info.received=c->transfer_acked;info.installed=c->transfer==Connection::Transfer::Complete;}
        }else if(player.peer==p.peer){info.total=p.checkpoint_size;info.received=p.downloaded?p.downloaded->size():p.checkpoint_confirmed?p.checkpoint_size:0;info.installed=p.checkpoint_confirmed;}
        result.push_back(info);
    }
    return result;
}
void Room::Poll(Clock::time_point now){
    auto& p=*impl_;if(!p.Active())return;
    if(p.state==RoomState::Running && now-p.progress>Timeout){p.Fail("Online room completed-world progress timed out; simulation stopped");return;}
    if((p.state==RoomState::Loading || p.state==RoomState::Bootstrapping) && now-p.loading_started>StartupTimeout){p.Fail("Online native world startup exceeded its 180-second deadline");return;}
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
                // A legal 64-frame sixteen-player V2 burst is larger than
                // eight 8 KiB packets. Keep it bounded by the queue budget.
                if(n>0){if(c.incoming.size()+size_t(n)>MaxQueued){alive=false;break;}c.incoming.insert(c.incoming.end(),buffer.begin(),buffer.begin()+n);}
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
            if(alive && p.host && p.checkpoint_mode)alive=p.PumpCheckpoint(c);
            if(alive)alive=c.Flush();
        }
        const bool transferring=p.checkpoint_mode && c.welcomed && c.transfer!=Connection::Transfer::Complete;
        if(transferring && now-c.transfer_progress>TransferTimeout)alive=false;
        if(now-c.received>((p.state==RoomState::Loading || p.state==RoomState::Bootstrapping)?StartupTimeout:transferring?TransferTimeout:Timeout))alive=false;
        if(!alive){
            if(!p.host || (c.welcomed && (p.state==RoomState::Loading || p.state==RoomState::Bootstrapping || p.state==RoomState::Running))){p.Fail("Online room participant disconnected, timed out, or sent invalid data; simulation stopped");return;}
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
    if(p.state==RoomState::Bootstrapping && !p.EmitRawFrame())return;
    if(p.state==RoomState::Running){
        if(p.host && !p.CompleteFrames(now))return;
        if(now-p.progress>Timeout)p.Fail("Online room completed-world progress timed out; simulation stopped");
    }
}
bool Room::Rename(std::string_view name){return impl_->Profile(1,name);}
bool Room::SetAppearance(LobbyCharacter c,uint8_t color){return impl_->Profile(2,{},c,color);}
bool Room::SetReady(bool ready){return impl_->Profile(3,{},LobbyCharacter::Mickey,0,ready);}
bool Room::Start(){return StartInternal(false);}
bool Room::StartNativeBootstrap(){return StartInternal(true);}
bool Room::StartInternal(bool bootstrap){
    auto& p=*impl_;if(!p.host || p.state!=RoomState::Lobby || p.roster.players().size()<2)return false;
    if(p.checkpoint_mode && std::any_of(p.connections.begin(),p.connections.end(),[](const auto& c){return c->welcomed && c->transfer!=Connection::Transfer::Complete;}))return false;
    if(p.roster.Start()!=LobbyResult::Ok)return false;p.lobby=Snapshot(p.roster);
    // Pending unadmitted TCP sockets do not become gameplay participants.
    for(size_t i=0;i<p.connections.size();)if(!p.connections[i]->welcomed)p.connections.erase(p.connections.begin()+i);else ++i;
    auto start=Packet(Wire::Start);U8(start,uint8_t(bootstrap));WriteRoster(start,p.lobby);if(!p.Broadcast(start))return false;
    p.native_bootstrap=bootstrap;p.BeginLoading(Clock::now());return true;
}
bool Room::WorldReady(const std::array<uint8_t,32>& token){return WorldReadyInternal(token,false);}
bool Room::BootstrapWorldReady(const std::array<uint8_t,32>& token){return WorldReadyInternal(token,true);}
bool Room::WorldReadyInternal(const std::array<uint8_t,32>& token,bool bootstrap){
    auto& p=*impl_;
    if(p.native_bootstrap!=bootstrap || (p.state!=RoomState::Loading && p.state!=RoomState::Bootstrapping && p.state!=RoomState::Running))return false;
    if(p.local_world)return *p.local_world==token;
    if(p.state!=RoomState::Loading || !std::any_of(token.begin(),token.end(),[](auto b){return b!=0;}))return false;
    const auto now=Clock::now();if(now-p.loading_started>StartupTimeout){p.Fail("Online native world startup exceeded its 180-second deadline");return false;}
    p.local_world=token;
    if(p.host){p.worlds[p.peer-1]=token;return p.ActivateIfReady(now);}
    auto packet=Packet(bootstrap?Wire::BootstrapWorldReady:Wire::WorldReady);packet.insert(packet.end(),token.begin(),token.end());return p.Queue(*p.connections.front(),packet);
}
bool Room::native_bootstrap()const{return impl_->native_bootstrap;}
uint64_t Room::bootstrap_tick()const{return impl_->raw_tick;}
bool Room::SubmitBootstrapPad(uint64_t tick,const Pad& pad){
    auto& p=*impl_;if(p.state!=RoomState::Bootstrapping || !p.native_bootstrap || tick!=p.raw_tick || p.local_raw_pad || p.raw_frame || p.local_proof)return false;
    if(Clock::now()-p.loading_started>StartupTimeout){p.Fail("Online native bootstrap exceeded its 180-second startup deadline");return false;}
    if(p.host)p.raw_pads[p.peer-1]=pad;
    else {auto packet=Packet(Wire::BootstrapPad);U64(packet,tick);WritePad(packet,pad);if(!p.Queue(*p.connections.front(),packet))return false;}
    p.local_raw_pad=pad;return true;
}
bool Room::PeekBootstrapFrame(RoomBootstrapFrame& frame)const{
    const auto& p=*impl_;if(p.state!=RoomState::Bootstrapping || !p.raw_frame || p.raw_consumed)return false;frame=*p.raw_frame;return true;
}
bool Room::TakeBootstrapFrame(RoomBootstrapFrame& frame){
    auto& p=*impl_;if(!PeekBootstrapFrame(frame))return false;p.raw_consumed=true;return true;
}
bool Room::CompleteBootstrap(uint64_t tick,bool ready,const Hash& digest){
    auto& p=*impl_;if(p.state!=RoomState::Bootstrapping || !p.raw_consumed || !p.raw_frame || tick!=p.raw_tick || p.local_proof || !Nonzero(digest))return false;
    if(Clock::now()-p.loading_started>StartupTimeout){p.Fail("Online native bootstrap exceeded its 180-second startup deadline");return false;}
    p.local_proof=Impl::BootstrapProof{ready,digest};
    if(p.host){p.raw_proofs[p.peer-1]=p.local_proof;return p.AdvanceRaw(Clock::now());}
    auto packet=Packet(Wire::BootstrapComplete);U64(packet,tick);U8(packet,uint8_t(ready));packet.insert(packet.end(),digest.begin(),digest.end());
    if(!p.Queue(*p.connections.front(),packet))return false;return true;
}
bool Room::SubmitLocal(uint64_t tick,const RoomCommand& command){
    auto& p=*impl_;if(p.state!=RoomState::Running || p.submit_exhausted || tick!=p.submit_next || tick<p.consume_next || tick-p.consume_next>=Window || !ValidCommand(command))return false;
    auto jump_state=p.submitted_jumps[p.peer-1];if(!AdvanceRoomJumpStream(jump_state,command))return false;
    if(p.host)p.inputs[tick][p.peer]=command;
    else {auto packet=Packet(Wire::Input);U64(packet,tick);WriteCommand(packet,command);if(!p.Queue(*p.connections.front(),packet))return false;p.echoes[tick]=command;}
    p.submitted_jumps[p.peer-1]=jump_state;
    if(p.submit_next==UINT64_MAX)p.submit_exhausted=true;else ++p.submit_next;return true;
}
bool Room::PeekFrame(RoomFrame& frame)const{
    const auto& p=*impl_;
    if(p.state!=RoomState::Running || p.consume_exhausted || p.FrameBlocked() || p.frames.empty() ||
       p.frames.front().tick!=p.consume_next || !p.ProofCredit(p.consume_next))return false;
    frame=p.frames.front();return true;
}
bool Room::TakeFrame(RoomFrame& frame){
    auto& p=*impl_;if(p.state!=RoomState::Running || p.consume_exhausted || p.FrameBlocked() || p.frames.empty())return false;
    if(p.frames.front().tick!=p.consume_next){p.Fail("Room frame consumption lost contiguous ordering");return false;}
    if(!p.ProofCredit(p.consume_next))return false;
    p.gameplay_pending=p.consume_next;
    frame=std::move(p.frames.front());p.frames.pop_front();if(p.consume_next==UINT64_MAX)p.consume_exhausted=true;else ++p.consume_next;return true;
}
bool Room::CompleteGameplay(uint64_t tick,const Hash& digest){
    auto& p=*impl_;
    if(p.state!=RoomState::Running || !p.gameplay_pending || tick!=*p.gameplay_pending || !p.ProofCredit(tick) ||
       p.gameplay_exhausted || p.gameplay_proof_exhausted[p.peer-1] || tick!=p.gameplay_proof_next[p.peer-1] ||
       !Nonzero(digest) || !p.local_world || p.LocalUnverified()>=p.gameplay_window)return false;
    const auto now=Clock::now();if(now-p.progress>Timeout){p.Fail("Online room completed-world progress timed out; simulation stopped");return false;}
    auto& round=p.gameplay_rounds[tick];if(round.local || round.proofs[p.peer-1])return false;
    round.local=digest;round.proofs[p.peer-1]=digest;
    p.gameplay_pending.reset();
    if(tick==UINT64_MAX)p.gameplay_proof_exhausted[p.peer-1]=true;else p.gameplay_proof_next[p.peer-1]=tick+1;
    if(p.host)return p.AdvanceGameplay(now);
    auto packet=Packet(Wire::GameplayComplete);p.WriteGameplayProof(packet,tick,digest);return p.Queue(*p.connections.front(),packet);
}
bool Room::gameplay_completion_pending()const{return impl_->gameplay_pending.has_value() || impl_->LocalUnverified()!=0;}
bool Room::gameplay_frame_blocked()const{return impl_->state!=RoomState::Running || impl_->consume_exhausted || impl_->FrameBlocked() || !impl_->ProofCredit(impl_->consume_next);}
size_t Room::pending_gameplay_verifications()const{return impl_->LocalUnverified();}
std::optional<uint64_t> Room::verified_gameplay_tick()const{return impl_->gameplay_verified;}
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
    uint8_t fixture_verification_window=1;
    auto pump=[&](auto done){const auto deadline=Clock::now()+std::chrono::seconds(5);do{for(auto& r:rooms)r->Poll();if(done())return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(Clock::now()<deadline);return false;};
    // Explicit fixture digests acknowledge fixture processing, not a real game world.
    auto fixture_digest=[](uint64_t tick){Hash digest{};digest[0]=0xF1;for(unsigned i=0;i<8;++i)digest[24+i]=uint8_t(tick>>(56-i*8));return digest;};
    auto complete_fixture_tick=[&](uint64_t tick){
        const auto digest=fixture_digest(tick);
        for(auto& room:rooms)if(!room->CompleteGameplay(tick,digest))return false;
        return pump([&]{return std::all_of(rooms.begin(),rooms.end(),[&](const auto& room){return !room->gameplay_completion_pending() && room->verified_gameplay_tick()==tick;});});
    };
    auto setup=[&](unsigned count){rooms.clear();rooms.push_back(std::make_unique<Room>());if(!rooms[0]->SetGameplayVerificationWindow(fixture_verification_window) || !rooms[0]->Host(id,"127.0.0.1",0,uint8_t(count),"Host"))return false;
        for(unsigned i=1;i<count;++i){rooms.push_back(std::make_unique<Room>());if(!rooms.back()->SetGameplayVerificationWindow(fixture_verification_window) || !rooms.back()->Join(id,"127.0.0.1",rooms[0]->port(),"Guest "+std::to_string(i)))return false;}
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
            check(r->TakeFrame(f) && f.tick==t && f.players_mask==0xFFFF && std::all_of(f.players.begin(),f.players.end(),[](const auto& p){return p==Neutral();}),"identical full-roster neutral delay frame");}
        if(t==id.first_tick){
            const auto digest=fixture_digest(t);
            for(unsigned peer=0;peer<15;++peer)check(rooms[peer]->CompleteGameplay(t,digest),"first fifteen native fixture completions submit once");
            check(pump([&]{const auto round=rooms[0]->impl_->gameplay_rounds.find(t);return round!=rooms[0]->impl_->gameplay_rounds.end() && std::count_if(round->second.proofs.begin(),round->second.proofs.end(),[](const auto& proof){return proof.has_value();})==15;}),
                "authority receives exactly fifteen of sixteen frozen peer proofs");
            for(auto& room:rooms){RoomFrame next;check(room->gameplay_completion_pending() && !room->TakeFrame(next),"one missing sixteenth world proof blocks every peer despite queued frame");}
            check(rooms[15]->CompleteGameplay(t,digest) && pump([&]{return std::all_of(rooms.begin(),rooms.end(),[&](const auto& room){return !room->gameplay_completion_pending() && room->verified_gameplay_tick()==t;});}),
                "only the sixteenth matching completion verifies the full frozen room");
        }else check(complete_fixture_tick(t),"all sixteen fixture world proofs agree before next neutral tick");}
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
    check(complete_fixture_tick(102),"all-peer command fixture completes before native timeline advances");
    for(uint64_t tick=103;tick<107;++tick)for(unsigned i=0;i<16;++i)check(rooms[i]->SubmitLocal(tick,commands[i]),"bounded pipelined contiguous input accepted");
    check(pump([&]{return std::all_of(rooms.begin(),rooms.end(),[](const auto& r){return r->impl_->frames.size()==4;});}),"four complete queued ticks arrive without gaps");
    for(uint64_t tick=103;tick<107;++tick){RoomFrame expected;check(rooms[0]->TakeFrame(expected) && expected.tick==tick,"host consumes pipelined ticks in order");
        for(unsigned i=1;i<16;++i){RoomFrame actual;check(rooms[i]->TakeFrame(actual) && actual==expected,"every client consumes the same contiguous pipelined sequence");}
        check(complete_fixture_tick(tick),"queued delayed inputs do not bypass completed-world consensus");}
    // All sixteen independent semantic edge streams survive the real TCP
    // codec and own-input echo; raw held state must not invent repeat presses.
    for(uint64_t tick=107;tick<111;++tick){
        for(unsigned i=0;i<16;++i){auto& command=commands[i];command.source=RoomMovementSource::NativeActionV2;
            command.pad={};command.jump_button_mask=uint16_t(1u<<i);command.jump_events={};command.jump_count=0;
            if(tick==107){command.jump_count=1;command.jump_events[0]={1,RoomJumpEdge::Pressed};}
            else if(tick==109){command.jump_count=2;command.jump_events={RoomJumpEvent{2,RoomJumpEdge::Released},RoomJumpEvent{3,RoomJumpEdge::Pressed}};}
            else if(tick==110){command.jump_count=1;command.jump_events[0]={4,RoomJumpEdge::Released};}
            if(tick!=110)command.pad.buttons=command.jump_button_mask;
            check(rooms[i]->SubmitLocal(tick,command),"sixteen V2 peers submit native press/held/release-press/release without pad inference");
        }
        check(pump([&]{return std::all_of(rooms.begin(),rooms.end(),[](const auto& r){return !r->impl_->frames.empty();});}),"all-peer native edge frame completes");
        RoomFrame expected;check(rooms[0]->TakeFrame(expected) && expected.tick==tick,"host consumes V2 frame in order");
        for(unsigned i=0;i<16;++i)check(expected.players[i]==commands[i],"native event order/sequence and distinct button masks retain peer association");
        for(unsigned i=1;i<16;++i){RoomFrame actual;check(rooms[i]->TakeFrame(actual) && actual==expected,"V2 fields echo bit-exactly to all clients");}
        check(complete_fixture_tick(tick),"native edge fixture completion agrees across sixteen peers");
    }
    for(uint64_t tick=111;tick<111+Window;++tick)for(unsigned i=0;i<16;++i){auto command=commands[i];command.jump_count=2;
        command.jump_events={RoomJumpEvent{5+(tick-111)*2,RoomJumpEdge::Pressed},RoomJumpEvent{6+(tick-111)*2,RoomJumpEdge::Released}};
        check(rooms[i]->SubmitLocal(tick,command),"full legal V2 frame window queues bounded two-edge native batches");}
    check(pump([&]{return std::all_of(rooms.begin(),rooms.end(),[](const auto& r){return r->impl_->frames.size()==Window;});}),"sixteen-peer maximum V2 burst fits the bounded receive budget");
    for(uint64_t tick=111;tick<111+Window;++tick){RoomFrame expected;check(rooms[0]->TakeFrame(expected) && expected.tick==tick,"host drains full V2 window contiguously");
        for(unsigned i=1;i<16;++i){RoomFrame actual;check(rooms[i]->TakeFrame(actual) && actual==expected,"maximum native edge payload remains identical across all sixteen peers");}
        check(complete_fixture_tick(tick),"full receive window drains only through per-tick completion barriers");}
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
    auto take_fixture_tick=[&](uint64_t tick){
        if(!pump([&]{return std::all_of(rooms.begin(),rooms.end(),[](const auto& room){RoomFrame frame;return room->PeekFrame(frame);});}))return false;
        for(auto& room:rooms){RoomFrame frame;if(!room->TakeFrame(frame) || frame.tick!=tick)return false;}return true;
    };
    auto proof_packet=[&](Wire kind,uint64_t tick,Hash digest,unsigned malformed=0){
        auto packet=Packet(kind);auto identity=rooms[0]->identity();if(malformed==1)identity.session[0]^=1;WriteIdentity(packet,identity);
        auto token=world;if(malformed==2)token[0]^=1;packet.insert(packet.end(),token.begin(),token.end());
        U64(packet,rooms[0]->lobby().revision+(malformed==3));U64(packet,malformed==4?1:3,2);U64(packet,tick+(malformed==5));
        if(malformed==6)digest={};packet.insert(packet.end(),digest.begin(),digest.end());return packet;
    };
    if(!check(running(),"remote-first gameplay completion fixture connects"))return false;
    RoomFrame ahead;
    if(!check(pump([&]{return rooms[1]->PeekFrame(ahead);}) && rooms[1]->TakeFrame(ahead) && rooms[1]->CompleteGameplay(ahead.tick,fixture_digest(ahead.tick)) &&
        pump([&]{const auto round=rooms[0]->impl_->gameplay_rounds.find(ahead.tick);return round!=rooms[0]->impl_->gameplay_rounds.end() && round->second.proofs[1].has_value();}),"peer may finish its current native tick before slower host consumes its copy"))return false;
    check(!rooms[0]->gameplay_completion_pending() && !rooms[0]->verified_gameplay_tick(),"remote proof alone never asserts host's world has executed");
    RoomFrame host_copy;
    check(rooms[0]->TakeFrame(host_copy) && host_copy==ahead && rooms[0]->CompleteGameplay(host_copy.tick,fixture_digest(host_copy.tick)) &&
        pump([&]{return !rooms[0]->gameplay_completion_pending() && !rooms[1]->gameplay_completion_pending();}),"host native consumption and own proof remain mandatory after early peer completion");
    if(!check(running(),"gameplay proof admission fixture connects"))return false;
    check(!rooms[0]->CompleteGameplay(id.first_tick,fixture_digest(id.first_tick)),"unconsumed frame cannot claim native world completion");
    if(!check(take_fixture_tick(id.first_tick),"all peers consume one fixture tick before proof"))return false;
    const auto proof=fixture_digest(id.first_tick);
    check(!rooms[0]->CompleteGameplay(id.first_tick,{}) && !rooms[0]->CompleteGameplay(id.first_tick+1,proof) &&
        rooms[0]->CompleteGameplay(id.first_tick,proof) && !rooms[0]->CompleteGameplay(id.first_tick,proof),
        "completion requires exact consumed tick, nonzero digest and a single immutable local proof");
    for(unsigned i=0;i<5;++i)for(auto& room:rooms)room->Poll();
    for(auto& room:rooms){RoomFrame next;check(room->gameplay_completion_pending() && !room->PeekFrame(next) && !room->TakeFrame(next) && !room->verified_gameplay_tick(),
        "queued next frame and heartbeats cannot bypass a missing peer's completed-world proof");}
    check(rooms[1]->CompleteGameplay(id.first_tick,proof) && pump([&]{return std::all_of(rooms.begin(),rooms.end(),[&](const auto& room){return !room->gameplay_completion_pending() && room->verified_gameplay_tick()==id.first_tick;});}),
        "matching all-peer proof receives explicit authoritative completion acknowledgement");
    check(take_fixture_tick(id.first_tick+1) && complete_fixture_tick(id.first_tick+1),"verified acknowledgement admits precisely the next native tick");
    if(!check(running() && take_fixture_tick(id.first_tick),"gameplay digest mismatch fixture consumes exact frame"))return false;
    auto wrong_proof=proof;wrong_proof[0]^=1;
    check(rooms[0]->CompleteGameplay(id.first_tick,proof) && rooms[1]->CompleteGameplay(id.first_tick,wrong_proof) &&
        pump([&]{return rooms[0]->state()==RoomState::Error;}) && !rooms[0]->verified_gameplay_tick(),"unequal completed gameplay digests stop authority instead of advancing");
    if(!check(running() && take_fixture_tick(id.first_tick) && complete_fixture_tick(id.first_tick),"old completed-proof replay fixture verifies first world"))return false;
    auto stale_proof=proof_packet(Wire::GameplayComplete,id.first_tick,proof);rooms[1]->impl_->connections.front()->Queue(stale_proof);
    check(pump([&]{return rooms[0]->state()==RoomState::Error;}),"previously verified proof cannot be replayed into a later native round");
    for(unsigned malformed=1;malformed<=6;++malformed){
        if(!check(running() && take_fixture_tick(id.first_tick),"malformed gameplay proof fixture consumes one tick"))return false;
        auto packet=proof_packet(Wire::GameplayComplete,id.first_tick,proof,malformed);rooms[1]->impl_->connections.front()->Queue(packet);
        check(pump([&]{return rooms[0]->state()==RoomState::Error;}),"host rejects completed proof with wrong identity/world/roster/mask/tick or zero digest");
    }
    if(!check(running() && take_fixture_tick(id.first_tick),"duplicate remote gameplay proof fixture consumes one tick"))return false;
    auto duplicate_proof=proof_packet(Wire::GameplayComplete,id.first_tick,proof);
    rooms[1]->impl_->connections.front()->Queue(duplicate_proof);rooms[1]->impl_->connections.front()->Queue(duplicate_proof);
    check(pump([&]{return rooms[0]->state()==RoomState::Error;}),"duplicate admitted peer proof cannot replace or re-ack an outstanding world");
    for(unsigned malformed=0;malformed<=7;++malformed){
        if(!check(running() && take_fixture_tick(id.first_tick),"forged host gameplay acknowledgement fixture consumes one tick"))return false;
        if(malformed && !rooms[1]->CompleteGameplay(id.first_tick,proof))return false;
        auto ack_digest=proof;if(malformed==7)ack_digest[0]^=1;
        auto packet=proof_packet(Wire::GameplayVerified,id.first_tick,ack_digest,malformed);
        rooms[0]->impl_->connections.front()->Queue(packet);
        check(pump([&]{return rooms[1]->state()==RoomState::Error;}),"client requires own consumed proof plus exact identity/world/roster/tick/digest before accepting host ack");
    }
    if(!check(running() && take_fixture_tick(id.first_tick),"guest cannot forge authoritative completion fixture consumes tick"))return false;
    auto forged_ack=proof_packet(Wire::GameplayVerified,id.first_tick,proof);rooms[1]->impl_->connections.front()->Queue(forged_ack);
    check(pump([&]{return rooms[0]->state()==RoomState::Error;}),"client cannot send authoritative gameplay verification even with valid scope");
    if(!check(running() && take_fixture_tick(id.first_tick),"gameplay stalled-world deadline fixture consumes tick"))return false;
    const auto world_progress=rooms[0]->impl_->progress;const auto stalled_clock=Clock::now();
    for(unsigned seconds=1;seconds<15;++seconds){
        for(auto& room:rooms)check(room->SubmitLocal(id.first_tick+id.input_delay+seconds-1,Neutral()),"bounded delayed input may queue while world proof is missing");
        for(auto& room:rooms)room->Poll(stalled_clock+std::chrono::seconds(seconds));
    }
    check(rooms[0]->state()==RoomState::Running && rooms[0]->impl_->progress==world_progress && rooms[0]->gameplay_completion_pending(),
        "queued authoritative inputs and heartbeat replies do not refresh world-progress deadline");
    rooms[0]->Poll(stalled_clock+Timeout+std::chrono::seconds(1));
    check(rooms[0]->state()==RoomState::Error && !rooms[0]->verified_gameplay_tick(),"absolute native world stall stops despite continuing transport activity");
    if(!check(running() && take_fixture_tick(id.first_tick),"gameplay pending peer loss fixture consumes tick"))return false;
    rooms[1]->Disconnect();check(pump([&]{return rooms[0]->state()==RoomState::Error;}),"disconnect with an incomplete native tick never implicitly verifies it");
    const auto gameplay_saved_id=id;id.first_tick=UINT64_MAX-Window;
    if(!check(running(),"gameplay completion overflow fixture begins legal late timeline"))return false;
    for(unsigned round=0;round<=Window;++round){
        const auto tick=id.first_tick+round;
        if(round+id.input_delay<=Window)for(auto& room:rooms)check(room->SubmitLocal(tick+id.input_delay,Neutral()),"late timeline queues only representable source tick");
        if(!check(take_fixture_tick(tick) && complete_fixture_tick(tick),"maximum timeline completes exact tick through real peer proof barrier"))return false;
    }
    for(auto& room:rooms){RoomFrame frame;check(room->verified_gameplay_tick()==UINT64_MAX && !room->gameplay_completion_pending() &&
        !room->TakeFrame(frame) && !room->SubmitLocal(0,Neutral()) && !room->CompleteGameplay(0,proof),"verified UINT64_MAX world cannot wrap native admission or proof ownership");}
    id=gameplay_saved_id;

    // Real TCP windowed completion remains exact tick/digest verification;
    // these digests acknowledge fixture execution, not a native game world.
    fixture_verification_window=8;
    auto window_running=[&](unsigned count){
        if(!setup(count))return false;
        for(auto& room:rooms)if(!room->SetReady(true))return false;
        if(!pump([&]{return std::none_of(rooms.begin(),rooms.end(),[](const auto& room){return room->profile_pending();}) &&
            std::all_of(rooms[0]->lobby().players.begin(),rooms[0]->lobby().players.end(),[](const auto& player){return player.ready;});}) || !rooms[0]->Start())return false;
        if(!pump([&]{return std::all_of(rooms.begin(),rooms.end(),[](const auto& room){return room->state()==RoomState::Loading;});}))return false;
        for(auto& room:rooms)if(!room->WorldReady(world))return false;
        return pump([&]{return std::all_of(rooms.begin(),rooms.end(),[](const auto& room){return room->state()==RoomState::Running && room->gameplay_verification_window()==8;});});
    };
    {Room limits;check(!limits.SetGameplayVerificationWindow(0) && !limits.SetGameplayVerificationWindow(9) &&
        limits.gameplay_verification_window()==1 && limits.SetGameplayVerificationWindow(8),"completion credit bound1..8 is explicit, default synchronous1");}
    if(!check(window_running(16),"all sixteen real peers negotiate eight completed-world credits"))return false;
    check(!rooms[0]->SetGameplayVerificationWindow(1) && !rooms[1]->SetGameplayVerificationWindow(1),"verification window cannot change after admission");
    for(uint64_t tick=id.first_tick+id.input_delay;tick<id.first_tick+10;++tick)
        for(auto& room:rooms)check(room->SubmitLocal(tick,Neutral()),"windowed fixture queues bounded independent intent");
    if(!check(pump([&]{return std::all_of(rooms.begin(),rooms.end(),[](const auto& room){return room->impl_->frames.size()>=9;});}),"queued frames exist beyond completion credits"))return false;
    const auto window_progress=rooms[0]->impl_->progress;
    RoomFrame missing;
    if(!check(rooms[15]->TakeFrame(missing) && missing.tick==id.first_tick && !rooms[15]->CompleteGameplay(missing.tick+1,proof),
        "one taken frame awaits its own exact local completion even with eight credits"))return false;
    for(unsigned round=0;round<8;++round){const auto tick=id.first_tick+round;
        for(unsigned peer=0;peer<15;++peer){RoomFrame frame;
            if(!check(rooms[peer]->TakeFrame(frame) && frame.tick==tick && rooms[peer]->CompleteGameplay(tick,fixture_digest(tick)),
                "first fifteen native fixture worlds complete sequentially without waiting for round-trip verification"))return false;
            check(!rooms[peer]->CompleteGameplay(tick,fixture_digest(tick)),"windowed local completion cannot duplicate a native tick");
        }
    }
    if(!check(pump([&]{const auto& rounds=rooms[0]->impl_->gameplay_rounds;
        return rounds.size()==8 && std::all_of(rounds.begin(),rounds.end(),[](const auto& item){return std::count_if(item.second.proofs.begin(),item.second.proofs.end(),[](const auto& p){return p.has_value();})==15;});}),
        "seven future rounds cannot overtake a missing sixteenth peer's oldest proof"))return false;
    for(unsigned peer=0;peer<15;++peer){RoomFrame blocked;
        check(rooms[peer]->pending_gameplay_verifications()==8 && rooms[peer]->gameplay_completion_pending() && rooms[peer]->gameplay_frame_blocked() &&
            !rooms[peer]->PeekFrame(blocked) && !rooms[peer]->TakeFrame(blocked),"eight completed worlds exhaust bounded admission credit despite queued input");}
    check(rooms[0]->impl_->progress==window_progress && !rooms[0]->verified_gameplay_tick(),"future completion proofs and input never extend oldest-world progress deadline");
    check(rooms[15]->CompleteGameplay(missing.tick,fixture_digest(missing.tick)) &&
        pump([&]{return std::all_of(rooms.begin(),rooms.end(),[&](const auto& room){return room->verified_gameplay_tick()==id.first_tick;});}),
        "oldest all-peer consensus releases exactly one ordered credit");
    RoomFrame ninth;
    if(!check(rooms[0]->pending_gameplay_verifications()==7 && !rooms[0]->gameplay_frame_blocked() && rooms[0]->TakeFrame(ninth) &&
        ninth.tick==id.first_tick+8 && rooms[0]->gameplay_frame_blocked(),"released credit permits one taken native tick, which still requires its own local completion"))return false;
    for(unsigned round=1;round<8;++round){RoomFrame frame;const auto tick=id.first_tick+round;
        if(!check(rooms[15]->TakeFrame(frame) && frame.tick==tick && rooms[15]->CompleteGameplay(tick,fixture_digest(tick)),"delayed final peer completes its own contiguous missing rounds"))return false;}
    if(!check(pump([&]{return std::all_of(rooms.begin(),rooms.end(),[&](const auto& room){return room->verified_gameplay_tick()==id.first_tick+7;});}),"authority emits every acknowledgement in exact oldest-first order"))return false;
    if(!check(rooms[0]->gameplay_frame_blocked() && rooms[0]->impl_->gameplay_pending==ninth.tick &&
        rooms[0]->CompleteGameplay(ninth.tick,fixture_digest(ninth.tick)),"older all-peer acknowledgements never erase a newer taken frame's pending native completion"))return false;
    for(unsigned peer=1;peer<16;++peer){RoomFrame frame;
        if(!check(rooms[peer]->TakeFrame(frame) && frame==ninth && rooms[peer]->CompleteGameplay(frame.tick,fixture_digest(frame.tick)),"all remaining native worlds execute released ninth frame"))return false;}
    check(pump([&]{return std::all_of(rooms.begin(),rooms.end(),[&](const auto& room){return !room->gameplay_completion_pending() &&
        room->pending_gameplay_verifications()==0 && room->verified_gameplay_tick()==ninth.tick;});}),"full verification queue drains without fabricating local native execution");
    // A remote may be eight completed worlds ahead of local consumption, but
    // cannot exceed that bound or skip its own proof sequence.
    if(!check(window_running(2),"remote-ahead eight-credit fixture connects"))return false;
    for(uint64_t tick=id.first_tick+id.input_delay;tick<id.first_tick+9;++tick)for(auto& room:rooms)if(!room->SubmitLocal(tick,Neutral()))return false;
    if(!check(pump([&]{return rooms[1]->impl_->frames.size()>=9;}),"remote-ahead fixture frames emitted"))return false;
    for(unsigned i=0;i<8;++i){RoomFrame frame;if(!check(rooms[1]->TakeFrame(frame) && rooms[1]->CompleteGameplay(frame.tick,fixture_digest(frame.tick)),"remote local completion may precede slow host by bounded window"))return false;}
    check(pump([&]{return rooms[0]->impl_->gameplay_rounds.size()==8;}) && !rooms[0]->gameplay_completion_pending() &&
        rooms[0]->pending_gameplay_verifications()==0 && !rooms[0]->verified_gameplay_tick(),"early remote proofs never assert that the host executed a world");
    auto overflow_proof=proof_packet(Wire::GameplayComplete,id.first_tick+8,fixture_digest(id.first_tick+8));
    rooms[1]->impl_->connections.front()->Queue(overflow_proof);
    check(pump([&]{return rooms[0]->state()==RoomState::Error;}),"ninth remote proof cannot allocate beyond negotiated world window");
    if(!check(window_running(2),"eight-credit stalled native-world deadline fixture connects"))return false;
    for(uint64_t tick=id.first_tick+id.input_delay;tick<id.first_tick+8;++tick)for(auto& room:rooms)if(!room->SubmitLocal(tick,Neutral()))return false;
    if(!check(pump([&]{return rooms[0]->impl_->frames.size()>=8;}),"deadline fixture has full eight-frame queue"))return false;
    for(unsigned round=0;round<8;++round){RoomFrame frame;
        if(!check(rooms[0]->TakeFrame(frame) && rooms[0]->CompleteGameplay(frame.tick,fixture_digest(frame.tick)),"host fixture completes bounded worlds while peer has not executed"))return false;}
    const auto credit_deadline=rooms[0]->impl_->progress,credit_clock=Clock::now();
    for(unsigned second=1;second<15;++second){
        for(auto& room:rooms)check(room->SubmitLocal(id.first_tick+7+second,Neutral()),"future intent may queue without extending completion deadline");
        for(auto& room:rooms)room->Poll(credit_clock+std::chrono::seconds(second));
    }
    check(rooms[0]->state()==RoomState::Running && rooms[0]->impl_->progress==credit_deadline && rooms[0]->pending_gameplay_verifications()==8,
        "local completion, future inputs and heartbeat traffic never renew oldest consensus deadline");
    rooms[0]->Poll(credit_clock+Timeout+std::chrono::seconds(1));
    check(rooms[0]->state()==RoomState::Error && !rooms[0]->verified_gameplay_tick(),"full verification window faults on absolute oldest-world deadline");
    if(!check(window_running(2),"future windowed digest mismatch fixture connects"))return false;
    if(!check(pump([&]{return std::all_of(rooms.begin(),rooms.end(),[](const auto& room){return room->impl_->frames.size()>=2;});}),"future mismatch fixture receives both native frames"))return false;
    for(unsigned round=0;round<2;++round){const auto tick=id.first_tick+round;
        for(unsigned peer=0;peer<2;++peer){RoomFrame frame;
            if(!rooms[peer]->TakeFrame(frame) || frame.tick!=tick)return false;
            auto digest=fixture_digest(tick);if(peer==1 && round==1)digest[1]^=1;
            if(!rooms[peer]->CompleteGameplay(tick,digest))return false;
        }
    }
    check(pump([&]{return rooms[0]->state()==RoomState::Error;}) && rooms[0]->verified_gameplay_tick()==id.first_tick,
        "future digest mismatch cannot be hidden by correct older consensus or release its credit");
    if(!check(window_running(2),"out-of-order eight-credit proof fixture connects"))return false;
    auto skipped_proof=proof_packet(Wire::GameplayComplete,id.first_tick+1,fixture_digest(id.first_tick+1));
    rooms[1]->impl_->connections.front()->Queue(skipped_proof);
    check(pump([&]{return rooms[0]->state()==RoomState::Error;}),"within-window future proof still cannot skip a peer's oldest completion");
    if(!check(window_running(2) && take_fixture_tick(id.first_tick),"forged future acknowledgement fixture consumes oldest world"))return false;
    for(auto& room:rooms)if(!room->CompleteGameplay(id.first_tick,proof))return false;
    auto skipped_ack=proof_packet(Wire::GameplayVerified,id.first_tick+1,fixture_digest(id.first_tick+1));
    rooms[0]->impl_->connections.front()->Queue(skipped_ack);
    check(pump([&]{return rooms[1]->state()==RoomState::Error;}),"authority acknowledgement cannot skip oldest local completed proof");
    // Exact handshake agreement; a mismatched newcomer cannot alter host policy.
    rooms.clear();rooms.push_back(std::make_unique<Room>());rooms.push_back(std::make_unique<Room>());
    if(!check(rooms[0]->SetGameplayVerificationWindow(8) && rooms[0]->Host(id,"127.0.0.1",0,2,"Host") &&
        rooms[1]->Join(id,"127.0.0.1",rooms[0]->port(),"Mismatched"),"mismatched credit request reaches native handshake"))return false;
    check(pump([&]{return rooms[1]->state()==RoomState::Error;}) && rooms[0]->gameplay_verification_window()==8 &&
        rooms[0]->lobby().players.size()==1,"Hello rejects mixed completion windows without changing authority");
    const auto window_saved_id=id;id.first_tick=UINT64_MAX-Window;
    if(!check(window_running(2),"eight-credit UINT64 tail fixture begins legal late timeline"))return false;
    for(unsigned round=0;round<Window-7;++round){const auto tick=id.first_tick+round;
        for(auto& room:rooms)if(!room->SubmitLocal(tick+id.input_delay,Neutral()))return false;
        if(!check(take_fixture_tick(tick) && complete_fixture_tick(tick),"late prefix completes exact native fixture worlds"))return false;
    }
    for(unsigned round=Window-7;round+id.input_delay<=Window;++round)
        for(auto& room:rooms)if(!room->SubmitLocal(id.first_tick+round+id.input_delay,Neutral()))return false;
    if(!check(pump([&]{return std::all_of(rooms.begin(),rooms.end(),[](const auto& room){return room->impl_->frames.size()>=8;});}),"last eight representable worlds are queued"))return false;
    for(unsigned round=Window-7;round<=Window;++round){const auto tick=id.first_tick+round;
        for(auto& room:rooms){RoomFrame frame;if(!check(room->TakeFrame(frame) && frame.tick==tick && room->CompleteGameplay(tick,fixture_digest(tick)),
            "full completion window reaches UINT64_MAX without wrapping proof sequence"))return false;}
    }
    check(pump([&]{return std::all_of(rooms.begin(),rooms.end(),[](const auto& room){return room->verified_gameplay_tick()==UINT64_MAX;});}),"all eight tail proofs verify through exact final tick");
    for(auto& room:rooms){RoomFrame frame;check(!room->gameplay_completion_pending() && room->gameplay_frame_blocked() && !room->TakeFrame(frame) &&
        !room->CompleteGameplay(0,proof) && !room->SubmitLocal(0,Neutral()),"exhausted verification/consumption/source clocks cannot restart atzero");}
    id=window_saved_id;
    fixture_verification_window=1;

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
    auto native_jump=[](uint64_t sequence=0,RoomJumpEdge edge=RoomJumpEdge::None){auto command=Neutral();
        command.source=RoomMovementSource::NativeActionV2;command.jump_button_mask=0x1000;
        if(sequence){command.jump_count=1;command.jump_events[0]={sequence,edge};}return command;};
    {
        auto command=native_jump(1,RoomJumpEdge::Pressed);command.jump_count=2;command.jump_events[1]={2,RoomJumpEdge::Released};
        Bytes encoded;WriteCommand(encoded,command);Reader decoded{encoded};
        check(ReadCommand(decoded)==command && decoded.Done(),"V2 native edge payload round trips without native addresses");
        for(size_t length=0;length<encoded.size();++length){Reader truncated{std::span(encoded).first(length)};ReadCommand(truncated);
            check(!truncated.Done(),"every truncated native edge payload rejects");}
        RoomJumpStream state;check(AdvanceRoomJumpStream(state,command) && state.sequence==2 && !state.held,"ordered two-edge batch updates semantic history once");
        const auto before=state;auto bad=native_jump(3,RoomJumpEdge::Pressed);bad.jump_count=2;bad.jump_events[1]={4,RoomJumpEdge::Pressed};
        check(!AdvanceRoomJumpStream(state,bad) && state==before,"invalid later edge leaves entire stream unchanged");
    }
    for(unsigned malformed=0;malformed<7;++malformed){
        if(!check(running(),"remote native edge validation fixture connects"))return false;
        auto bad=native_jump(1,RoomJumpEdge::Pressed);
        if(malformed==0)bad.jump_events[0].edge=RoomJumpEdge::Released;
        if(malformed==1)bad.jump_events[0].sequence=2;
        if(malformed==2)bad.jump_count=3;
        if(malformed==3)bad.jump_events[0].edge=RoomJumpEdge(255);
        if(malformed==4)bad.jump_button_mask=0;
        if(malformed==5)bad.source=RoomMovementSource::NativeActionV1;
        if(malformed==6)bad.jump_button_mask=0x3000;
        auto packet=Packet(Wire::Input);U64(packet,102);WriteCommand(packet,bad);rooms[1]->impl_->connections.front()->Queue(packet);
        check(pump([&]{return rooms[0]->state()==RoomState::Error;}),"host rejects release-without-press, gaps, oversized/unknown edges, missing/multiple button masks and false V1 provenance");
    }
    auto pressed_room=[&]{if(!running())return false;const auto command=native_jump(1,RoomJumpEdge::Pressed);
        if(!rooms[0]->SubmitLocal(102,command) || !rooms[1]->SubmitLocal(102,command) ||
            !pump([&]{return rooms[0]->impl_->frames.size()==3 && rooms[1]->impl_->frames.size()==3;}))return false;
        for(unsigned i=0;i<3;++i){RoomFrame a,b;if(!rooms[0]->TakeFrame(a) || !rooms[1]->TakeFrame(b) || a!=b || !complete_fixture_tick(a.tick))return false;}return true;};
    if(!check(pressed_room(),"held native edge streams agree over real TCP"))return false;
    {
        const auto next=rooms[1]->next_submit_tick();auto bad=native_jump(2,RoomJumpEdge::Pressed);
        bool rejected=!rooms[1]->SubmitLocal(next,bad);bad=native_jump(3,RoomJumpEdge::Released);rejected &= !rooms[1]->SubmitLocal(next,bad);
        bad=native_jump(2,RoomJumpEdge::Released);bad.jump_button_mask=0x2000;rejected &= !rooms[1]->SubmitLocal(next,bad);
        rejected &= !rooms[1]->SubmitLocal(next,Neutral());bad=native_jump();bad.jump_events[1]={2,RoomJumpEdge::Released};rejected &= !rooms[1]->SubmitLocal(next,bad);
        const auto release=native_jump(2,RoomJumpEdge::Released);
        check(rejected && rooms[1]->next_submit_tick()==next && rooms[1]->SubmitLocal(next,release) && rooms[0]->SubmitLocal(next,release) &&
            pump([&]{return !rooms[0]->impl_->frames.empty() && !rooms[1]->impl_->frames.empty();}),"local failed native batches cannot consume tick/sequence or silently downgrade/change binding mask");
    }
    for(unsigned malformed=0;malformed<4;++malformed){
        if(!check(pressed_room(),"remote held native stream fixture connects"))return false;
        auto bad=native_jump(2,RoomJumpEdge::Released);if(malformed==0)bad.jump_events[0].edge=RoomJumpEdge::Pressed;
        if(malformed==1)bad.jump_events[0].sequence=3;if(malformed==2)bad.jump_button_mask=0x2000;if(malformed==3)bad=Neutral();
        auto packet=Packet(Wire::Input);U64(packet,103);WriteCommand(packet,bad);rooms[1]->impl_->connections.front()->Queue(packet);
        check(pump([&]{return rooms[0]->state()==RoomState::Error;}),"authority rejects repeated held press, later gap, mask replacement and V2 downgrade on the socket too");
    }
    if(!check(pressed_room(),"remote frame edge validation fixture connects"))return false;
    {
        const auto release=native_jump(2,RoomJumpEdge::Released);check(rooms[1]->SubmitLocal(103,release),"client records exact local native echo");
        auto forged=Packet(Wire::Frame);U64(forged,103);U8(forged,2);U64(forged,1);WriteCommand(forged,native_jump(3,RoomJumpEdge::Released));
        U64(forged,2);WriteCommand(forged,release);rooms[0]->impl_->connections.front()->Queue(forged);
        check(pump([&]{return rooms[1]->state()==RoomState::Error;}),"client validates another peer's semantic edge history even when its own input echo matches");
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
        check(complete_fixture_tick(0),"zero native tick has explicit completion ownership distinct from no verified tick");
    }
    id=saved;
    // Actual zero-delay raw native bootstrap is distinct from interpreted
    // gameplay and advances only at a completed all-peer observation barrier.
    auto raw_loading=[&](unsigned count){
        if(!setup(count))return false;for(auto& room:rooms)if(!room->SetReady(true))return false;
        if(!pump([&]{return std::all_of(rooms[0]->lobby().players.begin(),rooms[0]->lobby().players.end(),[](const auto& player){return player.ready;}) &&
            std::none_of(rooms.begin(),rooms.end(),[](const auto& room){return room->profile_pending();});}))return false;
        return rooms[0]->StartNativeBootstrap() && pump([&]{return std::all_of(rooms.begin(),rooms.end(),[](const auto& room){return room->state()==RoomState::Loading && room->native_bootstrap();});});
    };
    auto raw_started=[&](unsigned count){if(!raw_loading(count))return false;
        for(auto& room:rooms)if(!room->BootstrapWorldReady(world))return false;
        return pump([&]{return std::all_of(rooms.begin(),rooms.end(),[](const auto& room){return room->state()==RoomState::Bootstrapping;});});
    };
    auto raw_take=[&](uint64_t tick){
        for(unsigned i=0;i<rooms.size();++i){Pad pad;pad.buttons=uint16_t(i*101);pad.left_x=int16_t(-int(i)*777);pad.left_trigger=uint8_t(i*11);
            if(!rooms[i]->SubmitBootstrapPad(tick,pad))return false;}
        if(!pump([&]{RoomBootstrapFrame frame;return std::all_of(rooms.begin(),rooms.end(),[&](const auto& room){return room->PeekBootstrapFrame(frame);});}))return false;
        RoomBootstrapFrame first;if(!rooms[0]->TakeBootstrapFrame(first))return false;
        for(unsigned i=1;i<rooms.size();++i){RoomBootstrapFrame frame;if(!rooms[i]->TakeBootstrapFrame(frame) || frame!=first)return false;}return true;
    };
    if(!check(raw_loading(16),"sixteen real peers negotiate raw bootstrap Loading explicitly"))return false;
    for(auto& room:rooms){RoomFrame interpreted;RoomBootstrapFrame raw;
        check(!room->WorldReady(world) && !room->SubmitLocal(id.first_tick,Neutral()) && !room->SubmitBootstrapPad(id.first_tick,{}) &&
            !room->PeekFrame(interpreted) && !room->PeekBootstrapFrame(raw),"Loading raw mode cannot use interpreted world assertion, input or either frame API");}
    for(unsigned i=0;i<15;++i)check(rooms[i]->BootstrapWorldReady(world),"loaded single-player world assertion does not require gameplay producer readiness");
    check(pump([&]{return std::count_if(rooms[0]->impl_->worlds.begin(),rooms[0]->impl_->worlds.end(),[](const auto& token){return bool(token);})==15;}),"raw bootstrap waits for final loaded native world");
    const auto bootstrap_slow=Clock::now()+std::chrono::seconds(20);for(unsigned i=0;i<15;++i)rooms[i]->Poll(bootstrap_slow);
    check(std::all_of(rooms.begin(),rooms.end(),[](const auto& room){return room->state()==RoomState::Loading;}),"last loader may block longer than runtime timeout without beginning a raw round");
    check(rooms[15]->BootstrapWorldReady(world) && pump([&]{return std::all_of(rooms.begin(),rooms.end(),[](const auto& room){return room->state()==RoomState::Bootstrapping;});}),"all matching loaded worlds open raw round at original first tick with zero delay");
    for(unsigned round=0;round<3;++round){
        const auto tick=id.first_tick+round;Hash digest{};digest[0]=uint8_t(50+round);
        for(auto& room:rooms){RoomFrame interpreted;check(room->bootstrap_tick()==tick && !room->PeekFrame(interpreted) && !room->SubmitLocal(tick,Neutral()) &&
            !room->CompleteBootstrap(tick,true,digest),"raw round is typed separately and cannot complete before consumed native frame");}
        for(unsigned i=0;i<15;++i){Pad pad;pad.buttons=uint16_t(i*101);pad.left_x=int16_t(-int(i)*777);pad.left_trigger=uint8_t(i*11);check(rooms[i]->SubmitBootstrapPad(tick,pad),"only current raw round input accepted");}
        for(unsigned i=0;i<3;++i)for(auto& room:rooms)room->Poll();RoomBootstrapFrame pending;
        check(!rooms[0]->PeekBootstrapFrame(pending) && !rooms[0]->SubmitBootstrapPad(tick+1,{}),"one missing peer stalls raw frame and future round submission");
        Pad last;last.buttons=1515;last.left_x=-11655;last.left_trigger=165;check(rooms[15]->SubmitBootstrapPad(tick,last),"last raw input completes full frozen roster");
        check(pump([&]{RoomBootstrapFrame frame;return std::all_of(rooms.begin(),rooms.end(),[&](const auto& room){return room->PeekBootstrapFrame(frame);});}),"same authoritative raw frame reaches all sixteen clients/host");
        RoomBootstrapFrame authoritative;
        for(unsigned i=0;i<16;++i){RoomBootstrapFrame frame;check(rooms[i]->TakeBootstrapFrame(frame) && frame.tick==tick && frame.players_mask==0xFFFF &&
            (i==0 || frame==authoritative),"raw input own-echo and complete frame agree before native tick");if(i==0)authoritative=frame;
            check(!rooms[i]->TakeBootstrapFrame(frame) && !rooms[i]->CompleteBootstrap(tick+1,true,digest),"each raw frame consumed once; future completion rejected");}
        for(unsigned i=0;i<15;++i){const bool ready=round==2 || (round==0?i!=5:true);
            check(rooms[i]->CompleteBootstrap(tick,ready,digest) && !rooms[i]->CompleteBootstrap(tick,ready,digest),"completed native proof is one-shot with a fresh per-round readiness vote");}
        for(unsigned i=0;i<3;++i)for(auto& room:rooms)room->Poll();
        check(rooms[0]->state()==RoomState::Bootstrapping && rooms[0]->bootstrap_tick()==tick && !rooms[0]->SubmitBootstrapPad(tick+1,{}),"no Continue or cutover before last peer completed same native tick");
        if(round==0){const auto slow_complete=Clock::now()+std::chrono::seconds(20);for(unsigned i=0;i<15;++i)rooms[i]->Poll(slow_complete);
            check(rooms[0]->state()==RoomState::Bootstrapping && rooms[0]->bootstrap_tick()==tick,"delayed native completion beyond15seconds retains startup barrier");}
        check(rooms[15]->CompleteBootstrap(tick,round!=1,digest),"last completed proof participates in this round only");
        if(round<2)check(pump([&]{return std::all_of(rooms.begin(),rooms.end(),[&](const auto& room){return room->state()==RoomState::Bootstrapping && room->bootstrap_tick()==tick+1;});}),"one false current readiness vote orders Continue without sticky previous votes");
        else check(pump([&]{return std::all_of(rooms.begin(),rooms.end(),[](const auto& room){return room->state()==RoomState::Running;});}),"all-ready matching proofs at same completed tick atomically activate interpreted gameplay");
    }
    const auto first_game=id.first_tick+3;
    for(auto& room:rooms){RoomBootstrapFrame raw;check(room->next_tick()==first_game && room->next_submit_tick()==first_game+id.input_delay &&
        !room->PeekBootstrapFrame(raw) && !room->TakeBootstrapFrame(raw) && !room->SubmitBootstrapPad(first_game,{}),"cutover clears raw state and retains negotiated gameplay input delay at next exact tick");}
    for(uint64_t tick=first_game;tick<first_game+id.input_delay;++tick){
        check(pump([&]{RoomFrame frame;return std::all_of(rooms.begin(),rooms.end(),[&](const auto& room){return room->PeekFrame(frame);});}),"post-bootstrap neutral gameplay delay frame arrives");
        for(auto& room:rooms){RoomFrame frame;check(room->TakeFrame(frame) && frame.tick==tick && frame.players_mask==0xFFFF &&
            std::all_of(frame.players.begin(),frame.players.end(),[](const auto& movement){return movement==Neutral();}),"neutral seeds begin after completed raw tick, never overwrite raw history");}
        check(complete_fixture_tick(tick),"raw cutover neutral gameplay frames require real fixture completions");
    }
    for(auto& room:rooms)check(room->SubmitLocal(first_game+id.input_delay,Neutral()),"first genuine interpreted command follows raw cutover seeds");
    check(pump([&]{RoomFrame frame;return std::all_of(rooms.begin(),rooms.end(),[&](const auto& room){return room->PeekFrame(frame);});}),"first full interpreted frame delivered after bootstrap");
    for(auto& room:rooms){RoomFrame frame;check(room->TakeFrame(frame) && frame.tick==first_game+id.input_delay,"gameplay consumes only interpreted frame timeline after cutover");}
    check(complete_fixture_tick(first_game+id.input_delay),"first interpreted gameplay fixture tick completes after raw cutover");
    auto old_raw=Packet(Wire::BootstrapPad);U64(old_raw,first_game);WritePad(old_raw,{});rooms[1]->impl_->connections.front()->Queue(old_raw);
    check(pump([&]{return rooms[0]->state()==RoomState::Error;}),"queued old raw input cannot survive interpreted gameplay cutover");
    if(!check(raw_loading(2),"raw loaded-world mismatch fixture freezes roster"))return false;
    check(rooms[0]->BootstrapWorldReady(world) && rooms[1]->BootstrapWorldReady(different) &&
        pump([&]{return rooms[0]->state()==RoomState::Error;}),"raw bootstrap loaded-world assertions must match before any raw native input");
    for(unsigned malformed=0;malformed<8;++malformed){
        if(!check(raw_started(2),"malformed raw bootstrap fixture starts real two-peer room"))return false;
        auto packet=Packet(Wire::BootstrapPad);U64(packet,malformed==7?id.first_tick-1:id.first_tick+(malformed==0?1:0));WritePad(packet,{});
        if(malformed==1)U64(packet,1); // Spoofed sender field is not in the wire shape.
        if(malformed==2){packet=Packet(Wire::BootstrapComplete);U64(packet,id.first_tick);U8(packet,1);packet.resize(packet.size()+32,1);}
        if(malformed==3)packet=Packet(Wire::BootstrapBegin);
        if(malformed==4)packet=Packet(Wire::BootstrapActivate);
        if(malformed==5)packet=Packet(Wire::BootstrapContinue);
        rooms[1]->impl_->connections.front()->Queue(packet);
        if(malformed==6)rooms[1]->impl_->connections.front()->Queue(packet);
        check(pump([&]{return rooms[0]->state()==RoomState::Error;}),"future/spoofed/duplicate raw input, premature completion and guest authority forgery rejected");
    }
    if(!check(raw_started(2) && raw_take(id.first_tick),"raw observation mismatch fixture consumes same frame"))return false;
    Hash observation{};observation[0]=1;auto mismatch=observation;++mismatch[0];
    check(rooms[0]->CompleteBootstrap(id.first_tick,true,observation) && rooms[1]->CompleteBootstrap(id.first_tick,true,mismatch) &&
        pump([&]{return rooms[0]->state()==RoomState::Error;}),"unequal completed native observations stop bootstrap instead of cutting over");
    for(unsigned malformed=0;malformed<5;++malformed){
        if(!check(raw_started(2) && raw_take(id.first_tick),"invalid raw completion fixture consumes common native tick"))return false;
        auto packet=Packet(Wire::BootstrapComplete);U64(packet,malformed==4?id.first_tick-1:id.first_tick+(malformed==0?1:0));U8(packet,malformed==1?2:1);
        packet.resize(packet.size()+32,malformed==2?0:1);rooms[1]->impl_->connections.front()->Queue(packet);
        if(malformed==3)rooms[1]->impl_->connections.front()->Queue(packet);
        check(pump([&]{return rooms[0]->state()==RoomState::Error;}),"future tick, invalid ready flag, empty digest and duplicate completed proof rejected");
    }
    if(!check(raw_started(2),"raw own-input echo fixture starts"))return false;
    check(rooms[1]->SubmitBootstrapPad(id.first_tick,{}),"client retains immutable raw sample for own echo");
    auto fake_raw=Packet(Wire::BootstrapFrame);U64(fake_raw,id.first_tick);U64(fake_raw,3,2);WritePad(fake_raw,{});Pad altered;altered.buttons=1;WritePad(fake_raw,altered);
    rooms[0]->impl_->connections.front()->Queue(fake_raw);
    check(pump([&]{return rooms[1]->state()==RoomState::Error;}) && rooms[0]->state()==RoomState::Bootstrapping,"client rejects raw authoritative own-input tampering before native delivery");
    for(unsigned malformed=0;malformed<4;++malformed){
        if(!check(raw_started(2) && raw_take(id.first_tick),"raw client-control validation fixture consumes common native tick"))return false;
        if(malformed!=3)check(rooms[1]->CompleteBootstrap(id.first_tick,malformed!=0,observation),"client retains completed proof before validating server round control");
        auto packet=Packet(malformed==1 || malformed==3?Wire::BootstrapContinue:Wire::BootstrapActivate);
        auto token=world;if(malformed==2)++token[0];packet.insert(packet.end(),token.begin(),token.end());U64(packet,rooms[0]->lobby().revision);U64(packet,3,2);
        U64(packet,id.first_tick);U64(packet,id.first_tick+(malformed==1?2:1));packet.insert(packet.end(),observation.begin(),observation.end());
        rooms[0]->impl_->connections.front()->Queue(packet);
        check(pump([&]{return rooms[1]->state()==RoomState::Error;}) && rooms[0]->state()==RoomState::Bootstrapping,
            "client rejects activation against false own-ready vote, wrong Continue tick/world and control before own completion");
    }
    if(!check(raw_started(2),"raw absolute startup timeout fixture starts"))return false;
    const auto raw_clock=Clock::now();for(unsigned seconds=10;seconds<=170;seconds+=10)for(auto& room:rooms)room->Poll(raw_clock+std::chrono::seconds(seconds));
    check(rooms[0]->state()==RoomState::Bootstrapping,"heartbeats allow bounded slow native bootstrap rounds");rooms[0]->Poll(raw_clock+StartupTimeout+std::chrono::seconds(1));
    check(rooms[0]->state()==RoomState::Error,"raw bootstrap cannot extend absolute startup deadline with heartbeats or repeated rounds");
    if(!check(raw_started(2),"raw peer loss fixture starts"))return false;rooms[1]->Disconnect();
    check(pump([&]{return rooms[0]->state()==RoomState::Error;}),"peer loss during synchronized native bootstrap stops whole room");
    const auto raw_saved=id;id.first_tick=UINT64_MAX-Window;
    if(!check(raw_started(2),"raw tick overflow fixture begins within valid identity range"))return false;
    for(unsigned round=0;round<Window+1;++round){const auto tick=id.first_tick+round;
        if(!check(raw_take(tick),"raw overflow fixture never skips a transport round"))return false;
        const bool last=round==Window;check(rooms[0]->CompleteBootstrap(tick,last,observation) && rooms[1]->CompleteBootstrap(tick,last,observation),"overflow proof submitted after actual frame consumption");
        if(last)check(pump([&]{return rooms[0]->state()==RoomState::Error;}),"UINT64_MAX completed round cannot wrap into another raw/gameplay tick");
        else if(!check(pump([&]{return rooms[0]->bootstrap_tick()==tick+1 && rooms[1]->bootstrap_tick()==tick+1;}),"raw continuation increments exact uint64 tick"))return false;
    }
    id=raw_saved;
    // Real checkpoint format and worker installation, using an exclusively
    // created temporary fixture root. No production save/content paths enter.
    namespace fs=std::filesystem;
    const auto fixture=fs::temp_directory_path()/("beetleworx-room-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    if(!check(CreateDirectoryW(fixture.c_str(),nullptr)!=0,"exclusive private checkpoint fixture root"))return false;
    struct Cleanup {fs::path root;~Cleanup(){std::error_code error;fs::remove_all(root,error);}} cleanup{fixture};
    try{
        fs::create_directory(fixture/"source");
        const std::string payload(512*1024,'Q');
        {std::ofstream file(fixture/"source"/"save.dat",std::ios::binary);file.write(payload.data(),payload.size());}
        {std::ofstream file(fixture/"normal-save.dat",std::ios::binary);file<<"original normal save";}
        auto packed=std::async(std::launch::async,[&]{return PackCheckpoint(fixture/"source");}).get();
        if(!check(bool(packed.value),"worker packs real bounded checkpoint bundle"))return false;
        auto bundle=std::make_shared<const CheckpointBundle>(std::move(*packed.value));auto transfer_id=id;transfer_id.checkpoint=bundle->identity;
        auto bootstrap=[&](unsigned count,std::shared_ptr<const CheckpointBundle> source){
            rooms.clear();rooms.push_back(std::make_unique<Room>());
            if(!rooms[0]->HostWithCheckpoint(transfer_id,"127.0.0.1",0,uint8_t(count),"Host",std::move(source)))return false;
            for(unsigned i=1;i<count;++i){
                rooms.push_back(std::make_unique<Room>());auto local=transfer_id;if(i&1)local.checkpoint={};else ++local.checkpoint[0];
                if(!rooms.back()->JoinWithCheckpoint(local,"127.0.0.1",rooms[0]->port(),"Guest "+std::to_string(i)))return false;
            }
            return pump([&]{return rooms[0]->lobby().players.size()==count && std::all_of(rooms.begin()+1,rooms.end(),[](const auto& r){return r->impl_->checkpoint_size>0;});});
        };
        auto install=[&](unsigned i,unsigned path){
            auto bytes=rooms[i]->DownloadedCheckpoint();if(!bytes)return false;
            const auto expected=rooms[i]->identity().checkpoint;const auto destination=fixture/("installed-"+std::to_string(path));
            auto installed=std::async(std::launch::async,[=]{return InstallCheckpoint(*bytes,expected,destination);}).get();
            return installed.installed && rooms[i]->ConfirmCheckpointInstalled(true);
        };
        if(!check(bootstrap(4,bundle),"four real peers negotiate host checkpoint with missing/different local saves"))return false;
        const auto fast_deadline=Clock::now()+std::chrono::seconds(3);
        while((!rooms[1]->DownloadedCheckpoint() || !rooms[2]->DownloadedCheckpoint()) && Clock::now()<fast_deadline){
            for(unsigned i=0;i<3;++i)rooms[i]->Poll();std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(rooms[1]->DownloadedCheckpoint() && rooms[2]->DownloadedCheckpoint() && !rooms[3]->DownloadedCheckpoint(),"one unpolled slow guest cannot starve two other checkpoint downloads");
        check(rooms[0]->impl_->offered.get()==bundle.get() && !rooms[0]->impl_->downloaded &&
            std::all_of(rooms[0]->impl_->connections.begin(),rooms[0]->impl_->connections.end(),[](const auto& c){return c->outgoing.size()-c->sent<=MaxQueued;}),"host shares one bundle, no per-guest bundle copies, all send queues bounded");
        check(!rooms[1]->SetReady(true) && !rooms[3]->ConfirmCheckpointInstalled(true),"download does not enable lobby Ready; incomplete download cannot report installation");
        if(!check(pump([&]{return bool(rooms[3]->DownloadedCheckpoint());}),"slow guest completes its exact ordered download when polled"))return false;
        check(install(1,1) && install(2,2) && pump([&]{return rooms[1]->checkpoint_ready() && rooms[2]->checkpoint_ready();}),"two independent workers install and mutually acknowledge host checkpoint");
        for(const auto& player:rooms[0]->lobby().players)rooms[0]->impl_->roster.SetReady(player.peer,true);
        check(!rooms[0]->Start() && rooms[0]->state()==RoomState::Lobby,"even a forged ready roster cannot bypass a third peer's installation barrier");
        for(const auto& player:rooms[0]->lobby().players)rooms[0]->impl_->roster.SetReady(player.peer,false);rooms[0]->impl_->Publish();
        check(install(3,3) && pump([&]{const auto peers=rooms[0]->checkpoint_peers();return rooms[3]->checkpoint_ready() && std::all_of(peers.begin(),peers.end(),[](const auto& p){return p.installed;});}),"every admitted peer must complete install acknowledgement");
        // Depart/rejoin before Start gets a new private installation and reuses
        // its logical slot, never an old participant's transfer state.
        rooms[3]->Disconnect();check(pump([&]{return rooms[0]->lobby().players.size()==3;}),"checkpoint-ready lobby guest may leave without closing host");rooms.pop_back();
        rooms.push_back(std::make_unique<Room>());auto missing=transfer_id;missing.checkpoint={};
        check(rooms[3]->JoinWithCheckpoint(missing,"127.0.0.1",rooms[0]->port(),"Rejoined") && pump([&]{return bool(rooms[3]->DownloadedCheckpoint());}) &&
            rooms[3]->local_peer()==4 && install(3,4) && pump([&]{return rooms[3]->checkpoint_ready();}),"rejoin slot performs its own complete transfer/install, no stale readiness inheritance");
        for(auto& r:rooms)check(r->SetReady(true),"installed bootstrap peers explicitly ready");
        check(pump([&]{return std::all_of(rooms[0]->lobby().players.begin(),rooms[0]->lobby().players.end(),[](const auto& p){return p.ready;}) &&
            std::none_of(rooms.begin(),rooms.end(),[](const auto& r){return r->profile_pending();});}) && rooms[0]->Start() &&
            pump([&]{return std::all_of(rooms.begin(),rooms.end(),[](const auto& r){return r->state()==RoomState::Loading;});}),"all installed and ready peers enter native Loading, not automatic gameplay");
        for(auto& r:rooms)check(r->WorldReady(world),"installed peers separately assert real loaded-world readiness");
        check(pump([&]{return std::all_of(rooms.begin(),rooms.end(),[](const auto& r){return r->state()==RoomState::Running;});}),"checkpoint barrier composes with explicit all-world activation");
        std::ifstream normal(fixture/"normal-save.dat",std::ios::binary);const std::string untouched((std::istreambuf_iterator<char>(normal)),std::istreambuf_iterator<char>());
        check(untouched=="original normal save","worker bootstrap never changes original normal save fixture");
        auto oversized=std::make_shared<CheckpointBundle>(*bundle);oversized->bytes.resize(kMaxCheckpointBundle+1);Room rejected;
        check(!rejected.HostWithCheckpoint(transfer_id,"127.0.0.1",0,2,"Host",oversized),"oversized host bundle rejected before socket creation");
        auto wrong=std::make_shared<CheckpointBundle>(*bundle);++wrong->identity[0];
        check(!rejected.HostWithCheckpoint(transfer_id,"127.0.0.1",0,2,"Host",wrong),"host bundle digest must match authoritative checkpoint identity");
        for(unsigned malformed=0;malformed<5;++malformed){
            if(!check(bootstrap(2,bundle),"malformed checkpoint stream fixture establishes explicit bootstrap"))return false;
            auto& sender=*rooms[0]->impl_->connections.front();sender.transfer=Connection::Transfer::AwaitInstalled;
            Bytes packet;
            if(malformed==0){packet=Packet(Wire::CheckpointChunk);U64(packet,UINT32_MAX,4);U64(packet,1,2);U8(packet,0);}
            if(malformed==1){packet=Packet(Wire::CheckpointChunk);U64(packet,0,4);U64(packet,1025,2);packet.resize(packet.size()+1025);}
            if(malformed==2){packet=Packet(Wire::CheckpointEnd);U64(packet,bundle->bytes.size(),4);}
            if(malformed==3){packet=Packet(Wire::CheckpointBegin);U64(packet,kMaxCheckpointBundle+1,4);packet.insert(packet.end(),transfer_id.checkpoint.begin(),transfer_id.checkpoint.end());}
            if(malformed==4){packet=Packet(Wire::CheckpointReady);packet.insert(packet.end(),transfer_id.checkpoint.begin(),transfer_id.checkpoint.end());}
            sender.Queue(packet);check(pump([&]{return rooms[1]->state()==RoomState::Error;}),"bad offset/oversized chunk/early end/repeated oversized announcement/unsolicited install-ready rejected over TCP");
        }
        if(!check(bootstrap(2,bundle),"wrong-direction checkpoint fixture connects"))return false;
        auto unexpected=Packet(Wire::CheckpointEnd);U64(unexpected,bundle->bytes.size(),4);rooms[1]->impl_->connections.front()->Queue(unexpected);
        check(pump([&]{return rooms[0]->state()==RoomState::Listening && rooms[1]->state()==RoomState::Error;}),"guest cannot send host checkpoint messages; malformed lobby peer evicted without closing listener");
        auto corrupt=std::make_shared<CheckpointBundle>(*bundle);corrupt->bytes.back()^=1;
        if(!check(bootstrap(2,corrupt) && pump([&]{return bool(rooms[1]->DownloadedCheckpoint());}),"corrupt payload arrives only as untrusted complete download"))return false;
        auto bytes=rooms[1]->DownloadedCheckpoint();auto failed=std::async(std::launch::async,[&]{return InstallCheckpoint(*bytes,transfer_id.checkpoint,fixture/"corrupt-install");}).get();
        check(!failed.installed && rooms[1]->ConfirmCheckpointInstalled(false) && pump([&]{return rooms[0]->state()==RoomState::Listening;}) &&
            !fs::exists(fixture/"corrupt-install"),"worker cryptographic validation failure closes guest, preserves host and publishes no installed save");
        if(!check(bootstrap(2,bundle) && pump([&]{return bool(rooms[1]->DownloadedCheckpoint());}),"stalled installation fixture downloads complete checkpoint"))return false;
        // Drain final real receipt acknowledgements before injecting a duplicate
        // alongside a heartbeat at the progress deadline.
        check(pump([&]{return rooms[0]->impl_->connections.front()->transfer_acked==bundle->bytes.size();}),"host observes actual received byte progress, not OS send buffering");
        auto duplicate=Packet(Wire::CheckpointProgress);U64(duplicate,bundle->bytes.size(),4);rooms[1]->impl_->connections.front()->Queue(duplicate);rooms[1]->impl_->connections.front()->Queue(Packet(Wire::Ping));rooms[1]->impl_->connections.front()->Flush();
        const auto stalled=rooms[0]->impl_->connections.front()->transfer_progress+TransferTimeout+std::chrono::seconds(1);rooms[0]->Poll(stalled);
        check(rooms[0]->state()==RoomState::Listening && rooms[0]->lobby().players.size()==1,"duplicate receipt progress and live heartbeat cannot keep uninstalled checkpoint alive beyond120seconds");
    }catch(const std::exception& exception){check(false,exception.what());}
    if(ok)REXLOG_INFO("Online room passed {} checks: real16-peer TCP, authoritative roster/ready, native movement basis, contiguous all-peer input, own echo, all-peer completed-world proof/ack barriers, malformed/spoof/stale/window rejection and nonextending native-progress deadlines",checks);
    return ok;
}
}
