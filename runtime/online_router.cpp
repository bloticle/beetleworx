#include "online_router.h"
#include "online_upnp.h"
#include "online_address.h"
#include "online_invite.h"
#include <iphlpapi.h>
#include <bcrypt.h>
#include <rex/logging.h>
#include <array>
#include <algorithm>
#include <span>
#include <vector>

namespace em2::net {
namespace {
using Time=RouterMapping::Time;using namespace std::chrono_literals;
uint16_t U16(std::span<const uint8_t> b,size_t i){return uint16_t((unsigned(b[i])<<8)|b[i+1]);}
uint32_t U32(std::span<const uint8_t> b,size_t i){return (uint32_t(U16(b,i))<<16)|U16(b,i+2);}
void Put16(std::span<uint8_t> b,size_t i,uint16_t v){b[i]=uint8_t(v>>8);b[i+1]=uint8_t(v);}
void Put32(std::span<uint8_t> b,size_t i,uint32_t v){Put16(b,i,uint16_t(v>>16));Put16(b,i+2,uint16_t(v));}
std::array<uint8_t,12> MapRequest(uint16_t internal,uint16_t external,uint32_t lifetime) {
    std::array<uint8_t,12> b{};b[1]=2;Put16(b,4,internal);Put16(b,6,external);Put32(b,8,lifetime);return b;
}
std::array<uint8_t,60> PcpRequest(std::span<const uint8_t,12> nonce,std::span<const uint8_t,16> client,
                                uint16_t internal,uint16_t external,std::span<const uint8_t,16> address,uint32_t lifetime) {
    std::array<uint8_t,60> b{};b[0]=2;b[1]=1;Put32(b,4,lifetime);
    std::copy(client.begin(),client.end(),b.begin()+8);std::copy(nonce.begin(),nonce.end(),b.begin()+24);
    b[36]=IPPROTO_TCP;Put16(b,40,internal);
    if(lifetime){Put16(b,42,external);std::copy(address.begin(),address.end(),b.begin()+44);}
    return b;
}
bool ValidPcpOptions(std::span<const uint8_t> b) {
    for(size_t at=60;at<b.size();) {
        if(b.size()-at<4)return false;
        const auto length=(size_t(U16(b,at+2))+3)&~size_t(3);
        if(length>b.size()-at-4)return false;at+=4+length;
    }
    return true; // Unknown response options are ignored after framing checks.
}
std::string Gateway(const NumericAddress& local) {
    ULONG bytes=16384;std::vector<uint8_t> buffer(bytes);ULONG result=ERROR_BUFFER_OVERFLOW;
    for(unsigned tries=0;tries<3 && result==ERROR_BUFFER_OVERFLOW;++tries){buffer.resize(bytes);result=GetAdaptersAddresses(AF_INET,GAA_FLAG_INCLUDE_GATEWAYS,nullptr,reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),&bytes);}
    if(result!=NO_ERROR)return {};
    for(auto* a=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());a;a=a->Next) {
        if(a->OperStatus!=IfOperStatusUp || a->IfType==IF_TYPE_SOFTWARE_LOOPBACK || a->IfType==IF_TYPE_TUNNEL)continue;
        for(auto* u=a->FirstUnicastAddress;u;u=u->Next) {
            if(!u->Address.lpSockaddr || u->DadState!=IpDadStatePreferred || u->Address.lpSockaddr->sa_family!=AF_INET || u->Address.iSockaddrLength<int(sizeof(sockaddr_in)) || !u->OnLinkPrefixLength || u->OnLinkPrefixLength>32)continue;
            const auto& ip=reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr)->sin_addr;
            if(std::memcmp(&ip,local.bytes(),4))continue;
            const uint32_t mask=u->OnLinkPrefixLength?UINT32_MAX<<(32-u->OnLinkPrefixLength):0;
            for(auto* g=a->FirstGatewayAddress;g;g=g->Next) {
                if(!g->Address.lpSockaddr || g->Address.lpSockaddr->sa_family!=AF_INET || g->Address.iSockaddrLength<int(sizeof(sockaddr_in)))continue;
                NumericAddress gateway;gateway.size=sizeof(sockaddr_in);std::memcpy(&gateway.storage,g->Address.lpSockaddr,gateway.size);
                const auto& gw=reinterpret_cast<const sockaddr_in*>(&gateway.storage)->sin_addr;
                if((ntohl(ip.s_addr)&mask)!=(ntohl(gw.s_addr)&mask))continue;
                if(auto parsed=ParseNumericAddress(gateway.Text());parsed && parsed->bytes()[0]!=127)return gateway.Text();
            }
        }
    }
    return {};
}
}
struct RouterMapping::Impl {
    std::unique_ptr<UpnpLease> upnp;UpnpFactory fallback;std::string local,gateway;
    SOCKET socket=INVALID_SOCKET;int startup=0;RouterStatus status;
    uint16_t internal=0,external=0;bool map_phase=false,maybe_mapped=false,leased=false,inflight=false;
    unsigned attempts=0;Time next{},expires{},renew{},deadline{},last_send{},epoch_time{};std::string pending_address;
    bool pcp=false,automatic=false,sent_pcp=false,epoch_seen=false;uint32_t epoch=0,jitter=1,retry_ms=0;
    std::array<uint8_t,12> nonce{};std::array<uint8_t,16> client_address{},external_address{};
    bool EpochReset(uint32_t value,Time now) {
        bool changed=false;
        if(epoch_seen) {
            const uint64_t client=uint64_t(std::max<int64_t>(0,std::chrono::duration_cast<std::chrono::seconds>(now-epoch_time).count()));
            const uint64_t server=value>=epoch?uint64_t(value)-epoch:0;
            changed=uint64_t(value)+1<epoch || client+2<server-server/16 || server+2<client-client/16;
        }
        epoch_seen=true;epoch=value;epoch_time=now;return changed;
    }
    uint32_t RetryDelay() {
        jitter^=jitter<<13;jitter^=jitter>>17;jitter^=jitter<<5;
        const uint32_t base=retry_ms?std::min(retry_ms*2,1024000u):3000u;
        retry_ms=uint32_t(uint64_t(base)*(9000+jitter%2001)/10000);return retry_ms;
    }
    void DeletePcp(){const auto b=PcpRequest(nonce,client_address,internal,0,external_address,0);send(socket,reinterpret_cast<const char*>(b.data()),int(b.size()),0);}
    void Downgrade(Time now,bool unsupported) {
        const auto limit=deadline;
        if(maybe_mapped && !unsupported)DeletePcp();
        pcp=false;maybe_mapped=false;sent_pcp=false;epoch_seen=false;Request(false,now);deadline=limit;
    }
    Impl(){WSADATA data{};startup=WSAStartup(MAKEWORD(2,2),&data);}
    ~Impl(){Close();if(!startup)WSACleanup();}
    void Close() {
        upnp.reset();
        if(socket!=INVALID_SOCKET) {
            if(maybe_mapped && internal){if(pcp)DeletePcp();else{const auto bytes=MapRequest(internal,0,0);send(socket,reinterpret_cast<const char*>(bytes.data()),int(bytes.size()),0);}}
            closesocket(socket);socket=INVALID_SOCKET;
        }
        maybe_mapped=leased=false;
    }
    bool TryUpnp(Time now) {
        if(!fallback || leased)return false;
        Close();upnp=std::make_unique<UpnpLease>();
        upnp->Start(local,gateway,internal,now,std::move(fallback));status=upnp->status();return true;
    }
    bool Fail(std::string text){status.state=RouterState::Failed;status.address.clear();status.port=0;status.error=std::move(text);return false;}
    void Request(bool mapping,Time now){map_phase=mapping;inflight=true;attempts=0;retry_ms=0;next=now;deadline=now+30s;if(!leased)status.state=mapping?RouterState::Mapping:RouterState::Discovering;}
};
RouterMapping::RouterMapping():impl_(std::make_unique<Impl>()){}
RouterMapping::~RouterMapping()=default;
const RouterStatus& RouterMapping::status()const{return impl_->status;}
uint16_t RouterMapping::internal_port()const{return impl_->internal;}
void RouterMapping::Stop(){impl_->Close();impl_->fallback={};impl_->status={};impl_->internal=0;}
std::optional<std::string> InternetInvitation(std::string_view code,const RouterStatus& status) {
    if(status.state!=RouterState::Active || !status.port)return {};
    auto address=ParseNumericAddress(status.address);auto invite=DecodeInvite(code);
    if(!invite || !address || !PublicRouterV4(*address))return {};
    invite->address=status.address;invite->port=status.port;return EncodeInvite(*invite);
}
bool RouterMapping::Start(std::string_view local,uint16_t internal) {
    Stop();auto address=ParseNumericAddress(local);
    if(!address || address->ipv6() || !internal)return impl_->Fail("Choose this PC's local IPv4 address for router mapping.");
    const auto gateway=Gateway(*address);if(gateway.empty())return impl_->Fail("No on-link IPv4 gateway found for this address.");
    return Open(local,internal,gateway,5351,Clock::now(),true,MakeNativeUpnpGateway);
}
bool RouterMapping::Open(std::string_view local,uint16_t internal,std::string_view gateway,uint16_t port,Time now,bool prefer_pcp,UpnpFactory fallback) {
    Stop();auto& p=*impl_;auto source=ParseNumericAddress(local),target=ParseNumericAddress(gateway);
    if(p.startup || !source || !target || source->ipv6() || target->ipv6() || !internal || !port)return p.Fail("Router mapping endpoint unavailable.");
    p.internal=internal;p.external=0;p.pending_address.clear();target->Port(port);
    p.local=std::string(local);p.gateway=std::string(gateway);p.fallback=std::move(fallback);
    p.pcp=p.automatic=prefer_pcp;p.sent_pcp=p.epoch_seen=false;p.retry_ms=0;p.external_address={};p.client_address={};
    p.client_address[10]=p.client_address[11]=255;std::copy_n(source->bytes(),4,p.client_address.begin()+12);
    if(prefer_pcp && BCryptGenRandom(nullptr,p.nonce.data(),ULONG(p.nonce.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)
        return p.Fail("Cannot create a router mapping nonce. LAN and manual invitations still work.");
    p.jitter=uint32_t(p.nonce[0])<<24|uint32_t(p.nonce[1])<<16|uint32_t(p.nonce[2])<<8|p.nonce[3];if(!p.jitter)p.jitter=1;
    p.socket=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);u_long nonblocking=1;
    if(p.socket==INVALID_SOCKET || ioctlsocket(p.socket,FIONBIO,&nonblocking) ||
       bind(p.socket,reinterpret_cast<sockaddr*>(&source->storage),source->size) ||
       connect(p.socket,reinterpret_cast<sockaddr*>(&target->storage),target->size)) {
        p.Close();return p.Fail("Cannot open the router mapping connection.");
    }
    p.Request(prefer_pcp,now);return true;
}
void RouterMapping::Poll(Time now) {
    auto& p=*impl_;
    if(p.upnp){p.upnp->Poll(now);p.status=p.upnp->status();return;}
    if(p.socket==INVALID_SOCKET || p.status.state==RouterState::Failed)return;
    if(p.leased && now>=p.expires){p.Fail("Router mapping expired. Use LAN or a forwarded invitation.");return;}
    if(p.leased && !p.inflight && now>=p.renew)p.Request(p.pcp,now);
    for(unsigned i=0;i<16;++i) {
        std::array<uint8_t,1100> b{};const int size=recv(p.socket,reinterpret_cast<char*>(b.data()),int(b.size()),0);
        if(size==SOCKET_ERROR){const int error=WSAGetLastError();if(error==WSAEMSGSIZE)continue;if(error!=WSAEWOULDBLOCK){if(p.TryUpnp(now))return;p.Fail("Router did not accept automatic setup. Use LAN or manual forwarding.");}break;}
        if(p.pcp) {
            if(!p.sent_pcp || size<8)continue;
            const auto bytes=std::span(b).first(size);
            // RFC6886's version error uses OP=0 (without the response bit).
            if(p.inflight && b[0]==0 && (b[1]==0 || b[1]==128) && U16(bytes,2)==1 && size==8) {
                p.Downgrade(now,true);continue;
            }
            if(size<60 || size%4 || b[0]!=2 || b[1]!=129 ||
               !std::equal(p.nonce.begin(),p.nonce.end(),b.begin()+24) || b[36]!=IPPROTO_TCP ||
               U16(bytes,40)!=p.internal || !ValidPcpOptions(bytes))continue;
            if(b[3]){p.Close();p.Fail("Router refused PCP (code "+std::to_string(b[3])+"). Check router permissions or use LAN/manual forwarding.");break;}
            const bool reset=p.EpochReset(U32(bytes,8),now);
            const auto lifetime=U32(bytes,4);
            if(!lifetime || reset) {
                p.leased=false;p.status={};p.Request(true,now);p.next=std::max(p.next,p.last_send+4s);continue;
            }
            NumericAddress address;address.size=sizeof(sockaddr_in);
            auto* v4=reinterpret_cast<sockaddr_in*>(&address.storage);v4->sin_family=AF_INET;
            const bool mapped=std::all_of(b.begin()+44,b.begin()+54,[](uint8_t value){return value==0;}) && b[54]==255 && b[55]==255;
            std::memcpy(&v4->sin_addr,b.data()+56,4);
            if(!mapped || !PublicRouterV4(address) || !U16(bytes,42)) {
                p.Close();p.Fail("Router has no usable public IPv4 endpoint. A VPN or relay may be needed.");break;
            }
            p.external=U16(bytes,42);std::copy_n(b.begin()+44,16,p.external_address.begin());p.pending_address=address.Text();
            const auto seconds=std::min(lifetime,86400u);
            p.expires=now+std::chrono::seconds(seconds);p.renew=now+std::chrono::milliseconds(std::max<uint64_t>(4000,uint64_t(seconds)*500));
            p.leased=true;p.inflight=false;p.attempts=0;p.status={RouterState::Active,p.pending_address,{},p.external};continue;
        }
        if(!p.inflight || !p.attempts || size<8 || b[0]!=0 || b[1]!=(p.map_phase?130:128))continue;
        const auto bytes=std::span(b).first(size);
        if(U16(bytes,2)){
            if(p.map_phase && size>=16 && U16(bytes,8)!=p.internal)continue;
            p.Fail("Router refused NAT-PMP (code "+std::to_string(U16(bytes,2))+"). Use LAN or manual forwarding.");break;
        }
        if(!p.map_phase) {
            if(size!=12)continue;NumericAddress address;address.size=sizeof(sockaddr_in);
            auto* v4=reinterpret_cast<sockaddr_in*>(&address.storage);v4->sin_family=AF_INET;std::memcpy(&v4->sin_addr,b.data()+8,4);
            if(!PublicRouterV4(address)){p.Fail("Router has no public IPv4 address. A VPN or relay may be needed.");break;}
            p.pending_address=address.Text();p.Request(true,now);
        } else {
            if(size!=16 || U16(bytes,8)!=p.internal || !U16(bytes,10) || !U32(bytes,12))continue;
            if(U32(bytes,12)>120){p.Close();p.Fail("Router returned an unsupported mapping lifetime.");break;}
            p.external=U16(bytes,10);p.expires=now+std::chrono::seconds(U32(bytes,12));
            p.renew=now+std::chrono::milliseconds(uint64_t(U32(bytes,12))*500);p.leased=true;p.inflight=false;p.attempts=0;p.next=p.renew;
            p.status={RouterState::Active,p.pending_address,{},p.external};
        }
    }
    if(p.status.state==RouterState::Failed)return;
    if(p.automatic && p.inflight && now>=p.deadline){if(p.TryUpnp(now))return;p.Close();p.Fail("Automatic router setup timed out. LAN and manual invitations still work.");return;}
    if(now<p.next || !p.inflight)return;
    if(p.pcp) {
        if(p.attempts>=3 && !p.leased){p.Downgrade(now,false);}
        else {
            const auto request=PcpRequest(p.nonce,p.client_address,p.internal,p.external,p.external_address,120);
            const auto sent=send(p.socket,reinterpret_cast<const char*>(request.data()),int(request.size()),0);
            if(sent==SOCKET_ERROR){if(WSAGetLastError()!=WSAEWOULDBLOCK){if(p.TryUpnp(now))return;p.Close();p.Fail("Cannot contact the router for PCP.");}return;}
            p.maybe_mapped=p.sent_pcp=true;p.last_send=now;p.next=now+std::chrono::milliseconds(p.RetryDelay());++p.attempts;return;
        }
    }
    if(p.attempts==9){p.Fail("Router mapping timed out. Use LAN or manual forwarding.");return;}
    const auto request=MapRequest(p.internal,p.external,120);const std::array<uint8_t,2> discovery{};
    const auto bytes=p.map_phase?std::span<const uint8_t>(request):std::span<const uint8_t>(discovery);
    const int sent=send(p.socket,reinterpret_cast<const char*>(bytes.data()),int(bytes.size()),0);
    if(sent==SOCKET_ERROR){if(WSAGetLastError()!=WSAEWOULDBLOCK){if(p.TryUpnp(now))return;p.Fail("Cannot contact the router for NAT-PMP.");}return;}
    if(p.map_phase)p.maybe_mapped=true;
    p.next=now+std::chrono::milliseconds(250ull<<p.attempts);++p.attempts;
}
}
