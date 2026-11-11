#include "online_world_random.h"
#include "online.h"
#include <rex/logging.h>
#include <rex/cvar.h>
REXCVAR_DECLARE(bool,em2_online_world_random);

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
    const bool ok=seed==0x480044ACu && seed==NativeWorldInitializationSeed(identity,"tcy_autotopia_visit1.sectiona_first",1) &&
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
    if(!ok)REXLOG_ERROR("Native world seed failed session/checkpoint/destination/sequence separation");
    return ok;
}
