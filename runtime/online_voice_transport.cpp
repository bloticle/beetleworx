#include "online_address.h"
#include "online_voice_transport.h"
#include <bcrypt.h>
#include <windows.h>
#include <rex/logging.h>
#include <deque>
#include <limits>
#include <span>
#include <thread>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace em2::net {
VoiceKeys::~VoiceKeys(){SecureZeroMemory(to_host.data(),to_host.size());SecureZeroMemory(to_guest.data(),to_guest.size());}
namespace {
using Bytes=std::vector<uint8_t>;
constexpr size_t Header=16,Tag=16,MaxDatagram=Header+Tag+11+kVoicePacketBytes;
enum class Kind:uint8_t {Hello=1,Ack,Audio};
void Put(Bytes& data,uint64_t value,unsigned count){for(unsigned i=count;i;--i)data.push_back(uint8_t(value>>(8*(i-1))));}
uint64_t Read(std::span<const uint8_t> data,size_t at,unsigned count){uint64_t value=0;for(unsigned i=0;i<count;++i)value=(value<<8)|data[at+i];return value;}
bool Nonzero(std::span<const uint8_t> data){return std::any_of(data.begin(),data.end(),[](auto x){return x!=0;});}
bool SameIp(const NumericAddress& a,const NumericAddress& b){return a.storage.ss_family==b.storage.ss_family && std::equal(a.bytes(),a.bytes()+(a.ipv6()?16:4),b.bytes());}
bool SameEndpoint(const NumericAddress& a,const NumericAddress& b){return SameIp(a,b) && a.Port()==b.Port();}
void Close(SOCKET& value){if(value!=INVALID_SOCKET){closesocket(value);value=INVALID_SOCKET;}}
bool Winsock(){struct Init{bool good=false;Init(){WSADATA d{};good=WSAStartup(MAKEWORD(2,2),&d)==0;}~Init(){if(good)WSACleanup();}};static Init init;return init.good;}
struct Replay {
    uint64_t high=0,bits=0;
    bool Allows(uint64_t n)const{return n && (n>high || (high-n<64 && !(bits&(uint64_t(1)<<(high-n)))));}
    void Commit(uint64_t n){if(n>high){const auto delta=n-high;bits=delta>=64?1:(bits<<delta)|1;high=n;}else bits|=uint64_t(1)<<(high-n);}
};
struct Rate {
    VoiceTime at{};double tokens=12;bool initialized=false;
    bool Take(VoiceTime now){if(!initialized){at=now;initialized=true;}if(now<at)return false;tokens=std::min(12.,tokens+std::chrono::duration<double>(now-at).count()*50);at=now;if(tokens<1)return false;--tokens;return true;}
};
struct Cipher {
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_KEY_HANDLE key=nullptr;Bytes object;
    ~Cipher(){if(key)BCryptDestroyKey(key);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);if(!object.empty())SecureZeroMemory(object.data(),object.size());}
    bool Initialize(const std::array<uint8_t,32>& material){
        if(!Nonzero(material) || BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_AES_ALGORITHM,nullptr,0)<0 ||
           BCryptSetProperty(algorithm,BCRYPT_CHAINING_MODE,reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),sizeof(BCRYPT_CHAIN_MODE_GCM),0)<0)return false;
        DWORD size=0,count=0;if(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&count,0)<0 || !size || size>65536)return false;
        object.resize(size);return BCryptGenerateSymmetricKey(algorithm,&key,object.data(),ULONG(object.size()),const_cast<PUCHAR>(material.data()),ULONG(material.size()),0)>=0;
    }
    bool Run(bool encrypt,std::span<const uint8_t> header,const std::array<uint8_t,16>& session,std::span<const uint8_t> input,std::span<uint8_t> output,std::span<uint8_t> tag){
        std::array<uint8_t,12> nonce{};nonce[0]=1;nonce[1]=header[7];nonce[2]=header[6];std::copy_n(header.data()+8,8,nonce.data()+4);
        std::array<uint8_t,Header+16> aad{};std::copy(header.begin(),header.end(),aad.begin());std::copy(session.begin(),session.end(),aad.begin()+Header);
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;BCRYPT_INIT_AUTH_MODE_INFO(info);info.pbNonce=nonce.data();info.cbNonce=ULONG(nonce.size());info.pbAuthData=aad.data();info.cbAuthData=ULONG(aad.size());info.pbTag=tag.data();info.cbTag=ULONG(tag.size());
        ULONG count=0;auto in=input.empty()?nullptr:const_cast<PUCHAR>(input.data());auto out=output.empty()?nullptr:output.data();
        const auto status=encrypt?BCryptEncrypt(key,in,ULONG(input.size()),&info,nullptr,0,out,ULONG(output.size()),&count,0):BCryptDecrypt(key,in,ULONG(input.size()),&info,nullptr,0,out,ULONG(output.size()),&count,0);
        return status>=0 && count==input.size();
    }
};
struct Link {
    Cipher send,receive;NumericAddress expected,endpoint;bool ready=false;Replay replay;
    VoiceTime hello_received{};
    uint64_t counter=0;
    bool Seal(Kind kind,unsigned peer,bool host,const std::array<uint8_t,16>& session,std::span<const uint8_t> plain,Bytes& out){
        if(counter==UINT64_MAX || plain.size()>11+kVoicePacketBytes)return false;
        out={'B','W','V','C',1,uint8_t(kind),uint8_t(peer),uint8_t(host)};Put(out,++counter,8);out.resize(Header+plain.size()+Tag);
        return send.Run(true,std::span(out).first(Header),session,plain,std::span(out).subspan(Header,plain.size()),std::span(out).last(Tag));
    }
    bool Open(std::span<const uint8_t> packet,const std::array<uint8_t,16>& session,Bytes& out){
        const auto counter=Read(packet,8,8);if(!replay.Allows(counter))return false;
        out.resize(packet.size()-Header-Tag);std::array<uint8_t,Tag> tag;std::copy_n(packet.end()-Tag,Tag,tag.begin());
        return receive.Run(false,packet.first(Header),session,packet.subspan(Header,out.size()),out,tag);
    }
};
bool ValidAudio(const VoicePacket& p){return p.size && p.size<=p.data.size() && p.sequence<UINT64_MAX-8;}
Bytes Audio(unsigned peer,const VoicePacket& p){Bytes out;out.reserve(p.size+11);out.push_back(uint8_t(peer));Put(out,p.sequence,8);Put(out,p.size,2);out.insert(out.end(),p.data.begin(),p.data.begin()+p.size);return out;}
bool Audio(std::span<const uint8_t> data,unsigned& peer,VoicePacket& p){
    if(data.size()<11)return false;peer=data[0];p.sequence=Read(data,1,8);p.size=uint16_t(Read(data,9,2));
    if(!peer || peer>16 || !ValidAudio(p) || data.size()!=11+p.size)return false;std::copy(data.begin()+11,data.end(),p.data.begin());return true;
}
}
struct VoiceTransport::Impl {
    SOCKET socket=INVALID_SOCKET;bool host=false;unsigned local=0;uint16_t bound=0,roster=0;
    std::array<uint8_t,16> session{};std::array<std::unique_ptr<Link>,16> links;
    std::array<Rate,16> media_rate;
    std::array<std::deque<VoicePacket>,16> incoming;VoiceTransportStats stats;Rate local_rate;VoiceTime hello_at{};
    ~Impl(){Close(socket);}
    bool Bind(NumericAddress address){
        if(!Winsock() || socket!=INVALID_SOCKET)return false;address.Port(0);socket=::socket(address.storage.ss_family,SOCK_DGRAM,IPPROTO_UDP);if(socket==INVALID_SOCKET)return false;
        u_long one=1;if(ioctlsocket(socket,FIONBIO,&one)!=0 || bind(socket,reinterpret_cast<const sockaddr*>(&address.storage),address.size)!=0){Close(socket);return false;}
        int size=sizeof(address.storage);if(getsockname(socket,reinterpret_cast<sockaddr*>(&address.storage),&size)!=0){Close(socket);return false;}bound=address.Port();return bound!=0;
    }
    bool Send(Link& link,unsigned route,Kind kind,std::span<const uint8_t> plain){
        Bytes packet;if(!link.Seal(kind,route,host,session,plain,packet))return false;
        const auto n=sendto(socket,reinterpret_cast<const char*>(packet.data()),int(packet.size()),0,reinterpret_cast<const sockaddr*>(&link.endpoint.storage),link.endpoint.size);
        if(n!=int(packet.size())){++stats.dropped;return false;}++stats.sent;return true;
    }
    void Remember(unsigned peer,const VoicePacket& p){auto& q=incoming[peer-1];if(q.size()==8){q.pop_front();++stats.dropped;}q.push_back(p);}
    void Relay(unsigned speaker,const VoicePacket& packet){
        const auto body=Audio(speaker,packet);
        for(unsigned peer=2;peer<=16;++peer)if(peer!=speaker && links[peer-1] && links[peer-1]->ready && (roster&(1u<<(peer-1))))Send(*links[peer-1],peer,Kind::Audio,body);
    }
};
VoiceTransport::VoiceTransport():impl_(std::make_unique<Impl>()){}
VoiceTransport::~VoiceTransport()=default;
bool VoiceTransport::Host(std::string_view address,const std::array<uint8_t,16>& session){
    auto& s=*impl_;const auto endpoint=ParseNumericAddress(address,true);if(!endpoint || !Nonzero(session) || !s.Bind(*endpoint))return false;
    s.host=true;s.local=1;s.session=session;s.roster=1;return true;
}
bool VoiceTransport::AddPeer(unsigned peer,std::string_view address,VoiceKeys& offer){
    auto& s=*impl_;const auto endpoint=ParseNumericAddress(address);if(!s.host || s.socket==INVALID_SOCKET || peer<2 || peer>16 || s.links[peer-1] || !endpoint)return false;
    VoiceKeys keys;if(BCryptGenRandom(nullptr,keys.to_host.data(),ULONG(keys.to_host.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0 || BCryptGenRandom(nullptr,keys.to_guest.data(),ULONG(keys.to_guest.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return false;
    auto link=std::make_unique<Link>();if(!link->send.Initialize(keys.to_guest) || !link->receive.Initialize(keys.to_host))return false;link->expected=*endpoint;
    s.links[peer-1]=std::move(link);offer=keys;return true;
}
bool VoiceTransport::Join(std::string_view address,uint16_t port,unsigned peer,const std::array<uint8_t,16>& session,const VoiceKeys& keys){
    auto& s=*impl_;auto endpoint=ParseNumericAddress(address);if(!endpoint || !port || peer<2 || peer>16 || !Nonzero(session))return false;
    auto bind=ParseNumericAddress(endpoint->ipv6()?"::":"0.0.0.0",true);if(!bind || !s.Bind(*bind))return false;
    auto link=std::make_unique<Link>();if(!link->send.Initialize(keys.to_host) || !link->receive.Initialize(keys.to_guest)){Close(s.socket);return false;}
    endpoint->Port(port);link->endpoint=link->expected=*endpoint;s.links[0]=std::move(link);s.local=peer;s.session=session;return true;
}
void VoiceTransport::RemovePeer(unsigned peer){if(peer>=2 && peer<=16){impl_->links[peer-1].reset();impl_->incoming[peer-1].clear();}}
void VoiceTransport::SetRoster(uint16_t mask){auto& s=*impl_;s.roster=mask;for(unsigned peer=1;peer<=16;++peer)if(!(mask&(1u<<(peer-1))))s.incoming[peer-1].clear();}
bool VoiceTransport::ready()const{return impl_->socket!=INVALID_SOCKET && (impl_->host || (impl_->links[0] && impl_->links[0]->ready));}
bool VoiceTransport::peer_ready(unsigned peer)const{return peer>=2 && peer<=16 && impl_->host && impl_->links[peer-1] && impl_->links[peer-1]->ready;}
uint16_t VoiceTransport::port()const{return impl_->bound;}
VoiceTransportStats VoiceTransport::stats()const{return impl_->stats;}
bool VoiceTransport::Send(const VoicePacket& packet,VoiceTime now){
    auto& s=*impl_;if(!ready() || !ValidAudio(packet) || !s.local_rate.Take(now))return false;
    if(s.host){s.Relay(1,packet);return true;}return s.Send(*s.links[0],s.local,Kind::Audio,Audio(s.local,packet));
}
void VoiceTransport::Poll(VoiceTime now){
    auto& s=*impl_;if(s.socket==INVALID_SOCKET)return;
    if(!s.host && s.links[0] && (s.hello_at==VoiceTime{} || now-s.hello_at>std::chrono::seconds(1))){s.hello_at=now;s.Send(*s.links[0],s.local,Kind::Hello,{});}
    for(unsigned work=0;work<64;++work){
        std::array<uint8_t,MaxDatagram+1> bytes{};NumericAddress from;from.size=sizeof(from.storage);
        const auto size=recvfrom(s.socket,reinterpret_cast<char*>(bytes.data()),int(bytes.size()),0,reinterpret_cast<sockaddr*>(&from.storage),&from.size);
        if(size==SOCKET_ERROR){if(WSAGetLastError()==WSAEWOULDBLOCK)break;++s.stats.rejected;continue;}
        ++s.stats.received;
        if(size<int(Header+Tag) || size>MaxDatagram || std::memcmp(bytes.data(),"BWVC",4) || bytes[4]!=1 || bytes[7]!=(s.host?0:1)){++s.stats.rejected;continue;}
        const auto route=unsigned(bytes[6]);const auto kind=Kind(bytes[5]);
        if(route<2 || route>16 || !(s.roster&(1u<<(route-1))) || (!s.host && route!=s.local) || (kind!=Kind::Hello && kind!=Kind::Ack && kind!=Kind::Audio)){++s.stats.rejected;continue;}
        auto* link=s.host?s.links[route-1].get():s.links[0].get();
        if(!link || !SameIp(from,link->expected) || (!s.host && !SameEndpoint(from,link->expected))){++s.stats.rejected;continue;}
        Bytes plain;const auto packet=std::span(bytes).first(size);
        if(!link->Open(packet,s.session,plain)){++s.stats.rejected;continue;}
        if(kind==Kind::Hello && s.host && plain.empty()){
            if(Read(packet,8,8)<=link->replay.high || (link->hello_received!=VoiceTime{} && now-link->hello_received<std::chrono::milliseconds(200))){++s.stats.rejected;continue;}
            link->hello_received=now;
            link->replay.Commit(Read(packet,8,8));link->endpoint=from;link->ready=true;s.Send(*link,route,Kind::Ack,{});continue;
        }
        if(kind==Kind::Ack && !s.host && plain.empty()){link->replay.Commit(Read(packet,8,8));link->ready=true;continue;}
        unsigned speaker=0;VoicePacket audio;
        if(kind!=Kind::Audio || !link->ready || !SameEndpoint(from,link->endpoint) || !Audio(plain,speaker,audio) ||
           !(s.roster&(1u<<(speaker-1))) || speaker==s.local || (s.host && speaker!=route)){++s.stats.rejected;continue;}
        link->replay.Commit(Read(packet,8,8));if(!s.media_rate[speaker-1].Take(now)){++s.stats.dropped;continue;}
        s.Remember(speaker,audio);if(s.host)s.Relay(speaker,audio);
    }
}
std::vector<ReceivedVoice> VoiceTransport::TakeReceived(){
    std::vector<ReceivedVoice> result;for(unsigned peer=1;peer<=16;++peer){auto& q=impl_->incoming[peer-1];for(auto& p:q)result.push_back({peer,p});q.clear();}return result;
}
bool TestOnlineVoiceTransport(){
    bool ok=true;unsigned checks=0;auto check=[&](bool v,const char* why){++checks;if(!v){ok=false;REXLOG_ERROR("Voice UDP regression: {}",why);}return v;};
    Replay window;check(window.Allows(1) && !window.Allows(0),"nonzero first counter");window.Commit(70);
    check(window.Allows(69) && !window.Allows(6),"bounded reorder window");window.Commit(69);check(!window.Allows(69) && !window.Allows(70),"duplicate counter rejection");
    std::array<uint8_t,16> session{};session[0]=41;
    std::vector<std::unique_ptr<VoiceTransport>> channels;for(unsigned i=0;i<16;++i)channels.push_back(std::make_unique<VoiceTransport>());
    if(!check(channels[0]->Host("127.0.0.1",session),"loopback UDP host"))return false;
    channels[0]->SetRoster(0xFFFF);
    for(unsigned peer=2;peer<=16;++peer){VoiceKeys keys;if(!check(channels[0]->AddPeer(peer,"127.0.0.1",keys) && channels[peer-1]->Join("127.0.0.1",channels[0]->port(),peer,session,keys),"independent random duplex keys and actual guest UDP socket"))return false;channels[peer-1]->SetRoster(0xFFFF);}
    auto pump=[&](auto done){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);do{for(auto& c:channels)c->Poll();if(done())return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(std::chrono::steady_clock::now()<end);return false;};
    if(!check(pump([&]{return std::all_of(channels.begin(),channels.end(),[](auto& c){return c->ready();});}),"sixteen encrypted hello/ack handshakes"))return false;
    std::array<VoicePacket,16> speech;
    for(unsigned peer=1;peer<=16;++peer){VoiceEncoder encoder;VoicePcm pcm;for(unsigned i=0;i<pcm.size();++i)pcm[i]=float(.15*std::sin(double(i)*(200+peer*20)*6.283185307179586/kVoiceRate));check(encoder.Encode(1000+peer,pcm,speech[peer-1]) && channels[peer-1]->Send(speech[peer-1]),"actual encoded source sends encrypted voice");}
    auto queued=[](const auto& channel){size_t count=0;for(const auto& q:channel.impl_->incoming)count+=q.size();return count;};
    if(!check(pump([&]{return std::all_of(channels.begin(),channels.end(),[&](const auto& c){return queued(*c)==15;});}),"host relays all fifteen other speakers to every participant"))return false;
    for(unsigned peer=1;peer<=16;++peer){
        const auto received=channels[peer-1]->TakeReceived();std::array<bool,16> seen{};VoiceMixer mixer;const auto now=std::chrono::steady_clock::now();
        for(const auto& m:received){check(m.peer>=1 && m.peer<=16 && m.peer!=peer && !seen[m.peer-1],"host-authorized unique sender and no self echo");seen[m.peer-1]=true;const auto& expected=speech[m.peer-1];check(m.packet.sequence==expected.sequence && m.packet.size==expected.size && m.packet.data==expected.data,"encrypted relay preserves actual Opus bytes");check(mixer.Submit(m.peer,m.packet,now),"network packet enters real decoder queue");}
        std::array<float,kVoiceSamples*2> pcm;mixer.Render(now+std::chrono::milliseconds(60),pcm);double energy=0;for(auto x:pcm)energy+=double(x)*x;
        check(energy>.001 && std::all_of(pcm.begin(),pcm.end(),[](float x){return std::isfinite(x) && std::abs(x)<=1;}),"received encoded audio produces bounded non-silent mixed PCM");
    }
    auto& guest=*channels[1]->impl_;auto& host=*channels[0]->impl_;auto& link=*guest.links[0];
    auto raw=[&](const Bytes& bytes){return sendto(guest.socket,reinterpret_cast<const char*>(bytes.data()),int(bytes.size()),0,reinterpret_cast<const sockaddr*>(&link.endpoint.storage),link.endpoint.size)==int(bytes.size());};
    auto settle=[&]{for(unsigned i=0;i<8;++i){for(auto& c:channels)c->Poll();std::this_thread::sleep_for(std::chrono::milliseconds(1));}};
    const auto rejected=host.stats.rejected;
    Bytes spoof;check(link.Seal(Kind::Audio,2,false,session,Audio(3,speech[1]),spoof) && raw(spoof),"spoofed payload sent with sender's own valid key");
    check(link.Seal(Kind::Audio,3,false,session,Audio(3,speech[1]),spoof) && raw(spoof),"spoofed route sent with wrong peer key");
    auto other_session=session;other_session[1]=1;check(link.Seal(Kind::Audio,2,false,other_session,Audio(2,speech[1]),spoof) && raw(spoof),"cross-session authentication packet sent");
    Bytes valid;check(link.Seal(Kind::Audio,2,false,session,Audio(2,speech[1]),valid),"valid comparison packet sealed");auto damaged=valid;damaged.back()^=1;check(raw(damaged),"tampered authentication tag sent");
    check(raw(Bytes{1,2,3}) && raw(Bytes(MaxDatagram+30,1)),"short/oversized datagrams sent");settle();
    check(host.stats.rejected>=rejected+6 && queued(*channels[0])==0,"spoof/session/tag/length failures do not enter playback");
    check(raw(valid),"valid original after tamper sent");settle();check(queued(*channels[0])==1,"unauthenticated counter did not poison replay window");
    check(raw(valid),"exact encrypted replay sent");settle();check(queued(*channels[0])==1,"replay cannot duplicate delivered audio");
    for(auto& c:channels)c->TakeReceived();
    std::array<Bytes,3> reordered;
    for(unsigned i=0;i<3;++i){auto p=speech[1];p.sequence=2000+i;check(link.Seal(Kind::Audio,2,false,session,Audio(2,p),reordered[i]),"reorder packet sealed");}
    check(raw(reordered[2]) && raw(reordered[0]) && raw(reordered[1]),"actual UDP receives deliberately reordered counters");settle();
    const auto reorder=channels[0]->TakeReceived();check(reorder.size()==3 && reorder[0].packet.sequence==2002 && reorder[1].packet.sequence==2000 && reorder[2].packet.sequence==2001,"authenticated reorder retained for jitter buffer");
    const auto policy_now=std::chrono::steady_clock::now()+std::chrono::seconds(1);unsigned accepted=0;
    for(unsigned i=0;i<30;++i)if(channels[1]->Send(speech[1],policy_now))++accepted;check(accepted==12,"outgoing burst is bounded independently of game inputs");
    for(unsigned i=0;i<24;++i){const auto now=policy_now+std::chrono::milliseconds(i*20+20);auto packet=speech[1];packet.sequence=3000+i;check(channels[1]->Send(packet,now),"continuous rate-limited voice frame");for(auto& c:channels)c->Poll(now);}
    check(host.incoming[1].size()<=8 && host.stats.dropped>0,"slow consumer drops old voice within fixed queue budget");
    channels[0]->RemovePeer(2);check(!channels[0]->peer_ready(2) && host.incoming[1].empty(),"departed peer keys, endpoint and buffered speech retired");
    check(raw(valid),"departed peer replay attempted");settle();check(host.incoming[1].empty(),"old peer cannot speak after removal");
    VoiceKeys replacement;auto fresh=std::make_unique<VoiceTransport>();check(channels[0]->AddPeer(2,"127.0.0.1",replacement) && fresh->Join("127.0.0.1",channels[0]->port(),2,session,replacement),"replacement receives fresh keys");fresh->SetRoster(0xFFFF);
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);while(!fresh->ready() && std::chrono::steady_clock::now()<end){fresh->Poll();channels[0]->Poll();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    check(fresh->ready(),"replacement endpoint authenticates independently");
    check(raw(valid),"old key replay after replacement attempted");settle();check(host.incoming[1].empty(),"replaced key cannot authenticate");
    REXLOG_INFO("Voice UDP {} {} checks: sixteen real encrypted sockets, host relay, actual Opus-to-PCM, sender/session/tag/replay rejection, reorder/rate/backlog/removal/rekey. No Room/native-gameplay or Internet relay assertion",ok?"passed":"FAILED",checks);
    return ok;
}
}
