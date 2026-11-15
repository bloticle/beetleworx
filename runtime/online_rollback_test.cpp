#include "online_rollback.h"
#include "online_live_lobby.h"
#include <rex/logging.h>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <set>
#include <algorithm>

namespace em2::net {
namespace {
// Allocated deterministic test world, not an EM2 native-state adapter. Dynamic
// ownership, contact/edge history, RNG and quest state all survive restoration.
struct FixtureWorld final:RollbackWorld {
    struct State {
        uint64_t tick=0,rng=0x12345678,quest=0,items=0,contacts=0;
        std::array<int64_t,16> x{},y{},vy{},health{},held{},avatars{};
        bool operator==(const State&)const=default;
    } state;
    uint32_t capabilities=RequiredRollbackCapabilities;
    unsigned captures=0,restores=0,steps=0,sounds=0;
    bool fail_replay=false,throw_replay=false,fail_restore=false;
    explicit FixtureWorld(uint16_t mask=3){for(unsigned i=0;i<16;++i)if(mask&(1u<<i)){state.avatars[i]=i+1;state.health[i]=5;}}
    uint32_t Capabilities()const noexcept override{return capabilities;}
    bool Capture(RollbackBytes& bytes)override{++captures;bytes.resize(sizeof(state));std::memcpy(bytes.data(),&state,sizeof(state));return true;}
    bool Restore(std::span<const uint8_t> bytes)override{++restores;if(fail_restore || bytes.size()!=sizeof(state))return false;std::memcpy(&state,bytes.data(),sizeof(state));return true;}
    bool Step(const RollbackInput& input,RollbackStep mode)override{
        ++steps;if(input.tick!=state.tick)return false;
        if(mode==RollbackStep::Replay && (fail_replay || throw_replay)){state.x[0]+=999;if(throw_replay)throw std::runtime_error("injected native replay failure");return false;}
        for(unsigned i=0;i<16;++i){if(!(input.players_mask&(1u<<i)))continue;if(!state.avatars[i])return false;
            const auto& pad=input.players[i];const auto edges=uint16_t(pad.buttons&~state.held[i]);
            state.x[i]+=pad.left_x/4096;
            if((edges&0x1000) && state.y[i]==0){state.vy[i]=9;if(mode==RollbackStep::Live)++sounds;}
            if(state.y[i] || state.vy[i]){state.y[i]+=state.vy[i];--state.vy[i];if(state.y[i]<=0){state.y[i]=state.vy[i]=0;++state.contacts;}}
            if(edges&0x4000){state.rng=(state.rng*214013+2531011)&0xffffffffu;state.items+=(state.rng>>16)&3;++state.quest;--state.health[(i+1)%16];if(mode==RollbackStep::Live)++sounds;}
            state.held[i]=pad.buttons;
        }
        ++state.tick;return true;
    }
};
RollbackScope Scope(uint16_t mask=3){RollbackScope s;s.session[0]=13;s.schema[0]=7;s.world_epoch=s.roster_revision=1;s.players_mask=mask;return s;}
RollbackLimits Limits(){RollbackLimits l;l.history_frames=32;l.max_prediction=16;l.future_inputs=32;l.snapshot_bytes=4096;l.history_bytes=64*1024;return l;}
Pad Input(unsigned peer,uint64_t tick){Pad p;p.left_x=int16_t(peer==1?(tick%2?8192:-8192):12288);if(tick>=2)p.buttons=0x1000;if(tick>=5 && tick<8)p.buttons|=0x4000;return p;}
}
bool TestOnlineRollback(){
    unsigned checks=0;auto check=[&](bool ok,const char* why){++checks;if(!ok)REXLOG_ERROR("Authoritative rollback: {}",why);return ok;};
    FixtureWorld unsupported;unsupported.capabilities&=~FullSimulationState;RollbackSession blocked(unsupported,RollbackRole::Host,Limits());
    if(!check(blocked.Begin(Scope())==RollbackResult::Rejected && unsupported.captures==0,"transform-only/checkpoint adapter cannot enable rollback"))return false;
    FixtureWorld baseline,predicted;RollbackSession a(baseline,RollbackRole::Host,Limits()),b(predicted,RollbackRole::Host,Limits());
    if(!check(a.Begin(Scope())==RollbackResult::Ready && b.Begin(Scope())==RollbackResult::Ready,"host captures a complete initial simulation boundary"))return false;
    std::vector<RollbackCommand> delayed;
    for(uint64_t tick=0;tick<12;++tick){
        const RollbackCommand commands[]={{1,tick,Input(1,tick)},{2,tick,Input(2,tick)}};
        if(a.SubmitInputs(commands)!=RollbackResult::Accepted || b.SubmitInput(1,tick,commands[0].pad)!=RollbackResult::Accepted ||
           a.Advance()!=RollbackResult::Advanced || b.Advance()!=RollbackResult::Advanced)return false;
        delayed.push_back(commands[1]);
    }
    const auto sounds=predicted.sounds;
    if(!check(b.SubmitInputs(delayed)==RollbackResult::Corrected && baseline.state==predicted.state && b.next_tick()==12 &&
       b.stats().restores==1 && b.stats().replayed_frames==12 && predicted.sounds==sounds,
       "batched late input restores and replays actual physics/edge/RNG/quest state once without duplicate sounds"))return false;
    if(!check(b.SubmitInputs(delayed)==RollbackResult::Duplicate,"duplicate input packets cannot replay the world twice"))return false;
    auto conflict=delayed.front();conflict.pad.buttons^=1;const auto stable=predicted.state;
    if(!check(b.SubmitInput(conflict.peer,conflict.tick,conflict.pad)==RollbackResult::Rejected && predicted.state==stable,"a known actual input cannot be rewritten"))return false;
    RollbackResult foreign=RollbackResult::Ready;std::thread other([&]{foreign=b.Advance();});other.join();
    if(!check(foreign==RollbackResult::WrongThread && predicted.state==stable,"foreign threads cannot run native restore/replay"))return false;
    const RollbackCommand invalid_batch[]={{1,12,Input(1,12)},{17,12,{}}};
    if(!check(b.SubmitInputs(invalid_batch)==RollbackResult::Rejected && b.SubmitInput(1,12,Input(1,12))==RollbackResult::Accepted,
       "invalid batched commands cannot partially commit valid input"))return false;

    FixtureWorld host_world,client_world;RollbackSession host(host_world,RollbackRole::Host,Limits()),client(client_world,RollbackRole::Client,Limits());
    if(host.Begin(Scope())!=RollbackResult::Ready || client.Begin(Scope())!=RollbackResult::Ready)return false;
    RollbackBytes correction;
    for(uint64_t tick=0;tick<10;++tick){
        Pad remote;remote.left_x=12288;if(tick>=2)remote.buttons=0x1000;
        const RollbackCommand commands[]={{1,tick,Input(1,tick)},{2,tick,remote}};
        if(host.SubmitInputs(commands)!=RollbackResult::Accepted || host.Advance()!=RollbackResult::Advanced ||
           client.SubmitInput(1,tick,commands[0].pad)!=RollbackResult::Accepted || client.Advance()!=RollbackResult::Advanced)return false;
        if(tick==3){auto packet=host.ExportAuthoritative();if(!packet)return false;correction=std::move(*packet);}
    }
    if(!check(client_world.state!=host_world.state && client.AcceptAuthoritative(correction)==RollbackResult::Corrected &&
       client_world.state==host_world.state && client.stats().replayed_frames==6 && client.next_tick()==10,
       "host correction restores boundary four and replays six newer local inputs, including held-jump history"))return false;
    auto latest=host.ExportAuthoritative();if(!latest)return false;
    if(!check(client.AcceptAuthoritative(*latest)==RollbackResult::Corrected && client_world.state==host_world.state &&
       client.AcceptAuthoritative(correction)==RollbackResult::Duplicate,"out-of-order authoritative snapshots cannot rewind a newer correction"))return false;
    const auto restore_count=client_world.restores;auto broken=*latest;broken[broken.size()/2]^=1;
    if(!check(client.AcceptAuthoritative(broken)==RollbackResult::Rejected && client_world.restores==restore_count,
       "corrupt snapshot is rejected before any world callback"))return false;
    auto decoded=DecodeRollbackSnapshot(*latest,4096);if(!decoded)return false;
    for(unsigned field=0;field<4;++field){auto changed=*decoded;
        if(field==0)changed.scope.session[0]^=1;else if(field==1)++changed.scope.world_epoch;else if(field==2)++changed.scope.roster_revision;else changed.scope.schema[0]^=1;
        auto packet=EncodeRollbackSnapshot(changed,4096);
        if(!check(packet && client.AcceptAuthoritative(*packet)==RollbackResult::Rejected && client_world.restores==restore_count,
            "other rooms, worlds, rosters and state schemas cannot restore this world"))return false;
    }
    if(!check(host.AcceptAuthoritative(*latest)==RollbackResult::Rejected,"clients cannot replace the host's authoritative world"))return false;
    if(!check(!DecodeRollbackSnapshot(*latest,8) && !EncodeRollbackSnapshot(*decoded,8),"state limits are checked before accepting or serializing large payloads"))return false;
    for(size_t length:{size_t(0),size_t(4),latest->size()-1})if(!check(!DecodeRollbackSnapshot(std::span(*latest).first(length),4096),"truncated envelopes cannot reach state restore"))return false;

    FixtureWorld joined_world(0);RollbackSession joined(joined_world,RollbackRole::Client,Limits());
    if(!check(joined.BeginFromAuthoritative(Scope(),*latest)==RollbackResult::Ready && joined.next_tick()==10 && joined_world.state==host_world.state,
       "late join installs current dynamic actors, physics, input-edge, RNG and quest state without a menu/save restart"))return false;
    FixtureWorld tight_world;auto tight_limits=Limits();tight_limits.history_frames=2;tight_limits.max_prediction=2;
    RollbackSession tight(tight_world,RollbackRole::Client,tight_limits);if(tight.Begin(Scope())!=RollbackResult::Ready)return false;
    if(!check(tight.Advance()==RollbackResult::Advanced && tight.Advance()==RollbackResult::Advanced && tight.Advance()==RollbackResult::Waiting,
       "missing authority is bounded to the prediction window instead of unbounded drift"))return false;
    if(!check(tight.AcceptAuthoritative(*latest)==RollbackResult::Corrected && tight.next_tick()==10 && tight_world.state==host_world.state,
       "a full host snapshot can safely catch up a client without fabricating replayed frames"))return false;
    if(!check(tight.SubmitInput(1,0,{})==RollbackResult::TooOld && tight.retained_bytes()<=tight_limits.history_bytes,"expired input and retained state memory stay bounded"))return false;

    for(bool throws:{false,true}){
        FixtureWorld failing;RollbackSession session(failing,RollbackRole::Host,Limits());if(session.Begin(Scope())!=RollbackResult::Ready)return false;
        for(unsigned i=0;i<8;++i)if(session.Advance()!=RollbackResult::Advanced)return false;
        const auto before=failing.state;failing.fail_replay=!throws;failing.throw_replay=throws;
        if(!check(session.SubmitInput(2,2,Input(2,2))==RollbackResult::NeedFullState && session.next_tick()==8 && failing.state==before &&
           session.Advance()==RollbackResult::NeedFullState,"failed or throwing replay rolls back the transaction and requests a full-state boundary"))return false;
    }
    FixtureWorld lost;RollbackSession broken_restore(lost,RollbackRole::Host,Limits());if(broken_restore.Begin(Scope())!=RollbackResult::Ready)return false;
    if(broken_restore.Advance()!=RollbackResult::Advanced)return false;lost.fail_restore=true;
    if(!check(broken_restore.SubmitInput(2,0,Input(2,0))==RollbackResult::Faulted && broken_restore.Advance()==RollbackResult::Faulted,
       "failure to restore either correction or recovery state faults closed"))return false;
    FixtureWorld connection_world;RollbackSession connection(connection_world,RollbackRole::Host,Limits());
    if(connection.Begin(Scope())!=RollbackResult::Ready || connection.SubmitInput(2,0,Input(2,0))!=RollbackResult::Accepted || connection.Advance()!=RollbackResult::Advanced)return false;
    if(connection.SubmitInput(2,3,Input(2,3))!=RollbackResult::Accepted)return false;
    const auto stopped_x=connection_world.state.x[1];
    if(!check(connection.SetPeerConnected(2,false)==RollbackResult::Accepted && connection.SubmitInput(2,1,Input(2,1))==RollbackResult::Rejected &&
       connection.Advance()==RollbackResult::Advanced && connection.Advance()==RollbackResult::Advanced && connection_world.state.x[1]==stopped_x,
       "host continues immediately while disconnected player input is neutral and stale packets are rejected"))return false;
    if(!check(connection.SetPeerConnected(2,true)==RollbackResult::Accepted && connection.Advance()==RollbackResult::Advanced && connection_world.state.x[1]==stopped_x,
       "rejoin never executes queued future controls from the previous connection"))return false;
    if(!check(connection.SubmitInput(1,1,Input(1,1))==RollbackResult::Corrected && connection_world.state.x[1]==stopped_x,
       "rolling back another player's late input preserves the disconnected player's neutral input history"))return false;
    REXLOG_INFO("Authoritative rollback passed {} checks: actual restore/replay, delayed/batched input, host corrections, held edges, replay side effects, late join, bounds, packet integrity, scope fences and failure recovery; allocated fixture world, not native EM2 gameplay",checks);
    return true;
}

bool TestLiveOnlineLobby(){
    unsigned checks=0;auto check=[&](bool ok,const char* why){++checks;if(!ok)REXLOG_ERROR("Live online lobby: {}",why);return ok;};
    FixtureWorld world(1);RollbackSession host(world,RollbackRole::Host,Limits());auto scope=Scope(1);
    if(host.Begin(scope)!=RollbackResult::Ready)return false;
    for(unsigned i=0;i<5;++i)if(host.Advance()!=RollbackResult::Advanced)return false;
    LiveLobby lobby;const auto before=world.state;
    if(!check(lobby.Open(host,"Host")==LiveLobbyResult::Accepted && world.state==before && host.next_tick()==5,
       "opening a lobby from a running world neither reloads the game nor discards campaign progress"))return false;
    LiveAdmission guest;if(lobby.Join("Friend",guest)!=LiveLobbyResult::Accepted)return false;
    if(!check(guest.slot==2 && guest.player_id && !lobby.SnapshotFor(2),"join reserves a slot and unique color but waits for a real host avatar snapshot"))return false;
    world.state.avatars[1]=guest.player_id;world.state.health[1]=5;scope.players_mask=3;++scope.roster_revision;
    if(host.Begin(scope,5)!=RollbackResult::Ready)return false;auto packet=host.ExportAuthoritative();if(!packet || lobby.Publish(*packet)!=LiveLobbyResult::Accepted)return false;
    auto state=lobby.SnapshotFor(2);if(!state)return false;auto snapshot=DecodeRollbackSnapshot(*state->snapshot,4096);if(!snapshot)return false;
    if(!check(lobby.PrepareHostFrame(host)==LiveLobbyResult::Accepted && host.SubmitInput(2,5,Input(2,5))==RollbackResult::Rejected,
       "pending join cannot move an avatar before installing and acknowledging its snapshot"))return false;
    FixtureWorld client_world(0);RollbackSession client(client_world,RollbackRole::Client,Limits());
    if(!check(client.BeginFromAuthoritative(scope,*state->snapshot)==RollbackResult::Ready && client_world.state==world.state && client.next_tick()==5,
       "mid-game join receives the actual current host snapshot"))return false;
    auto wrong=scope;++wrong.world_epoch;
    if(!check(lobby.Acknowledge(2,guest.ticket,wrong,5,snapshot->revision)==LiveLobbyResult::StaleSnapshot &&
       lobby.Acknowledge(2,guest.ticket,scope,5,snapshot->revision)==LiveLobbyResult::Accepted,"control activates only after the exact world/roster snapshot acknowledgement"))return false;
    if(!check(lobby.PrepareHostFrame(host)==LiveLobbyResult::Accepted && host.SubmitInput(2,6,Input(2,6))==RollbackResult::Accepted,
       "acknowledged player receives host input authority at the next boundary"))return false;
    const auto now=LiveLobby::Clock::now();if(lobby.Drop(2,now)!=LiveLobbyResult::Accepted)return false;
    if(!check(lobby.PrepareHostFrame(host)==LiveLobbyResult::Accepted && host.SubmitInput(2,5,Input(2,5))==RollbackResult::Rejected,
       "disconnect revokes player input without stopping the host simulation"))return false;
    LiveAdmission third;if(lobby.Join("Other friend",third)!=LiveLobbyResult::Accepted)return false;
    if(!check(lobby.SetAppearance(third.slot,LobbyCharacter::Oswald,1)==LiveLobbyResult::ColorInUse,"a reconnecting player's pants/name color remains reserved"))return false;
    RejoinTicket invalid=guest.ticket;invalid[0]^=1;LiveAdmission resumed;
    if(!check(lobby.Resume(2,invalid,now,resumed)==LiveLobbyResult::WrongState && lobby.Resume(2,guest.ticket,now,resumed)==LiveLobbyResult::Accepted &&
       resumed.player_id==guest.player_id && resumed.slot==guest.slot && resumed.ticket!=guest.ticket,
       "rejoin preserves the avatar/slot and rotates its private credential"))return false;
    auto rejoin=lobby.SnapshotFor(2);if(!rejoin)return false;
    if(!check(lobby.Acknowledge(2,guest.ticket,scope,5,snapshot->revision)==LiveLobbyResult::WrongState &&
       lobby.Acknowledge(2,resumed.ticket,scope,5,snapshot->revision)==LiveLobbyResult::Accepted,"old reconnect credentials cannot acknowledge or take over a restored player"))return false;
    if(!check(lobby.Leave(2)==LiveLobbyResult::Accepted && lobby.CommitDeparture(2)==LiveLobbyResult::StaleSnapshot && lobby.open(),
       "leaving does not close the lobby or free a still-live avatar's color"))return false;
    world.state.avatars[1]=0;scope.players_mask=1;++scope.roster_revision;
    if(host.Begin(scope,5)!=RollbackResult::Ready)return false;packet=host.ExportAuthoritative();if(!packet || lobby.Publish(*packet)!=LiveLobbyResult::Accepted)return false;
    if(!check(lobby.CommitDeparture(2)==LiveLobbyResult::Accepted && lobby.open() && lobby.Resume(2,resumed.ticket,now,resumed)==LiveLobbyResult::UnknownPlayer,
       "departure commits only after a host snapshot proves removal; stale tickets cannot resurrect the avatar"))return false;
    LiveAdmission replacement;if(lobby.Join("Replacement",replacement)!=LiveLobbyResult::Accepted)return false;
    if(!check(replacement.slot==2 && replacement.player_id!=guest.player_id,"reused slots receive a fresh stable player identity"))return false;
    for(unsigned i=4;i<=16;++i){LiveAdmission join;if(lobby.Join("Player "+std::to_string(i),join)!=LiveLobbyResult::Accepted)return false;}
    std::set<unsigned> colors;for(const auto& p:lobby.players())colors.insert(p.profile.color);
    LiveAdmission extra;
    if(!check(lobby.players().size()==16 && colors.size()==16 && lobby.Join("Overflow",extra)==LiveLobbyResult::Full,
       "sixteen live/pending participants have unique colors and bounded capacity"))return false;
    if(!check(lobby.Kick(third.slot)==LiveLobbyResult::Accepted && lobby.Kick(1)==LiveLobbyResult::Invalid && lobby.open(),
       "host kick leaves other players and the running lobby intact"))return false;
    if(lobby.Drop(2,now)!=LiveLobbyResult::Accepted || lobby.Drop(2,now+std::chrono::seconds(119))!=LiveLobbyResult::Accepted)return false;
    lobby.Expire(now+std::chrono::seconds(121));
    const auto players=lobby.players();const auto entry=std::find_if(players.begin(),players.end(),[](const auto& p){return p.profile.peer==2;});
    if(!check(entry!=players.end() && entry->presence==LivePresence::Leaving,"repeated disconnect notifications cannot extend the reconnect deadline forever"))return false;
    lobby.Close();if(!check(!lobby.open() && lobby.players().empty(),"only explicit host closure tears down the lobby"))return false;
    FixtureWorld travelling_world(1);RollbackSession travelling(travelling_world,RollbackRole::Host,Limits());auto travel_scope=Scope(1);LiveLobby travel_lobby;
    if(travelling.Begin(travel_scope)!=RollbackResult::Ready || travel_lobby.Open(travelling,"Host")!=LiveLobbyResult::Accepted)return false;
    LiveAdmission traveller;if(travel_lobby.Join("Traveller",traveller)!=LiveLobbyResult::Accepted)return false;
    travelling_world.state.avatars[1]=traveller.player_id;travel_scope.players_mask=3;++travel_scope.roster_revision;
    if(travelling.Begin(travel_scope)!=RollbackResult::Ready)return false;auto travel_packet=travelling.ExportAuthoritative();
    if(!travel_packet || travel_lobby.Publish(*travel_packet)!=LiveLobbyResult::Accepted)return false;
    auto travel_snapshot=travel_lobby.SnapshotFor(2);if(!travel_snapshot)return false;
    if(travel_lobby.Acknowledge(2,traveller.ticket,travel_scope,0,1)!=LiveLobbyResult::Accepted || travel_lobby.Drop(2,now)!=LiveLobbyResult::Accepted)return false;
    travelling_world.state.quest=87;++travel_scope.world_epoch;travel_scope.roster_revision=1;
    if(travelling.Begin(travel_scope)!=RollbackResult::Ready)return false;travel_packet=travelling.ExportAuthoritative();
    if(!travel_packet || travel_lobby.Publish(*travel_packet)!=LiveLobbyResult::Accepted || travel_lobby.Resume(2,traveller.ticket,now,resumed)!=LiveLobbyResult::Accepted)return false;
    travel_snapshot=travel_lobby.SnapshotFor(2);if(!travel_snapshot)return false;
    FixtureWorld travelled_client(0);RollbackSession arrived(travelled_client,RollbackRole::Client,Limits());
    if(!check(arrived.BeginFromAuthoritative(travel_scope,*travel_snapshot->snapshot)==RollbackResult::Ready && travelled_client.state.quest==87 &&
       travel_lobby.Acknowledge(2,resumed.ticket,travel_scope,0,1)==LiveLobbyResult::Accepted,
       "a guest reconnecting after the host changes level installs the latest world rather than the original join snapshot"))return false;
    if(travel_lobby.Leave(2)!=LiveLobbyResult::Accepted)return false;
    travelling_world.state.avatars[1]=0;++travel_scope.world_epoch;travel_scope.roster_revision=1;travel_scope.players_mask=1;
    if(travelling.Begin(travel_scope)!=RollbackResult::Ready)return false;travel_packet=travelling.ExportAuthoritative();
    if(!travel_packet || travel_lobby.Publish(*travel_packet)!=LiveLobbyResult::Accepted)return false;
    if(!check(travel_lobby.Leave(2)==LiveLobbyResult::Accepted && travel_lobby.CommitDeparture(2)==LiveLobbyResult::Accepted && travel_lobby.open(),
       "duplicate leave requests and level changes cannot strand a departed slot or close the host lobby"))return false;
    REXLOG_INFO("Live lobby passed {} running-world/snapshot/late-join/reconnect/quit/kick/16-player/color checks; transport and native avatar integration remain separate",checks);return true;
}
}
