#include "online_world_random.h"
#include "online.h"
#include <rex/logging.h>
#include <rex/cvar.h>
#include <windows.h>
#include <algorithm>
#include <thread>
REXCVAR_DECLARE(bool,em2_online_world_random);

bool em2::net::NativeInitializationContext::Arm(const Identity& identity){
    const auto nonzero=[](const auto& bytes){return std::any_of(bytes.begin(),bytes.end(),[](auto value){return value!=0;});};
    if(armed_ || uint8_t(identity.initialization_policy)>uint8_t(InitializationPolicy::SharedNativeV1) ||
       !nonzero(identity.build) || !nonzero(identity.content) || !nonzero(identity.checkpoint) || !nonzero(identity.session))return false;
    identity_=identity;world_sequence_=0;player_sequences_={};thread_=0;failed_.store(false,std::memory_order_release);armed_=true;return true;
}
bool em2::net::NativeInitializationContext::BindThread(){
    if(!armed_ || failed_.load(std::memory_order_acquire))return false;
    const auto current=GetCurrentThreadId();if(thread_ && thread_!=current)return false;thread_=current;return true;
}
void em2::net::NativeInitializationContext::Release(){
    armed_=false;thread_=0;identity_={};world_sequence_=0;player_sequences_={};failed_.store(false,std::memory_order_release);
}
bool em2::net::NativeInitializationContext::Eligible()const{
    return armed_ && !failed_.load(std::memory_order_acquire) && thread_==GetCurrentThreadId() &&
        identity_.initialization_policy==InitializationPolicy::SharedNativeV1;
}
std::optional<em2::net::NativeInitializationSeed> em2::net::NativeInitializationContext::World(std::string_view destination){
    if(!Eligible() || destination.empty() || destination.size()>96 || world_sequence_==UINT64_MAX)return {};
    const auto sequence=++world_sequence_;return NativeInitializationSeed{NativeWorldInitializationSeed(identity_,destination,sequence),sequence};
}
std::optional<em2::net::NativeInitializationSeed> em2::net::NativeInitializationContext::Player(uint32_t character,uint32_t mode,bool agreed_timeline_active){
    if(!Eligible() || character>=player_sequences_.size() || mode>2 || agreed_timeline_active || player_sequences_[character]==UINT64_MAX)return {};
    const auto sequence=++player_sequences_[character];return NativeInitializationSeed{NativePlayerInitializationSeed(identity_,character,mode,sequence),sequence};
}

em2::net::InitializationPolicy em2::net::ConfiguredNativeInitializationPolicy(){
    return REXCVAR_GET(em2_online_world_random)?InitializationPolicy::SharedNativeV1:InitializationPolicy::Original;
}

uint32_t em2::net::NativeWorldInitializationSeed(const Identity& identity,std::string_view destination,uint64_t sequence){
    uint32_t value=2166136261u;
    auto byte=[&](uint8_t next){value^=next;value*=16777619u;};
    for(uint8_t next:std::string_view("EM2-native-world-init-v1"))byte(next);
    for(auto next:identity.session)byte(next);
    for(auto next:identity.checkpoint)byte(next);
    for(unsigned shift=64;shift;shift-=8)byte(uint8_t(sequence>>(shift-8)));
    for(unsigned shift=32;shift;shift-=8)byte(uint8_t(uint32_t(destination.size())>>(shift-8)));
    for(uint8_t next:destination)byte(next);
    return value;
}
uint32_t em2::net::NativePlayerInitializationSeed(const Identity& identity,uint32_t slot,uint32_t mode,uint64_t sequence){
    uint32_t value=2166136261u;
    auto byte=[&](uint8_t next){value^=next;value*=16777619u;};
    for(uint8_t next:std::string_view("EM2-native-player-init-v1"))byte(next);
    for(auto next:identity.session)byte(next);
    for(auto next:identity.checkpoint)byte(next);
    for(unsigned shift=64;shift;shift-=8)byte(uint8_t(sequence>>(shift-8)));
    for(auto word:{slot,mode})for(unsigned shift=32;shift;shift-=8)byte(uint8_t(word>>(shift-8)));
    return value;
}
bool em2::net::TestNativeWorldInitializationSeed(){
    Identity identity;identity.session[0]=7;identity.checkpoint[0]=29;
    const auto seed=NativeWorldInitializationSeed(identity,"tcy_autotopia_visit1.sectiona_first",1);
    auto other_session=identity;other_session.session[0]^=1;
    auto other_checkpoint=identity;other_checkpoint.checkpoint[0]^=1;
    const auto player=NativePlayerInitializationSeed(identity,0,0,1);
    bool ok=seed==0x480044ACu && seed==NativeWorldInitializationSeed(identity,"tcy_autotopia_visit1.sectiona_first",1) &&
        seed!=NativeWorldInitializationSeed(identity,"mainmenu.start",1) &&
        seed!=NativeWorldInitializationSeed(identity,"tcy_autotopia_visit1.sectiona_first",2) &&
        seed!=NativeWorldInitializationSeed(other_session,"tcy_autotopia_visit1.sectiona_first",1) &&
        seed!=NativeWorldInitializationSeed(other_checkpoint,"tcy_autotopia_visit1.sectiona_first",1) &&
        player==0x18ABC8A1u && player==NativePlayerInitializationSeed(identity,0,0,1) && player!=seed &&
        player!=NativePlayerInitializationSeed(identity,0,0,2) &&
        player!=NativePlayerInitializationSeed(identity,1,0,1) &&
        player!=NativePlayerInitializationSeed(identity,0,1,1) &&
        player!=NativePlayerInitializationSeed(other_session,0,0,1) &&
        player!=NativePlayerInitializationSeed(other_checkpoint,0,0,1);
    identity.build[0]=1;identity.content[0]=2;identity.initialization_policy=InitializationPolicy::SharedNativeV1;
    NativeInitializationContext a,b;
    ok &= !a.World("mainmenu.start") && !a.Player(0,0,false) && !a.BindThread() && !a.Arm(Identity{});
    ok &= a.Arm(identity) && b.Arm(identity) && !a.Arm(identity) && !a.World("mainmenu.start") && a.BindThread() && b.BindThread();
    // Independent transport owners produce identical seeds without consulting
    // a legacy Session, a wall clock or the mutable console preference.
    const auto aw=a.World("mainmenu.start"),bw=b.World("mainmenu.start");
    ok &= aw && bw && aw->value==bw->value && aw->sequence==1 && bw->sequence==1;
    ok &= !a.Player(0,0,true) && !a.Player(2,0,false) && !a.Player(0,3,false) && !a.World({});
    const auto ap=a.Player(0,0,false),bp=b.Player(0,0,false);
    ok &= ap && bp && ap->value==bp->value && ap->sequence==1 && bp->sequence==1;
    bool wrong_thread=false;
    std::thread other([&]{wrong_thread=!a.BindThread() && !a.World("mainmenu.start") && !a.Player(0,0,false);});other.join();
    ok &= wrong_thread;
    a.world_sequence_=UINT64_MAX;a.player_sequences_[0]=UINT64_MAX;
    ok &= !a.World("mainmenu.start") && !a.Player(0,0,false);
    a.Fail();ok &= !a.BindThread() && !a.Player(1,0,false);
    a.Release();ok &= !a.World("mainmenu.start") && a.Arm(identity) && a.BindThread();
    const auto restarted=a.World("mainmenu.start");ok &= restarted && aw && restarted->sequence==1 && restarted->value==aw->value;
    a.Release();identity.initialization_policy=InitializationPolicy::Original;
    ok &= a.Arm(identity) && a.BindThread() && !a.World("mainmenu.start") && !a.Player(0,0,false);
    if(!ok)REXLOG_ERROR("Native world seed failed identity/sequence separation or transport-independent owner lifecycle/thread guards");
    return ok;
}
