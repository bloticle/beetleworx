#include "online_address.h"
#include "online_discovery.h"
#include <iphlpapi.h>
#include <mswsock.h>
#include <rex/logging.h>
#include <thread>
#include <span>
#include <cstddef>

namespace em2::net {
namespace {
using Clock=std::chrono::steady_clock;
using Time=Clock::time_point;
using Packet=std::vector<uint8_t>;
struct Subnet {uint32_t address,mask;};
std::vector<Subnet> Subnets() {
    ULONG bytes=16384,result=ERROR_BUFFER_OVERFLOW;std::vector<uint8_t> storage;
    for(unsigned i=0;i<3 && result==ERROR_BUFFER_OVERFLOW;++i) {
        if(bytes>1024*1024)return {};
        storage.resize(bytes);result=GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,
            nullptr,reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data()),&bytes);
    }
    std::vector<Subnet> found;if(result!=NO_ERROR)return found;
    for(auto* adapter=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());adapter;adapter=adapter->Next) {
        if(adapter->OperStatus!=IfOperStatusUp || adapter->IfType==IF_TYPE_SOFTWARE_LOOPBACK || adapter->IfType==IF_TYPE_TUNNEL)continue;
        for(auto* item=adapter->FirstUnicastAddress;item;item=item->Next) {
            if(item->Length<offsetof(IP_ADAPTER_UNICAST_ADDRESS,OnLinkPrefixLength)+sizeof(item->OnLinkPrefixLength) ||
                !item->Address.lpSockaddr || item->Address.iSockaddrLength<sizeof(sockaddr_in) || item->Address.lpSockaddr->sa_family!=AF_INET ||
                item->DadState!=IpDadStatePreferred || !item->OnLinkPrefixLength || item->OnLinkPrefixLength>30)continue;
            const auto ip=ntohl(reinterpret_cast<sockaddr_in*>(item->Address.lpSockaddr)->sin_addr.s_addr);
            if(!ip || ip>>24>=224 || ip>>24==127)continue;
            found.push_back({ip,0xFFFFFFFFu<<(32-item->OnLinkPrefixLength)});
            if(found.size()==32)return found;
        }
    }
    return found;
}
bool OnLink(uint32_t address,const std::vector<Subnet>& subnets) {
    if(address>>24==127)return true;
    if(!address || address>>24>=224)return false;
    return std::any_of(subnets.begin(),subnets.end(),[&](auto subnet){return (address&subnet.mask)==(subnet.address&subnet.mask) &&
        address!=(subnet.address&subnet.mask) && address!=((subnet.address&subnet.mask)|~subnet.mask);});
}
Packet Header(uint8_t kind,const std::array<uint8_t,16>& nonce) {
    Packet packet={'E','M','2','L','A','N',1,kind};packet.insert(packet.end(),nonce.begin(),nonce.end());return packet;
}
bool ValidHeader(std::span<const uint8_t> packet,uint8_t kind) {
    constexpr std::array<uint8_t,7> prefix={'E','M','2','L','A','N',1};
    return packet.size()>=24 && std::equal(prefix.begin(),prefix.end(),packet.begin()) && packet[7]==kind;
}
struct Socket {
    SOCKET value=INVALID_SOCKET;int startup=0;uint16_t port=0;std::string error;
    Socket(){WSADATA data{};startup=WSAStartup(MAKEWORD(2,2),&data);}
    ~Socket(){Close();if(!startup)WSACleanup();}
    void Close(){if(value!=INVALID_SOCKET)closesocket(value);value=INVALID_SOCKET;port=0;}
    bool Fail(const char* message){error=std::string(message)+" (Win32 "+std::to_string(WSAGetLastError())+")";Close();return false;}
    bool Open(std::string_view bind_address,uint16_t requested,bool broadcast) {
        Close();error.clear();if(startup){error="Winsock unavailable";return false;}
        auto address=ParseNumericAddress(bind_address,true);if(!address || address->ipv6()){error="LAN discovery needs IPv4";return false;}
        address->Port(requested);value=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(value==INVALID_SOCKET)return Fail("UDP socket");
        u_long nonblocking=1;BOOL enabled=TRUE,reset=FALSE;DWORD returned=0;int ttl=1;
        if(ioctlsocket(value,FIONBIO,&nonblocking) ||
            setsockopt(value,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&enabled),sizeof(enabled)) ||
            setsockopt(value,IPPROTO_IP,IP_TTL,reinterpret_cast<const char*>(&ttl),sizeof(ttl)) ||
            (broadcast && setsockopt(value,SOL_SOCKET,SO_BROADCAST,reinterpret_cast<const char*>(&enabled),sizeof(enabled))) ||
            WSAIoctl(value,SIO_UDP_CONNRESET,&reset,sizeof(reset),nullptr,0,&returned,nullptr,nullptr) ||
            bind(value,reinterpret_cast<sockaddr*>(&address->storage),address->size))return Fail("LAN socket setup/bind");
        int size=address->size;if(getsockname(value,reinterpret_cast<sockaddr*>(&address->storage),&size))return Fail("LAN socket address");
        port=address->Port();return true;
    }
    bool Send(std::span<const uint8_t> packet,const sockaddr_in& destination) {
        const auto sent=sendto(value,reinterpret_cast<const char*>(packet.data()),int(packet.size()),0,
            reinterpret_cast<const sockaddr*>(&destination),sizeof(destination));
        return sent==int(packet.size());
    }
    int Receive(std::array<uint8_t,129>& packet,sockaddr_in& source) {
        int size=sizeof(source);return recvfrom(value,reinterpret_cast<char*>(packet.data()),int(packet.size()),0,
            reinterpret_cast<sockaddr*>(&source),&size);
    }
};
std::optional<LANGame> DecodeResponse(std::span<const uint8_t> bytes,const std::array<uint8_t,16>& nonce,
    const sockaddr_in& source,const std::vector<Subnet>& subnets) {
    if(bytes.size()!=86 || !ValidHeader(bytes,2) || !std::equal(nonce.begin(),nonce.end(),bytes.begin()+8) ||
        !OnLink(ntohl(source.sin_addr.s_addr),subnets))return {};
    auto invite=DecodeInvite(std::string_view(reinterpret_cast<const char*>(bytes.data()+24),62));
    if(!invite || ParseNumericAddress(invite->address)->ipv6())return {};
    // Advertised Internet addresses need not work through LAN NAT hairpinning.
    // Route a discovered connection to the actual response's on-link source.
    NumericAddress address;address.size=sizeof(source);std::memcpy(&address.storage,&source,sizeof(source));
    invite->address=address.Text();const auto code=EncodeInvite(*invite);if(!code)return {};
    return LANGame{*code,invite->address,invite->port};
}
}
std::vector<LANDiscoveryTarget> LocalLANDiscoveryTargets() {
    std::vector<LANDiscoveryTarget> result;
    for(auto subnet:Subnets()) {
        NumericAddress numeric;numeric.size=sizeof(sockaddr_in);
        auto& address=*reinterpret_cast<sockaddr_in*>(&numeric.storage);address.sin_family=AF_INET;
        address.sin_addr.s_addr=htonl(subnet.address|~subnet.mask);const auto text=numeric.Text();
        if(std::none_of(result.begin(),result.end(),[&](const auto& prior){return prior.address==text;}))result.push_back({text,kLANDiscoveryPort});
    }
    return result;
}
struct LANAdvertiser::Impl {
    Socket socket;std::string invitation;std::vector<Subnet> subnets;
    struct Reply {uint32_t address=0;Time sent{};};std::array<Reply,32> replies{};
    Time window{};unsigned sent=0;
};
LANAdvertiser::LANAdvertiser():impl_(std::make_unique<Impl>()){}
LANAdvertiser::~LANAdvertiser()=default;
bool LANAdvertiser::Start(const Invite& invite,std::string_view bind_address,uint16_t port) {
    Stop();const auto code=EncodeInvite(invite);
    if(!code || code->size()!=62){impl_->socket.error="LAN discovery advertises IPv4 hosts only";return false;}
    if(!impl_->socket.Open(bind_address,port,false))return false;
    impl_->invitation=*code;impl_->subnets=Subnets();impl_->replies={};impl_->sent=0;impl_->window={};return true;
}
void LANAdvertiser::Stop(){impl_->socket.Close();impl_->invitation.clear();}
uint16_t LANAdvertiser::port()const{return impl_->socket.port;}
const std::string& LANAdvertiser::error()const{return impl_->socket.error;}
void LANAdvertiser::Poll(Time now) {
    auto& p=*impl_;if(p.socket.value==INVALID_SOCKET)return;
    if(now-p.window>=std::chrono::seconds(1)){p.window=now;p.sent=0;}
    for(unsigned i=0;i<32;++i) {
        std::array<uint8_t,129> bytes{};sockaddr_in source{};const auto size=p.socket.Receive(bytes,source);
        if(size==SOCKET_ERROR){const auto error=WSAGetLastError();if(error==WSAEWOULDBLOCK)break;if(error==WSAEMSGSIZE || error==WSAECONNRESET)continue;p.socket.Fail("LAN receive");break;}
        const auto ip=ntohl(source.sin_addr.s_addr);
        if(size!=24 || !ValidHeader(std::span(bytes).first(size),1) || !OnLink(ip,p.subnets) || p.sent>=16)continue;
        auto entry=std::find_if(p.replies.begin(),p.replies.end(),[&](const auto& reply){return reply.address==ip;});
        if(entry!=p.replies.end() && now-entry->sent<std::chrono::milliseconds(250))continue;
        if(entry==p.replies.end())entry=std::min_element(p.replies.begin(),p.replies.end(),[](auto a,auto b){return a.sent<b.sent;});
        Packet response(bytes.begin(),bytes.begin()+24);response[7]=2;response.insert(response.end(),p.invitation.begin(),p.invitation.end());
        if(p.socket.Send(response,source)){*entry={ip,now};++p.sent;}
    }
}
struct LANBrowser::Impl {
    Socket socket;std::vector<sockaddr_in> targets;std::vector<Subnet> subnets;
    std::array<uint8_t,16> nonce{};std::vector<LANGame> games;std::vector<Time> seen;
    Time end{},next{};bool active=false,sent=false;
};
LANBrowser::LANBrowser():impl_(std::make_unique<Impl>()){}
LANBrowser::~LANBrowser()=default;
void LANBrowser::Stop(){impl_->socket.Close();impl_->active=false;impl_->games.clear();impl_->seen.clear();}
bool LANBrowser::Start(const std::vector<LANDiscoveryTarget>& targets,Time now) {
    Stop();auto& p=*impl_;p.targets.clear();
    if(targets.empty() || targets.size()>32){p.socket.error="No usable IPv4 LAN interfaces";return false;}
    for(const auto& target:targets) {
        const auto address=ParseNumericAddress(target.address);
        if(!address || address->ipv6() || !target.port){p.socket.error="Invalid LAN scan target";return false;}
        auto destination=*reinterpret_cast<const sockaddr_in*>(&address->storage);destination.sin_port=htons(target.port);p.targets.push_back(destination);
    }
    if(!NewSessionNonce(p.nonce)){p.socket.error="Could not initialize LAN scan";return false;}
    if(!p.socket.Open("0.0.0.0",0,true))return false;
    p.subnets=Subnets();p.end=now+std::chrono::seconds(3);p.next=now;p.active=true;p.sent=false;return true;
}
bool LANBrowser::scanning()const{return impl_->active;}
const std::vector<LANGame>& LANBrowser::games()const{return impl_->games;}
const std::string& LANBrowser::error()const{return impl_->socket.error;}
void LANBrowser::Poll(Time now) {
    auto& p=*impl_;
    for(size_t i=p.games.size();i--;)if(now-p.seen[i]>=std::chrono::seconds(15)){p.games.erase(p.games.begin()+i);p.seen.erase(p.seen.begin()+i);}
    if(!p.active)return;
    if(now>=p.end){p.active=false;p.socket.Close();if(!p.sent)p.socket.error="LAN queries could not be sent";return;}
    if(now>=p.next){const auto packet=Header(1,p.nonce);for(const auto& target:p.targets)p.sent=p.socket.Send(packet,target)||p.sent;p.next=now+std::chrono::milliseconds(750);}
    for(unsigned i=0;i<32;++i) {
        std::array<uint8_t,129> bytes{};sockaddr_in source{};const auto size=p.socket.Receive(bytes,source);
        if(size==SOCKET_ERROR){const auto error=WSAGetLastError();if(error==WSAEWOULDBLOCK)break;if(error==WSAEMSGSIZE || error==WSAECONNRESET)continue;p.socket.Fail("LAN scan receive");p.active=false;break;}
        const auto game=DecodeResponse(std::span(bytes).first(size),p.nonce,source,p.subnets);if(!game)continue;
        const auto prior=std::find_if(p.games.begin(),p.games.end(),[&](const auto& existing){return existing.address==game->address && existing.port==game->port;});
        if(prior!=p.games.end()){*prior=*game;p.seen[size_t(prior-p.games.begin())]=now;continue;}
        if(p.games.size()<32){p.games.push_back(*game);p.seen.push_back(now);}
    }
}
bool TestLANDiscovery() {
    auto check=[](bool value,const char* message){if(!value)REXLOG_ERROR("LAN discovery test: {}",message);return value;};
    Invite invite{"203.0.113.19",41036};invite.session[0]=7;invite.first_tick=45;invite.input_delay=3;
    LANAdvertiser host;LANBrowser browser;
    if(!check(host.Start(invite,"127.0.0.1",0) && host.port(),"exclusive ephemeral UDP listener"))return false;
    const auto now=Clock::now();
    if(!check(browser.Start({{"127.0.0.1",host.port()}},now),"explicit loopback-only scan"))return false;
    const auto deadline=now+std::chrono::seconds(2);
    while(Clock::now()<deadline && browser.games().empty()){browser.Poll();host.Poll();browser.Poll();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    if(!check(browser.games().size()==1,"live UDP query/challenge/response discovers host"))return false;
    browser.Poll(now+std::chrono::milliseconds(800));host.Poll(now+std::chrono::milliseconds(800));browser.Poll(now+std::chrono::milliseconds(800));
    if(!check(browser.games().size()==1,"repeated replies deduplicate"))return false;
    const auto discovered=DecodeInvite(browser.games()[0].invitation);
    auto expected=invite;expected.address="127.0.0.1";
    if(!check(discovered==expected,"LAN source replaces public endpoint while preserving port/session/timing"))return false;
    browser.Poll(now+std::chrono::seconds(4));if(!check(!browser.scanning() && browser.games().size()==1,"scan stops but valid result remains selectable"))return false;
    browser.Poll(now+std::chrono::seconds(17));if(!check(browser.games().empty(),"stale discoveries expire"))return false;
    std::array<uint8_t,16> nonce{};nonce[0]=9;auto response=Header(2,nonce);const auto code=*EncodeInvite(invite);response.insert(response.end(),code.begin(),code.end());
    sockaddr_in source{};source.sin_family=AF_INET;source.sin_addr.s_addr=htonl(0x7F000001);
    if(!check(DecodeResponse(response,nonce,source,{}).has_value(),"valid response fixture"))return false;
    for(size_t i=0;i<response.size();++i){auto broken=response;broken[i]^=1;if(!check(!DecodeResponse(broken,nonce,source,{}),"every single-byte response corruption rejected"))return false;}
    auto old=nonce;old[1]=1;if(!check(!DecodeResponse(response,old,source,{}),"unsolicited/prior scan nonce rejected"))return false;
    source.sin_addr.s_addr=htonl(0x08080808);if(!check(!DecodeResponse(response,nonce,source,{}),"off-link source rejected"))return false;
    if(!check(!OnLink(0xC0A801FF,{{0xC0A80105,0xFFFFFF00}}) && OnLink(0xC0A80109,{{0xC0A80105,0xFFFFFF00}}),"on-link unicast bounds"))return false;
    if(!check(!browser.Start({}) && !browser.Start({{"::1",41037}}),"empty/IPv6 targets rejected without broadcast"))return false;
    LANAdvertiser occupied;if(!check(!occupied.Start(invite,"127.0.0.1",host.port()) && host.port(),"discovery port collision cannot replace another listener"))return false;
    Socket fake;
    if(!check(fake.Open("127.0.0.1",0,false) && browser.Start({{"127.0.0.1",fake.port}}),"untrusted peer fixture opens loopback only"))return false;
    browser.Poll();std::array<uint8_t,129> query{};sockaddr_in return_to{};
    const auto query_end=Clock::now()+std::chrono::seconds(1);int received=SOCKET_ERROR;
    do {received=fake.Receive(query,return_to);if(received==24)break;std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(Clock::now()<query_end);
    if(!check(received==24 && ValidHeader(std::span(query).first(24),1),"actual query captured by loopback peer"))return false;
    std::array<uint8_t,16> live_nonce{};std::copy_n(query.begin()+8,16,live_nonce.begin());
    Packet malformed(512,0xEE);fake.Send(malformed,return_to);
    auto stale=Header(2,nonce);stale.insert(stale.end(),code.begin(),code.end());fake.Send(stale,return_to);
    browser.Poll();if(!check(browser.games().empty() && browser.scanning(),"oversized and stale-nonce live datagrams ignored without killing scan"))return false;
    auto receive_until=[&](auto ready) {
        const auto until=Clock::now()+std::chrono::seconds(1);
        do{browser.Poll();if(ready())return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(Clock::now()<until);
        return false;
    };
    for(unsigned i=0;i<40;++i) {
        auto other=invite;other.port=uint16_t(42000+i);auto packet=Header(2,live_nonce);const auto payload=*EncodeInvite(other);
        packet.insert(packet.end(),payload.begin(),payload.end());fake.Send(packet,return_to);
        if(i%8==7)browser.Poll();
    }
    if(!check(receive_until([&]{return browser.games().size()==32;}),"untrusted discovered host list has a hard bound"))return false;
    auto updated=invite;updated.port=42000;updated.session[0]=8;
    auto packet=Header(2,live_nonce);const auto payload=*EncodeInvite(updated);packet.insert(packet.end(),payload.begin(),payload.end());fake.Send(packet,return_to);
    if(!check(receive_until([&]{const auto changed=DecodeInvite(browser.games()[0].invitation);
        return browser.games().size()==32 && changed && changed->session==updated.session;}),"same endpoint refreshes its session without duplicating or exceeding cap"))return false;
    if(!check(browser.Start({{"127.0.0.1",fake.port}}) && browser.games().empty(),"new scan clears prior selectable rows"))return false;
    fake.Send(packet,return_to);browser.Poll();if(!check(browser.games().empty(),"old scan response cannot populate a restarted scan"))return false;
    host.Stop();browser.Stop();if(!check(!host.port() && !browser.scanning() && browser.games().empty(),"stop clears sockets/results"))return false;
    REXLOG_INFO("LAN discovery passed real loopback UDP, endpoint remapping, exact nonce/packet bounds, corruption, oversized/stale live datagrams, 32-host cap/session refresh, port collision, off-link rejection, timeout/expiry and cancellation; no LAN broadcast sent");return true;
}
}
