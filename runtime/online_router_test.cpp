#include "online_router.h"
#include "online_address.h"
#include "online.h"
#include "online_invite.h"
#include <rex/logging.h>
#include <array>
#include <thread>
#include <vector>
namespace em2::net {
namespace {
using Bytes=std::vector<uint8_t>;using namespace std::chrono_literals;
uint16_t Read16(const Bytes& b,size_t i){return uint16_t(unsigned(b[i])*256+b[i+1]);}
uint32_t Read32(const Bytes& b,size_t i){return uint32_t(Read16(b,i))*65536+Read16(b,i+2);}
void Write16(Bytes& b,size_t i,uint16_t n){b[i]=uint8_t(n>>8);b[i+1]=uint8_t(n);}
void Write32(Bytes& b,size_t i,uint32_t n){Write16(b,i,uint16_t(n>>16));Write16(b,i+2,uint16_t(n));}
Bytes PublicReply(std::array<uint8_t,4> ip={8,8,8,8}) {
    Bytes b(12);b[1]=128;Write32(b,4,100);std::copy(ip.begin(),ip.end(),b.begin()+8);return b;
}
Bytes MappingReply(uint16_t port,uint16_t external=41234,uint32_t lifetime=120) {
    Bytes b(16);b[1]=130;Write32(b,4,100);Write16(b,8,port);Write16(b,10,external);Write32(b,12,lifetime);return b;
}
Bytes PcpReply(const Bytes& request,uint32_t lifetime=120,uint32_t epoch=100,std::array<uint8_t,4> ip={8,8,8,8}) {
    if(request.size()!=60)return {};
    Bytes b(60);b[0]=2;b[1]=129;Write32(b,4,lifetime);Write32(b,8,epoch);
    std::copy_n(request.begin()+24,20,b.begin()+24);Write16(b,42,41234);
    b[54]=b[55]=255;std::copy(ip.begin(),ip.end(),b.begin()+56);return b;
}
struct GatewayFixture {
    SOCKET fd=INVALID_SOCKET;sockaddr_in peer{};uint16_t port=0;bool started=false;
    GatewayFixture() {
        WSADATA data{};started=WSAStartup(MAKEWORD(2,2),&data)==0;if(!started)return;
        fd=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);auto local=*ParseNumericAddress("127.0.0.1");u_long nonblocking=1;
        if(fd==INVALID_SOCKET || ioctlsocket(fd,FIONBIO,&nonblocking) || bind(fd,reinterpret_cast<sockaddr*>(&local.storage),local.size))return;
        int size=local.size;if(getsockname(fd,reinterpret_cast<sockaddr*>(&local.storage),&size)==0)port=local.Port();
    }
    ~GatewayFixture(){if(fd!=INVALID_SOCKET)closesocket(fd);if(started)WSACleanup();}
    Bytes Receive() {
        std::array<uint8_t,128> bytes{};int size=sizeof(peer);
        const int count=recvfrom(fd,reinterpret_cast<char*>(bytes.data()),int(bytes.size()),0,reinterpret_cast<sockaddr*>(&peer),&size);
        return count>0?Bytes(bytes.begin(),bytes.begin()+count):Bytes{};
    }
    bool Reply(const Bytes& bytes,bool stranger=false) {
        SOCKET from=stranger?socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP):fd;
        const auto sent=sendto(from,reinterpret_cast<const char*>(bytes.data()),int(bytes.size()),0,reinterpret_cast<sockaddr*>(&peer),sizeof(peer));
        if(stranger)closesocket(from);return sent==int(bytes.size());
    }
};
}
bool TestRouterMapping() {
    bool ok=true;unsigned checks=0;
    auto check=[&](bool value,const char* text){++checks;if(!value){ok=false;REXLOG_ERROR("Router mapping test: {}",text);}return value;};
    GatewayFixture gateway;if(!check(gateway.port!=0,"isolated loopback router socket"))return false;
    auto now=RouterMapping::Clock::now();RouterMapping mapping;
    if(!check(!mapping.Start("::1",45001) && !mapping.Start("127.0.0.1",0),"invalid family/zero internal port rejected before gateway requests"))return false;
    if(!check(mapping.Open("127.0.0.1",45001,"127.0.0.1",gateway.port,now),"open explicit loopback-only test endpoint"))return false;
    mapping.Poll(now);check(gateway.Receive()==Bytes({0,0}),"external-address request wire bytes");
    mapping.Poll(now+249ms);check(gateway.Receive().empty(),"retry does not occur early");
    mapping.Poll(now+250ms);check(gateway.Receive()==Bytes({0,0}),"first retry at250ms");
    gateway.Reply(PublicReply(),true);mapping.Poll(now+251ms);
    check(mapping.status().state==RouterState::Discovering,"foreign source port cannot answer connected gateway socket");
    gateway.Reply(Bytes{0});auto invalid=PublicReply();invalid[0]=2;gateway.Reply(invalid);
    invalid=PublicReply();invalid[1]=130;gateway.Reply(invalid);mapping.Poll(now+252ms);
    check(mapping.status().state==RouterState::Discovering && gateway.Receive().empty(),"truncated/version/opcode mismatches ignored");
    gateway.Reply(PublicReply());mapping.Poll(now+253ms);auto request=gateway.Receive();
    check(request.size()==12 && request[0]==0 && request[1]==2 && Read16(request,2)==0 && Read16(request,4)==45001 && Read16(request,6)==0 && Read32(request,8)==120,
        "TCP-only mapping requests exact internal port, automatic external port and bounded lease");
    gateway.Reply(MappingReply(45002));mapping.Poll(now+254ms);check(mapping.status().state==RouterState::Mapping,"wrong internal port ignored");
    invalid=MappingReply(45002);Write16(invalid,2,2);gateway.Reply(invalid);mapping.Poll(now+254ms);
    check(mapping.status().state==RouterState::Mapping,"error response for another internal port cannot abort this request");
    gateway.Reply(MappingReply(45001));mapping.Poll(now+255ms);
    check(mapping.status().state==RouterState::Active && mapping.status().address=="8.8.8.8" && mapping.status().port==41234,"gateway-selected external endpoint published");
    Invite original{"127.0.0.1",45001};original.session[0]=17;original.first_tick=9876;original.input_delay=12;
    const auto local=EncodeInvite(original);const auto internet=local?InternetInvitation(*local,mapping.status()):std::nullopt;
    const auto decoded=internet?DecodeInvite(*internet):std::nullopt;
    check(decoded && decoded->address=="8.8.8.8" && decoded->port==41234 && decoded->session==original.session && decoded->first_tick==original.first_tick && decoded->input_delay==12 && DecodeInvite(*local)->address=="127.0.0.1",
        "Internet invitation changes endpoint only; LAN invitation and canonical identity remain intact");
    gateway.Reply(MappingReply(45001));mapping.Poll(now+1s);check(gateway.Receive().empty(),"duplicate mapping reply cannot start another request");
    mapping.Poll(now+60255ms);check(gateway.Receive()==Bytes({0,0}),"half-life renewal refreshes public address");
    gateway.Reply(PublicReply());mapping.Poll(now+60256ms);request=gateway.Receive();
    check(request.size()==12 && Read16(request,6)==41234,"renewal suggests the assigned external port");
    gateway.Reply(MappingReply(45001));mapping.Poll(now+60257ms);mapping.Stop();request=gateway.Receive();
    check(request.size()==12 && Read16(request,4)==45001 && Read16(request,6)==0 && Read32(request,8)==0,"stop deletes only the owned TCP internal port, with external port/lifetime zero");
    for(auto address:{std::array<uint8_t,4>{10,0,0,1},{100,64,0,1},{172,16,0,1},{192,168,1,1},{127,0,0,1},{169,254,1,1},{203,0,113,1},{224,0,0,1}}) {
        mapping.Open("127.0.0.1",45001,"127.0.0.1",gateway.port,now);mapping.Poll(now);gateway.Receive();gateway.Reply(PublicReply(address));mapping.Poll(now+1ms);
        check(mapping.status().state==RouterState::Failed && !InternetInvitation(*local,mapping.status()) && gateway.Receive().empty(),"nonpublic/CGNAT address never creates a mapping or Internet invitation");mapping.Stop();
    }
    for(unsigned code=1;code<=5;++code) {
        mapping.Open("127.0.0.1",45001,"127.0.0.1",gateway.port,now);mapping.Poll(now);gateway.Receive();auto error=PublicReply();Write16(error,2,uint16_t(code));gateway.Reply(error);mapping.Poll(now+1ms);
        check(mapping.status().state==RouterState::Failed && !mapping.status().error.empty(),"router result code is surfaced");mapping.Stop();
    }
    mapping.Open("127.0.0.1",45001,"127.0.0.1",gateway.port,now);auto retry=now;
    for(unsigned attempt=0;attempt<9;++attempt){mapping.Poll(retry);check(gateway.Receive()==Bytes({0,0}),"bounded exponential retransmission");retry+=std::chrono::milliseconds(250ull<<attempt);}
    mapping.Poll(retry);check(mapping.status().state==RouterState::Failed && gateway.Receive().empty(),"ninth attempt timeout terminates without wall-clock waiting");mapping.Stop();
    mapping.Open("127.0.0.1",45001,"127.0.0.1",gateway.port,now);mapping.Poll(now);gateway.Receive();gateway.Reply(PublicReply());mapping.Poll(now+1ms);gateway.Receive();mapping.Stop();request=gateway.Receive();
    check(request.size()==12 && Read16(request,4)==45001 && Read32(request,8)==0,"lost mapping acknowledgement still receives best-effort cleanup");
    for(bool oversized:{false,true}) {
        mapping.Open("127.0.0.1",45001,"127.0.0.1",gateway.port,now);mapping.Poll(now);gateway.Receive();gateway.Reply(PublicReply());mapping.Poll(now+1ms);gateway.Receive();
        gateway.Reply(MappingReply(45001,41234,oversized?121:2));mapping.Poll(now+2ms);
        if(!oversized){check(mapping.status().state==RouterState::Active,"short lease accepted");mapping.Poll(now+2002ms);}
        check(mapping.status().state==RouterState::Failed && !InternetInvitation(*local,mapping.status()),"oversized or expired lease cannot publish an Internet invitation");
        if(oversized){request=gateway.Receive();check(request.size()==12 && Read32(request,8)==0,"unsupported grant is released immediately");}
        mapping.Stop();request=gateway.Receive();check(oversized?request.empty():request.size()==12 && Read32(request,8)==0,"failed lease cleanup is bounded and never repeated after socket close");
    }
    // Public Start chooses PCP first; explicit Open(false) cases above retain
    // coverage of the legacy protocol without contacting a real gateway.
    now=RouterMapping::Clock::now();
    check(mapping.Open("127.0.0.1",45001,"127.0.0.1",gateway.port,now,true),"PCP-first loopback endpoint");
    mapping.Poll(now);auto pcp_request=gateway.Receive();
    if(!check(pcp_request.size()==60,"PCP request is60 bytes"))return false;
    check(pcp_request[0]==2 && pcp_request[1]==1 && Read16(pcp_request,2)==0 && Read32(pcp_request,4)==120 &&
          std::all_of(pcp_request.begin()+8,pcp_request.begin()+18,[](uint8_t b){return !b;}) &&
          pcp_request[18]==255 && pcp_request[19]==255 && pcp_request[20]==127 && pcp_request[21]==0 && pcp_request[22]==0 && pcp_request[23]==1 &&
          pcp_request[36]==6 && !pcp_request[37] && !pcp_request[38] && !pcp_request[39] && Read16(pcp_request,40)==45001 && !Read16(pcp_request,42) &&
          std::all_of(pcp_request.begin()+44,pcp_request.end(),[](uint8_t b){return !b;}),"PCP v2 TCP MAP fields, mapped client address, reserved bytes and automatic external endpoint");
    mapping.Poll(now+2600ms);check(gateway.Receive().empty(),"PCP initial randomized retry never before2.7 seconds");
    mapping.Poll(now+3400ms);check(gateway.Receive()==pcp_request,"PCP retransmission retains nonce and request bytes");
    gateway.Reply(PcpReply(pcp_request),true);mapping.Poll(now+3401ms);
    check(mapping.status().state==RouterState::Mapping,"PCP foreign source cannot publish endpoint");
    for(unsigned bad=0;bad<7;++bad) {
        auto reply=PcpReply(pcp_request);
        if(bad==0)reply[24]^=1;
        if(bad==1)reply[36]=17;
        if(bad==2)Write16(reply,40,45002);
        if(bad==3){reply[24]^=1;reply[3]=2;}
        if(bad==4)reply.resize(59);
        if(bad==5){reply.resize(64);Write16(reply,62,8);}
        if(bad==6)reply[1]=1;
        gateway.Reply(reply);mapping.Poll(now+3402ms);
        check(mapping.status().state==RouterState::Mapping,"PCP nonce/protocol/port/error/size/options/response-bit mismatch ignored");
    }
    auto pcp_response=PcpReply(pcp_request,1200);pcp_response.resize(68);pcp_response[60]=200;Write16(pcp_response,62,2);pcp_response[64]=1;pcp_response[65]=2;
    gateway.Reply(pcp_response);mapping.Poll(now+3403ms);
    check(mapping.status().state==RouterState::Active && mapping.status().address=="8.8.8.8" && mapping.status().port==41234 &&
          InternetInvitation(*local,mapping.status()).has_value(),"PCP accepts sane longer grant and framed unknown response option, exposes Internet invitation");
    mapping.Poll(now+603403ms);request=gateway.Receive();
    check(request.size()==60 && std::equal(request.begin()+24,request.begin()+36,pcp_request.begin()+24) && Read16(request,42)==41234 &&
          std::equal(request.begin()+44,request.end(),pcp_response.begin()+44),"PCP renewal retains nonce and assigned suggested endpoint");
    gateway.Reply(PcpReply(request,120,700));mapping.Poll(now+603404ms);
    check(mapping.status().state==RouterState::Active,"PCP renewal accepted with advancing epoch");
    // A matching unsolicited update invalidates the old invitation and
    // recreates the mapping after a reboot; only the confirmed grant publishes.
    gateway.Reply(PcpReply(request,120,1));mapping.Poll(now+603405ms);
    check(mapping.status().state==RouterState::Mapping && !InternetInvitation(*local,mapping.status()),"PCP reboot epoch withdraws stale Internet endpoint");
    mapping.Poll(now+607403ms);auto recreated=gateway.Receive();
    check(recreated.size()==60 && Read32(recreated,4)==120 && std::equal(recreated.begin()+24,recreated.begin()+36,pcp_request.begin()+24),"PCP reboot sends same-nonce recreation without rapid renewal flood");
    gateway.Reply(PcpReply(recreated,120,5));mapping.Poll(now+607404ms);
    check(mapping.status().state==RouterState::Active,"PCP recreated mapping republishes endpoint");
    mapping.Stop();auto deletion=gateway.Receive();
    check(deletion.size()==60 && !Read32(deletion,4) && Read16(deletion,40)==45001 && !Read16(deletion,42) &&
          std::equal(deletion.begin()+24,deletion.begin()+36,pcp_request.begin()+24) &&
          std::all_of(deletion.begin()+44,deletion.end(),[](uint8_t b){return !b;}),"PCP cancellation deletes only nonce-bound TCP/internal-port mapping with zero suggested endpoint");
    for(unsigned outcome=0;outcome<4;++outcome) {
        now=RouterMapping::Clock::now();mapping.Open("127.0.0.1",45001,"127.0.0.1",gateway.port,now,true);mapping.Poll(now);request=gateway.Receive();
        check(request.size()==60 && !std::equal(request.begin()+24,request.begin()+36,pcp_request.begin()+24),"fresh PCP start has fresh cryptographic nonce");
        if(outcome==0)gateway.Reply(PcpReply(pcp_request)); // Prior attempt cannot complete this one.
        if(outcome==1){auto denied=PcpReply(request);denied[3]=2;gateway.Reply(denied);}
        if(outcome==2)gateway.Reply(PcpReply(request,120,100,{100,64,1,1}));
        if(outcome==3)gateway.Reply(PcpReply(request,2));
        mapping.Poll(now+1ms);
        if(outcome==0)check(mapping.status().state==RouterState::Mapping,"old PCP nonce ignored after retry");
        if(outcome==1 || outcome==2)check(mapping.status().state==RouterState::Failed && !InternetInvitation(*local,mapping.status()),"PCP refusal/private outer address cannot publish Internet invitation");
        if(outcome==3){mapping.Poll(now+2001ms);check(mapping.status().state==RouterState::Failed,"PCP expired grant withdrawn");}
        if(outcome==1 || outcome==2){deletion=gateway.Receive();check(deletion.size()==60 && !Read32(deletion,4),"PCP failed grant cleaned up immediately");}
        mapping.Stop();deletion=gateway.Receive();
        if(outcome==0 || outcome==3)check(deletion.size()==60 && !Read32(deletion,4),"PCP lost acknowledgement/expired lease still gets best-effort deletion");
    }
    for(uint8_t version_error_op:{uint8_t(0),uint8_t(128)}) {
        now=RouterMapping::Clock::now();mapping.Open("127.0.0.1",45001,"127.0.0.1",gateway.port,now,true);mapping.Poll(now);gateway.Receive();
        Bytes unsupported(8);unsupported[1]=version_error_op;Write16(unsupported,2,1);gateway.Reply(unsupported);mapping.Poll(now+1ms);
        check(gateway.Receive()==Bytes({0,0}),"PCP unsupported-version negotiates immediate legacy discovery");
        gateway.Reply(PublicReply());mapping.Poll(now+2ms);request=gateway.Receive();
        check(request.size()==12 && Read16(request,4)==45001,"legacy negotiated TCP map preserves listener port");
        gateway.Reply(MappingReply(45001));mapping.Poll(now+3ms);
        check(mapping.status().state==RouterState::Active,"NAT-PMP fallback remains usable");mapping.Stop();gateway.Receive();
    }
    now=RouterMapping::Clock::now();mapping.Open("127.0.0.1",45001,"127.0.0.1",gateway.port,now,true);mapping.Poll(now);request=gateway.Receive();
    mapping.Poll(now+3400ms);gateway.Receive();mapping.Poll(now+11000ms);gateway.Receive();mapping.Poll(now+27000ms);
    deletion=gateway.Receive();auto legacy_discovery=gateway.Receive();
    check(deletion.size()==60 && !Read32(deletion,4) && legacy_discovery==Bytes({0,0}),"PCP timeout cleans uncertain mapping before legacy fallback");
    mapping.Poll(now+31000ms);check(mapping.status().state==RouterState::Failed,"automatic protocol fallback preserves total30-second deadline");mapping.Stop();gateway.Receive();
    Session host,client;Identity identity;identity.build.fill(1);identity.content.fill(2);identity.checkpoint.fill(3);identity.session.fill(4);
    if(!check(host.Host(identity,"127.0.0.1") && client.Join(identity,"127.0.0.1",host.port()),"session ownership fixture connects"))return false;
    const auto deadline=RouterMapping::Clock::now()+2s;
    while((host.state()!=State::Ready || client.state()!=State::Ready) && RouterMapping::Clock::now()<deadline){host.Poll();client.Poll();std::this_thread::sleep_for(1ms);}
    if(!check(host.state()==State::Ready && client.state()==State::Ready,"real TCP peers ready before router ownership transfer"))return false;
    now=RouterMapping::Clock::now();const auto internal=host.port();auto retained=std::make_unique<RouterMapping>();
    auto wrong=std::make_unique<RouterMapping>();wrong->Open("127.0.0.1",internal==65535?1:uint16_t(internal+1),"127.0.0.1",gateway.port,now);
    check(!host.KeepRouterMapping(wrong) && bool(wrong),"host rejects ownership for a different listener port");wrong.reset();
    retained->Open("127.0.0.1",internal,"127.0.0.1",gateway.port,now);retained->Poll(now);gateway.Receive();gateway.Reply(PublicReply());retained->Poll(now+1ms);gateway.Receive();gateway.Reply(MappingReply(internal,41234,2));retained->Poll(now+2ms);
    if(!check(retained->status().state==RouterState::Active && !client.KeepRouterMapping(retained) && retained && host.KeepRouterMapping(retained) && !retained,"only the ready host adopts a matching listener lease"))return false;
    host.Poll(now+1002ms);check(gateway.Receive()==Bytes({0,0}),"retained session polls renewal after frontend ownership ends");
    gateway.Reply(PublicReply());host.Poll(now+1003ms);request=gateway.Receive();check(request.size()==12 && Read16(request,4)==internal && Read16(request,6)==41234,"session sends renewal for actual listener");
    gateway.Reply(MappingReply(internal,41234,2));host.Poll(now+1004ms);
    Pad a{},b{};a.buttons=7;b.buttons=3;Frame ha{},cb{};bool got_host=false,got_client=false;
    check(host.SubmitLocal(0,a) && client.SubmitLocal(0,b),"retained mapping permits canonical input submission");
    for(unsigned i=0;i<200 && (!got_host || !got_client);++i) {
        const auto time=now+std::chrono::milliseconds(1005+i);host.Poll(time);client.Poll(time);
        if(!got_host)got_host=host.TakeFrame(ha);if(!got_client)got_client=client.TakeFrame(cb);
        std::this_thread::sleep_for(1ms);
    }
    check(got_host && got_client && ha==cb && ha.tick==0 && ha.players[0]==a && ha.players[1]==b,"real TCP canonical frame still agrees with retained router lease");
    host.Disconnect();request=gateway.Receive();client.Disconnect();
    check(request.size()==12 && Read16(request,4)==internal && Read16(request,6)==0 && Read32(request,8)==0,"session disconnect releases retained mapping");
    // Keep the PCP lease alive after the connection screen hands the real
    // listener session to gameplay, then remove it on session disconnect.
    if(!check(host.Host(identity,"127.0.0.1") && client.Join(identity,"127.0.0.1",host.port()),"PCP session ownership fixture connects"))return false;
    const auto pcp_ready=RouterMapping::Clock::now()+2s;
    while((host.state()!=State::Ready || client.state()!=State::Ready) && RouterMapping::Clock::now()<pcp_ready){host.Poll();client.Poll();std::this_thread::sleep_for(1ms);}
    if(!check(host.state()==State::Ready && client.state()==State::Ready,"real PCP ownership peers ready"))return false;
    now=RouterMapping::Clock::now();auto pcp_owned=std::make_unique<RouterMapping>();
    pcp_owned->Open("127.0.0.1",host.port(),"127.0.0.1",gateway.port,now,true);pcp_owned->Poll(now);auto owned_request=gateway.Receive();
    if(!check(owned_request.size()==60,"PCP ownership request created"))return false;
    gateway.Reply(PcpReply(owned_request,8));pcp_owned->Poll(now+1ms);
    if(!check(pcp_owned->status().state==RouterState::Active && host.KeepRouterMapping(pcp_owned) && !pcp_owned,"ready host adopts matching PCP mapping"))return false;
    host.Poll(now+4001ms);client.Poll(now+4001ms);auto owned_renew=gateway.Receive();
    check(owned_renew.size()==60 && std::equal(owned_renew.begin()+24,owned_renew.begin()+36,owned_request.begin()+24),"session renews retained PCP mapping after frontend ownership ends");
    gateway.Reply(PcpReply(owned_renew,8,104));host.Poll(now+4002ms);client.Poll(now+4002ms);
    host.Disconnect();client.Disconnect();auto owned_delete=gateway.Receive();
    check(owned_delete.size()==60 && !Read32(owned_delete,4) && Read16(owned_delete,40)==Read16(owned_request,40) &&
          std::equal(owned_delete.begin()+24,owned_delete.begin()+36,owned_request.begin()+24),"session disconnect releases exact retained PCP nonce and listener mapping");
    if(ok)REXLOG_INFO("Router PCP/NAT-PMP mapping passed {} checks: real loopback UDP, nonce/tuple/options/source validation, bounded retries/fallback, public-address rejection, invitation identity, epoch recreation, renewal/cancellation and real TCP ownership for both protocols; no actual gateway contacted",checks);
    return ok;
}
}
