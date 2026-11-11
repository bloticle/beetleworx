#include "online_upnp.h"
#include "online_address.h"
#include "online.h"
#include <rex/logging.h>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string_view>
#include <thread>

namespace em2::net {
namespace {
using namespace std::chrono_literals;
struct FakeRouter {
    std::mutex mutex;std::condition_variable changed;
    std::map<uint16_t,UpnpEntry> entries;
    std::vector<std::pair<uint16_t,UpnpEntry>> adds;
    std::vector<uint16_t> deletes;
    std::string address="8.8.8.8";
    bool discover_ok=true,add_ok=true,lost_ack=false,block_discover=false,block_add=false,release=false,entered=false;
    bool occupy_all=false;uint32_t granted=120;
};
struct FakeGateway final:UpnpGateway {
    std::shared_ptr<FakeRouter> state;
    explicit FakeGateway(std::shared_ptr<FakeRouter> s):state(std::move(s)){}
    bool Discover(std::string_view gateway,std::string& error)override {
        std::unique_lock lock(state->mutex);state->entered=true;state->changed.notify_all();
        if(state->block_discover)state->changed.wait(lock,[&]{return state->release;});
        if(gateway!="127.0.0.1" || !state->discover_ok){error="fixture: discovery unavailable";return false;}return true;
    }
    bool ExternalAddress(std::string& address,std::string&)override{std::lock_guard lock(state->mutex);address=state->address;return true;}
    UpnpQuery Query(uint16_t external,UpnpEntry& entry,std::string&)override {
        std::lock_guard lock(state->mutex);const auto found=state->entries.find(external);
        if(found!=state->entries.end()){entry=found->second;return UpnpQuery::Found;}
        if(state->occupy_all){entry={"192.168.1.20","another application",50000,0,true};return UpnpQuery::Found;}
        return UpnpQuery::Missing;
    }
    bool Add(uint16_t external,const UpnpEntry& entry,std::string& error)override {
        std::unique_lock lock(state->mutex);state->adds.emplace_back(external,entry);state->changed.notify_all();
        if(state->block_add)state->changed.wait(lock,[&]{return state->release;});
        if(state->add_ok || state->lost_ack){auto actual=entry;actual.lifetime=state->granted;state->entries[external]=actual;}
        if(!state->add_ok){error="fixture: mapping refused/lost ACK";return false;}return true;
    }
    bool Delete(uint16_t external,std::string&)override {std::lock_guard lock(state->mutex);state->deletes.push_back(external);state->entries.erase(external);return true;}
};
UpnpFactory Factory(const std::shared_ptr<FakeRouter>& state){return [state]{return std::make_unique<FakeGateway>(state);};}
template<class F>bool Until(F fn) {
    const auto limit=RouterMapping::Clock::now()+3s;
    do{if(fn())return true;std::this_thread::sleep_for(1ms);}while(RouterMapping::Clock::now()<limit);return false;
}
void Release(const std::shared_ptr<FakeRouter>& state){std::lock_guard lock(state->mutex);state->release=true;state->changed.notify_all();}
bool Idle(){return Until([]{return UpnpWorkerIdleForTest();});}
}
bool TestUpnpRouterMapping() {
    WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data))return false;
    struct WinsockCleanup{~WinsockCleanup(){WSACleanup();}} winsock_cleanup;
    bool ok=true;unsigned checks=0;const auto now=RouterMapping::Clock::now();
    auto check=[&](bool value,const char* label){++checks;if(!value){ok=false;REXLOG_ERROR("UPnP lease: {}",label);}return value;};
    auto active=[&](UpnpLease& lease,auto time){return Until([&]{lease.Poll(time);return lease.status().state==RouterState::Active || lease.status().state==RouterState::Failed;}) && lease.status().state==RouterState::Active;};
    auto failed=[&](UpnpLease& lease,auto time){return Until([&]{lease.Poll(time);return lease.status().state==RouterState::Failed;});};
    if(!check(Idle(),"no previous worker"))return false;
    UpnpLease lease;auto router=std::make_shared<FakeRouter>();
    check(lease.Start("127.0.0.1","127.0.0.1",45001,now,Factory(router)) && active(lease,now),"background setup publishes verified mapping");
    const auto original_status=lease.status();std::string first_tag;
    {
        std::lock_guard lock(router->mutex);
        check(original_status.address=="8.8.8.8" && original_status.port==45001 && router->adds.size()==1 && router->adds[0].second.lifetime==120 && router->adds[0].second.enabled && router->adds[0].second.client=="127.0.0.1","temporary TCP target retained");
        first_tag=router->adds.empty()?"":router->adds[0].second.description;
        check(first_tag.starts_with("beetleworx ") && first_tag.size()==std::string_view("beetleworx ").size()+24,"96-bit random ownership description");
    }
    lease.Poll(now+59s);{std::lock_guard lock(router->mutex);check(router->adds.size()==1,"no early renewal");}
    {std::lock_guard lock(router->mutex);router->address="8.8.4.4";}
    lease.Poll(now+60s);
    check(Until([&]{std::lock_guard lock(router->mutex);return router->adds.size()==2;}),"half-life renewal executes");
    check(Until([&]{lease.Poll(now+61s);return lease.status().state==RouterState::Active && lease.status().address=="8.8.4.4";}),"renewal republishes newly verified public address");
    lease.Stop();check(Idle(),"stop releases worker");
    {std::lock_guard lock(router->mutex);check(router->deletes==std::vector<uint16_t>{45001} && router->entries.empty() && router->adds.size()==2 && router->adds[1].second.description==first_tag,"stop deletes only verified owned lease and renewal keeps tag");}
    router=std::make_shared<FakeRouter>();router->entries[45001]={"127.0.0.1","manual mapping",45001,0,true};
    lease.Start("127.0.0.1","127.0.0.1",45001,now,Factory(router));check(active(lease,now),"occupied external port gets another candidate");
    check(lease.status().port!=45001 && lease.status().port>=49152,"alternative external port published");lease.Stop();check(Idle(),"collision worker cleanup");
    {std::lock_guard lock(router->mutex);check(router->entries.size()==1 && router->entries.contains(45001) && router->entries.at(45001).description=="manual mapping" && router->deletes.size()==1 && router->deletes[0]!=45001 && !router->adds.empty() && router->adds[0].second.description!=first_tag,"existing mapping preserved; fresh start replaces ownership tag");}
    for(const auto address:{"100.64.0.1","192.168.1.1","::1","203.0.113.1"}) {
        router=std::make_shared<FakeRouter>();router->address=address;lease.Start("127.0.0.1","127.0.0.1",45001,now,Factory(router));
        check(failed(lease,now) && Idle(),"nonpublic address fails before mapping");{std::lock_guard lock(router->mutex);check(router->adds.empty() && router->deletes.empty() && lease.status().address.empty(),"no unusable invitation or mapping");}lease.Stop();
    }
    for(uint32_t granted:{0u,121u}) {
        router=std::make_shared<FakeRouter>();router->granted=granted;lease.Start("127.0.0.1","127.0.0.1",45001,now,Factory(router));
        check(failed(lease,now) && Idle(),"permanent/excessive lease refused");{std::lock_guard lock(router->mutex);check(router->adds.size()==1 && router->adds[0].second.lifetime==120 && router->entries.empty() && router->deletes.size()==1,"owned unsupported lease cleaned without requesting permanent mapping");}lease.Stop();
    }
    router=std::make_shared<FakeRouter>();router->occupy_all=true;lease.Start("127.0.0.1","127.0.0.1",45001,now,Factory(router));
    check(failed(lease,now) && Idle(),"all occupied candidate ports fail");{std::lock_guard lock(router->mutex);check(router->adds.empty() && router->deletes.empty(),"collision exhaustion makes no changes");}lease.Stop();
    router=std::make_shared<FakeRouter>();router->add_ok=false;router->lost_ack=true;
    lease.Start("127.0.0.1","127.0.0.1",45001,now,Factory(router));check(failed(lease,now) && Idle(),"lost Add ACK fails without publishing");
    {std::lock_guard lock(router->mutex);check(router->entries.empty() && router->deletes==std::vector<uint16_t>{45001},"lost ACK still performs ownership-checked cleanup");}lease.Stop();
    router=std::make_shared<FakeRouter>();lease.Start("127.0.0.1","127.0.0.1",45001,now,Factory(router));check(active(lease,now),"ownership-replacement setup");
    {std::lock_guard lock(router->mutex);router->entries[45001].description="another process";}
    lease.Poll(now+60s);check(failed(lease,now+60s) && Idle(),"changed owner withdraws invitation");
    {std::lock_guard lock(router->mutex);check(router->adds.size()==1 && router->deletes.empty() && router->entries.contains(45001) && router->entries.at(45001).description=="another process","foreign replacement is neither renewed nor deleted");}lease.Stop();
    router=std::make_shared<FakeRouter>();lease.Start("127.0.0.1","127.0.0.1",45001,now,Factory(router));check(active(lease,now),"renewal-refusal setup");
    {std::lock_guard lock(router->mutex);router->add_ok=false;}
    lease.Poll(now+60s);check(failed(lease,now+60s) && Idle(),"refused renewal withdraws endpoint");
    {std::lock_guard lock(router->mutex);check(router->entries.empty() && router->deletes.size()==1 && router->adds.size()==2,"refused renewal cleans owned old lease");}lease.Stop();
    router=std::make_shared<FakeRouter>();router->granted=1;lease.Start("127.0.0.1","127.0.0.1",45001,now,Factory(router));check(active(lease,now),"one-second granted lease setup");
    {std::lock_guard lock(router->mutex);router->address="8.8.4.4";}
    lease.Poll(now+500ms);check(Until([&]{lease.Poll(now+501ms);return lease.status().state==RouterState::Active && lease.status().address=="8.8.4.4";}),"short lease renews before expiry");lease.Stop();check(Idle(),"short lease cleanup");
    // Cancel while a blocking RPC is in progress. The frontend must return
    // immediately; when Add finally succeeds, the detached owner removes it.
    router=std::make_shared<FakeRouter>();router->block_add=true;
    lease.Start("127.0.0.1","127.0.0.1",45001,now,Factory(router));check(Until([&]{std::lock_guard lock(router->mutex);return !router->adds.empty();}),"blocked Add entered");
    const auto stop_start=RouterMapping::Clock::now();lease.Stop();check(RouterMapping::Clock::now()-stop_start<500ms && lease.status().state==RouterState::Idle,"cancellation never waits on blocking router call");
    UpnpLease retry;check(!retry.Start("127.0.0.1","127.0.0.1",45001,now,Factory(router)),"cancel/retry cannot accumulate blocked workers");
    Release(router);check(Idle(),"late Add cancellation finishes");{std::lock_guard lock(router->mutex);check(router->entries.empty() && router->deletes.size()==1,"late successful Add cleaned after frontend owner gone");}
    router=std::make_shared<FakeRouter>();router->block_discover=true;lease.Start("127.0.0.1","127.0.0.1",45001,now,Factory(router));
    check(Until([&]{std::lock_guard lock(router->mutex);return router->entered;}),"blocked discovery entered");lease.Poll(now+31s);
    check(lease.status().state==RouterState::Failed && lease.status().address.empty(),"30-second deadline withdraws pending invitation");Release(router);check(Idle(),"timed-out discovery unwinds");
    {std::lock_guard lock(router->mutex);check(router->adds.empty(),"late discovery cannot create mapping after timeout");}lease.Stop();
    router=std::make_shared<FakeRouter>();lease.Start("127.0.0.1","127.0.0.1",45001,now,Factory(router));check(active(lease,now),"expiry setup");lease.Poll(now+121s);
    check(lease.status().state==RouterState::Failed && lease.status().address.empty() && Idle(),"expired lease withdrawn and cleaned");lease.Stop();
    // RouterMapping's real UDP deadline falls back only through the supplied
    // fake backend. No Windows discovery or home-gateway traffic occurs.
    SOCKET udp=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);sockaddr_in loop{};loop.sin_family=AF_INET;loop.sin_addr.s_addr=htonl(INADDR_LOOPBACK);int size=sizeof(loop);
    if(!check(udp!=INVALID_SOCKET && !bind(udp,reinterpret_cast<sockaddr*>(&loop),sizeof(loop)) && !getsockname(udp,reinterpret_cast<sockaddr*>(&loop),&size),"loopback UDP fixture")){if(udp!=INVALID_SOCKET)closesocket(udp);return false;}
    router=std::make_shared<FakeRouter>();RouterMapping mapping;
    check(mapping.Open("127.0.0.1",45001,"127.0.0.1",ntohs(loop.sin_port),now,true,Factory(router)),"PCP-first coordinator with fake fallback");
    mapping.Poll(now);mapping.Poll(now+31s);
    check(Until([&]{mapping.Poll(now+31s);return mapping.status().state==RouterState::Active || mapping.status().state==RouterState::Failed;}) && mapping.status().state==RouterState::Active,"silent UDP protocols select background UPnP fallback");
    mapping.Stop();check(Idle(),"coordinator cancellation cleans UPnP");
    // An explicit PCP authorization refusal must not select another protocol.
    std::array<uint8_t,1100> request{};sockaddr_in peer{};size=sizeof(peer);u_long nonblocking=1;ioctlsocket(udp,FIONBIO,&nonblocking);int got=0;
    while(recvfrom(udp,reinterpret_cast<char*>(request.data()),int(request.size()),0,reinterpret_cast<sockaddr*>(&peer),&size)>0){}
    router=std::make_shared<FakeRouter>();mapping.Open("127.0.0.1",45001,"127.0.0.1",ntohs(loop.sin_port),now,true,Factory(router));mapping.Poll(now);
    while((got=recvfrom(udp,reinterpret_cast<char*>(request.data()),int(request.size()),0,reinterpret_cast<sockaddr*>(&peer),&size))>0) {
        if(got==60 && request[0]==2 && request[1]==1){auto reply=request;reply[1]=129;reply[3]=2;sendto(udp,reinterpret_cast<const char*>(reply.data()),60,0,reinterpret_cast<sockaddr*>(&peer),size);}
    }
    check(Until([&]{mapping.Poll(now+1ms);return mapping.status().state==RouterState::Failed;}),"explicit PCP refusal remains failure");
    {std::lock_guard lock(router->mutex);check(!router->entered && router->adds.empty(),"authorization refusal does not invoke UPnP");}mapping.Stop();
    // Transfer the pending fallback into a real ready TCP session. The frontend
    // object disappears, but Session::Poll must keep renewing the UPnP lease.
    Session host,client;Identity identity;identity.build.fill(1);identity.content.fill(2);identity.checkpoint.fill(3);identity.session[0]=19;
    if(!check(host.Host(identity,"127.0.0.1") && client.Join(identity,"127.0.0.1",host.port()),"real TCP ownership fixture connects")){closesocket(udp);return false;}
    check(Until([&]{host.Poll();client.Poll();return host.state()==State::Ready && client.state()==State::Ready;}),"real TCP peers ready");
    router=std::make_shared<FakeRouter>();router->block_discover=true;router->granted=2;
    const auto session_now=RouterMapping::Clock::now();const auto listener_port=host.port();
    auto retained=std::make_unique<RouterMapping>();
    retained->Open("127.0.0.1",listener_port,"127.0.0.1",ntohs(loop.sin_port),session_now-31s,true,Factory(router));retained->Poll(session_now-31s);retained->Poll(session_now);
    check(Until([&]{std::lock_guard lock(router->mutex);return router->entered;}),"pending UPnP worker ready for handoff");
    check(!client.KeepRouterMapping(retained) && retained && host.KeepRouterMapping(retained) && !retained,"only host takes pending fallback for its exact listener");
    Release(router);
    check(Until([&]{host.Poll(session_now);client.Poll(session_now);std::lock_guard lock(router->mutex);return router->adds.size()==1;}),"session polls setup after frontend ownership ends");
    // Consume the successful grant before advancing the simulated lease clock.
    for(unsigned i=0;i<10;++i){host.Poll(session_now);client.Poll(session_now);std::this_thread::sleep_for(1ms);}
    {std::lock_guard lock(router->mutex);router->address="8.8.4.4";}
    check(Until([&]{host.Poll(session_now+1s);client.Poll(session_now+1s);std::lock_guard lock(router->mutex);return router->adds.size()==2;}),"retained session renews pending UPnP ownership at half-life");
    Pad a{},b{};a.buttons=7;b.buttons=3;Frame ha{},cb{};bool got_host=false,got_client=false;
    check(host.SubmitLocal(0,a) && client.SubmitLocal(0,b),"canonical TCP input submits with retained UPnP lease");
    check(Until([&]{host.Poll(session_now+1s);client.Poll(session_now+1s);if(!got_host)got_host=host.TakeFrame(ha);if(!got_client)got_client=client.TakeFrame(cb);return got_host && got_client;} ) && ha==cb && ha.players[0]==a && ha.players[1]==b,"canonical TCP frame agrees after UPnP handoff/renewal");
    host.Disconnect();client.Disconnect();check(Idle(),"session disconnect finishes UPnP cleanup");
    {std::lock_guard lock(router->mutex);check(router->entries.empty() && router->deletes==std::vector<uint16_t>{listener_port} && router->adds.size()==2,"session disconnect deletes exact owned external TCP lease");}
    closesocket(udp);
    if(ok)REXLOG_INFO("UPnP lease/coordinator passed {} checks: background setup/renewal, collision/ownership/permanent/CGNAT/lost-ACK guards, nonblocking cancel, late-call cleanup, deadline/expiry, real loopback UDP fallback and real TCP session ownership/input/renewal/cleanup; no actual router contacted",checks);
    return ok;
}
}
