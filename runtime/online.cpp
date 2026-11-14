#include <winsock2.h>
#include <ws2tcpip.h>
#include "online_address.h"
#include "online.h"
#include "online_checkpoint.h"
#include "online_router.h"
#include <rex/logging.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <deque>
#include <limits>
#include <optional>
#include <span>
#include <thread>
#include <type_traits>
#include <variant>
#include <vector>
#include <fstream>

namespace em2::net {
namespace {
using Clock=std::chrono::steady_clock;
using Bytes=std::vector<uint8_t>;
constexpr size_t kWindow=256,kQueueLimit=256,kStreamLimit=4096;
constexpr uint16_t kVersion=9;
enum class Kind:uint8_t {Hello=1,Input=2,Frame=3,Ping=4,Pong=5,Digest=6,CheckpointBegin=7,CheckpointChunk=8,CheckpointEnd=9,CheckpointInstalled=10,CheckpointReady=11,CheckpointProgress=12,Load=13,LobbyStart=14,LobbyProfile=15,LobbySnapshot=16,LobbyReply=17};
enum class LobbyStart:uint8_t {ClientReady=1,Start=2,Acknowledge=3};
enum class LobbyStartPhase {Waiting,ClientReady,Requested,Confirmed};
struct Hello {Identity identity;uint8_t role;bool checkpoint=false;bool operator==(const Hello&) const=default;};
struct Input {uint64_t tick;uint8_t player;Pad pad;bool operator==(const Input&) const=default;};
struct Ping {uint32_t sequence;bool operator==(const Ping&) const=default;};
struct Pong {uint32_t sequence;bool operator==(const Pong&) const=default;};
struct Digest {uint64_t tick;std::array<uint8_t,32> hash;bool operator==(const Digest&) const=default;};
struct CheckpointBegin {uint32_t size;std::array<uint8_t,32> identity;bool operator==(const CheckpointBegin&)const=default;};
struct CheckpointChunk {uint32_t offset;uint16_t size;std::array<uint8_t,1024> data{};bool operator==(const CheckpointChunk&)const=default;};
struct CheckpointProgress {uint32_t received;bool operator==(const CheckpointProgress&)const=default;};
struct CheckpointEnd {uint32_t size;bool operator==(const CheckpointEnd&)const=default;};
struct CheckpointInstalled {std::array<uint8_t,32> identity;bool operator==(const CheckpointInstalled&)const=default;};
struct CheckpointReady {std::array<uint8_t,32> identity;bool operator==(const CheckpointReady&)const=default;};
enum class ProfileAction:uint8_t {Rename=1,Appearance=2,Ready=3};
struct LobbyProfile {
    uint32_t sequence=0;ProfileAction action=ProfileAction::Rename;
    uint8_t character=0,color=0;bool ready=false;std::array<char,96> name{};
    bool operator==(const LobbyProfile&)const=default;
};
struct LobbyReply {
    uint32_t sequence=0;LobbyResult result=LobbyResult::Ok;uint64_t revision=0;
    bool operator==(const LobbyReply&)const=default;
};
LobbySnapshot Snapshot(const LobbyRoster& roster) {
    return {roster.revision(),roster.capacity(),roster.locked(),roster.players()};
}
using Message=std::variant<Hello,Input,Frame,Ping,Pong,Digest,CheckpointBegin,CheckpointChunk,CheckpointEnd,CheckpointInstalled,CheckpointReady,CheckpointProgress,LoadPoint,LobbyStart,LobbyProfile,LobbySnapshot,LobbyReply>;
bool ValidLoadPoint(const LoadPoint& p) {
    if(p.phase!=LoadPhase::Begin && p.phase!=LoadPhase::End)return false;
    if(p.phase==LoadPhase::Begin && p.success)return false;
    bool end=false;size_t length=0;
    for(auto c:p.destination){if(!c){end=true;continue;}if(end || c<32 || static_cast<unsigned char>(c)>=127)return false;++length;}
    return end && length;
}
class LoadExchange {
public:
    bool Add(unsigned side,const LoadPoint& p,Clock::time_point now) {
        if(side>1 || !ValidLoadPoint(p))return false;
        if(last_ && *last_==p)return true;
        if(exhausted_ || p.sequence!=next_ || (last_ && p.tick<last_->tick))return false;
        if(p.phase==LoadPhase::Begin){if(depth_==stack_.size() || (depth_ && p.tick!=stack_[depth_-1].tick))return false;}
        else if(!depth_ || p.tick!=stack_[depth_-1].tick || p.destination!=stack_[depth_-1].destination)return false;
        if((points_[side] && *points_[side]!=p) || (points_[1-side] && *points_[1-side]!=p))return false;
        if(!deadline_)deadline_=now;
        points_[side]=p;
        if(points_[0] && points_[1]) {
            if(p.phase==LoadPhase::Begin)stack_[depth_++]=p;else --depth_;
            last_=p;points_={};deadline_=depth_?std::optional(now):std::nullopt;
            if(next_==UINT64_MAX)exhausted_=true;else ++next_;
        }
        return true;
    }
    bool Agreed(const LoadPoint& p) const{return last_ && *last_==p;}
    bool Blocks(uint64_t tick) const{return (depth_ && stack_[depth_-1].tick<=tick) ||
        (points_[0] && points_[0]->tick<=tick) || (points_[1] && points_[1]->tick<=tick);}
    bool Expired(Clock::time_point now) const{return deadline_ && now-*deadline_>std::chrono::seconds(120);}
private:
    uint64_t next_=0;bool exhausted_=false;size_t depth_=0;
    std::array<LoadPoint,8> stack_{};
    std::array<std::optional<LoadPoint>,2> points_{};
    std::optional<LoadPoint> last_;
    std::optional<Clock::time_point> deadline_;
};
bool ValidIdentity(const Identity& id,bool allow_missing_checkpoint=false) {
    auto nonzero=[](const auto& bytes){return std::any_of(bytes.begin(),bytes.end(),[](auto b){return b!=0;});};
    return id.input_delay<=12 && uint8_t(id.startup_policy)<=uint8_t(StartupPolicy::NativeMenu) &&
        uint8_t(id.initialization_policy)<=uint8_t(InitializationPolicy::SharedNativeV1) &&
        nonzero(id.build) && nonzero(id.content) && (allow_missing_checkpoint || nonzero(id.checkpoint)) && nonzero(id.session);
}
void Put(Bytes& out,uint64_t value,unsigned count) {
    for(unsigned i=count;i;--i) out.push_back(uint8_t(value>>((i-1)*8)));
}
uint64_t Get(std::span<const uint8_t> in,size_t& offset,unsigned count) {
    uint64_t value=0;for(unsigned i=0;i<count;++i) value=(value<<8)|in[offset++];return value;
}
void PutPad(Bytes& out,const Pad& pad) {
    Put(out,pad.buttons,2);Put(out,pad.left_trigger,1);Put(out,pad.right_trigger,1);
    for(auto value:{pad.left_x,pad.left_y,pad.right_x,pad.right_y}) Put(out,std::bit_cast<uint16_t>(value),2);
    Put(out,pad.menu_pause?1:0,1);
}
Pad GetPad(std::span<const uint8_t> in,size_t& at) {
    Pad pad;pad.buttons=uint16_t(Get(in,at,2));pad.left_trigger=uint8_t(Get(in,at,1));pad.right_trigger=uint8_t(Get(in,at,1));
    pad.left_x=std::bit_cast<int16_t>(uint16_t(Get(in,at,2)));pad.left_y=std::bit_cast<int16_t>(uint16_t(Get(in,at,2)));
    pad.right_x=std::bit_cast<int16_t>(uint16_t(Get(in,at,2)));pad.right_y=std::bit_cast<int16_t>(uint16_t(Get(in,at,2)));
    pad.menu_pause=Get(in,at,1)!=0;return pad;
}
size_t BodySize(Kind kind) {
    switch(kind) {
        case Kind::Hello:return 128;
        case Kind::CheckpointBegin:return 36;
        case Kind::CheckpointChunk:return 1032;
        case Kind::CheckpointEnd:case Kind::CheckpointProgress:return 4;
        case Kind::CheckpointInstalled:case Kind::CheckpointReady:return 32;
        case Kind::Input:return 25;
        case Kind::Frame:return 34;
        case Kind::Digest:return 40;
        case Kind::Load:return 116;
        case Kind::LobbyStart:return 1;
        case Kind::LobbyProfile:return 104;
        case Kind::LobbySnapshot:return 1740;
        case Kind::LobbyReply:return 16;
        case Kind::Ping:case Kind::Pong:return 4;
        default:return 0;
    }
}
Bytes Encode(const Message& message) {
    Bytes body;Kind kind{};
    std::visit([&](const auto& value) {
        using T=std::decay_t<decltype(value)>;
        if constexpr(std::is_same_v<T,Hello>) {
            kind=Kind::Hello;Put(body,value.role,1);Put(body,value.identity.input_delay,1);Put(body,30,2);
            Put(body,value.identity.first_tick,8);
            Put(body,value.checkpoint?1:0,1);
            Put(body,uint8_t(value.identity.startup_policy),1);Put(body,uint8_t(value.identity.initialization_policy),1);Put(body,0,1);
            for(const auto* hash:{&value.identity.build,&value.identity.content,&value.identity.checkpoint})
                body.insert(body.end(),hash->begin(),hash->end());
            body.insert(body.end(),value.identity.session.begin(),value.identity.session.end());
        } else if constexpr(std::is_same_v<T,Input>) {
            kind=Kind::Input;Put(body,value.tick,8);Put(body,value.player,1);Put(body,0,3);PutPad(body,value.pad);
        } else if constexpr(std::is_same_v<T,Frame>) {
            kind=Kind::Frame;Put(body,value.tick,8);for(const auto& pad:value.players) PutPad(body,pad);
        } else if constexpr(std::is_same_v<T,Digest>) {
            kind=Kind::Digest;Put(body,value.tick,8);body.insert(body.end(),value.hash.begin(),value.hash.end());
        } else if constexpr(std::is_same_v<T,LoadPoint>) {
            kind=Kind::Load;Put(body,value.sequence,8);Put(body,value.tick,8);Put(body,uint8_t(value.phase),1);Put(body,value.success?1:0,1);Put(body,0,2);
            body.insert(body.end(),value.destination.begin(),value.destination.end());
        } else if constexpr(std::is_same_v<T,CheckpointBegin>) {
            kind=Kind::CheckpointBegin;Put(body,value.size,4);body.insert(body.end(),value.identity.begin(),value.identity.end());
        } else if constexpr(std::is_same_v<T,CheckpointChunk>) {
            kind=Kind::CheckpointChunk;Put(body,value.offset,4);Put(body,value.size,2);Put(body,0,2);body.insert(body.end(),value.data.begin(),value.data.end());
        } else if constexpr(std::is_same_v<T,CheckpointProgress>) {
            kind=Kind::CheckpointProgress;Put(body,value.received,4);
        } else if constexpr(std::is_same_v<T,CheckpointEnd>) {
            kind=Kind::CheckpointEnd;Put(body,value.size,4);
        } else if constexpr(std::is_same_v<T,CheckpointInstalled> || std::is_same_v<T,CheckpointReady>) {
            kind=std::is_same_v<T,CheckpointInstalled>?Kind::CheckpointInstalled:Kind::CheckpointReady;
            body.insert(body.end(),value.identity.begin(),value.identity.end());
        } else if constexpr(std::is_same_v<T,LobbyProfile>) {
            kind=Kind::LobbyProfile;Put(body,value.sequence,4);Put(body,uint8_t(value.action),1);
            Put(body,value.character,1);Put(body,value.color,1);Put(body,value.ready,1);
            body.insert(body.end(),value.name.begin(),value.name.end());
        } else if constexpr(std::is_same_v<T,LobbyReply>) {
            kind=Kind::LobbyReply;Put(body,value.sequence,4);Put(body,uint8_t(value.result),1);Put(body,0,3);Put(body,value.revision,8);
        } else if constexpr(std::is_same_v<T,LobbySnapshot>) {
            kind=Kind::LobbySnapshot;Put(body,value.revision,8);Put(body,value.capacity,1);
            Put(body,value.players.size(),1);Put(body,value.locked,1);Put(body,0,1);
            for(size_t i=0;i<16;++i) {
                if(i>=value.players.size()){body.resize(body.size()+108,0);continue;}
                const auto& p=value.players[i];Put(body,p.peer,8);Put(body,uint8_t(p.character),1);
                Put(body,p.color,1);Put(body,p.ready,1);Put(body,p.name.size(),1);
                body.insert(body.end(),p.name.begin(),p.name.end());body.resize(body.size()+96-p.name.size(),0);
            }
        } else if constexpr(std::is_same_v<T,LobbyStart>) {
            kind=Kind::LobbyStart;Put(body,uint8_t(value),1);
        } else {
            kind=std::is_same_v<T,Ping>?Kind::Ping:Kind::Pong;Put(body,value.sequence,4);
        }
    },message);
    Bytes wire={'E','M','2','N'};Put(wire,kVersion,2);Put(wire,uint8_t(kind),1);Put(wire,0,1);Put(wire,body.size(),4);
    wire.insert(wire.end(),body.begin(),body.end());return wire;
}
std::optional<Message> Decode(Kind kind,std::span<const uint8_t> body) {
    size_t at=0;
    if(kind==Kind::Hello) {
        Hello hello{};hello.role=uint8_t(Get(body,at,1));hello.identity.input_delay=uint8_t(Get(body,at,1));
        if(hello.role>1 || Get(body,at,2)!=30) return {};
        hello.identity.first_tick=Get(body,at,8);
        const auto flags=Get(body,at,1);if(flags>1)return {};hello.checkpoint=flags!=0;
        hello.identity.startup_policy=StartupPolicy(Get(body,at,1));
        hello.identity.initialization_policy=InitializationPolicy(Get(body,at,1));
        if(Get(body,at,1))return {};
        for(auto* hash:{&hello.identity.build,&hello.identity.content,&hello.identity.checkpoint}) {
            std::copy_n(body.begin()+at,hash->size(),hash->begin());at+=hash->size();
        }
        std::copy_n(body.begin()+at,hello.identity.session.size(),hello.identity.session.begin());
        if(!ValidIdentity(hello.identity,hello.checkpoint && hello.role==1)) return {};
        return hello;
    }
    if(kind==Kind::LobbyProfile) {
        LobbyProfile p;p.sequence=uint32_t(Get(body,at,4));p.action=ProfileAction(Get(body,at,1));
        p.character=uint8_t(Get(body,at,1));p.color=uint8_t(Get(body,at,1));auto ready=Get(body,at,1);
        if(!p.sequence || uint8_t(p.action)<1 || uint8_t(p.action)>3 || ready>1)return {};
        p.ready=ready!=0;std::copy_n(body.begin()+at,96,p.name.begin());
        const bool empty=std::all_of(p.name.begin(),p.name.end(),[](char c){return c==0;});
        if(p.action!=ProfileAction::Rename && !empty)return {};
        if(p.action!=ProfileAction::Appearance && (p.character || p.color))return {};
        if(p.action!=ProfileAction::Ready && p.ready)return {};
        return p;
    }
    if(kind==Kind::LobbyReply) {
        LobbyReply p;p.sequence=uint32_t(Get(body,at,4));p.result=LobbyResult(Get(body,at,1));
        if(!p.sequence || uint8_t(p.result)>uint8_t(LobbyResult::NotReady) || Get(body,at,3))return {};
        p.revision=Get(body,at,8);return p;
    }
    if(kind==Kind::LobbySnapshot) {
        LobbySnapshot snap;snap.revision=Get(body,at,8);snap.capacity=uint8_t(Get(body,at,1));
        auto count=Get(body,at,1),locked=Get(body,at,1);
        if(!snap.capacity || snap.capacity>16 || count>snap.capacity || locked>1 || Get(body,at,1))return {};
        snap.locked=locked!=0;LobbyRoster validation(snap.capacity);
        for(size_t i=0;i<16;++i) {
            if(i>=count){for(size_t j=0;j<108;++j)if(Get(body,at,1))return {};continue;}
            LobbyPlayer p;p.peer=Get(body,at,8);p.character=LobbyCharacter(Get(body,at,1));p.color=uint8_t(Get(body,at,1));
            auto ready=Get(body,at,1),length=Get(body,at,1);if(ready>1 || !length || length>96)return {};
            p.ready=ready!=0;p.name.assign(reinterpret_cast<const char*>(body.data()+at),size_t(length));at+=size_t(length);
            for(size_t j=size_t(length);j<96;++j)if(Get(body,at,1))return {};
            if(validation.Join(p.peer,p.name)!=LobbyResult::Ok || validation.Find(p.peer)->name!=p.name)return {};
            snap.players.push_back(std::move(p));
        }
        // Validate final choices after all joins, so default-color allocation
        // cannot spuriously conflict with a valid swapped palette.
        std::array<bool,16> colors{};
        for(const auto& p:snap.players) {
            if(uint8_t(p.character)>1 || p.color>=16 || colors[p.color])return {};
            colors[p.color]=true;if(snap.locked && !p.ready)return {};
        }
        if(snap.locked && count<2)return {};
        return snap;
    }
    if(kind==Kind::Input) {
        if(body[24]>1)return {};
        Input input{};input.tick=Get(body,at,8);input.player=uint8_t(Get(body,at,1));
        if(input.player>1 || Get(body,at,3)!=0) return {};
        input.pad=GetPad(body,at);return input;
    }
    if(kind==Kind::Frame) {
        if(body[20]>1 || body[33]>1)return {};
        Frame frame{};frame.tick=Get(body,at,8);for(auto& pad:frame.players) pad=GetPad(body,at);return frame;
    }
    if(kind==Kind::CheckpointBegin){CheckpointBegin value{};value.size=uint32_t(Get(body,at,4));std::copy_n(body.begin()+at,32,value.identity.begin());return value;}
    if(kind==Kind::CheckpointChunk){
        CheckpointChunk value{};value.offset=uint32_t(Get(body,at,4));value.size=uint16_t(Get(body,at,2));
        if(!value.size || value.size>value.data.size() || Get(body,at,2))return {};
        std::copy_n(body.begin()+at,value.data.size(),value.data.begin());
        if(std::any_of(value.data.begin()+value.size,value.data.end(),[](auto b){return b!=0;}))return {};
        return value;
    }
    if(kind==Kind::Load){
        LoadPoint p;p.sequence=Get(body,at,8);p.tick=Get(body,at,8);p.phase=LoadPhase(Get(body,at,1));
        const auto success=Get(body,at,1);if(success>1 || Get(body,at,2))return {};p.success=success!=0;
        std::copy_n(body.begin()+at,p.destination.size(),p.destination.begin());if(!ValidLoadPoint(p))return {};return p;
    }
    if(kind==Kind::CheckpointProgress)return CheckpointProgress{uint32_t(Get(body,at,4))};
    if(kind==Kind::CheckpointEnd)return CheckpointEnd{uint32_t(Get(body,at,4))};
    if(kind==Kind::CheckpointInstalled){CheckpointInstalled value{};std::copy_n(body.begin(),32,value.identity.begin());return value;}
    if(kind==Kind::CheckpointReady){CheckpointReady value{};std::copy_n(body.begin(),32,value.identity.begin());return value;}
    if(kind==Kind::Ping) return Ping{uint32_t(Get(body,at,4))};
    if(kind==Kind::LobbyStart){
        const auto action=Get(body,at,1);
        if(action<1 || action>3)return {};
        return LobbyStart(action);
    }
    if(kind==Kind::Pong) return Pong{uint32_t(Get(body,at,4))};
    if(kind==Kind::Digest) {
        Digest digest{};digest.tick=Get(body,at,8);std::copy_n(body.begin()+at,digest.hash.size(),digest.hash.begin());return digest;
    }
    return {};
}
class Decoder {
public:
    size_t available() const {return bytes_.size()-size_;}
    bool failed() const {return failed_;}
    bool Append(std::span<const uint8_t> bytes) {
        if(failed_ || bytes.size()>available()) {failed_=true;return false;}
        std::copy(bytes.begin(),bytes.end(),bytes_.begin()+size_);size_+=bytes.size();return true;
    }
    std::optional<Message> Next() {
        if(failed_ || size_<12) return {};
        const auto bytes=std::span<const uint8_t>(bytes_.data(),size_);size_t at=4;
        const auto version=Get(bytes,at,2);const auto kind=Kind(Get(bytes,at,1));const auto reserved=Get(bytes,at,1);
        const auto length=Get(bytes,at,4);
        if(std::memcmp(bytes.data(),"EM2N",4) || version!=kVersion || reserved || !BodySize(kind) || length!=BodySize(kind)) {
            failed_=true;return {};
        }
        if(size_<12+length) return {};
        auto result=Decode(kind,bytes.subspan(12,size_t(length)));
        if(!result) {failed_=true;return {};}
        size_-=12+size_t(length);std::memmove(bytes_.data(),bytes_.data()+12+length,size_);return result;
    }
private:
    std::array<uint8_t,kStreamLimit> bytes_{};size_t size_=0;bool failed_=false;
};
enum class Added {New,Duplicate,Invalid};
class TickBuffer {
public:
    void Reset(uint64_t first) {first_=next_=first;exhausted_=false;for(auto& cell:cells_) cell.mask=0;}
    uint64_t next() const {return next_;}
    bool Consumed(uint64_t tick) const {return tick>=first_ && (exhausted_?tick<=next_:tick<next_);}
    Added Check(uint8_t player,uint64_t tick,const Pad& pad) const {
        if(player>1 || exhausted_ || tick<next_ || tick-next_>=kWindow) return Added::Invalid;
        const auto& cell=cells_[tick%kWindow];
        if(!cell.mask) return Added::New;
        if(cell.tick!=tick) return Added::Invalid;
        if(!(cell.mask&(1u<<player))) return Added::New;
        return cell.pads[player]==pad?Added::Duplicate:Added::Invalid;
    }
    Added Add(uint8_t player,uint64_t tick,const Pad& pad) {
        const auto result=Check(player,tick,pad);if(result!=Added::New) return result;
        auto& cell=cells_[tick%kWindow];cell.tick=tick;cell.pads[player]=pad;cell.mask|=uint8_t(1u<<player);return result;
    }
    bool Peek(Frame& frame) const {
        if(exhausted_) return false;
        const auto& cell=cells_[next_%kWindow];if(cell.mask!=3 || cell.tick!=next_) return false;
        frame={next_,cell.pads};return true;
    }
    bool OwnMatches(uint8_t player,uint64_t tick,const Pad& pad) const {return Check(player,tick,pad)==Added::Duplicate;}
    bool Take(Frame& frame) {
        if(!Peek(frame)) return false;
        cells_[next_%kWindow].mask=0;
        if(next_==std::numeric_limits<uint64_t>::max()) exhausted_=true;else ++next_;
        return true;
    }
private:
    struct Cell {uint64_t tick=0;std::array<Pad,2> pads{};uint8_t mask=0;};
    std::array<Cell,kWindow> cells_{};uint64_t first_=0,next_=0;bool exhausted_=false;
};

class DigestBuffer {
public:
    void Reset(uint64_t first) {next_=first;verified_.reset();exhausted_=false;for(auto& c:cells_) c.mask=0;}
    std::optional<uint64_t> verified() const {return verified_;}
    bool Add(unsigned side,const Digest& digest) {
        if(side>1 || exhausted_ || digest.tick<next_ || digest.tick-next_>=kWindow) return false;
        auto& cell=cells_[digest.tick%kWindow];
        if(cell.mask && (cell.digest!=digest)) return false;
        cell.digest=digest;cell.mask|=uint8_t(1u<<side);
        while(cells_[next_%kWindow].mask==3 && cells_[next_%kWindow].digest.tick==next_) {
            cells_[next_%kWindow].mask=0;verified_=next_;
            if(next_==UINT64_MAX) {exhausted_=true;break;}
            ++next_;
        }
        return true;
    }
private:
    struct Cell {Digest digest{};uint8_t mask=0;};
    std::array<Cell,kWindow> cells_{};uint64_t next_=0;bool exhausted_=false;
    std::optional<uint64_t> verified_;
};

std::string SocketFailureText(const char* operation,int code) {
    switch(code) {
    case WSAECONNREFUSED:return "Connection refused. Ask the host to host again and send a fresh invitation.";
    case WSAETIMEDOUT:return "The host did not respond. Check the invitation address and firewall/router setup, then retry.";
    case WSAENETUNREACH:case WSAEHOSTUNREACH:return "The host cannot be reached. Check the network connection and invitation address.";
    case WSAEADDRINUSE:return "This port is already in use. Choose another port or close the previous host session.";
    case WSAEADDRNOTAVAIL:return "That address is not assigned to this PC. Choose this PC's local address for hosting.";
    case WSAEACCES:return "Windows denied this address or port. It may already be in use; check firewall permissions or choose another host port.";
    case WSAECONNRESET:case WSAECONNABORTED:return "The other player disconnected or the connection was interrupted. Start a new session together.";
    case WSAENOBUFS:return "Windows has insufficient network resources. Close unused connections and retry.";
    default:return code?"The network operation failed. Retry the connection; details are in the game log.":std::string(operation);
    }
}
class TcpChannel {
public:
    enum class Status {Idle,Listening,Connecting,Connected,Closed,Failed};
    TcpChannel() {WSADATA data{};startup_error_=WSAStartup(MAKEWORD(2,2),&data);}
    ~TcpChannel() {Close();if(!startup_error_) WSACleanup();}
    TcpChannel(const TcpChannel&)=delete;TcpChannel& operator=(const TcpChannel&)=delete;
    Status status=Status::Idle;std::string error;uint16_t port=0;
    bool Open(bool host,std::string_view address,uint16_t requested_port) {
        Close();error.clear();decoder_=Decoder{};
        if(startup_error_) return Fail("WSAStartup",startup_error_);
        auto parsed=ParseNumericAddress(address,host);
        if(!parsed || (!host && !requested_port))return Fail("Invalid numeric IPv4/IPv6 endpoint (use unscoped unicast)",0);
        auto& endpoint=*parsed;endpoint.Port(requested_port);
        socket_=socket(endpoint.storage.ss_family,SOCK_STREAM,IPPROTO_TCP);
        if(socket_==INVALID_SOCKET) return Fail("socket",WSAGetLastError());
        if(endpoint.ipv6()) {
            // Explicit family policy: mapped IPv4 endpoints use the IPv4 path.
            const DWORD only=1;
            if(setsockopt(socket_,IPPROTO_IPV6,IPV6_V6ONLY,reinterpret_cast<const char*>(&only),sizeof(only)))
                return Fail("IPV6_V6ONLY",WSAGetLastError());
        }
        if(!Configure(socket_)) return false;
        if(host) {
            int exclusive=1;
            if(setsockopt(socket_,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive)))
                return Fail("SO_EXCLUSIVEADDRUSE",WSAGetLastError());
            if(bind(socket_,reinterpret_cast<sockaddr*>(&endpoint.storage),endpoint.size) || listen(socket_,1)) return Fail("bind/listen",WSAGetLastError());
            int size=endpoint.size;if(getsockname(socket_,reinterpret_cast<sockaddr*>(&endpoint.storage),&size)) return Fail("getsockname",WSAGetLastError());
            port=endpoint.Port();status=Status::Listening;
        } else {
            const auto result=connect(socket_,reinterpret_cast<sockaddr*>(&endpoint.storage),endpoint.size);
            if(result && WSAGetLastError()!=WSAEWOULDBLOCK) return Fail("connect",WSAGetLastError());
            port=requested_port;status=result?Status::Connecting:Status::Connected;
        }
        return true;
    }
    bool Queue(const Message& message) {
        if(status!=Status::Connected || outgoing_.size()>=kQueueLimit) return false;
        outgoing_.push_back(Encode(message));return true;
    }
    std::optional<Message> Receive() {
        if(incoming_.empty()) return {};
        auto message=std::move(incoming_.front());incoming_.pop_front();return message;
    }
    void Close() {
        if(socket_!=INVALID_SOCKET) {shutdown(socket_,SD_SEND);closesocket(socket_);socket_=INVALID_SOCKET;}
        outgoing_.clear();incoming_.clear();offset_=0;port=0;status=Status::Closed;
    }
    void Pump() {
        if(status==Status::Listening) {
            const auto accepted=accept(socket_,nullptr,nullptr);
            if(accepted==INVALID_SOCKET) {if(WSAGetLastError()!=WSAEWOULDBLOCK) Fail("accept",WSAGetLastError());return;}
            closesocket(socket_);socket_=accepted;
            if(!Configure(socket_)) return;status=Status::Connected;
        }
        if(status==Status::Connecting) {
            fd_set writable{},errors{};FD_ZERO(&writable);FD_ZERO(&errors);FD_SET(socket_,&writable);FD_SET(socket_,&errors);
            timeval timeout{};const int result=select(0,nullptr,&writable,&errors,&timeout);
            if(result==SOCKET_ERROR) {Fail("select",WSAGetLastError());return;}
            if(!result) return;
            int failure=0,size=sizeof(failure);
            if(getsockopt(socket_,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&failure),&size) || failure) {
                Fail("connect completion",failure?failure:WSAGetLastError());return;
            }
            status=Status::Connected;
        }
        if(status!=Status::Connected) return;
        for(unsigned calls=0;calls<32 && !outgoing_.empty();++calls) {
            const auto& front=outgoing_.front();
            const auto sent=send(socket_,reinterpret_cast<const char*>(front.data()+offset_),int(front.size()-offset_),0);
            if(sent==SOCKET_ERROR) {if(WSAGetLastError()!=WSAEWOULDBLOCK) Fail("send",WSAGetLastError());break;}
            if(!sent) {Fail("zero-byte send",0);return;}
            offset_+=size_t(sent);if(offset_==front.size()) {outgoing_.pop_front();offset_=0;}
        }
        for(unsigned calls=0;calls<32 && status==Status::Connected;++calls) {
            std::array<uint8_t,kStreamLimit> bytes{};
            const auto received=recv(socket_,reinterpret_cast<char*>(bytes.data()),int(decoder_.available()),0);
            if(received==SOCKET_ERROR) {if(WSAGetLastError()!=WSAEWOULDBLOCK) Fail("recv",WSAGetLastError());break;}
            if(!received) {Close();return;}
            if(!decoder_.Append(std::span(bytes).first(size_t(received)))) {Fail("receive buffer limit",0);return;}
            while(auto message=decoder_.Next()) {
                if(incoming_.size()>=kQueueLimit) {Fail("message queue limit",0);return;}
                incoming_.push_back(std::move(*message));
            }
            if(decoder_.failed()) {Fail("invalid EM2 network packet",0);return;}
        }
    }
private:
    bool Configure(SOCKET value) {
        u_long nonblocking=1;int nodelay=1;
        if(ioctlsocket(value,FIONBIO,&nonblocking) ||
            setsockopt(value,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&nodelay),sizeof(nodelay)))
            return Fail("nonblocking/TCP_NODELAY",WSAGetLastError());
        return true;
    }
    bool Fail(const char* operation,int code) {error=SocketFailureText(operation,code);REXLOG_WARN("Online TCP {} failed ({}): {}",operation,code,error);Close();status=Status::Failed;return false;}
    SOCKET socket_=INVALID_SOCKET;int startup_error_=0;Decoder decoder_;
    std::deque<Bytes> outgoing_;std::deque<Message> incoming_;size_t offset_=0;
};
}

struct Session::Impl {
    std::unique_ptr<RouterMapping> router;
    TcpChannel channel;Identity identity;uint8_t role=0;State state=State::Idle;std::string error;
    TickBuffer ticks;bool hello_sent=false,wire_exhausted=false;uint64_t wire_tick=0;
    DigestBuffer digests;uint64_t local_digest_next=0,remote_digest_next=0;
    bool local_digest_exhausted=false,remote_digest_exhausted=false;
    std::optional<Digest> last_local_digest,last_remote_digest;
    LoadExchange loads;
    LobbyRoster roster{2};LobbySnapshot lobby;
    uint32_t profile_sequence=0,remote_profile_sequence=0,pending_profile=0;
    LobbyResult profile_result=LobbyResult::Ok;
    bool PublishLobby(){lobby=Snapshot(roster);return channel.Queue(lobby);}
    LobbyResult ApplyProfile(uint64_t peer,const LobbyProfile& p) {
        if(p.action==ProfileAction::Appearance)return roster.SetAppearance(peer,LobbyCharacter(p.character),p.color);
        if(p.action==ProfileAction::Ready)return roster.SetReady(peer,p.ready);
        const auto end=std::find(p.name.begin(),p.name.end(),char(0));
        if(std::any_of(end,p.name.end(),[](char c){return c!=0;}))return LobbyResult::InvalidName;
        return roster.Rename(peer,std::string_view(p.name.data(),size_t(end-p.name.begin())));
    }
    bool Profile(LobbyProfile p) {
        if(state!=State::Ready || gameplay_started || pending_profile || lobby_start==LobbyStartPhase::Requested || lobby_start==LobbyStartPhase::Confirmed)return false;
        if(!role){profile_result=ApplyProfile(1,p);return PublishLobby();}
        if(profile_sequence==UINT32_MAX)return false;
        p.sequence=profile_sequence+1;if(!channel.Queue(p))return false;
        profile_sequence=p.sequence;pending_profile=p.sequence;return true;
    }
    Clock::time_point started{},last_received{},last_ping{};uint32_t ping_sequence=0,last_pong=0,peer_ping=0;
    bool checkpoint_mode=false,checkpoint_begin=false,checkpoint_end=false,checkpoint_installed=false;
    bool gameplay_started=false;Clock::time_point ready_at{};
    LobbyStartPhase lobby_start=LobbyStartPhase::Waiting;
    Clock::time_point lobby_requested_at{};
    size_t checkpoint_sent=0,checkpoint_size=0;
    std::shared_ptr<const CheckpointBundle> offered;
    std::shared_ptr<Bytes> downloaded;
    Clock::time_point last_transfer_progress{};
    size_t checkpoint_acknowledged=0;
    size_t checkpoint_reported=0;
    bool UsesLobbyStart()const{
        return checkpoint_mode && identity.startup_policy==StartupPolicy::NativeMenu;
    }
    void Fail(const char* reason) {error=reason;channel.Close();router.reset();state=State::Error;ticks.Reset(ticks.next());}
    bool Start(const Identity& id,bool host,std::string_view address,uint16_t port,bool transfer=false,
               std::shared_ptr<const CheckpointBundle> bundle={},bool reserved=false) {
        if(!reserved)channel.Close();
        router.reset();ticks.Reset(id.first_tick);identity=id;role=host?0:1;hello_sent=false;wire_exhausted=false;
        digests.Reset(id.first_tick);local_digest_next=remote_digest_next=id.first_tick;
        local_digest_exhausted=remote_digest_exhausted=false;last_local_digest.reset();last_remote_digest.reset();
        loads=LoadExchange{};roster=LobbyRoster(2);lobby={};
        profile_sequence=remote_profile_sequence=pending_profile=0;profile_result=LobbyResult::Ok;
        if(host)roster.Join(1,"Player 1");
        wire_tick=id.first_tick;ping_sequence=last_pong=peer_ping=0;error.clear();started=Clock::now();last_received=last_ping=started;
        checkpoint_mode=transfer;checkpoint_begin=checkpoint_end=checkpoint_installed=false;
        gameplay_started=false;ready_at={};
        lobby_start=LobbyStartPhase::Waiting;lobby_requested_at={};
        checkpoint_sent=checkpoint_size=checkpoint_acknowledged=checkpoint_reported=0;downloaded.reset();offered=std::move(bundle);
        if(!ValidIdentity(id,transfer && !host)) {Fail("Missing or invalid build/content/checkpoint/session identity");return false;}
        if(transfer && host && (!offered || offered->identity!=id.checkpoint || offered->bytes.size()<52 || offered->bytes.size()>kMaxCheckpointBundle)) {
            Fail("Missing, oversized or incompatible host checkpoint bundle");return false;
        }
        if(!reserved && !channel.Open(host,address,port)) {error=channel.error;state=State::Error;return false;}
        state=host?State::Listening:State::Connecting;return true;
    }
    void Process(const Message& message,Clock::time_point now) {
        if(const auto* hello=std::get_if<Hello>(&message)) {
            auto compatible=hello->identity;
            if(checkpoint_mode)compatible.checkpoint=identity.checkpoint;
            const char* mismatch=nullptr;
            if(state!=State::Handshaking)mismatch="Unexpected or repeated online handshake. Start a new session together.";
            else if(hello->role==role)mismatch="Both copies selected the same role. One player must host and the other must join.";
            else if(hello->checkpoint!=checkpoint_mode)mismatch="Different online startup methods. Both players must use the same Host/Join flow.";
            else if(compatible.startup_policy!=identity.startup_policy)mismatch="Different online startup paths. Both players must open Online Multiplayer from the native title menu, or both use the same prelaunch diagnostic flow.";
            else if(compatible.initialization_policy!=identity.initialization_policy)mismatch="Different native initialization policies. Both players must enable the same deterministic online initialization before hosting or joining.";
            else if(compatible.build!=identity.build)mismatch="Different Beetleworx versions. Install the same release version on both devices. Unversioned development copies must use the same build.";
            else if(compatible.content!=identity.content)mismatch="Different game content or mods. Both players must use matching game files.";
            else if(compatible.session!=identity.session)mismatch="This invitation belongs to a different session. Copy the host's current invitation and retry.";
            else if(compatible.first_tick!=identity.first_tick || compatible.input_delay!=identity.input_delay)mismatch="Different session timing or input buffer. Join using the host's current invitation.";
            else if(compatible.checkpoint!=identity.checkpoint)mismatch="Different saves. Start from the shared host save before connecting.";
            if(mismatch){Fail(mismatch);return;}
            if(checkpoint_mode && role)identity.checkpoint=hello->identity.checkpoint;
            if(!role)roster.Join(2,"Player 2");
            state=checkpoint_mode?State::Transferring:State::Ready;last_transfer_progress=now;ready_at=now;last_received=now;
            if(!role && !PublishLobby())Fail("Lobby snapshot queue full");return;
        }
        if(state==State::Transferring || state==State::Ready) {
            if(const auto* snapshot=std::get_if<LobbySnapshot>(&message)) {
                if(!role || snapshot->capacity!=2 || snapshot->players.size()!=2 ||
                   snapshot->players[0].peer!=1 || snapshot->players[1].peer!=2 ||
                   snapshot->revision<lobby.revision || (snapshot->revision==lobby.revision && *snapshot!=lobby)) {
                    Fail("Invalid authoritative lobby roster");return;
                }
                lobby=*snapshot;last_received=now;return;
            }
            if(const auto* request=std::get_if<LobbyProfile>(&message)) {
                if(role || request->sequence!=remote_profile_sequence+1 || remote_profile_sequence==UINT32_MAX){Fail("Invalid lobby profile request ordering or direction");return;}
                remote_profile_sequence=request->sequence;
                auto result=(state!=State::Ready || gameplay_started || lobby_start==LobbyStartPhase::Requested || lobby_start==LobbyStartPhase::Confirmed)
                    ?LobbyResult::Locked:ApplyProfile(2,*request);
                if(!PublishLobby() || !channel.Queue(LobbyReply{request->sequence,result,roster.revision()})){Fail("Lobby reply queue full");return;}
                last_received=now;return;
            }
            if(const auto* reply=std::get_if<LobbyReply>(&message)) {
                if(!role || !pending_profile || reply->sequence!=pending_profile || reply->revision!=lobby.revision){Fail("Invalid lobby profile acknowledgement");return;}
                pending_profile=0;profile_result=reply->result;last_received=now;return;
            }
            if(const auto* ping=std::get_if<Ping>(&message)) {
                if(!ping->sequence || ping->sequence<=peer_ping){Fail("Invalid heartbeat sequence");return;}
                peer_ping=ping->sequence;if(!channel.Queue(Pong{ping->sequence})){Fail("Heartbeat send queue full");return;}
                last_received=now;return;
            }
            if(const auto* pong=std::get_if<Pong>(&message)) {
                if(pong->sequence<=last_pong || pong->sequence>ping_sequence){Fail("Unsolicited or repeated heartbeat response");return;}
                last_pong=pong->sequence;last_received=now;return;
            }
        }
        if(state==State::Transferring) {
            if(const auto* progress=std::get_if<CheckpointProgress>(&message)) {
                if(role || !checkpoint_begin || progress->received<checkpoint_acknowledged || progress->received>checkpoint_sent){Fail("Invalid save transfer progress acknowledgement");return;}
                if(progress->received>checkpoint_acknowledged){checkpoint_acknowledged=progress->received;last_transfer_progress=now;}
                last_received=now;return;
            }
            if(const auto* begin=std::get_if<CheckpointBegin>(&message)) {
                if(!role || checkpoint_begin || begin->size<52 || begin->size>kMaxCheckpointBundle || begin->identity!=identity.checkpoint){Fail("Invalid checkpoint announcement");return;}
                checkpoint_begin=true;checkpoint_size=begin->size;downloaded=std::make_shared<Bytes>();downloaded->reserve(checkpoint_size);
            } else if(const auto* chunk=std::get_if<CheckpointChunk>(&message)) {
                if(!role || !checkpoint_begin || checkpoint_end || chunk->offset!=downloaded->size() || chunk->size>checkpoint_size-downloaded->size()) {
                    Fail("Checkpoint chunk violates length or ordering");return;
                }
                downloaded->insert(downloaded->end(),chunk->data.begin(),chunk->data.begin()+chunk->size);
            } else if(const auto* end=std::get_if<CheckpointEnd>(&message)) {
                if(!role || !checkpoint_begin || checkpoint_end || end->size!=checkpoint_size || downloaded->size()!=checkpoint_size){Fail("Incomplete or repeated checkpoint end");return;}
                checkpoint_end=true;REXLOG_INFO("Online checkpoint download complete: {} bytes; installation required",checkpoint_size);
            } else if(const auto* installed=std::get_if<CheckpointInstalled>(&message)) {
                if(role || !checkpoint_end || checkpoint_installed || installed->identity!=identity.checkpoint){Fail("Invalid checkpoint installation acknowledgement");return;}
                if(!channel.Queue(CheckpointReady{identity.checkpoint})){Fail("Checkpoint acknowledgement queue full");return;}
                checkpoint_installed=true;
                REXLOG_INFO("Online checkpoint installation acknowledged by client; awaiting ready confirmation");
            } else if(const auto* ready=std::get_if<CheckpointReady>(&message)) {
                if(!checkpoint_installed || ready->identity!=identity.checkpoint){Fail("Unexpected checkpoint-ready acknowledgement");return;}
                if(role && !channel.Queue(CheckpointReady{identity.checkpoint})){Fail("Checkpoint ready-confirmation queue full");return;}
                state=State::Ready;ready_at=now;downloaded.reset();offered.reset();
                REXLOG_INFO("Online checkpoint installation mutually acknowledged; {} input channel ready",role?"client":"host");
            } else {Fail("Gameplay or unexpected message during checkpoint transfer");return;}
            last_transfer_progress=last_received=now;return;
        }
        if(state!=State::Ready) {Fail("Gameplay input arrived before compatible handshake");return;}
        if(const auto* action=std::get_if<LobbyStart>(&message)){
            if(!UsesLobbyStart()){
                Fail("Unexpected lobby start message");
                return;
            }
            if(!role && lobby_start==LobbyStartPhase::Waiting && *action==LobbyStart::ClientReady && roster.Find(2) && roster.Find(2)->ready){
                lobby_start=LobbyStartPhase::ClientReady;
            }else if(role && lobby_start==LobbyStartPhase::ClientReady && *action==LobbyStart::Start && lobby.locked){
                if(!channel.Queue(LobbyStart::Acknowledge)){Fail("Lobby start acknowledgement queue full");return;}
                lobby_start=LobbyStartPhase::Confirmed;
                ready_at=now;
            }else if(!role && lobby_start==LobbyStartPhase::Requested && *action==LobbyStart::Acknowledge){
                lobby_start=LobbyStartPhase::Confirmed;
                ready_at=now;
            }else{
                Fail("Invalid or repeated lobby start message");
                return;
            }
            last_received=now;
            return;
        }
        if(UsesLobbyStart() && lobby_start!=LobbyStartPhase::Confirmed){
            Fail("Gameplay arrived before the lobby start was confirmed");
            return;
        }
        if(const auto* input=std::get_if<Input>(&message)) {
            if(role || input->player!=1 || ticks.Add(1,input->tick,input->pad)==Added::Invalid) {
                Fail("Invalid player, conflicting input or input outside synchronization window");return;
            }
        } else if(const auto* frame=std::get_if<Frame>(&message)) {
            if(!role || wire_exhausted || frame->tick!=wire_tick || !ticks.OwnMatches(1,frame->tick,frame->players[1]) ||
                ticks.Add(0,frame->tick,frame->players[0])==Added::Invalid) {
                Fail("Authoritative frame changed local input or violated tick ordering");return;
            }
            if(wire_tick==std::numeric_limits<uint64_t>::max()) wire_exhausted=true;else ++wire_tick;
        } else if(const auto* digest=std::get_if<Digest>(&message)) {
            if(last_remote_digest && *last_remote_digest==*digest) {last_received=now;return;}
            if(remote_digest_exhausted || digest->tick!=remote_digest_next || !digests.Add(1,*digest)) {
                REXLOG_ERROR("Online state digest rejected at tick {}",digest->tick);
                Fail("Peer world state diverged or state digest violated completion ordering/window");return;
            }
            last_remote_digest=*digest;
            if(remote_digest_next==UINT64_MAX) remote_digest_exhausted=true;else ++remote_digest_next;
        } else if(const auto* load=std::get_if<LoadPoint>(&message)) {
            const auto next=ticks.next();
            const bool frame_sent=role?(wire_exhausted || load->tick<wire_tick):ticks.Consumed(load->tick);
            if(!frame_sent || load->tick<identity.first_tick || (load->tick>next && load->tick-next>=kWindow) ||
               !loads.Add(1,*load,now)) {Fail("Peer native level load differs or violates load ordering");return;}
        } else {Fail("Unexpected checkpoint message after transfer");return;}
        last_received=now;
    }
};
Session::Session():impl_(std::make_unique<Impl>()) {}
Session::~Session()=default;
const LobbySnapshot& Session::lobby()const{return impl_->lobby;}
LobbyResult Session::last_lobby_result()const{return impl_->profile_result;}
bool Session::lobby_request_pending()const{return impl_->pending_profile!=0;}
bool Session::RenameLobbyPlayer(std::string_view name){
    if(name.size()>96)return false;LobbyProfile p;p.action=ProfileAction::Rename;
    std::copy(name.begin(),name.end(),p.name.begin());return impl_->Profile(p);
}
bool Session::SetLobbyAppearance(LobbyCharacter character,uint8_t color){
    LobbyProfile p;p.action=ProfileAction::Appearance;p.character=uint8_t(character);p.color=color;return impl_->Profile(p);
}
bool Session::SetLobbyReady(bool ready){LobbyProfile p;p.action=ProfileAction::Ready;p.ready=ready;return impl_->Profile(p);}
bool Session::ArmLobbyStart(){
    auto& p=*impl_;
    if(p.state!=State::Ready || !p.UsesLobbyStart() || !p.role)return false;
    if(p.lobby_start!=LobbyStartPhase::Waiting){
        if(p.lobby_start==LobbyStartPhase::ClientReady && !p.pending_profile && p.lobby.players.size()==2 && !p.lobby.players[1].ready)return SetLobbyReady(true);
        return true;
    }
    if(!SetLobbyReady(true))return false;
    if(!p.channel.Queue(LobbyStart::ClientReady)){p.Fail("Lobby ready queue full");return false;}
    p.lobby_start=LobbyStartPhase::ClientReady;
    return true;
}
bool Session::CanRequestLobbyStart()const{
    const auto& p=*impl_;
    return p.state==State::Ready && p.UsesLobbyStart() && !p.role &&
        p.lobby_start==LobbyStartPhase::ClientReady && p.roster.Find(2) && p.roster.Find(2)->ready;
}
bool Session::RequestLobbyStart(){
    if(!CanRequestLobbyStart())return false;
    auto& p=*impl_;
    if(p.roster.SetReady(1,true)!=LobbyResult::Ok || p.roster.Start()!=LobbyResult::Ok)return false;
    if(!p.PublishLobby() || !p.channel.Queue(LobbyStart::Start)){p.Fail("Lobby start queue full");return false;}
    p.lobby_start=LobbyStartPhase::Requested;
    p.lobby_requested_at=Clock::now();
    return true;
}
bool Session::LobbyStartConfirmed()const{
    const auto& p=*impl_;
    return p.state==State::Ready && p.UsesLobbyStart() && p.lobby_start==LobbyStartPhase::Confirmed;
}
bool Session::Host(const Identity& id,std::string_view address,uint16_t port) {return impl_->Start(id,true,address,port);}
bool Session::Join(const Identity& id,std::string_view address,uint16_t port) {return impl_->Start(id,false,address,port);}
bool Session::HostWithCheckpoint(const Identity& id,std::string_view address,uint16_t port,std::shared_ptr<const CheckpointBundle> bundle) {
    return impl_->Start(id,true,address,port,true,std::move(bundle));
}
bool Session::ReserveHostListener(std::string_view address,uint16_t port){
    auto& p=*impl_;
    if(p.state!=State::Idle || p.channel.status==TcpChannel::Status::Listening)return false;
    if(!p.channel.Open(true,address,port)){
        p.error=p.channel.error;
        p.state=State::Error;
        return false;
    }
    return true;
}
bool Session::StartReservedHost(const Identity& id,std::shared_ptr<const CheckpointBundle> bundle){
    auto& p=*impl_;
    if(p.state!=State::Idle || p.channel.status!=TcpChannel::Status::Listening ||
       id.startup_policy!=StartupPolicy::NativeMenu)return false;
    return p.Start(id,true,{},0,true,std::move(bundle),true);
}
bool Session::JoinWithCheckpoint(const Identity& id,std::string_view address,uint16_t port){return impl_->Start(id,false,address,port,true);}
std::shared_ptr<const std::vector<uint8_t>> Session::DownloadedCheckpoint() const {
    const auto& p=*impl_;return p.role && p.state==State::Transferring && p.checkpoint_end && !p.checkpoint_installed?p.downloaded:nullptr;
}
bool Session::ConfirmCheckpointInstalled(bool success) {
    auto& p=*impl_;if(!DownloadedCheckpoint())return false;
    if(!success){p.Fail("Received checkpoint could not be validated or installed");return false;}
    if(!p.channel.Queue(CheckpointInstalled{p.identity.checkpoint})){p.Fail("Checkpoint installation queue full");return false;}
    p.checkpoint_installed=true;return true;
}
void Session::Disconnect() {impl_->channel.Close();impl_->router.reset();impl_->ticks.Reset(impl_->ticks.next());impl_->state=State::Disconnected;}
bool Session::KeepRouterMapping(std::unique_ptr<RouterMapping>& router) {
    auto& p=*impl_;if(p.role || p.state!=State::Ready || p.router || !router || router->internal_port()!=p.channel.port || router->status().state==RouterState::Idle)return false;
    p.router=std::move(router);return true;
}
State Session::state() const {return impl_->state;}
uint16_t Session::port() const {return impl_->channel.port;}
uint64_t Session::next_tick() const {return impl_->ticks.next();}
const Identity& Session::identity() const {return impl_->identity;}
const std::string& Session::error() const {return impl_->error;}
void Session::Poll(Clock::time_point now) {
    auto& p=*impl_;
    if(p.state==State::Ready && p.loads.Expired(now)){p.Fail("Native level loading rendezvous timed out");return;}
    if(p.state==State::Idle || p.state==State::Error || p.state==State::Disconnected) return;
    if(p.router)p.router->Poll(now);
    p.channel.Pump();
    if(p.channel.status==TcpChannel::Status::Failed) {p.error=p.channel.error;p.router.reset();p.state=State::Error;p.ticks.Reset(p.ticks.next());return;}
    if(p.channel.status==TcpChannel::Status::Closed) {Disconnect();return;}
    if(p.channel.status==TcpChannel::Status::Connected && !p.hello_sent) {
        if(!p.channel.Queue(Hello{p.identity,p.role,p.checkpoint_mode})) {p.Fail("Handshake send queue full");return;}
        p.hello_sent=true;p.state=State::Handshaking;p.started=p.last_received=p.last_ping=now;
        // Send our identity before rejecting a mismatched incoming hello, so
        // the joining copy can diagnose the same mismatch instead of only EOF.
        p.channel.Pump();
        if(p.channel.status==TcpChannel::Status::Failed){p.error=p.channel.error;p.state=State::Error;p.router.reset();return;}
        if(p.channel.status==TcpChannel::Status::Closed){Disconnect();return;}
    }
    while(auto message=p.channel.Receive()) {p.Process(*message,now);if(p.state==State::Error) return;}
    if(p.state==State::Transferring && !p.role && !p.checkpoint_end) {
        if(!p.checkpoint_begin) {
            if(!p.channel.Queue(CheckpointBegin{uint32_t(p.offered->bytes.size()),p.identity.checkpoint})){p.Fail("Checkpoint announcement queue full");return;}
            p.checkpoint_begin=true;
        }
        const auto previous_sent=p.checkpoint_sent;
        for(unsigned i=0;i<32 && p.checkpoint_sent<p.offered->bytes.size();++i) {
            CheckpointChunk chunk{};chunk.offset=uint32_t(p.checkpoint_sent);chunk.size=uint16_t(std::min<size_t>(1024,p.offered->bytes.size()-p.checkpoint_sent));
            std::copy_n(p.offered->bytes.begin()+p.checkpoint_sent,chunk.size,chunk.data.begin());
            if(!p.channel.Queue(chunk))break;p.checkpoint_sent+=chunk.size;
        }
        // Queue growth is bounded. New chunks can only continue to enter as
        // the nonblocking channel drains; heartbeats alone are not progress.
        if(p.checkpoint_sent!=previous_sent)p.last_transfer_progress=now;
        if(p.checkpoint_sent==p.offered->bytes.size() && p.channel.Queue(CheckpointEnd{uint32_t(p.checkpoint_sent)}))p.checkpoint_end=true;
    }
    // Report actual validated receipt independently of the sender's TCP send
    // buffer. Coalesce reports per poll and per 32 KiB, including the final tail.
    if(p.state==State::Transferring && p.role && p.downloaded && p.downloaded->size()>p.checkpoint_reported &&
       (p.downloaded->size()-p.checkpoint_reported>=32768 || p.checkpoint_end)) {
        if(p.channel.Queue(CheckpointProgress{uint32_t(p.downloaded->size())}))p.checkpoint_reported=p.downloaded->size();
    }
    if(p.state==State::Ready || p.state==State::Transferring) {
        if(p.state==State::Transferring && now-p.last_transfer_progress>std::chrono::seconds(120)){p.Fail("No save transfer or installation progress for two minutes. Retry the connection.");return;}
        if(p.lobby_start==LobbyStartPhase::Requested && now-p.lobby_requested_at>std::chrono::seconds(10)){
            p.Fail("The other player did not confirm the lobby start. Rejoin and try again.");
            return;
        }
        const bool loading=p.state==State::Ready && p.checkpoint_mode && !p.gameplay_started &&
            (!p.UsesLobbyStart() || p.lobby_start==LobbyStartPhase::Confirmed);
        if(loading && now-p.ready_at>std::chrono::seconds(120)){p.Fail("Runtime/world startup timed out");return;}
        if(!loading && now-p.last_received>std::chrono::seconds(5)) {p.Fail("Peer stopped responding");return;}
        if(now-p.last_ping>=std::chrono::seconds(1)) {
            if(p.ping_sequence==UINT32_MAX) {p.Fail("Heartbeat counter exhausted; reconnect required");return;}
            if(!p.channel.Queue(Ping{++p.ping_sequence})) {p.Fail("Heartbeat send queue full");return;}
            p.last_ping=now;
        }
    }else if(p.state==State::Connecting || p.state==State::Handshaking){
        // A native guest can finish its own verification before a host on a
        // slower drive. The TCP connection is already open in Handshaking.
        const bool waiting_for_host=p.state==State::Handshaking && p.role && p.UsesLobbyStart();
        const auto timeout=waiting_for_host?std::chrono::seconds(120):std::chrono::seconds(10);
        if(now-p.started>timeout)p.Fail(waiting_for_host
            ?"The host is taking too long to finish checking game files."
            :"Connection/handshake timed out");
    }
}
bool Session::SubmitLocal(uint64_t tick,const Pad& pad) {
    auto& p=*impl_;
    if(p.state!=State::Ready || (p.UsesLobbyStart() && !LobbyStartConfirmed()))return false;
    const auto result=p.ticks.Check(p.role,tick,pad);
    if(result==Added::Invalid) return false;
    if(result==Added::Duplicate) return true;
    if(p.role && !p.channel.Queue(Input{tick,1,pad})) return false;
    return p.ticks.Add(p.role,tick,pad)==Added::New;
}
bool Session::TakeFrame(Frame& frame) {
    auto& p=*impl_;if(p.state!=State::Ready || !p.ticks.Peek(frame)) return false;
    if(!p.role && !p.channel.Queue(frame)) return false;
    const bool consumed=p.ticks.Take(frame);if(consumed)p.gameplay_started=true;return consumed;
}
unsigned Session::local_player() const {return impl_->role;}
std::optional<uint64_t> Session::verified_tick() const {return impl_->state==State::Ready?impl_->digests.verified():std::nullopt;}
bool Session::SubmitStateDigest(uint64_t tick,const std::array<uint8_t,32>& hash) {
    auto& p=*impl_;if(p.state!=State::Ready) return false;
    const Digest digest{tick,hash};
    if(p.last_local_digest && *p.last_local_digest==digest) return true;
    if(p.loads.Blocks(tick))return false;
    if(p.local_digest_exhausted || tick!=p.local_digest_next || !p.ticks.Consumed(tick)) return false;
    // Queue failure leaves the local sequence untouched for a later retry.
    if(!p.channel.Queue(digest)) return false;
    if(!p.digests.Add(0,digest)) {p.Fail("Local world state differs from the peer or exceeded verification window");return false;}
    p.last_local_digest=digest;
    if(p.local_digest_next==UINT64_MAX) p.local_digest_exhausted=true;else ++p.local_digest_next;
    return true;
}
bool Session::SubmitLoadPoint(const LoadPoint& point) {
    auto& p=*impl_;if(p.state!=State::Ready || p.local_digest_exhausted ||
        point.tick!=p.local_digest_next || !p.ticks.Consumed(point.tick))return false;
    auto next=p.loads;
    if(!next.Add(0,point,Clock::now()) || !p.channel.Queue(point))return false;
    p.loads=std::move(next);return true;
}
bool Session::LoadPointAgreed(const LoadPoint& point) const{return impl_->state==State::Ready && impl_->loads.Agreed(point);}

bool TestCheckpointTransport(const std::filesystem::path& root) {
    auto check=[](bool ok,const char* text){if(!ok)REXLOG_ERROR("Checkpoint transport test: {}",text);return ok;};
    try {
        std::filesystem::create_directories(root/"source");
        std::string payload(1024*1024+137,'\0');for(size_t i=0;i<payload.size();++i)payload[i]=char(i*73);
        {std::ofstream file(root/"source"/"save.dat",std::ios::binary);file.write(payload.data(),payload.size());}
        auto packed=PackCheckpoint(root/"source");if(!check(bool(packed.value),"source checkpoint packed"))return false;
        auto bundle=std::make_shared<CheckpointBundle>(std::move(*packed.value));
        Identity identity;identity.build[0]=1;identity.content[0]=2;identity.session[0]=3;identity.checkpoint=bundle->identity;
        Session host,client;
        auto pump=[&](auto done) {
            const auto end=Clock::now()+std::chrono::seconds(3);
            do{host.Poll();client.Poll();if(done())return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(Clock::now()<end);
            REXLOG_ERROR("Checkpoint transfer states {} / {}: '{}' / '{}'",int(host.state()),int(client.state()),host.error(),client.error());return false;
        };
        auto fresh_client=identity;fresh_client.checkpoint={};
        if(!check(host.HostWithCheckpoint(identity,"127.0.0.1",0,bundle) && client.JoinWithCheckpoint(fresh_client,"127.0.0.1",host.port()) &&
            !client.ConfirmCheckpointInstalled(true) && !host.ConfirmCheckpointInstalled(true),"checkpoint peers open without allowing premature installation acknowledgement"))return false;
        if(!check(pump([&]{return bool(client.DownloadedCheckpoint());}),"chunked real TCP download completes"))return false;
        const auto received=client.DownloadedCheckpoint();Frame frame;
        if(!check(*received==bundle->bytes && client.identity().checkpoint==identity.checkpoint && host.state()==State::Transferring && client.state()==State::Transferring &&
            !host.SubmitLocal(0,{}) && !client.SubmitLocal(0,{}) && !host.TakeFrame(frame),"exact checkpoint bytes received while all gameplay input remains blocked"))return false;
        const auto installed=InstallCheckpoint(*received,identity.checkpoint,std::filesystem::absolute(root/"received"));
        if(!check(installed.installed && client.ConfirmCheckpointInstalled(true) &&
            pump([&]{return host.state()==State::Ready && client.state()==State::Ready;}),"both peers wait for verified installation and mutual acknowledgement"))return false;
        host.Poll(Clock::now()+std::chrono::seconds(6));
        if(!check(host.state()==State::Ready,"checkpoint session tolerates runtime initialization before first agreed frame"))return false;
        if(!check(host.SubmitLocal(0,Pad{0x1000}) && client.SubmitLocal(0,Pad{0x2000}) &&
            pump([&]{return host.TakeFrame(frame);}) && pump([&]{Frame other;return client.TakeFrame(other)&&other==frame;}),"same TCP connection transitions into agreed gameplay frames after checkpoint installation"))return false;
        host.Poll(Clock::now()+std::chrono::seconds(6));
        if(host.state()==State::Ready)host.Poll(Clock::now()+std::chrono::seconds(12));
        if(!check(host.state()==State::Error,"five-second response timeout resumes after gameplay consumes its first frame"))return false;
        host.Disconnect();client.Disconnect();
        if(!check(host.HostWithCheckpoint(identity,"127.0.0.1",0,bundle) && client.JoinWithCheckpoint(fresh_client,"127.0.0.1",host.port()) &&
            pump([&]{return bool(client.DownloadedCheckpoint());}),"startup deadline fixture downloads"))return false;
        const auto startup_save=InstallCheckpoint(*client.DownloadedCheckpoint(),identity.checkpoint,std::filesystem::absolute(root/"startup-cap"));
        if(!check(startup_save.installed && client.ConfirmCheckpointInstalled(true) &&
            pump([&]{return host.state()==State::Ready && client.state()==State::Ready;}),"startup deadline fixture installs"))return false;
        host.Poll(Clock::now()+std::chrono::seconds(121));
        if(!check(host.state()==State::Error,"runtime startup grace has an absolute bounded deadline"))return false;
        host.Disconnect();client.Disconnect();
        auto damaged=std::make_shared<CheckpointBundle>(*bundle);damaged->bytes.back()^=1;
        if(!check(host.HostWithCheckpoint(identity,"127.0.0.1",0,damaged) && client.JoinWithCheckpoint(identity,"127.0.0.1",host.port()) &&
            pump([&]{return bool(client.DownloadedCheckpoint());}),"damaged transport fixture reaches validation stage"))return false;
        const auto bad=InstallCheckpoint(*client.DownloadedCheckpoint(),identity.checkpoint,std::filesystem::absolute(root/"corrupt"));
        if(!check(!bad.installed && !client.ConfirmCheckpointInstalled(false) && client.state()==State::Error && host.state()!=State::Ready,
            "corruption or installation failure cannot open gameplay"))return false;
        host.Disconnect();client.Disconnect();
        if(!check(host.Host(identity,"127.0.0.1",0) && client.JoinWithCheckpoint(identity,"127.0.0.1",host.port()) &&
            pump([&]{return host.state()==State::Error || client.state()==State::Error;}),"checkpoint transfer requires explicit opt-in by both peers"))return false;
        host.Disconnect();client.Disconnect();
        auto rejected=[&](std::vector<Message> messages) {
            TcpChannel peer;Session victim;
            if(!peer.Open(true,"127.0.0.1",0) || !victim.JoinWithCheckpoint(identity,"127.0.0.1",peer.port))return false;
            bool sent=false;const auto end=Clock::now()+std::chrono::seconds(2);
            do {
                victim.Poll();peer.Pump();while(peer.Receive()){}
                if(!sent && peer.status==TcpChannel::Status::Connected){
                    if(!peer.Queue(Hello{identity,0,true}))return false;
                    for(const auto& message:messages)if(!peer.Queue(message))return false;sent=true;
                }
                if(victim.state()==State::Error)return !victim.DownloadedCheckpoint();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }while(Clock::now()<end);return false;
        };
        const CheckpointBegin begin{uint32_t(bundle->bytes.size()),identity.checkpoint};
        if(!check(rejected({CheckpointBegin{uint32_t(kMaxCheckpointBundle+1),identity.checkpoint}}) &&
            rejected({CheckpointChunk{0,1,{}}}) && rejected({begin,CheckpointChunk{1,1,{}}}) &&
            rejected({begin,CheckpointChunk{0,1,{}},CheckpointChunk{0,1,{}}}) &&
            rejected({begin,CheckpointEnd{begin.size}}) && rejected({begin,begin}) &&
            rejected({CheckpointReady{identity.checkpoint}}) && rejected({Frame{0,{}}}),
            "oversized, missing, repeated, out-of-order, truncated and premature-ready/gameplay packets rejected"))return false;
        REXLOG_INFO("Checkpoint transport tests passed: real TCP chunks, exact binary save, install/ack barrier, same-channel gameplay, corruption, opt-in and malformed sequencing");return true;
    }catch(const std::exception& error){REXLOG_ERROR("Checkpoint transport test exception: {}",error.what());return false;}
}
bool TestOnlineSession() {
    auto check=[](bool ok,const char* text){if(!ok) REXLOG_ERROR("Online input regression: {}",text);return ok;};
    Identity identity;identity.build[0]=1;identity.content[0]=2;identity.checkpoint[0]=3;identity.session[0]=4;identity.first_tick=0x100000001ull;
    // Use real connected loopback channels for production Process/Poll, but
    // advance the policy clock instead of sleeping for several minutes.
    for(bool complete:{false,true}) {
        TcpChannel peer;Session receiving;
        if(!check(peer.Open(true,"127.0.0.1",0) && receiving.JoinWithCheckpoint(identity,"127.0.0.1",peer.port),"slow-save fixture connects"))return false;
        bool hello=false;const auto deadline=Clock::now()+std::chrono::seconds(2);
        while(receiving.state()!=State::Transferring && Clock::now()<deadline) {
            peer.Pump();if(peer.status==TcpChannel::Status::Connected && !hello){hello=peer.Queue(Hello{identity,0,true});peer.Pump();}
            receiving.Poll();std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if(!check(receiving.state()==State::Transferring,"slow-save fixture negotiates checkpoint mode"))return false;
        auto& state=*receiving.impl_;auto now=Clock::now();
        state.Process(CheckpointBegin{4096,identity.checkpoint},now);
        for(unsigned i=0;i<(complete?4u:3u);++i) {
            now+=std::chrono::seconds(100);CheckpointChunk chunk{};chunk.offset=i*1024;chunk.size=1024;chunk.data.fill(uint8_t(i));
            state.Process(chunk,now);receiving.Poll(now);peer.Pump();while(peer.Receive()){}
            if(!check(receiving.state()==State::Transferring,"valid save chunks keep a transfer alive beyond two minutes total"))return false;
        }
        if(complete){state.Process(CheckpointEnd{4096},now);if(!check(bool(receiving.DownloadedCheckpoint()),"completed save waits for installation"))return false;}
        state.Process(Ping{1},now+std::chrono::seconds(119));receiving.Poll(now+std::chrono::seconds(119));
        if(!check(receiving.state()==State::Transferring,"heartbeat keeps connection alive within save-progress grace"))return false;
        state.Process(Ping{2},now+std::chrono::seconds(121));receiving.Poll(now+std::chrono::seconds(121));
        if(!check(receiving.state()==State::Error && receiving.error().find("No save transfer or installation progress")!=std::string::npos,"heartbeats cannot hide a stalled download or installation"))return false;
    }
    {
        Session sending;TcpChannel peer;auto offered=std::make_shared<CheckpointBundle>();offered->identity=identity.checkpoint;offered->bytes.resize(1024*1024);
        if(!check(sending.HostWithCheckpoint(identity,"127.0.0.1",0,offered) && peer.Open(false,"127.0.0.1",sending.port()),"sender progress fixture connects"))return false;
        bool hello=false;const auto deadline=Clock::now()+std::chrono::seconds(2);
        while(sending.state()!=State::Transferring && Clock::now()<deadline) {
            peer.Pump();if(peer.status==TcpChannel::Status::Connected && !hello){hello=peer.Queue(Hello{identity,1,true});peer.Pump();}
            sending.Poll();std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if(!check(sending.state()==State::Transferring,"sender progress fixture negotiates"))return false;
        auto& state=*sending.impl_;auto now=Clock::now();
        for(unsigned i=0;i<128 && !state.checkpoint_end;++i){sending.Poll(now);peer.Pump();while(peer.Receive()){};}
        if(!check(state.checkpoint_end,"sender queues whole save on real TCP channel"))return false;
        now+=std::chrono::seconds(100);state.Process(CheckpointProgress{32768},now);sending.Poll(now);peer.Pump();while(peer.Receive()){};
        if(!check(state.checkpoint_acknowledged==32768 && sending.state()==State::Transferring,"new received bytes renew host progress after OS buffering"))return false;
        state.Process(Ping{1},now+std::chrono::seconds(119));sending.Poll(now+std::chrono::seconds(119));
        if(!check(sending.state()==State::Transferring,"host remains alive beyond original deadline after receiver progress"))return false;
        now+=std::chrono::seconds(119);state.Process(CheckpointProgress{65536},now);sending.Poll(now);
        if(!check(state.checkpoint_acknowledged==65536 && sending.state()==State::Transferring,"partial receipt keeps buffered host transfer alive independently of ping order"))return false;
        state.Process(CheckpointProgress{65536},now+std::chrono::seconds(121));sending.Poll(now+std::chrono::seconds(121));
        if(!check(sending.state()==State::Error && sending.error().find("No save transfer or installation progress")!=std::string::npos,"repeated byte acknowledgements cannot hide stalled installation"))return false;
    }
    for(unsigned scenario=0;scenario<4;++scenario) {
        Session malformed;auto& state=*malformed.impl_;state.state=State::Transferring;
        state.role=scenario==3?1:0;state.checkpoint_begin=scenario!=2;state.checkpoint_sent=1024;state.checkpoint_acknowledged=512;
        state.Process(CheckpointProgress{scenario==0?1025u:scenario==1?511u:1024u},Clock::now());
        if(!check(malformed.state()==State::Error && malformed.error()=="Invalid save transfer progress acknowledgement","out-of-range, regressing, premature and wrong-direction progress are rejected"))return false;
    }
    {
        Session receiving;auto& p=*receiving.impl_;p.state=State::Ready;p.role=0;
        LobbyRoster forged(2);forged.Join(1,"Host");forged.Join(2,"Client");
        p.Process(Snapshot(forged),Clock::now());
        if(!check(receiving.state()==State::Error,"client cannot replace authoritative host roster"))return false;
    }
    {
        Session receiving;auto& p=*receiving.impl_;p.state=State::Ready;p.role=1;
        p.Process(LobbyReply{1,LobbyResult::Ok,0},Clock::now());
        if(!check(receiving.state()==State::Error,"unsolicited profile acknowledgements rejected"))return false;
    }
    {
        Session receiving;auto& p=*receiving.impl_;p.state=State::Ready;p.role=0;
        LobbyProfile replay;replay.sequence=2;p.Process(replay,Clock::now());
        if(!check(receiving.state()==State::Error,"out of order profile request rejected"))return false;
    }
    REXLOG_INFO("Save transfer deadline tests passed: progressive 300/400-second transfers, explicit partial-receipt acknowledgement after OS buffering, stalled download and installation despite live heartbeats/repeated byte acknowledgements; invalid progress bounds/direction (injected policy clock, connected loopback transport)");
    const Pad a{0xF3FF,123,255,-32768,32767,-12345,718},b{0x1000,0,91,32767,-32768,999,-999};
    LoadPoint load;load.destination[0]='a';load.tick=identity.first_tick;
    const std::array<Message,16> messages={Hello{identity,0},Input{identity.first_tick,1,b},Frame{identity.first_tick,{a,b}},Ping{17},Pong{17},Digest{identity.first_tick,identity.checkpoint},
        CheckpointBegin{52,identity.checkpoint},CheckpointChunk{0,3,{1,2,3}},CheckpointEnd{52},CheckpointInstalled{identity.checkpoint},CheckpointReady{identity.checkpoint},CheckpointProgress{1024},load,
        LobbyStart::ClientReady,LobbyStart::Start,LobbyStart::Acknowledge};
    {
        LoadExchange exchange;const auto now=Clock::now();
        auto wrong=load;wrong.destination[0]='b';
        if(!check(exchange.Add(1,load,now) && !exchange.Agreed(load) && !exchange.Blocks(load.tick-1) &&
            exchange.Blocks(load.tick) && !exchange.Add(0,wrong,now) && exchange.Add(0,load,now) && exchange.Agreed(load),
            "load entry requires matching destination and allows earlier uncompleted ticks"))return false;
        auto end=load;end.sequence=1;end.phase=LoadPhase::End;end.success=true;
        auto failed=end;failed.success=false;
        if(!check(exchange.Add(0,end,now) && !exchange.Add(1,failed,now) && exchange.Add(1,end,now) &&
            exchange.Agreed(end) && !exchange.Blocks(load.tick) && !exchange.Expired(now+std::chrono::seconds(121)),
            "native load return must agree before verification resumes"))return false;
        LoadExchange nested;
        for(unsigned depth=0;depth<8;++depth){auto p=load;p.sequence=depth;
            if(!check(nested.Add(0,p,now) && nested.Add(1,p,now),"bounded nested native loads agree"))return false;}
        auto overflow=load;overflow.sequence=8;
        if(!check(!nested.Add(0,overflow,now) && nested.Expired(now+std::chrono::seconds(121)),"nested load bound and absolute loading timeout"))return false;
        for(unsigned depth=0;depth<8;++depth){auto p=end;p.sequence=8+depth;
            if(!check(nested.Add(1,p,now) && nested.Add(0,p,now),"nested load stack unwinds in order"))return false;}
        for(unsigned field=0;field<6;++field){auto bad=load;
            if(field==0)bad.destination.fill('x');if(field==1)bad.destination[2]='x';
            if(field==2)bad.destination[0]=char(128);if(field==3)bad.phase=LoadPhase(2);
            if(field==4)bad.success=true;if(field==5)bad.destination.fill(0);
            const auto bytes=Encode(bad);Decoder invalid;
            if(!check(invalid.Append(bytes) && !invalid.Next() && invalid.failed(),"noncanonical load messages rejected by wire decoder"))return false;
        }
    }
    {
        LobbyRoster roster(16);for(uint64_t peer=1;peer<=16;++peer)roster.Join(peer,"Player");
        auto snapshot=Snapshot(roster);const Message message=snapshot;auto bytes=Encode(message);Decoder decoder;
        for(size_t i=0;i<bytes.size();++i){
            if(!check(decoder.Append(std::span(bytes).subspan(i,1)),"maximum roster fragmented append"))return false;
            auto next=decoder.Next();if(!check(i+1==bytes.size()?next==std::optional<Message>(message):!next,"sixteen-entry roster fragmented roundtrip"))return false;
        }
        for(unsigned field:{0u,1u,2u,3u,4u}) {
            auto bad=snapshot;
            if(field==0)bad.players[1].color=bad.players[0].color;
            if(field==1)bad.players[1].peer=bad.players[0].peer;
            if(field==2)bad.players[1].name="bad\nname";
            if(field==3)bad.players[1].character=LobbyCharacter(99);
            if(field==4)bad.locked=true;
            Decoder invalid;invalid.Append(Encode(bad));invalid.Next();if(!check(invalid.failed(),"invalid authoritative roster rejected at decoder"))return false;
        }
        LobbyProfile request;request.sequence=1;request.action=ProfileAction::Ready;request.ready=true;
        for(Message value:{Message(request),Message(LobbyReply{1,LobbyResult::ColorInUse,12})}){
            Decoder d;d.Append(Encode(value));if(!check(d.Next()==std::optional<Message>(value),"profile request and rejection reply codec"))return false;
        }
        request.name[0]='x';Decoder invalid;invalid.Append(Encode(request));invalid.Next();
        if(!check(invalid.failed(),"noncanonical profile fields rejected"))return false;
    }
    unsigned decoded=0;
    {
        auto paused=b;paused.menu_pause=true;
        for(const Message& message:{Message(Input{identity.first_tick,1,paused}),Message(Frame{identity.first_tick,{a,paused}})}){
            const auto bytes=Encode(message);Decoder good;good.Append(bytes);
            if(!check(good.Next()==std::optional<Message>(message),"menu pause request survives the authoritative input/echo codec"))return false;
            auto bad=bytes;bad.back()=2;Decoder invalid;invalid.Append(bad);invalid.Next();
            if(!check(invalid.failed(),"non-boolean menu pause metadata rejected"))return false;
        }
    }
    for(const auto& message:messages) {
        const auto wire=Encode(message);Decoder decoder;
        for(size_t i=0;i<wire.size();++i) {
            if(!check(decoder.Append(std::span(wire).subspan(i,1)),"fragment append")) return false;
            const auto next=decoder.Next();
            if(!check((i+1==wire.size())?next && *next==message:!next,"fragmented packet round trip and signed analog extrema")) return false;
        }
        ++decoded;
    }
    Bytes combined;for(const auto& message:messages) {const auto wire=Encode(message);combined.insert(combined.end(),wire.begin(),wire.end());}
    Decoder coalesced;if(!check(coalesced.Append(combined),"coalesced stream append")) return false;
    for(const auto& message:messages) if(!check(coalesced.Next()==std::optional<Message>(message),"coalesced stream ordering")) return false;
    if(!check(!coalesced.Next() && !coalesced.failed(),"empty decoded stream")) return false;
    for(unsigned field:{0u,5u,6u,7u,11u,12u,15u,25u,26u,27u}) {
        auto wire=Encode(messages[0]);wire[field]=0xFF;Decoder invalid;invalid.Append(wire);invalid.Next();
        if(!check(invalid.failed(),"invalid magic/version/type/reserved/size/role/tick rate rejected")) return false;
    }
    for(auto startup:{StartupPolicy::Prelaunch,StartupPolicy::NativeMenu})for(auto initialization:{InitializationPolicy::Original,InitializationPolicy::SharedNativeV1}){
        auto policy=identity;policy.startup_policy=startup;policy.initialization_policy=initialization;
        Message hello=Hello{policy,0};Decoder decoder;
        if(!check(decoder.Append(Encode(hello)) && decoder.Next()==std::optional<Message>(hello) && !decoder.failed(),
            "startup and initialization policies survive the actual handshake codec"))return false;
    }
    TickBuffer buffer;buffer.Reset(identity.first_tick);Frame frame;
    if(!check(buffer.Add(1,identity.first_tick+1,b)==Added::New && !buffer.Take(frame) &&
        buffer.Add(0,identity.first_tick,a)==Added::New && !buffer.Take(frame) &&
        buffer.Add(1,identity.first_tick,b)==Added::New && buffer.Add(1,identity.first_tick,b)==Added::Duplicate &&
        buffer.Add(1,identity.first_tick,a)==Added::Invalid && buffer.Take(frame) && frame==Frame{identity.first_tick,{a,b}},
        "tick barrier waits for both players, handles future input and rejects conflicting duplicates")) return false;
    if(!check(buffer.Add(0,identity.first_tick,a)==Added::Invalid && buffer.Add(0,buffer.next()+kWindow,a)==Added::Invalid,
        "stale and too-far-future input rejected")) return false;
    buffer.Reset(UINT64_MAX);buffer.Add(0,UINT64_MAX,a);buffer.Add(1,UINT64_MAX,b);
    if(!check(buffer.Take(frame) && !buffer.Take(frame) && buffer.Add(0,0,a)==Added::Invalid,"tick counter never wraps into a previous session")) return false;
    DigestBuffer states;states.Reset(identity.first_tick);
    Digest state_digest{identity.first_tick,identity.checkpoint};auto changed=state_digest;changed.hash[2]^=1;
    if(!check(states.Add(1,state_digest) && !states.verified() && !states.Add(0,changed) &&
        states.Add(0,state_digest) && states.verified()==identity.first_tick,"state verification waits for both matching observations and rejects mismatches")) return false;
    states.Reset(UINT64_MAX);state_digest.tick=UINT64_MAX;
    if(!check(states.Add(0,state_digest) && states.Add(1,state_digest) && states.verified()==UINT64_MAX &&
        !states.Add(0,Digest{0,{}}),"state verification counter cannot wrap")) return false;
    states.Reset(identity.first_tick);
    for(unsigned i=0;i<kWindow;++i) if(!check(states.Add(0,Digest{identity.first_tick+i,identity.checkpoint}),"bounded unverified state window")) return false;
    if(!check(!states.Add(0,Digest{identity.first_tick+kWindow,identity.checkpoint}) && !states.verified() &&
        states.Add(1,Digest{identity.first_tick,identity.checkpoint}) && states.verified()==identity.first_tick &&
        states.Add(0,Digest{identity.first_tick+kWindow,identity.checkpoint}),"verification frees a bounded window cell without skipping earlier ticks")) return false;
    Session host,client;
    if(!check(host.Host(identity,"127.0.0.1") && host.port()!=0 && client.Join(identity,"127.0.0.1",host.port()),"loopback listener and nonblocking connect")) return false;
    auto pump_until=[&](auto predicate) {
        const auto deadline=Clock::now()+std::chrono::seconds(2);
        do {host.Poll();client.Poll();if(predicate()) return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));} while(Clock::now()<deadline);
        REXLOG_ERROR("Loopback states {} / {}, errors '{}' / '{}'",int(host.state()),int(client.state()),host.error(),client.error());return false;
    };
    if(!check(pump_until([&]{return host.state()==State::Ready && client.state()==State::Ready;}),"compatible role/build/content/checkpoint/session handshake")) return false;
    if(!check(pump_until([&]{return client.lobby()==host.lobby() && client.lobby().players.size()==2;}),"real TCP host roster replicates default red Mickey/blue Oswald"))return false;
    if(!check(client.SetLobbyAppearance(LobbyCharacter::Mickey,0) && client.lobby_request_pending() &&
        !client.SetLobbyReady(true) && pump_until([&]{return !client.lobby_request_pending();}) &&
        client.last_lobby_result()==LobbyResult::ColorInUse && client.lobby()==host.lobby() &&
        client.lobby().players[1].color==1,"host rejects occupied color without disconnecting or speculative client mutation"))return false;
    if(!check(client.SetLobbyAppearance(LobbyCharacter::Mickey,2) && pump_until([&]{return !client.lobby_request_pending();}) &&
        client.last_lobby_result()==LobbyResult::Ok && client.lobby()==host.lobby() && client.lobby().players[1].color==2 &&
        client.lobby().players[0].character==LobbyCharacter::Mickey && client.lobby().players[1].character==LobbyCharacter::Mickey,
        "same character accepted with unique green color across real peers"))return false;
    if(!check(client.RenameLobbyPlayer("  Remote player  ") && pump_until([&]{return !client.lobby_request_pending();}) &&
        client.lobby().players[1].name=="Remote player" && host.lobby().players[0].name=="Player 1",
        "client can rename only its assigned identity, host normalizes names"))return false;
    if(!check(client.RenameLobbyPlayer("bad\nname") && pump_until([&]{return !client.lobby_request_pending();}) &&
        client.last_lobby_result()==LobbyResult::InvalidName && host.lobby().players[1].name=="Remote player",
        "invalid name rejected by host without roster corruption"))return false;
    if(!check(host.SetLobbyReady(true) && client.SetLobbyReady(true) && pump_until([&]{return !client.lobby_request_pending() && client.lobby()==host.lobby();}) &&
        host.lobby().players[0].ready && host.lobby().players[1].ready,"ready state replicates in both directions"))return false;
    if(!check(host.SetLobbyAppearance(LobbyCharacter::Oswald,3) && pump_until([&]{return client.lobby()==host.lobby();}) &&
        !host.lobby().players[0].ready && !client.lobby().players[1].ready,"appearance changes invalidate both ready states over TCP"))return false;
    if(!check(!host.SubmitStateDigest(identity.first_tick,identity.checkpoint) && !host.verified_tick(),"unconsumed input is not a completed world tick")) return false;
    for(uint64_t i=0;i<300;++i) {
        const auto tick=identity.first_tick+i;auto local=a;auto remote=b;local.buttons=uint16_t(i);remote.right_x=int16_t(int(i)-150);
        local.menu_pause=i%7==1;remote.menu_pause=i%7==3;
        if(!check(host.SubmitLocal(tick,local) && !host.TakeFrame(frame) && client.SubmitLocal(tick,remote),"local input cannot advance an incomplete tick")) return false;
        Frame authoritative{},received{};bool host_taken=false,client_taken=false;
        if(!check(pump_until([&] {
            if(!host_taken) host_taken=host.TakeFrame(authoritative);
            if(!client_taken) client_taken=client.TakeFrame(received);
            return host_taken && client_taken;
        }) && authoritative==received && received==Frame{tick,{local,remote}},"real TCP authoritative input frames agree for both peers")) return false;
        auto digest=identity.checkpoint;digest[1]=uint8_t(i);digest[2]=uint8_t(i>>8);
        if(i==2){
            auto point=load;point.tick=tick;
            if(!check(host.SubmitLoadPoint(point) && !host.LoadPointAgreed(point) && !host.SubmitStateDigest(tick,digest) &&
                client.SubmitLoadPoint(point) && pump_until([&]{return host.LoadPointAgreed(point) && client.LoadPointAgreed(point);}),
                "real TCP native load entry blocks digest until both peers rendezvous"))return false;
            point.sequence=1;point.phase=LoadPhase::End;point.success=true;
            if(!check(client.SubmitLoadPoint(point) && !client.LoadPointAgreed(point) && host.SubmitLoadPoint(point) &&
                pump_until([&]{return host.LoadPointAgreed(point) && client.LoadPointAgreed(point);}),
                "real TCP native load return rendezvous permits normal completion"))return false;
        }
        if(!check(host.SubmitStateDigest(tick,digest) && client.SubmitStateDigest(tick,digest) &&
            pump_until([&]{return host.verified_tick()==tick && client.verified_tick()==tick;}),"completed state digests agree across the verification ring")) return false;
    }
    client.Disconnect();
    if(!check(pump_until([&]{return host.state()==State::Disconnected || host.state()==State::Error;}) && !host.TakeFrame(frame),"disconnect stops tick consumption")) return false;
    if(!check(host.Host(identity,"::",0) && host.port()!=0 && client.Join(identity,"::1",host.port()),"IPv6 wildcard listener and loopback connect"))return false;
    if(!check(pump_until([&]{return host.state()==State::Ready && client.state()==State::Ready;}),"IPv6 compatible session handshake"))return false;
    for(uint64_t i=0;i<300;++i) {
        const auto tick=identity.first_tick+i;auto local=a;auto remote=b;
        local.buttons=uint16_t(i);remote.right_x=int16_t(int(i)-150);
        if(!check(host.SubmitLocal(tick,local) && client.SubmitLocal(tick,remote),"IPv6 local input submission"))return false;
        Frame authoritative{},received{};bool ht=false,ct=false;
        if(!check(pump_until([&]{
            if(!ht)ht=host.TakeFrame(authoritative);if(!ct)ct=client.TakeFrame(received);return ht && ct;
        }) && authoritative==Frame{tick,{local,remote}} && received==authoritative,"IPv6 authoritative frames match"))return false;
        auto digest=identity.checkpoint;digest[1]=uint8_t(i);digest[2]=uint8_t(i>>8);
        if(!check(host.SubmitStateDigest(tick,digest) && client.SubmitStateDigest(tick,digest) &&
            pump_until([&]{return host.verified_tick()==tick && client.verified_tick()==tick;}),"IPv6 state digest ring wraps and agrees"))return false;
    }
    client.Disconnect();
    if(!check(pump_until([&]{return host.state()==State::Disconnected || host.state()==State::Error;}) && !host.TakeFrame(frame),"IPv6 disconnect stops tick consumption"))return false;
    auto incompatible=identity;incompatible.checkpoint[0]^=1;
    if(!check(host.Host(identity,"127.0.0.1") && client.Join(incompatible,"127.0.0.1",host.port()) &&
        pump_until([&]{return host.state()==State::Error || client.state()==State::Error;}),"mismatched checkpoints cannot synchronize")) return false;
    client.Disconnect();host.Disconnect();identity.session[0]^=1;
    if(!check(host.Host(identity,"127.0.0.1") && client.Join(identity,"127.0.0.1",host.port()) &&
        pump_until([&]{return host.state()==State::Ready && client.state()==State::Ready;}),"fresh-session reconnect")) return false;
    if(!check(!host.verified_tick() && !client.verified_tick(),"reconnect clears previous session verification")) return false;
    host.SubmitLocal(identity.first_tick,a);client.SubmitLocal(identity.first_tick,b);
    bool host_done=false,client_done=false;
    if(!check(pump_until([&]{
        if(!host_done) host_done=host.TakeFrame(frame);
        if(!client_done) client_done=client.TakeFrame(frame);
        return host_done && client_done;
    }),"consume input before divergence test")) return false;
    auto different=identity.checkpoint;different[3]^=1;
    if(!check(host.SubmitStateDigest(identity.first_tick,identity.checkpoint) && client.SubmitStateDigest(identity.first_tick,different) &&
        pump_until([&]{return host.state()==State::Error || client.state()==State::Error;}) &&
        !host.TakeFrame(frame) && !client.TakeFrame(frame),"divergent completed states stop the real TCP input session")) return false;
    host.Disconnect();client.Disconnect();identity.session[0]^=2;
    if(!check(host.Host(identity,"127.0.0.1") && client.Join(identity,"127.0.0.1",host.port()) &&
        pump_until([&]{return host.state()==State::Ready && client.state()==State::Ready;}),"restart after divergence")) return false;
    host.Poll(Clock::now()+std::chrono::seconds(6));
    if(!check(host.state()==State::Error && !host.SubmitLocal(identity.first_tick,a) && !host.TakeFrame(frame),"unresponsive peer pauses the input barrier")) return false;
    // Exercise the session validator through a real socket, rather than only
    // asking the codec whether each individual packet is well formed.
    auto rejected_peer=[&](std::initializer_list<Message> payload,bool handshake=true,std::string_view reason={}) {
        Session victim;TcpChannel peer;
        if(!victim.Host(identity,"127.0.0.1") || !peer.Open(false,"127.0.0.1",victim.port())) return false;
        bool sent=false;const auto deadline=Clock::now()+std::chrono::seconds(2);
        do {
            peer.Pump();victim.Poll();
            if(!sent && peer.status==TcpChannel::Status::Connected) {
                if(handshake && !peer.Queue(Hello{identity,1})) return false;
                for(const auto& message:payload) if(!peer.Queue(message)) return false;
                sent=true;
            }
            if(victim.state()==State::Error) return !victim.TakeFrame(frame) && victim.next_tick()==identity.first_tick && (reason.empty() || victim.error().find(reason)!=std::string::npos);
            if(victim.state()==State::Disconnected || peer.status==TcpChannel::Status::Failed) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while(Clock::now()<deadline);
        return false;
    };
    for(unsigned field=0;field<8;++field) {
        auto wrong=identity;const char* reason=nullptr;
        switch(field) {
        case 0:wrong.build.back()^=1;reason="Different Beetleworx versions";break;
        case 1:wrong.content.back()^=1;reason="Different game content";break;
        case 2:wrong.checkpoint.back()^=1;reason="Different saves";break;
        case 3:wrong.session.back()^=1;reason="different session";break;
        case 4:++wrong.first_tick;reason="Different session timing";break;
        case 5:wrong.input_delay=uint8_t((wrong.input_delay+1)%13);reason="Different session timing";break;
        case 6:wrong.startup_policy=StartupPolicy::NativeMenu;reason="Different online startup paths";break;
        default:wrong.initialization_policy=InitializationPolicy::SharedNativeV1;reason="Different native initialization policies";break;
        }
        if(!check(rejected_peer({Hello{wrong,1}},false,reason),"specific handshake mismatch must explain recovery through real TCP"))return false;
    }
    {
        Session occupied,second;
        if(!check(occupied.Host(identity,"127.0.0.1"),"port-conflict fixture listens"))return false;
        const auto held_port=occupied.port();
        if(!check(!second.Host(identity,"127.0.0.1",held_port) && second.error().find("already in use")!=std::string::npos,
                  "busy port has actionable recovery"))return false;
        occupied.Disconnect();
        if(!check(second.Host(identity,"127.0.0.1",held_port),"failed host attempt releases socket and can retry"))return false;
        second.Disconnect();Session refused;
        const auto opened=refused.Join(identity,"127.0.0.1",held_port);
        const auto deadline=Clock::now()+std::chrono::seconds(8);
        while(opened && refused.state()!=State::Error && Clock::now()<deadline){refused.Poll();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        REXLOG_INFO("Closed-host recovery result: state {}, error {}",int(refused.state()),refused.error());
        if(!check(refused.state()==State::Error && refused.error().find("refused")!=std::string::npos,"closed host has fresh-invitation guidance"))return false;
        if(!check(refused.Host(identity,"127.0.0.1"),"failed join releases socket and permits new hosting"))return false;
    }
    REXLOG_INFO("Online connection recovery passed: six precise identity/timing mismatch reasons over TCP, real port conflict/released-port retry and refused connection/new-host retry; no guest world or Internet peer");
    if(!check(rejected_peer({Input{identity.first_tick,1,b}},false),"pre-handshake input rejected through TCP")) return false;
    if(!check(rejected_peer({Frame{identity.first_tick,{a,b}}}),"client cannot impersonate the authoritative frame sender")) return false;
    if(!check(rejected_peer({Input{identity.first_tick,0,a}}),"client cannot submit host input")) return false;
    if(!check(rejected_peer({Input{identity.first_tick,1,b},Input{identity.first_tick,1,a}}),"conflicting remote input cannot advance simulation")) return false;
    if(!check(rejected_peer({Input{identity.first_tick+kWindow,1,b}}),"remote input window enforced through TCP")) return false;
    if(!check(rejected_peer({Pong{1}}),"unsolicited heartbeat response rejected")) return false;
    if(!check(rejected_peer({Ping{1},Ping{1}}),"repeated heartbeat sequence rejected")) return false;
    if(!check(rejected_peer({Hello{identity,1}}),"duplicate handshake cannot reset an active session")) return false;
    if(!check(rejected_peer({load}),"native load boundary before an authoritative frame is rejected"))return false;
    if(!check(rejected_peer({Digest{identity.first_tick,identity.checkpoint}},false),"state digest before handshake rejected")) return false;
    if(!check(rejected_peer({Digest{identity.first_tick+1,identity.checkpoint}}),"state digest cannot skip completion ticks")) return false;
    auto changed_hash=identity.checkpoint;changed_hash[4]^=1;
    if(!check(rejected_peer({Digest{identity.first_tick,identity.checkpoint},Digest{identity.first_tick,changed_hash}}),"peer cannot rewrite a completed-state report")) return false;
    Session invalid_endpoint;
    const std::string embedded_null("127.0.0.1\0extra",15);
    if(!check(!invalid_endpoint.Host(identity,embedded_null) && invalid_endpoint.port()==0,"embedded-null address rejected without opening a listener")) return false;
    REXLOG_INFO("Online input tests passed: {} wire round trips, fragmented/coalesced/malformed streams, bounded input/state queues, 300 IPv4 and 300 IPv6 real loopback frames and matching completed-state digests, deliberate state divergence, handshake mismatch, reconnect and timeout",decoded);
    return true;
}
}
