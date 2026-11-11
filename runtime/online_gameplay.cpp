#include "simulation.h"
#include "players.h"
#include "online.h"
#include "online_runtime.h"
#include "native_random.h"
#include "game_console.h"
#include "controls.h"
#include <rex/input/input_system.h>
#include <rex/input/input_driver.h>
#include <rex/logging.h>
#include <limits>
#include <thread>
#include <immintrin.h>
#include <rex/sha256.h>
#include <charconv>
#include <cmath>
#include <bit>
#include <windows.h>
PPC_EXTERN_FUNC(sub_831BB758);

namespace em2::net {
namespace {
struct HostInputMath {
    unsigned saved=_mm_getcsr();
    HostInputMath(){_mm_setcsr(0x1F80);}
    ~HostInputMath(){_mm_setcsr(saved);}
};
std::optional<std::array<uint8_t,32>> HashEncoded(std::string_view bytes) {
    const auto hex=rex::util::sha256(bytes);if(hex.size()!=64) return {};
    std::array<uint8_t,32> digest{};
    for(unsigned i=0;i<digest.size();++i) {
        unsigned value=0;const auto result=std::from_chars(hex.data()+i*2,hex.data()+i*2+2,value,16);
        if(result.ec!=std::errc{} || result.ptr!=hex.data()+i*2+2) return {};
        digest[i]=uint8_t(value);
    }
    return digest;
}
}
InputGate& GameplayInputGate() {static InputGate gate;return gate;}
InputGate::~InputGate() {Detach();}
std::optional<std::array<uint8_t,32>> HashPlayerObservations(uint64_t tick,const std::array<PlayerObservation,2>& players) {
    std::string bytes="EM2-player-observation-v1";
    auto put=[&](uint64_t value,unsigned count){for(unsigned i=count;i;--i) bytes.push_back(char(value>>((i-1)*8)));};
    put(tick,8);
    for(const auto& player:players) {
        put(player.mode,4);
        for(float value:player.position) {
            if(!std::isfinite(value)) return {};
            put(std::bit_cast<uint32_t>(value),4);
        }
    }
    return HashEncoded(bytes);
}
std::optional<std::array<uint8_t,32>> HashGameplayObservation(uint64_t tick,const GameplayObservation& state) {
    std::string bytes="EM2-gameplay-observation-v4";
    auto put=[&](uint64_t value,unsigned count){for(unsigned i=count;i;--i)bytes.push_back(char(value>>((i-1)*8)));};
    put(tick,8);
    for(const auto* name:{&state.world.level,&state.world.state,&state.world.interior}){
        size_t length=0;while(length<name->size() && (*name)[length]){const auto c=static_cast<unsigned char>((*name)[length]);if(c<32 || c>=127)return {}; ++length;}
        if(length==name->size())return {};
        put(length,2);bytes.append(name->data(),length);
    }
    for(const auto& player:state.players) {
        put(player.mode,4);
        for(float value:player.position){if(!std::isfinite(value))return {};put(std::bit_cast<uint32_t>(value),4);}
    }
    for(auto value:state.life)put(value,4);
    for(float value:state.ammo){if(!std::isfinite(value))return {};put(std::bit_cast<uint32_t>(value),4);}
    put(state.random.calls,8);put(state.random.seeds,8);put(state.random.last_state,4);put(state.random.sequence,8);
    return HashEncoded(bytes);
}
bool InputGate::Attach(Session& session,rex::input::InputSystem& input,unsigned device) {
    HostInputMath math;
    if(active() || device>=2 || session.state()!=State::Ready || session.next_tick()!=session.identity().first_tick) return false;
    const auto& id=session.identity();
    if(id.first_tick>UINT64_MAX-id.input_delay || !BeginSynchronizedControls(id.first_tick)) return false;
    session_=&session;input_=&input;identity_=id;device_=device;tick_=id.first_tick;
    stopped_=prepared_=exhausted_=waiting_=false;error_.clear();
    completion_pending_=false;native_join_pending_=false;
    load_sequence_=0;owner_thread_.store(GetCurrentThreadId(),std::memory_order_release);
    wait_reason_=InputWait::None;
    // Policy readers must also cover admission setup, before neutral seeding.
    active_snapshot_.store(true,std::memory_order_release);
    em2::ResetOnlineRandomTraceHistory();
    em2::ResetNativeSynchronizedJoinTiming();
    em2::BeginNativeOnlineRandom(identity_);
    em2::CancelQueuedGameLua();
    // Seed the agreed delay with neutral input from each peer. Thereafter a
    // sample taken at source tick T is submitted for T + input_delay.
    for(unsigned i=0;i<id.input_delay;++i) if(!session.SubmitLocal(tick_+i,{})) {
        session.Disconnect();Detach();return false;
    }
    return true;
}
void InputGate::Detach() {
    HostInputMath math;
    if(session_) {EndSynchronizedControls();em2::EndNativeOnlineRandom();em2::ResetNativeSynchronizedJoinTiming();em2::ResetOnlineRandomTraceHistory();}
    session_=nullptr;input_=nullptr;stopped_=prepared_=exhausted_=waiting_=false;error_.clear();
    completion_pending_=false;native_join_pending_=false;wait_reason_=InputWait::None;
    active_snapshot_.store(false,std::memory_order_release);
    owner_thread_.store(0,std::memory_order_release);load_sequence_=0;
    cancel_loads_.store(false,std::memory_order_release);
}
void InputGate::Stop(const char* reason) {
    // A native tick may still own its finish phase. Keep its published input
    // frozen until explicit detach, including on a mid-batch disconnect.
    error_=reason;stopped_=waiting_=true;prepared_=false;
    wait_reason_=InputWait::None;
    REXLOG_ERROR("Online simulation stopped: {}",reason);
}
void InputGate::Poll(std::chrono::steady_clock::time_point now) {
    HostInputMath math;
    if(!active() || stopped_) return;
    session_->Poll(now);
    if(session_->state()!=State::Ready || session_->identity()!=identity_) Stop("Session lost or replaced; world resynchronization required");
}
Admission InputGate::Prepare(Frame& frame) {
    HostInputMath math;
    if(!active()) return Admission::Inactive;
    Poll();if(stopped_) return Admission::Stopped;
    if(prepared_) {frame=pending_;return Admission::Ready;}
    if(exhausted_ || tick_>UINT64_MAX-identity_.input_delay || session_->next_tick()!=tick_) {
        Stop("Simulation/input timeline exhausted or changed outside its owner");return Admission::Stopped;
    }
    const auto verified=session_->verified_tick();
    if(verified && *verified>=tick_) {Stop("State verification advanced outside the simulation owner");return Admission::Stopped;}
    const auto unverified=verified?tick_-*verified-1:tick_-identity_.first_tick;
    if(unverified>=8) {waiting_=true;wait_reason_=InputWait::Verification;return Admission::Waiting;}
    Pad local;
    if(!SampleLocalControls(*input_,device_,tick_,local)) {waiting_=true;wait_reason_=InputWait::LocalController;return Admission::Waiting;}
    if(native_join_pending_){
        // Initial/repeated native joining is a canonical input phase. Only P2
        // supplies Start; movement stays neutral until native co-op is ready.
        // These exact pads travel over TCP and reach both native controllers.
        local={};if(session_->local_player()==1)local.buttons=0x10;
    }
    if(!session_->SubmitLocal(tick_+identity_.input_delay,local)) {
        Stop("Unable to submit the next local input");return Admission::Stopped;
    }
    if(!session_->TakeFrame(pending_)) {waiting_=true;wait_reason_=InputWait::Peer;return Admission::Waiting;}
    if(pending_.tick!=tick_) {Stop("Unexpected authoritative tick");return Admission::Stopped;}
    waiting_=false;wait_reason_=InputWait::None;prepared_=true;frame=pending_;return Admission::Ready;
}
bool InputGate::Commit(const Frame& frame) {
    HostInputMath math;
    if(!active() || stopped_ || !prepared_ || completion_pending_ || frame!=pending_) return false;
    if(session_->state()!=State::Ready || session_->identity()!=identity_ || !PublishSynchronizedControls(frame)) {
        Stop("Unable to publish the agreed controller frame");return false;
    }
    prepared_=false;
    completion_pending_=true;
    committed_tick_=frame.tick;
    if(tick_==UINT64_MAX) exhausted_=true;else ++tick_;
    return true;
}
bool InputGate::Complete(const std::array<uint8_t,32>& hash) {
    HostInputMath math;
    if(!active() || stopped_ || !completion_pending_) return false;
    if(!session_->SubmitStateDigest(committed_tick_,hash)) {Stop("Completed player observation differs or could not be submitted");return false;}
    completion_pending_=false;return true;
}
void InputGate::ObservationFailed() {HostInputMath math;if(active() && !stopped_) Stop("Native world/player observation unavailable; resynchronization required");}
bool InputGate::NativeLoadPoint(std::string_view destination,LoadPhase phase,bool success) {
    if(!active())return true;
    if(owner_thread_.load(std::memory_order_acquire)!=GetCurrentThreadId())return false;
    HostInputMath math;
    if(stopped_)return false;
    if(cancel_loads_.load(std::memory_order_acquire)){Stop("Native level load cancelled for title shutdown");return false;}
    if(!completion_pending_ || destination.empty() || destination.size()>=LoadPoint{}.destination.size() || load_sequence_==UINT64_MAX) {
        Stop("Native level load occurred outside an agreed tick or has an invalid destination");return false;
    }
    LoadPoint point;point.sequence=load_sequence_;point.tick=committed_tick_;point.phase=phase;point.success=success;
    std::copy(destination.begin(),destination.end(),point.destination.begin());
    if(!session_->SubmitLoadPoint(point)){Stop("Unable to synchronize native level load boundary");return false;}
    REXLOG_INFO("Online native level load rendezvous: sequence {}, tick {}, phase {}, destination '{}'",load_sequence_,committed_tick_,unsigned(phase),destination);
    // Always service the outgoing queue at least once. If the peer arrived
    // first, SubmitLoadPoint can complete local agreement immediately, but its
    // queued reply still has to leave before native finalization resumes.
    for(;;) {
        if(cancel_loads_.load(std::memory_order_acquire)){Stop("Native level load cancelled for title shutdown");return false;}
        Poll();if(stopped_)return false;
        if(session_->LoadPointAgreed(point))break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ++load_sequence_;return true;
}

bool TestOnlineGameplay() {
    auto check=[](bool ok,const char* text){if(!ok) REXLOG_ERROR("Online gameplay gate test: {}",text);return ok;};
    auto& gate=GameplayInputGate();
    std::array<PlayerObservation,2> observations{{{0,{1.f,-2.f,3.f}},{0,{-4.f,5.f,6.f}}}};
    const auto digest=HashPlayerObservations(99,observations);
    GameplayObservation gameplay;gameplay.players=observations;gameplay.life={5,5,5,5};gameplay.ammo={10,10,20,20,30,30};
    const auto gameplay_digest=HashGameplayObservation(99,gameplay);
    for(unsigned i=0;i<3;++i){auto changed=gameplay;auto& name=i==0?changed.world.level:i==1?changed.world.state:changed.world.interior;
        name[0]='a';if(!check(HashGameplayObservation(99,changed)!=gameplay_digest,"level, state and interior each participate in verification"))return false;
        name.fill('x');if(!check(!HashGameplayObservation(99,changed),"unterminated world names rejected"))return false;
        name.fill(0);name[0]=char(128);if(!check(!HashGameplayObservation(99,changed),"noncanonical world identifiers rejected"))return false;
    }
    if(!check(gameplay_digest && gameplay_digest!=digest,"expanded gameplay observation has a distinct schema"))return false;
    for(unsigned variant=0;variant<4;++variant) {
        auto changed=gameplay;
        if(variant==0)++changed.random.calls;
        if(variant==1)++changed.random.seeds;
        if(variant==2)++changed.random.last_state;
        if(variant==3)++changed.random.sequence;
        if(!check(HashGameplayObservation(99,changed)!=gameplay_digest,"each CRT stream field participates in online verification"))return false;
    }
    {
        RandomObservation first,second;
        first.Record(true,0x829E8520,0,0);first.Record(false,0x1234,2531011,38);
        second.Record(false,0x1234,2531011,38);second.Record(true,0x829E8520,0,0);
        if(!check(first.calls==1 && first.seeds==1 && first.sequence!=second.sequence,
            "CRT stream checksum distinguishes event order and reseeding"))return false;
        auto other=RandomObservation{};other.Record(false,0x1235,2531011,38);
        auto reference=RandomObservation{};reference.Record(false,0x1234,2531011,38);
        if(!check(other.sequence!=reference.sequence,"CRT stream checksum includes native caller identity"))return false;
    }
    for(unsigned field=0;field<gameplay.life.size();++field) {
        auto changed=gameplay;++changed.life[field];
        if(!check(HashGameplayObservation(99,changed)!=gameplay_digest,"life and capacity differences cannot pass gameplay verification"))return false;
    }
    for(unsigned field=0;field<gameplay.ammo.size();++field) {
        auto changed=gameplay;changed.ammo[field]=std::nextafter(changed.ammo[field],0.f);
        if(!check(HashGameplayObservation(99,changed)!=gameplay_digest,"every resource/current-capacity field participates in verification"))return false;
        changed.ammo[field]=NAN;
        if(!check(!HashGameplayObservation(99,changed),"nonfinite resources cannot become a valid digest"))return false;
    }
    if(!check(digest && digest==HashPlayerObservations(99,observations) && digest!=HashPlayerObservations(100,observations),"canonical player digest includes simulation tick")) return false;
    observations[1].position[0]=std::nextafter(observations[1].position[0],0.f);
    if(!check(digest!=HashPlayerObservations(99,observations),"single-bit player-position divergence changes digest")) return false;
    observations[0].position[0]=std::numeric_limits<float>::infinity();
    if(!check(!HashPlayerObservations(99,observations),"invalid observation cannot masquerade as a matching player state")) return false;
    struct Cleanup {InputGate& gate;~Cleanup(){gate.Detach();ClearControlKeys();}} cleanup{gate};
    for(uint8_t delay=0;delay<=12;++delay) {
        gate.Detach();ClearControlKeys();
        Identity id;id.build[0]=1;id.content[0]=2;id.checkpoint[0]=3;id.session[0]=uint8_t(8+delay);id.first_tick=100;id.input_delay=delay;
        Session host,client;
        // Detach before either referenced session is destroyed on any return.
        struct Detach {InputGate& gate;~Detach(){gate.Detach();}} detach{gate};
        rex::input::InputSystem input(nullptr);input.AddDriver(CreateKeyboardFallback(nullptr));
        if(!check(host.Host(id,"127.0.0.1") && client.Join(id,"127.0.0.1",host.port()),"open gate test sockets")) return false;
        auto until=[&](auto predicate) {
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            do {host.Poll();client.Poll();if(predicate()) return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
            while(std::chrono::steady_clock::now()<deadline);return false;
        };
        if(!check(until([&]{return host.state()==State::Ready && client.state()==State::Ready;}) && gate.Attach(host,input,0),"attach compatible session")) return false;
        if(!check(SynchronizedSessionActive(),"standalone input gate publishes synchronized policy without a retained owner"))return false;
        for(unsigned i=0;i<delay;++i) if(!check(client.SubmitLocal(id.first_tick+i,{}),"seed remote delay")) return false;
        ControlKey('W',true);
        alignas(16) std::array<uint8_t,4096> guest{};
        auto read_native=[&](unsigned user) {
            PPCContext context{};context.r3.u64=user;context.r4.u64=256;
            sub_831BB758(context,guest.data());
            const auto& pad=reinterpret_cast<rex::input::X_INPUT_STATE*>(guest.data()+256)->gamepad;
            return Pad{uint16_t(pad.buttons),pad.left_trigger,pad.right_trigger,int16_t(pad.thumb_lx),int16_t(pad.thumb_ly),int16_t(pad.thumb_rx),int16_t(pad.thumb_ry)};
        };
        Frame last{};
        bool math_preserved=true;
        auto prepare=[&](Frame& frame) {
            const auto saved=_mm_getcsr();const auto guest=(saved&~0x6000u)|0x6000u|0x20u;
            _mm_setcsr(guest);const auto result=gate.Prepare(frame);const auto restored=_mm_getcsr();_mm_setcsr(saved);
            math_preserved&=restored==guest;return result;
        };
        for(unsigned i=0;i<8+delay;++i) {
            Frame frame{};const auto tick=id.first_tick+i;
            if(i==1) {ControlKey(VK_SPACE,true);ControlKey(VK_SPACE,false);}
            if(i==4) ControlKey('W',false);
            if(!delay) {
                if(!check(prepare(frame)==Admission::Waiting && gate.waiting() && gate.wait_reason()==InputWait::Peer && host.next_tick()==tick,"missing peer input cannot admit a tick")) return false;
            }
            Pad remote{};remote.buttons=uint16_t(0x1000+i);
            if(!check(client.SubmitLocal(tick+delay,remote),"submit remote future input")) return false;
            if(!check(until([&]{return prepare(frame)==Admission::Ready;}) && !gate.waiting() && gate.wait_reason()==InputWait::None && math_preserved,"admit agreed tick without changing guest floating-point state")) return false;
            Frame again{};
            const auto repeated=prepare(again);
            if(!check(math_preserved,"input admission preserves guest rounding and exception flags")) return false;
            if(!check(frame.tick==tick && repeated==Admission::Ready && again==frame &&
                frame.players[0].left_y==((i>=delay && i<4+delay)?32767:0) &&
                frame.players[0].buttons==((i==1+delay)?0x1000:0) &&
                frame.players[1].buttons==(i<delay?0:0x1000+i-delay),"prepared frame holds and press/release/remote delay is real")) return false;
            if(!check(gate.Commit(frame) && !gate.Commit(frame),"publish each admitted frame once")) return false;
            if(!check(read_native(0)==frame.players[0] && read_native(1)==frame.players[1],"agreed socket input reaches both native controller slots")) return false;
            Frame peer{};bool peer_taken=false;
            if(!delay && i==0) {
                if(!check(until([&]{return peer_taken=client.TakeFrame(peer);}),"load fixture consumes matching peer tick"))return false;
                bool peer_ok=true;
                std::thread loader_peer([&]{
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    for(unsigned phase=0;phase<2;++phase){
                        LoadPoint point;point.sequence=phase;point.tick=tick;point.phase=phase?LoadPhase::End:LoadPhase::Begin;point.success=phase!=0;
                        std::copy_n("test",4,point.destination.begin());
                        if(!client.SubmitLoadPoint(point)){peer_ok=false;client.Disconnect();return;}
                        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
                        while(!client.LoadPointAgreed(point) && std::chrono::steady_clock::now()<deadline){client.Poll();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
                        if(!client.LoadPointAgreed(point)){peer_ok=false;client.Disconnect();return;}
                    }
                });
                const auto csr=_mm_getcsr();
                const bool synced=gate.NativeLoadPoint("test",LoadPhase::Begin) && gate.NativeLoadPoint("test",LoadPhase::End,true);
                loader_peer.join();
                if(!check(synced && peer_ok && gate.completion_pending() && gate.committed_tick()==tick && _mm_getcsr()==csr,
                    "native owner waits through both real TCP load boundaries without consuming another tick or changing FP state"))return false;
            }
            if(i==7+delay && delay) {
                Frame following;
                if(!check(prepare(following)==Admission::Ready && following.tick==tick+1 && gate.committed_tick()==tick,
                    "prefetching a future frame cannot overwrite the tick awaiting native completion")) return false;
            }
            auto observation=id.checkpoint;observation[1]=uint8_t(i);
            if(!check(gate.completion_pending() && gate.Complete(observation) && !gate.Complete(observation),"native completion reports each committed tick once")) return false;
            last=frame;
            if(!check((peer_taken || until([&]{return client.TakeFrame(peer);})) && peer==frame,"peer receives the same admitted frame")) return false;
            if(!check(client.SubmitStateDigest(tick,observation) && until([&]{return host.verified_tick()==tick && client.verified_tick()==tick;}),"both completed observations verify before the next test tick")) return false;
        }
        client.Disconnect();Frame frame{};
        if(!check(until([&]{return prepare(frame)==Admission::Stopped;}) && math_preserved && gate.active() && !gate.Commit(frame),
            "disconnect leaves simulation stopped until explicit detach/resynchronization")) return false;
        if(!check(read_native(0)==last.players[0] && read_native(1)==last.players[1],"disconnect freezes the last agreed inputs for pending native finish")) return false;
        gate.Detach();if(!check(gate.Prepare(frame)==Admission::Inactive && !SynchronizedSessionActive(),"explicit detach restores inactive gate and offline policy")) return false;
    }
    {
        gate.Detach();ClearControlKeys();
        Identity id;id.build[0]=1;id.content[0]=2;id.checkpoint[0]=3;id.session[0]=55;id.input_delay=0;
        Session host,client;
        struct Detach {InputGate& gate;~Detach(){gate.Detach();}} detach{gate};
        rex::input::InputSystem input(nullptr);input.AddDriver(CreateKeyboardFallback(nullptr));
        auto until=[&](auto predicate) {
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            do {host.Poll();client.Poll();if(predicate()) return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
            while(std::chrono::steady_clock::now()<deadline);return false;
        };
        if(!check(host.Host(id,"127.0.0.1") && client.Join(id,"127.0.0.1",host.port()) &&
            until([&]{return host.state()==State::Ready && client.state()==State::Ready;}) && gate.Attach(host,input,0),"open verification backlog session")) return false;
        for(uint64_t tick=0;tick<8;++tick) {
            Frame frame,peer;
            if(!check(client.SubmitLocal(tick,{}) && until([&]{return gate.Prepare(frame)==Admission::Ready;}) &&
                gate.Commit(frame) && gate.Complete(id.checkpoint) && until([&]{return client.TakeFrame(peer);}),"bounded run-ahead while peer completion is delayed")) return false;
        }
        Frame pending;
        if(!check(client.SubmitLocal(8,{}) && gate.Prepare(pending)==Admission::Waiting && gate.waiting() && gate.wait_reason()==InputWait::Verification &&
            host.next_tick()==8 && !host.verified_tick(),"available input cannot bypass the eight-tick verification limit")) return false;
        if(!check(client.SubmitStateDigest(0,id.checkpoint) && until([&]{return gate.Prepare(pending)==Admission::Ready;}) &&
            pending.tick==8 && host.verified_tick()==0,"delayed peer verification releases bounded run-ahead")) return false;
    }
    {
        Session host,client;struct Detach{InputGate& gate;~Detach(){gate.Detach();}}detach{gate};gate.Detach();ClearControlKeys();
        using rex::X_RESULT;using rex::X_STATUS;
        class Device final:public rex::input::InputDriver {
        public:
            bool connected=false;Device():InputDriver(nullptr,0){}
            rex::X_STATUS Setup()override{return X_STATUS_SUCCESS;}
            rex::X_RESULT GetCapabilities(uint32_t,uint32_t,rex::input::X_INPUT_CAPABILITIES*)override{return connected?X_ERROR_SUCCESS:X_ERROR_DEVICE_NOT_CONNECTED;}
            rex::X_RESULT GetState(uint32_t,rex::input::X_INPUT_STATE* out)override{if(out)*out={};return connected?X_ERROR_SUCCESS:X_ERROR_DEVICE_NOT_CONNECTED;}
            rex::X_RESULT SetState(uint32_t,rex::input::X_INPUT_VIBRATION*)override{return X_ERROR_SUCCESS;}
            rex::X_RESULT GetKeystroke(uint32_t,uint32_t,rex::input::X_INPUT_KEYSTROKE*)override{return X_ERROR_EMPTY;}
        };
        rex::input::InputSystem input(nullptr);auto device=std::make_unique<Device>();auto* physical=device.get();input.AddDriver(std::move(device));
        Identity id;id.build[0]=1;id.content[0]=2;id.checkpoint[0]=3;id.session[0]=23;
        auto until=[&](auto predicate){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            do{host.Poll();client.Poll();if(predicate())return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(std::chrono::steady_clock::now()<deadline);return false;};
        if(!check(host.Host(id,"127.0.0.1") && client.Join(id,"127.0.0.1",host.port()) &&
            until([&]{return host.state()==State::Ready && client.state()==State::Ready;}) && gate.Attach(host,input,0),"open local-controller recovery fixture"))return false;
        Frame frame;
        if(!check(client.SubmitLocal(0,{}) && gate.Prepare(frame)==Admission::Waiting && gate.wait_reason()==InputWait::LocalController,
            "missing local device is distinct from peer/verification waits"))return false;
        physical->connected=true;
        if(!check(until([&]{return gate.Prepare(frame)==Admission::Ready;}) && gate.wait_reason()==InputWait::None,
            "reconnecting local input clears the wait without replacing the session"))return false;
        if(!check(gate.Commit(frame),"commit shutdown rendezvous fixture tick"))return false;
        std::thread closer([&]{std::this_thread::sleep_for(std::chrono::milliseconds(20));gate.CancelNativeLoads();});
        const auto began=std::chrono::steady_clock::now();
        const bool loaded=gate.NativeLoadPoint("test",LoadPhase::Begin);closer.join();
        if(!check(!loaded && gate.error().find("shutdown")!=std::string::npos && std::chrono::steady_clock::now()-began<std::chrono::seconds(1),
            "title shutdown interrupts a real pending rendezvous without waiting for the peer timeout"))return false;
        gate.Detach();if(!check(gate.wait_reason()==InputWait::None,"detach clears presentation reason"))return false;
    }
    for(bool client_gate:{false,true})for(uint8_t delay:{uint8_t(0),uint8_t(2)}){
        gate.Detach();ClearControlKeys();ControlKey('W',true);
        Identity id;id.build[0]=1;id.content[0]=2;id.checkpoint[0]=3;id.input_delay=delay;id.session[0]=uint8_t(90+client_gate*3+delay);
        Session host,client;struct Detach {InputGate& gate;~Detach(){gate.Detach();}}detach{gate};
        rex::input::InputSystem input(nullptr);input.AddDriver(CreateKeyboardFallback(nullptr));
        auto until=[&](auto condition){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            do{host.Poll();client.Poll();if(condition())return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(std::chrono::steady_clock::now()<end);return false;};
        auto& owned=client_gate?client:host;auto& peer=client_gate?host:client;
        if(!check(host.Host(id,"127.0.0.1") && client.Join(id,"127.0.0.1",host.port()) &&
            until([&]{return host.state()==State::Ready && client.state()==State::Ready;}) &&
            host.local_player()==0 && client.local_player()==1 && gate.Attach(owned,input,0),"attach canonical native-join fixture for either wire lane"))return false;
        gate.SetNativeJoinPending(true);
        for(unsigned i=0;i<delay;++i)if(!peer.SubmitLocal(i,{}))return false;
        for(unsigned i=0;i<12+delay;++i){
            if(i==3 || i==9)gate.SetNativeJoinPending(false);
            if(i==6)gate.SetNativeJoinPending(true); // Level load restored AI.
            const bool joining=i<3 || (i>=6 && i<9);
            Pad remote;if(!client_gate && joining)remote.buttons=0x10;
            if(!peer.SubmitLocal(i+delay,remote))return false;
            Frame frame,other;bool taken=false;
            if(!check(until([&]{
                if(client_gate && !taken)taken=peer.TakeFrame(other);
                const auto ready=gate.Prepare(frame)==Admission::Ready;
                if(!client_gate && !taken)taken=peer.TakeFrame(other);
                return ready && taken;
            }),"prepare bootstrap through the real authoritative frame channel"))return false;
            const bool start=i>=delay && (i<3+delay || (i>=6+delay && i<9+delay));
            const bool neutral=i<delay || start;
            const auto& local=frame.players[client_gate?1:0];
            if(!check(frame==other && frame.players[1].buttons==(start?0x10:0) && frame.players[0].buttons==0 &&
                (neutral?local.left_y==0:local.left_y>0) && gate.Commit(frame),
                "initial and repeated level-load joins send neutral/P2 Start through agreed delayed frames, restoring held movement after each join"))return false;
            alignas(16) std::array<uint8_t,4096> memory{};
            for(unsigned user=0;user<2;++user){PPCContext ctx{};ctx.r3.u64=user;ctx.r4.u64=256;sub_831BB758(ctx,memory.data());
                const auto& pad=reinterpret_cast<rex::input::X_INPUT_STATE*>(memory.data()+256)->gamepad;
                if(!check(pad.buttons==frame.players[user].buttons && pad.thumb_ly==frame.players[user].left_y,"canonical startup controls reach native Xam unchanged"))return false;
            }
            if(!check(gate.Complete(id.checkpoint) && peer.SubmitStateDigest(i,id.checkpoint) &&
                until([&]{return host.verified_tick()==i && client.verified_tick()==i;}),"verify each bootstrap completion before advancing"))return false;
        }
    }
    REXLOG_INFO("Online gameplay gate tests passed: real TCP, missing-input barrier, local-controller recovery, wait reasons, all 0–12 tick delays, local sampling, native input publication, completed observation comparison, bounded run-ahead, prefetch/finish ownership and disconnect; native world determinism remains unverified");
    return true;
}
}
