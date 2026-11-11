#include "online_room_bootstrap.h"
#include "online_runtime.h"
#include "controls.h"
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace em2::net {
bool TestRoomBootstrapGate(){
    struct Environment {unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(csr);SetLastError(error);}} environment;
    unsigned checks=0;
    auto check=[&](bool value,const char* why){++checks;if(!value)REXLOG_ERROR("Room native bootstrap gate: {}",why);return value;};
    if(!check(!RoomBootstrapActive() && !LegacyNativeSessionActive() && !em2::SynchronizedControlsGeneration(),
        "fixture starts without borrowed controls or native session ownership"))return false;
    struct LegacyCleanup {~LegacyCleanup(){if(LegacyNativeSessionActive())ReleaseNativeOnlineSession();}} legacy_cleanup;
    using Clock=std::chrono::steady_clock;
    Identity identity;identity.build[0]=1;identity.content[0]=2;identity.checkpoint[0]=3;identity.session[0]=4;
    identity.first_tick=100;identity.input_delay=2;identity.startup_policy=StartupPolicy::NativeMenu;
    identity.initialization_policy=InitializationPolicy::SharedNativeV1;
    std::array<uint8_t,32> world{};world[0]=9;
    std::array<uint8_t,32> observation{};observation[0]=17; // Explicit fixture digest, no native world claimed.
    std::vector<std::unique_ptr<Room>> rooms;
    RoomBootstrapGate gate; // Must detach before the borrowed Room objects die.
    auto pump=[&](auto complete){const auto deadline=Clock::now()+std::chrono::seconds(5);
        do{for(auto& room:rooms)room->Poll();if(complete())return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(Clock::now()<deadline);return false;};
    auto setup=[&](){
        gate.Detach();rooms.clear();rooms.push_back(std::make_unique<Room>());
        if(!rooms[0]->Host(identity,"127.0.0.1",0,3,"Host"))return false;
        for(unsigned i=1;i<3;++i){rooms.push_back(std::make_unique<Room>());
            if(!rooms.back()->Join(identity,"127.0.0.1",rooms[0]->port(),"Guest "+std::to_string(i)))return false;}
        if(!pump([&]{return std::all_of(rooms.begin(),rooms.end(),[](const auto& r){return r->lobby().players.size()==3;});}))return false;
        for(auto& room:rooms)if(!room->SetReady(true))return false;
        if(!pump([&]{return std::all_of(rooms[0]->lobby().players.begin(),rooms[0]->lobby().players.end(),[](const auto& p){return p.ready;}) &&
            std::none_of(rooms.begin(),rooms.end(),[](const auto& r){return r->profile_pending();});}))return false;
        if(!rooms[0]->StartNativeBootstrap() || !pump([&]{return std::all_of(rooms.begin(),rooms.end(),[](const auto& r){return r->state()==RoomState::Loading;});}))return false;
        for(auto& room:rooms)if(!room->BootstrapWorldReady(world))return false;
        return pump([&]{return std::all_of(rooms.begin(),rooms.end(),[](const auto& r){return r->state()==RoomState::Bootstrapping;});}) && gate.Attach(*rooms[0]);
    };
    auto remote_submit=[&](uint64_t tick,bool invalid=false){Pad start;start.buttons=0x10;Pad third;if(invalid)third.left_x=1;
        return rooms[1]->SubmitBootstrapPad(tick,start) && rooms[2]->SubmitBootstrapPad(tick,third);};
    auto await_raw=[&](){return pump([&]{RoomBootstrapFrame raw;return rooms[0]->PeekBootstrapFrame(raw);});};
    auto remote_finish=[&](uint64_t tick,bool ready){
        for(unsigned i=1;i<3;++i){RoomBootstrapFrame frame;
            if(!rooms[i]->TakeBootstrapFrame(frame) || frame.tick!=tick || !rooms[i]->CompleteBootstrap(tick,ready,observation))return false;}
        return true;
    };
    auto unchanged=[&](uint64_t generation,const Frame& expected){uint64_t actual=0;Frame frame;
        return em2::ReadPublishedSynchronizedControls(actual,frame) && actual==generation && frame==expected;};
    if(!check(setup(),"three real TCP peers enter SharedNativeV1 bootstrap and only host acquires native controls"))return false;
    if(!check(RoomBootstrapActive() && gate.active() && gate.Owns(*rooms[0]) && !gate.Owns(*rooms[1]) &&
        !em2::BeginSynchronizedControls(100),
        "one native owner excludes a second controls lease and distinguishes its actual Room"))return false;
    {uint64_t absent_generation=0;Frame absent;
        if(!check(!gate.PublishedRetailFrame(absent_generation,absent) && !em2::ReadPublishedSynchronizedControls(absent_generation,absent),
            "attachment alone cannot expose an unpublished retail controller frame"))return false;
    }
    {
        auto legacy=std::make_unique<Session>();Session client;
        if(!check(legacy->Host(identity,"127.0.0.1") && client.Join(identity,"127.0.0.1",legacy->port()),"real legacy exclusion fixture connects"))return false;
        const auto deadline=Clock::now()+std::chrono::seconds(3);
        do{legacy->Poll();client.Poll();if(legacy->state()==State::Ready && client.state()==State::Ready)break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(Clock::now()<deadline);
        const auto* pointer=legacy.get();
        if(!check(legacy->state()==State::Ready && client.state()==State::Ready && !AdoptNativeOnlineSession(legacy) && legacy.get()==pointer &&
            !LegacyNativeSessionActive() && gate.active(),"active Room native gate rejects legacy adoption without stealing caller's Ready Session"))return false;
    }
    RoomBootstrapFrame frame;
    {
        const auto generation=em2::SynchronizedControlsGeneration();BootstrapAdmission result=BootstrapAdmission::Inactive;
        RoomBootstrapFrame foreign;foreign.tick=999;const auto before=foreign;
        std::thread other([&]{result=gate.Prepare(foreign);});other.join();
        if(!check(result==BootstrapAdmission::WrongThread && foreign==before && em2::SynchronizedControlsGeneration()==generation &&
            rooms[0]->SubmitBootstrapPad(99,{})==false,"foreign-thread preparation has no output, control or transport effect"))return false;
    }
    uint64_t generation=0;Frame retail;
    for(unsigned round=0;round<2;++round){
        const uint64_t tick=identity.first_tick+round;
        if(!check(gate.Prepare(frame)==BootstrapAdmission::Waiting && gate.Prepare(frame)==BootstrapAdmission::Waiting &&
            !rooms[0]->SubmitBootstrapPad(tick,{}) && !gate.SetJoining(false),
            "Prepare submits host neutral once and retains its exact join policy while waiting for peers"))return false;
        if(!check(remote_submit(tick) && await_raw() && gate.Prepare(frame)==BootstrapAdmission::Ready,
            "peer two Start and peer three neutral form a real authoritative bootstrap frame"))return false;
        RoomBootstrapFrame peek,cached;
        if(!check(frame.tick==tick && frame.players_mask==7 && frame.players[0]==Pad{} && frame.players[1].buttons==0x10 && frame.players[2]==Pad{} &&
            gate.Prepare(cached)==BootstrapAdmission::Ready && cached==frame && rooms[0]->PeekBootstrapFrame(peek) && peek==frame,
            "cached Prepare neither consumes nor replaces the agreed raw frame"))return false;
        auto changed=frame;changed.players[0].left_x=1;
        if(!check(!gate.Commit(changed) && gate.Commit(frame) && !gate.Commit(frame) && gate.completion_pending() &&
            gate.PublishedRetailFrame(generation,retail) && generation && retail.tick==tick && retail.players[0]==frame.players[0] && retail.players[1]==frame.players[1] &&
            unchanged(generation,retail),"exact Commit publishes both retail lanes once under a nonzero controls generation"))return false;
        const auto published_generation=generation;
        if(!check(!gate.SetJoining(false) && gate.Prepare(cached)==BootstrapAdmission::Waiting && unchanged(generation,retail),
            "pending native finish blocks further Prepare and policy changes without replacing publication"))return false;
        const bool ready=round==1;
        if(!check(gate.Complete(ready,observation) && !gate.Complete(ready,observation) && !gate.PublishedRetailFrame(generation,retail) &&
            unchanged(published_generation,retail),"one explicit completed observation ends the pending phase but retains the full published frame"))return false;
        generation=published_generation;
        if(!check(pump([&]{RoomBootstrapFrame raw;return rooms[1]->PeekBootstrapFrame(raw) && rooms[2]->PeekBootstrapFrame(raw);}),"remote peers receive exactly the host's raw round"))return false;
        RoomBootstrapFrame remote;
        if(!check(rooms[1]->TakeBootstrapFrame(remote) && remote==frame && rooms[1]->CompleteBootstrap(tick,ready,observation),
            "second peer consumes and acknowledges the same completed fixture observation"))return false;
        for(unsigned i=0;i<4;++i)for(auto& room:rooms)room->Poll();
        if(!check(rooms[0]->state()==RoomState::Bootstrapping && rooms[0]->bootstrap_tick()==tick &&
            gate.Prepare(cached)==BootstrapAdmission::Waiting && unchanged(generation,retail),"delayed final completion freezes the next raw round and last native publication"))return false;
        if(!check(rooms[2]->TakeBootstrapFrame(remote) && remote==frame && rooms[2]->CompleteBootstrap(tick,ready,observation),"last peer explicitly completes the same round"))return false;
        if(!check(pump([&]{return std::all_of(rooms.begin(),rooms.end(),[&](const auto& room){return ready?room->state()==RoomState::Running:
            room->state()==RoomState::Bootstrapping && room->bootstrap_tick()==tick+1;});}),"only unanimous same-round ready votes activate gameplay"))return false;
    }
    const auto last=identity.first_tick+1;
    if(!check(rooms[0]->next_tick()==last+1 && rooms[0]->next_submit_tick()==last+1+identity.input_delay &&
        gate.Prepare(frame)==BootstrapAdmission::Gameplay && gate.Prepare(frame)==BootstrapAdmission::Gameplay &&
        !em2::BeginSynchronizedGameplaySampling(generation,last) && unchanged(generation,retail) && gate.active() && RoomBootstrapActive() &&
        !em2::BeginSynchronizedControls(last+1),"Gameplay cutover rebases unused sampling exactly once, retains delay and preserves generation/publication/ownership"))return false;
    gate.Detach();
    if(!check(!RoomBootstrapActive() && !gate.active() && !em2::ReadPublishedSynchronizedControls(generation,retail) &&
        em2::BeginSynchronizedControls(500),"only explicit Detach releases publication and controls/RNG ownership"))return false;
    em2::EndSynchronizedControls();
    for(unsigned failure=0;failure<3;++failure){
        if(!check(setup(),"failure-isolation fixture enters a fresh real three-peer bootstrap"))return false;
        if(!check(gate.Prepare(frame)==BootstrapAdmission::Waiting && remote_submit(100) && await_raw() &&
            gate.Prepare(frame)==BootstrapAdmission::Ready && gate.Commit(frame) && em2::ReadPublishedSynchronizedControls(generation,retail),
            "failure fixture first establishes a real immutable native publication"))return false;
        if(failure==0){
            if(!check(gate.Complete(false,observation) && pump([&]{RoomBootstrapFrame raw;return rooms[1]->PeekBootstrapFrame(raw) && rooms[2]->PeekBootstrapFrame(raw);}) &&
                remote_finish(100,false) && pump([&]{return std::all_of(rooms.begin(),rooms.end(),[](const auto& room){return room->state()==RoomState::Bootstrapping && room->bootstrap_tick()==101;});}) && gate.Prepare(frame)==BootstrapAdmission::Waiting &&
                remote_submit(101,true) && await_raw(),"malformed third peer intent arrives in a later real raw frame"))return false;
        }else if(failure==1){rooms[2]->Disconnect();if(!pump([&]{return rooms[0]->state()==RoomState::Error;}))return check(false,"disconnect reaches host");}
        else rooms[0]->Poll(Clock::now()+std::chrono::seconds(181));
        if(!check(gate.Prepare(frame)==BootstrapAdmission::Stopped && !gate.error().empty() && unchanged(generation,retail) &&
            gate.active() && RoomBootstrapActive() && !gate.Commit(frame) && !gate.Complete(false,observation) && !em2::BeginSynchronizedControls(999),
            "noncanonical extra movement, disconnect or absolute stall freezes the last native frame until Detach"))return false;
        gate.Detach();if(!check(!RoomBootstrapActive() && !em2::ReadPublishedSynchronizedControls(generation,retail),"failure cleanup explicitly releases its owned controls generation"))return false;
    }
    REXLOG_INFO("Room native bootstrap gate passed {} checks: real three-peer TCP, canonical retail publication, finish/ready barriers, sampler cutover and frozen failure ownership; fixture digests only, no native world or gameplay executed",checks);
    return true;
}
}
