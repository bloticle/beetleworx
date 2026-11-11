#include "em1_ink_restoration.h"
#include "emr_ability_animation_probe.h"
#include "guest_read_batch.h"
#include "gameplay_mods.h"
#include "console_tools.h"
#include "native_engine.h"
#include "ppc_config.h"
#include "ppc_context.h"
#include "players.h"
#include "simulation.h"
#include "online.h"
#include "online_runtime.h"
#include <rex/logging.h>
#include <rex/cvar.h>
#include <rex/input/input.h>
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DEFINE_STRING(em2_qa_emr_clip_directory,"","Debug","Private profile only: sample converted move clips against supplied animation poses without activating an actor");

PPC_EXTERN_FUNC(__imp__sub_8278D4D8);
PPC_EXTERN_FUNC(__imp__sub_8278BF80);
PPC_EXTERN_FUNC(__imp__sub_82787440);
PPC_EXTERN_FUNC(__imp__sub_8278F930);
PPC_EXTERN_FUNC(__imp__sub_82788320);
PPC_EXTERN_FUNC(__imp__sub_82786E88);
PPC_EXTERN_FUNC(__imp__sub_82740440);
PPC_EXTERN_FUNC(__imp__sub_8276B878);
PPC_EXTERN_FUNC(__imp__sub_827B58D0);
PPC_EXTERN_FUNC(__imp__sub_82A69948);

namespace {
constexpr int sprint_priority=-190021,console_priority=-190020;
std::mutex tuning_mutex,state_mutex,command_mutex;
em2::GameplayTuning tuning;
std::filesystem::path tuning_file;
std::atomic<float> console_speed{-1};
struct Impulse {float x,y,z,decay;};
std::deque<Impulse> impulses;
bool Valid(const em2::GameplayTuning& t){
    auto in=[](float value,float low,float high){return std::isfinite(value) && value>=low && value<=high;};
    return (t.pound_damage_type==em2::PoundDamageType::Physical || t.pound_damage_type==em2::PoundDamageType::Spin) && in(t.sprint_multiplier,1,8) && in(t.dash_speed,1,100) && in(t.dash_duration,.05f,2) && in(t.dash_cooldown,0,10) && in(t.pound_speed,1,100) && in(t.pound_damage_amount,0,1000) && in(t.pound_radius,.1f,50);
}
struct MotionModel {
    bool dash_down=false,pound_down=false,pounding=false,dashing=false,sprinting=false,air_dash_used=false;
    double dash_left=0,cooldown=0;
    bool keyboard_sprint_down=false,pad_sprint_down=false,sprint_latched=false;
    bool dash_was_air=false;
    void Advance(const em2::GameplayTuning& config,em2::GameplayModInput input,bool ground,bool air,double dt,bool active){
        if(!active){*this={};return;}
        cooldown=std::max(0.,cooldown-dt);
        const bool keyboard_sprint=input.sprint && !input.pad_sprint;
        if((config.keyboard_sprint_toggle && keyboard_sprint && !keyboard_sprint_down) || (config.controller_sprint_toggle && input.pad_sprint && !pad_sprint_down))sprint_latched=!sprint_latched;
        keyboard_sprint_down=keyboard_sprint;pad_sprint_down=input.pad_sprint;
        if(!config.keyboard_sprint_toggle && !config.controller_sprint_toggle)sprint_latched=false;
        if(air && dash_left>0)dash_was_air=true;
        if(ground){air_dash_used=false;pounding=false;if(dash_was_air){dash_left=0;dash_was_air=false;}}
        if(!config.pound || (!air && !ground))pounding=false;
        const bool pound_pressed=input.pound && !pound_down,dash_pressed=input.dash && !dash_down;
        pound_down=input.pound;dash_down=input.dash;
        if(config.pound && pound_pressed && air){pounding=true;dash_left=0;}
        if(!config.dash || (!ground && !air))dash_left=0;
        if(config.dash && dash_pressed && !pounding && cooldown<=1e-9 && (ground || (air && config.air_dash && !air_dash_used))){
            dash_left=config.dash_duration;cooldown=config.dash_cooldown;dash_was_air=air;if(air)air_dash_used=true;
        }
        dashing=dash_left>1e-8 && !pounding;
        dash_left=std::max(0.,dash_left-dt);
        sprinting=config.sprint && (sprint_latched || (!config.keyboard_sprint_toggle && keyboard_sprint) || (!config.controller_sprint_toggle && input.pad_sprint)) && ground && !dashing && !pounding;
    }
};
struct ActorState {
    uint32_t motion=0;
    uint64_t identity=0;
    uint32_t animation=0,interrupt_serial=0;
    uint64_t epoch=UINT64_MAX;
    MotionModel model;
    bool sprint_override=false,console_override=false;
    bool dash_animation_owned=false;
    bool pound_animation_owned=false;
    bool sprint_animation_owned=false;
    float base_speed=0,dash_x=0,dash_z=1;
};
std::array<ActorState,2> actors;
struct Binding {int actor=-1,user=-1;uint32_t manager=0,entity=0,animation=0;uint64_t identity=0;};
struct Targets {std::array<uint32_t,2> motion{},entity{},animation{};std::array<uint64_t,2> identity{};std::array<int,2> user{-1,-1};uint32_t manager=0;};
std::mutex target_mutex;
Targets targets;
struct GuestScope {
    PPCContext& ctx;PPCContext saved;unsigned csr;
    explicit GuestScope(PPCContext& c):ctx(c),saved(c),csr(_mm_getcsr()){}
    ~GuestScope(){ctx=saved;_mm_setcsr(csr);}
};
bool Readable(uint32_t address,size_t size,uint8_t* base){
    if(address<0x1000 || !size || uint64_t(address)+size>0x100000000ull)return false;
    return em2::GuestReadBatch::Readable(base+address,size);
}
uint32_t MotionForEntity(uint32_t entity,uint8_t* base){
    if((entity&3) || !Readable(entity,96,base))return 0;
    // Original 82740440: the BasicMotion type slot resolves inherited player
    // controllers too. Component arrays contain the native +8 interface.
    const auto types=PPC_LOAD_U32(entity+88),components=PPC_LOAD_U32(entity+36);
    const auto type=PPC_LOAD_U8(0x833DAF54);if(!Readable(types,unsigned(type)+1,base))return 0;
    const auto slot=PPC_LOAD_U8(types+type);if(slot==255)return 0;
    if(!Readable(components,unsigned(slot)*4+4,base))return 0;
    const auto component=PPC_LOAD_U32(components+unsigned(slot)*4);return component>=8 && !(component&3) && Readable(component-8,976,base)?component-8:0;
}
uint32_t ResolvePlayerEntity(PPCContext& ctx,uint8_t* base,uint32_t manager,unsigned slot){
    // PlayerManager+20 is an entity HANDLE. Retail 8276B878 resolves it
    // through the entity registry (82732A18), returning the NiEntity interface.
    if(!PPC_LOAD_U32(manager+slot*320+20))return 0;
    const auto registry=PPC_LOAD_U32(0x833DA7E4);
    if(!Readable(registry,24,base))return 0;
    const auto buckets=PPC_LOAD_U32(registry+20);
    if(!buckets || buckets>1048576 || PPC_LOAD_U32(registry+4)!=0x827B58D0 ||
       PPC_LOAD_U32(registry+8)!=0x82A69948 || !Readable(PPC_LOAD_U32(registry+16),size_t(buckets)*4,base))return 0;
    GuestScope scope(ctx);ctx.r3.u64=manager;ctx.r4.u64=slot;__imp__sub_8276B878(ctx,base);
    const auto entity=ctx.r3.u32;return !(entity&3) && Readable(entity,96,base)?entity:0;
}
bool NativeMovementReady(uint32_t motion,uint8_t* base){
    // Match the retail BasicMotion update's entry checks, rather than
    // requiring a particular 3D camera class (which excludes 2D controllers).
    if(!motion)return false;
    const auto input=PPC_LOAD_U32(motion+80);
    return Readable(input,180,base) && PPC_LOAD_U32(input+176) && PPC_LOAD_U8(input+145);
}
Binding FindBinding(uint32_t motion,uint8_t*){
    std::lock_guard lock(target_mutex);
    if(motion)for(unsigned slot=0;slot<2;++slot)if(targets.motion[slot]==motion)return {int(slot),targets.user[slot],targets.manager,targets.entity[slot],targets.animation[slot],targets.identity[slot]};
    return {};
}
float BaseMoveSpeed(uint32_t motion,uint8_t* base){
    // 82787440's no-override path resolves108, then82886D68 returns its
    // Max Move Speed at+72. Read that authored baseline every tick: a
    // temporary scripted slowdown must not become the held sprint's base.
    const auto parameters=PPC_LOAD_U32(motion+108);return Readable(parameters,76,base)?std::bit_cast<float>(PPC_LOAD_U32(parameters+72)):0.f;
}
void SetSpeed(PPCContext& ctx,uint8_t* base,uint32_t motion,int priority,float speed){
    GuestScope scope(ctx);ctx.r3.u64=motion;ctx.r5.s64=priority;ctx.f1.f64=speed;__imp__sub_8278F930(ctx,base);
}
void RemoveSpeed(PPCContext& ctx,uint8_t* base,uint32_t motion,int priority){
    GuestScope scope(ctx);ctx.r3.u64=motion;ctx.r4.s64=priority;__imp__sub_82788320(ctx,base);
}
void PrepareMotion(PPCContext& ctx,uint8_t* base,uint32_t motion,double dt){
    const auto binding=FindBinding(motion,base);if(binding.actor<0)return;
    std::lock_guard state_lock(state_mutex);auto& state=actors[binding.actor];
    if(state.motion!=motion || state.identity!=binding.identity){state={};state.motion=motion;state.identity=binding.identity;REXLOG_INFO("Gameplay mods bound to native actor {} / motion {:08X} / identity {:016X}",binding.actor,motion,binding.identity);}
    const auto epoch=em2::simulation_steps.load(std::memory_order_relaxed);if(state.epoch==epoch)return;
    if(state.epoch!=UINT64_MAX && epoch>state.epoch+2){
        if(state.sprint_override)RemoveSpeed(ctx,base,motion,sprint_priority);
        if(state.console_override)RemoveSpeed(ctx,base,motion,console_priority);
        state={};state.motion=motion;state.identity=binding.identity;
    }
    state.epoch=epoch;
    const auto config=em2::GetGameplayTuning();
    const bool offline=!em2::net::SynchronizedSessionActive();
    const bool active=offline && binding.user>=0 && NativeMovementReady(motion,base) && !em2::ControlBindingsOpen() && std::isfinite(dt) && dt>0 && dt<=.25;
    const auto input=active?em2::ReadGameplayModInput(unsigned(binding.user)):em2::GameplayModInput{};
    const auto native_state=PPC_LOAD_U32(motion+384);
    state.animation=binding.animation;
    const auto interrupt=em2::NativeAnimationInterruptSerial(binding.animation);
    if(interrupt!=state.interrupt_serial){state.interrupt_serial=interrupt;state.model={};state.model.dash_down=input.dash;state.model.pound_down=input.pound;state.dash_animation_owned=state.pound_animation_owned=state.sprint_animation_owned=false;}
    const bool was_dashing=state.model.dashing,was_pounding=state.model.pounding;
    state.model.Advance(config,input,native_state==0,native_state==1 || native_state==2,active?dt:0,active);
    const bool sprint_pose=binding.actor==0 && active && config.native_animations && state.model.sprinting && input.moving;
    if(state.sprint_animation_owned && !sprint_pose){
        em2::NativeAnimationEvent(ctx,base,binding.animation,"EVENT_Start_AttackChargeEnd");state.sprint_animation_owned=false;
    }
    if(state.dash_animation_owned && (!state.model.dashing || !config.native_animations || !config.dash_spin_animation)){
        em2::NativeAnimationEvent(ctx,base,binding.animation,native_state==0?"EVENT_PC_DashStopGround":"EVENT_PC_DashStopAir");state.dash_animation_owned=false;
    }
    if(state.pound_animation_owned && (!state.model.pounding || !config.native_animations)){
        em2::NativeAnimationEvent(ctx,base,binding.animation,native_state==0?"EVENT_Start_Land":"EVENT_PC_GroundPoundCancel");state.pound_animation_owned=false;
    }
    if(active){
        if(sprint_pose && !state.sprint_animation_owned)state.sprint_animation_owned=em2::NativeAnimationEvent(ctx,base,binding.animation,"EVENT_Start_AttackChargeLoop");
        if(config.native_animations && state.model.pounding && !was_pounding)state.pound_animation_owned=em2::NativeAnimationEvent(ctx,base,binding.animation,"EVENT_PC_GroundPound");
        if(config.native_animations && state.model.dashing && !was_dashing){
            if(config.dash_spin_animation)state.dash_animation_owned=em2::NativeAnimationEvent(ctx,base,binding.animation,native_state==0?"EVENT_PC_DashGround":"EVENT_PC_DashAir");
            else em2::NativeAnimationEvent(ctx,base,binding.animation,native_state==0?"EVENT_Start_Running_On_Ground":"EVENT_Start_Fall");
        }
        if(config.pound && was_pounding && native_state==0){
            // Spin is the game's normal player melee stimulus: scripts for
            // enemies and breakables specifically test ST_SPIN. Generic
            // physical damage remains available as a live menu choice.
            std::array<em2::NativeStimulus,2> impact{};unsigned count=0;
            if(config.pound_damage)impact[count++]={unsigned(config.pound_damage_type),config.pound_damage_amount};
            if(config.pound_knockback)impact[count++]={3,1};
            if(count)em2::NativeStimuliSphere(ctx,base,binding.entity,std::span(impact.data(),count),config.pound_radius);
        }
    }
    if(state.model.dashing && !was_dashing){
        const auto x=std::bit_cast<float>(PPC_LOAD_U32(motion+728)),z=std::bit_cast<float>(PPC_LOAD_U32(motion+736));
        const auto length=std::hypot(x,z);if(std::isfinite(length) && length>.001f){state.dash_x=x/length;state.dash_z=z/length;}
    }
    state.base_speed=state.model.sprinting?BaseMoveSpeed(motion,base):0;
    if(state.model.sprinting && std::isfinite(state.base_speed) && state.base_speed>0){
        SetSpeed(ctx,base,motion,sprint_priority,state.base_speed*config.sprint_multiplier);state.sprint_override=true;
    }else if(state.sprint_override){RemoveSpeed(ctx,base,motion,sprint_priority);state.sprint_override=false;}
    const auto speed=console_speed.load(std::memory_order_relaxed);
    if(offline && binding.user==0 && speed>=0){SetSpeed(ctx,base,motion,console_priority,speed);state.console_override=true;}
    else if(state.console_override){RemoveSpeed(ctx,base,motion,console_priority);state.console_override=false;}
    if(offline && binding.user==0){
        std::optional<Impulse> impulse;
        {std::lock_guard command_lock(command_mutex);if(!impulses.empty()){impulse=impulses.front();impulses.pop_front();}}
        if(impulse){GuestScope scope(ctx);const auto stack=ctx.r1.u32-256;ctx.r1.u64=stack;const auto vector=stack+128;
            PPC_STORE_U32(vector,std::bit_cast<uint32_t>(impulse->x));PPC_STORE_U32(vector+4,std::bit_cast<uint32_t>(impulse->y));PPC_STORE_U32(vector+8,std::bit_cast<uint32_t>(impulse->z));PPC_STORE_U32(vector+12,0);
            ctx.r3.u64=motion;ctx.r4.u64=vector;ctx.f1.f64=impulse->decay;__imp__sub_82786E88(ctx,base);
        }
    }
}
void ApplyRequestedVelocity(uint32_t motion,const ActorState& state,const em2::GameplayTuning& config,uint8_t* base){
    if(state.model.dashing && config.dash){PPC_STORE_U32(motion+240,std::bit_cast<uint32_t>(state.dash_x*config.dash_speed));PPC_STORE_U32(motion+248,std::bit_cast<uint32_t>(state.dash_z*config.dash_speed));}
    if(state.model.pounding && config.pound){PPC_STORE_U32(motion+240,0);PPC_STORE_U32(motion+248,0);const auto y=std::bit_cast<float>(PPC_LOAD_U32(motion+244));if(std::isfinite(y))PPC_STORE_U32(motion+244,std::bit_cast<uint32_t>(std::min(y,-config.pound_speed)));}
}
void ApplyVelocity(uint32_t motion,uint8_t* base){
    const auto binding=FindBinding(motion,base);
    if(em2::net::SynchronizedSessionActive() || em2::ControlBindingsOpen() || binding.user<0)return;
    std::lock_guard state_lock(state_mutex);
    const auto found=std::find_if(actors.begin(),actors.end(),[&](const auto& state){return state.motion==motion && state.identity==binding.identity;});
    if(found==actors.end() || !PPC_LOAD_U8(motion+640) || em2::NativeAnimationInterruptSerial(found->animation)!=found->interrupt_serial)return;
    const auto config=em2::GetGameplayTuning();const auto& state=*found;
    // 8278BF80 accumulates native external forces into this desired velocity.
    // The original Havok controller subsequently resolves contacts and gravity.
    // World/scene transforms and authored external-force storage stay native.
    ApplyRequestedVelocity(motion,state,config,base);
}
}

void em2::UpdateGameplayModTargets(PPCContext& ctx,uint8_t* base){
    GuestReadBatch inspection;
    Targets next;
    std::array<NativeActor,2> engine_actors{};
    const auto manager=PPC_LOAD_U32(0x833DAB60),profile=PPC_LOAD_U32(0x833DC87C);
    // Starting at the profile usually covers the later manager in the same
    // committed arena; still query separately whenever it does not.
    if(Readable(profile,28,base) && Readable(manager,640,base)){
        next.manager=manager;const auto count=PPC_LOAD_U32(profile+24);
        const unsigned selected=OswaldIsSelected()?1:0;
        for(unsigned slot=0;slot<2;++slot){
            next.entity[slot]=ResolvePlayerEntity(ctx,base,manager,slot);
            next.motion[slot]=MotionForEntity(next.entity[slot],base);
            // Pointer addresses can be recycled by the native component pool.
            // Handles are scoped to the entity registry, not to those addresses.
            if(next.entity[slot])next.identity[slot]=(uint64_t(PPC_LOAD_U32(0x833DA7E4))<<32)|PPC_LOAD_U32(manager+slot*320+20);
            const bool ready=(count==1 || count==2) && NativeMovementReady(next.motion[slot],base);
            if(ready && next.motion[slot])next.animation[slot]=DiscoverNativeAnimation(next.entity[slot],base);
            engine_actors[slot]={next.entity[slot],next.motion[slot],next.animation[slot],ready && bool(next.motion[slot]),next.identity[slot]};
            if(ready && next.motion[slot])next.user[slot]=count==2?int(slot):(slot==selected?0:-1);
        }
    }
    {
        std::lock_guard lock(target_mutex);
        for(unsigned slot=0;slot<2;++slot)if(next.entity[slot]!=targets.entity[slot] || next.motion[slot]!=targets.motion[slot] || next.identity[slot]!=targets.identity[slot])
            REXLOG_INFO("Native ability target P{}: entity {:08X}, motion {:08X}, animation {:08X}, input user {}, identity {:016X}",
                slot+1,next.entity[slot],next.motion[slot],next.animation[slot],next.user[slot],next.identity[slot]);
        targets=next;
    }
    UpdateNativeEngineActors(engine_actors);
    em1::UpdateOriginalInkDrips(ctx,base,engine_actors[0]);
    static bool clips_probed=false,graph_probed=false;
    if((!clips_probed || !graph_probed) && engine_actors[0].ready && REXCVAR_GET(em2_private_profile)) {
        const auto path=REXCVAR_GET(em2_qa_emr_clip_directory);
        if(!path.empty() && !clips_probed){
            clips_probed=true;std::string error;
            const bool okay=ProbeEMRAbilityClips(ctx,base,std::filesystem::path(path),error);
            REXLOG_INFO("EMR move clip native validation {}: {}",okay?"passed":"failed",error);
        }
        if(!path.empty() && !graph_probed && NativeAnimationGraphHasEvent(engine_actors[0].animation,405,base) &&
           NativeAnimationGraphHasEvent(engine_actors[0].animation,406,base)){
            graph_probed=true;REXLOG_INFO("EMR native movement graph registered on Mickey");
            std::string error;const bool okay=ProbeEMRAbilityClips(ctx,base,std::filesystem::path(path),error,engine_actors[0].animation);
            REXLOG_INFO("EMR live bound clip validation {}: {}",okay?"passed":"failed",error);
        }
    }
}

em2::GameplayTuning em2::GetGameplayTuning(){std::lock_guard lock(tuning_mutex);return tuning;}
bool em2::GameplayModOption(int id,int command,int& result){
    const int index=id-6000;if(index<0 || index>=18)return false;
    if(index==17){
        auto config=GetGameplayTuning();
        if(command==-2)config.pound_damage_type=GameplayTuning{}.pound_damage_type;
        else if(command==0 || command==1)config.pound_damage_type=command?PoundDamageType::Physical:PoundDamageType::Spin;
        else if(command!=-1)return false;
        result=config.pound_damage_type==PoundDamageType::Physical?1:0;
        return command==-1 || ChangeGameplayTuning(config);
    }
    if(index==16){auto config=GetGameplayTuning();if(command==-2)config.dash_spin_animation=GameplayTuning{}.dash_spin_animation;else if(command==0 || command==1)config.dash_spin_animation=command!=0;else if(command!=-1)return false;result=config.dash_spin_animation;return command==-1 || ChangeGameplayTuning(config);}
    if(index>=9){
        auto config=GetGameplayTuning();const GameplayTuning defaults;
        bool* flags[]={&config.keyboard_sprint_toggle,&config.controller_sprint_toggle,&config.native_animations,&config.pound_damage,&config.pound_knockback};
        const bool initial[]={defaults.keyboard_sprint_toggle,defaults.controller_sprint_toggle,defaults.native_animations,defaults.pound_damage,defaults.pound_knockback};
        if(index<14){const auto n=index-9;if(command==-2)*flags[n]=initial[n];else if(command>=0 && command<=1)*flags[n]=command!=0;else if(command!=-1)return false;result=*flags[n];}
        else{static constexpr float damage[]={.5f,1,2,3,5,10,25,50},radius[]={1,1.5f,2,3,4,6,8,12};const auto* options=index==14?damage:radius;auto& value=index==14?config.pound_damage_amount:config.pound_radius;
            if(command==-2)value=index==14?defaults.pound_damage_amount:defaults.pound_radius;else if(command>=0 && command<8)value=options[command];else if(command!=-1)return false;
            result=0;for(int i=1;i<8;++i)if(std::abs(options[i]-value)<std::abs(options[result]-value))result=i;
        }
        return command==-1 || ChangeGameplayTuning(config);
    }
    static constexpr float speeds[]={1,1.25f,1.5f,1.6f,1.75f,2,2.5f,3,4};
    static constexpr float dash[]={6,8,10,12,16,20,30,40};
    static constexpr float duration[]={.1f,.15f,.2f,.25f,.3f,.4f,.6f};
    static constexpr float cooldown[]={0,.25f,.4f,.65f,.8f,1,1.5f,2};
    static constexpr float pound[]={8,12,16,20,25,30,40,50};
    auto config=GetGameplayTuning();
    bool* flags[]={&config.sprint,&config.dash,&config.pound,&config.air_dash};
    float* values[]={&config.sprint_multiplier,&config.dash_speed,&config.dash_duration,&config.dash_cooldown,&config.pound_speed};
    const float* choices[]={speeds,dash,duration,cooldown,pound};const int sizes[]={9,8,7,8,8};
    if(command==-2){const GameplayTuning initial;const bool initial_flags[]={initial.sprint,initial.dash,initial.pound,initial.air_dash};const float initial_values[]={initial.sprint_multiplier,initial.dash_speed,initial.dash_duration,initial.dash_cooldown,initial.pound_speed};if(index<4)*flags[index]=initial_flags[index];else *values[index-4]=initial_values[index-4];}
    if(command>=0){if(index<4){if(command>1)return false;*flags[index]=command!=0;}
        else {if(command>=sizes[index-4])return false;*values[index-4]=choices[index-4][command];}}
    else if(command!=-1 && command!=-2)return false;
    if(command!=-1 && !ChangeGameplayTuning(config))return false;
    if(index<4)result=*flags[index];else{const auto n=index-4;result=0;for(int i=1;i<sizes[n];++i)if(std::abs(choices[n][i]-*values[n])<std::abs(choices[n][result]-*values[n]))result=i;}
    return true;
}
bool em2::ChangeGameplayTuning(const GameplayTuning& value){
    if(!Valid(value))return false;std::lock_guard lock(tuning_mutex);tuning=value;if(tuning_file.empty())return true;
    auto temporary=tuning_file;temporary+=".tmp";std::ofstream out(temporary);out<<std::setprecision(9)
        <<"sprint="<<value.sprint<<"\ndash="<<value.dash<<"\npound="<<value.pound<<"\nair_dash="<<value.air_dash
        <<"\nsprint_multiplier="<<value.sprint_multiplier<<"\ndash_speed="<<value.dash_speed<<"\ndash_duration="<<value.dash_duration
        <<"\ndash_cooldown="<<value.dash_cooldown<<"\npound_speed="<<value.pound_speed
        <<"\nkeyboard_sprint_toggle="<<value.keyboard_sprint_toggle<<"\ncontroller_sprint_toggle="<<value.controller_sprint_toggle
        <<"\nnative_animations="<<value.native_animations<<"\npound_damage="<<value.pound_damage<<"\npound_knockback="<<value.pound_knockback
        <<"\npound_damage_amount="<<value.pound_damage_amount<<"\npound_radius="<<value.pound_radius<<"\ndash_spin_animation="<<value.dash_spin_animation
        <<"\npound_damage_type="<<unsigned(value.pound_damage_type)<<'\n';out.close();
    return out.good() && MoveFileExW(temporary.c_str(),tuning_file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
}
void em2::InitializeGameplayMods(const std::filesystem::path& directory){
    GameplayTuning loaded;std::ifstream in(directory/"gameplay-mods.ini");std::string line;
    while(std::getline(in,line)){const auto split=line.find('=');if(split==line.npos)continue;const auto key=line.substr(0,split);std::istringstream text(line.substr(split+1));float number=0;char extra=0;if(!(text>>number) || text>>extra || !std::isfinite(number))continue;
        if(key=="sprint")loaded.sprint=number!=0;else if(key=="dash")loaded.dash=number!=0;else if(key=="pound")loaded.pound=number!=0;else if(key=="air_dash")loaded.air_dash=number!=0;
        else if(key=="keyboard_sprint_toggle")loaded.keyboard_sprint_toggle=number!=0;else if(key=="controller_sprint_toggle")loaded.controller_sprint_toggle=number!=0;
        else if(key=="native_animations")loaded.native_animations=number!=0;else if(key=="pound_damage")loaded.pound_damage=number!=0;else if(key=="pound_knockback")loaded.pound_knockback=number!=0;
        else if(key=="pound_damage_amount")loaded.pound_damage_amount=number;else if(key=="pound_radius")loaded.pound_radius=number;
        else if(key=="dash_spin_animation")loaded.dash_spin_animation=number!=0;
        else if(key=="pound_damage_type" && (number==2 || number==8))loaded.pound_damage_type=number==2?PoundDamageType::Physical:PoundDamageType::Spin;
        else if(key=="sprint_multiplier")loaded.sprint_multiplier=number;else if(key=="dash_speed")loaded.dash_speed=number;else if(key=="dash_duration")loaded.dash_duration=number;else if(key=="dash_cooldown")loaded.dash_cooldown=number;else if(key=="pound_speed")loaded.pound_speed=number;
    }
    std::lock_guard lock(tuning_mutex);tuning=Valid(loaded)?loaded:GameplayTuning{};tuning_file=directory/"gameplay-mods.ini";
}
bool em2::QueuePlayerImpulse(float x,float y,float z,float decay){
    if(em2::net::SynchronizedSessionActive() || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(decay) || std::abs(x)>500 || std::abs(y)>500 || std::abs(z)>500 || decay<0 || decay>1)return false;
    std::lock_guard lock(command_mutex);if(impulses.size()>=32)return false;impulses.push_back({x,y,z,decay});return true;
}
bool em2::SetPlayerSpeedOverride(float speed){if(!std::isfinite(speed) || (speed<0 && speed!=-1) || speed>500 || (speed>=0 && em2::net::SynchronizedSessionActive()))return false;console_speed.store(speed,std::memory_order_relaxed);return true;}
float em2::PlayerSpeedOverride(){return console_speed.load(std::memory_order_relaxed);}
// The player override 829FB9C0 reaches this base motion update at829FBBE0.
// Prepare before its force-accumulation call at8278DF08, not its later
// housekeeping call to8278C0A0: newly pressed moves belong to this tick.
PPC_FUNC_IMPL(sub_8278D4D8){const auto motion=ctx.r3.u32;const auto csr=_mm_getcsr();PrepareMotion(ctx,base,motion,ctx.f1.f64);_mm_setcsr(csr);__imp__sub_8278D4D8(ctx,base);}
PPC_FUNC_IMPL(sub_8278BF80){const auto motion=ctx.r3.u32;__imp__sub_8278BF80(ctx,base);const auto csr=_mm_getcsr();ApplyVelocity(motion,base);_mm_setcsr(csr);}

bool em2::TestGameplayMods(const std::filesystem::path& directory){
    auto check=[](bool ok,const char* reason){if(!ok)REXLOG_ERROR("Gameplay mod test: {}",reason);return ok;};
    GameplayTuning config;
    {
        struct RestoreSettings {
            GameplayTuning value=GetGameplayTuning();std::filesystem::path file=tuning_file;
            ~RestoreSettings(){std::lock_guard lock(tuning_mutex);tuning=value;tuning_file=file;}
        } restore;
        std::filesystem::create_directories(directory);InitializeGameplayMods(directory);
        ExecuteConsoleTool({"movement","dash_speed","42"});ExecuteConsoleTool({"movement","dash_cooldown","0"});ExecuteConsoleTool({"movement","sprint","off"});ExecuteConsoleTool({"movement","pound_damage_type","physical"});
        InitializeGameplayMods(directory);
        if(!check(GetGameplayTuning().dash_speed==42 && GetGameplayTuning().dash_cooldown==0 && !GetGameplayTuning().sprint && GetGameplayTuning().pound_damage_type==PoundDamageType::Physical,"console movement changes persist in isolated fixture"))return false;
        ExecuteConsoleTool({"movement","dash_speed","nan"});ExecuteConsoleTool({"movement","sprint","-1"});
        if(!check(GetGameplayTuning().dash_speed==42 && !GetGameplayTuning().sprint,"invalid console values preserve movement tuning"))return false;
        ExecuteConsoleTool({"movement","reset"});
        int choice=-1;
        if(!check(unsigned(PoundDamageType::Spin)==8 && unsigned(PoundDamageType::Physical)==2 &&
            GameplayModOption(6017,-1,choice) && choice==0,"ground-pound defaults use retail spin stimulus 8"))return false;
        if(!check(GameplayModOption(6017,1,choice) && choice==1 && GetGameplayTuning().pound_damage_type==PoundDamageType::Physical,"native menu applies physical damage immediately"))return false;
        if(!check(!GameplayModOption(6017,2,choice) && GetGameplayTuning().pound_damage_type==PoundDamageType::Physical,"unsupported damage choices leave the current mode unchanged"))return false;
        auto invalid=GetGameplayTuning();invalid.pound_damage_type=static_cast<PoundDamageType>(3);
        if(!check(!ChangeGameplayTuning(invalid),"knockback cannot be substituted as the primary damage type"))return false;
        invalid.pound_damage_type=static_cast<PoundDamageType>(7);
        if(!check(!ChangeGameplayTuning(invalid),"magic cannot be substituted for melee damage"))return false;
        InitializeGameplayMods(directory);
        if(!check(GetGameplayTuning().pound_damage_type==PoundDamageType::Physical,"saved physical damage type survives an INI round trip"))return false;
        if(!check(GameplayModOption(6017,-2,choice) && choice==0 && GetGameplayTuning().pound_damage_type==PoundDamageType::Spin,"native menu restores spin damage without restarting"))return false;
        InitializeGameplayMods(directory);
        if(!check(GetGameplayTuning().pound_damage_type==PoundDamageType::Spin,"saved spin damage type survives an INI round trip"))return false;
        {std::ofstream legacy(directory/"gameplay-mods.ini");legacy<<"sprint_multiplier=2.5\npound_damage=0\npound_damage_type=2.5\n";}
        InitializeGameplayMods(directory);
        if(!check(GetGameplayTuning().pound_damage_type==PoundDamageType::Spin && GetGameplayTuning().sprint_multiplier==2.5f && !GetGameplayTuning().pound_damage,"legacy and invalid damage-type entries preserve other user choices"))return false;
    }
    {MotionModel pad;pad.Advance(config,{true,false,false,true},true,false,1./30,true);pad.Advance(config,{},true,false,1./30,true);if(!check(pad.sprinting,"controller sprint toggles on a click and survives release"))return false;
        pad.Advance(config,{true,false,false,true},true,false,1./30,true);if(!check(!pad.sprinting,"a second controller click releases sprint"))return false;}
    for(unsigned hz:{30u,60u,120u,240u}){
        MotionModel state;unsigned dash_ticks=0;for(unsigned frame=0;frame<hz;++frame){state.Advance(config,{false,frame==0,false},true,false,1./hz,true);dash_ticks+=state.dashing;}
        if(!check(std::abs(double(dash_ticks)/hz-config.dash_duration)<1.01/hz,"dash duration follows simulation time at every timestep"))return false;
    }
    MotionModel state;state.Advance(config,{true,false,false},true,false,1./30,true);if(!check(state.sprinting,"sprint starts while held on the ground"))return false;
    state.Advance(config,{true,true,false},false,true,1./30,true);if(!check(state.dashing && !state.sprinting && state.air_dash_used,"air dash admits one charge and cancels sprint"))return false;
    state.Advance(config,{},false,true,1,true);state.Advance(config,{false,true,false},false,true,1./30,true);if(!check(!state.dashing,"air dash cannot recharge before landing"))return false;
    state.Advance(config,{},true,false,1./30,true);state.Advance(config,{false,true,false},false,true,1./30,true);if(!check(state.dashing,"landing restores the air dash charge"))return false;
    state.Advance(config,{false,false,true},false,true,1./30,true);if(!check(state.pounding && !state.dashing,"ground pound cancels a dash in the air"))return false;
    state.Advance(config,{},true,false,1./30,true);if(!check(!state.pounding,"ground contact ends a pound"))return false;
    state.Advance(config,{true,true,true},false,true,1./30,false);if(!check(!state.pounding && !state.dashing && !state.sprinting,"menus and inactive control ownership cancel movement mods"))return false;
    auto invalid=config;invalid.dash_speed=INFINITY;if(!check(!Valid(invalid),"nonfinite tuning is rejected"))return false;
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));if(!base)return false;
    struct Release {uint8_t* base;unsigned csr=_mm_getcsr();~Release(){_mm_setcsr(csr);VirtualFree(base,0,MEM_RELEASE);}}release{base};
    if(!VirtualAlloc(base,0x10000,MEM_COMMIT,PAGE_READWRITE) || !VirtualAlloc(base+0x833D0000,0x10000,MEM_COMMIT,PAGE_READWRITE))return false;
    constexpr uint32_t motion=0x1000,table=0x4000,entity=0x5000,types=0x5200,components=0x5400,vector=0x6000;
    auto put=[&](uint32_t address,float value){PPC_STORE_U32(address,std::bit_cast<uint32_t>(value));};
    auto get=[&](uint32_t address){return std::bit_cast<float>(PPC_LOAD_U32(address));};
    PPC_STORE_U8(0x833DAF54,17);PPC_STORE_U32(entity+88,types);PPC_STORE_U32(entity+36,components);PPC_STORE_U8(types+17,0);PPC_STORE_U32(components,motion+8);
    const auto old_targets=[](){std::lock_guard lock(target_mutex);return targets;}();
    struct RestoreTargets {Targets saved;~RestoreTargets(){std::lock_guard lock(target_mutex);targets=saved;}}restore_targets{old_targets};
    if(!check(!MotionForEntity(1,base) && !MotionForEntity(0x300000,base),"startup sentinels and uncommitted entities are rejected without a guest read"))return false;
    PPC_STORE_U32(entity+88,1);if(!check(!MotionForEntity(entity,base),"startup component-type table sentinels are rejected"))return false;PPC_STORE_U32(entity+88,types);
    PPC_STORE_U32(entity+36,1);if(!check(!MotionForEntity(entity,base),"startup component-array sentinels are rejected"))return false;PPC_STORE_U32(entity+36,components);
    PPC_STORE_U32(components,1);if(!check(!MotionForEntity(entity,base),"startup component-entry sentinels are rejected"))return false;PPC_STORE_U32(components,motion+8);
    constexpr uint32_t manager=0x8000,profile=0x9000,camera=0xA000;
    PPCContext ctx{};ctx.fpscr.InitHost();ctx.r1.u64=0xF000;ctx.r3.u64=motion;ctx.lr=0x12345678;
    PPC_STORE_U32(0x833DAB60,manager);PPC_STORE_U32(0x833DC87C,profile);PPC_STORE_U32(profile+24,1);
    PPC_STORE_U32(manager+20,1);PPC_STORE_U32(manager+28,1);PPC_STORE_U32(manager+340,1);
    UpdateGameplayModTargets(ctx,base);
    if(!check(FindBinding(motion,base).actor<0,"launch-time manager placeholder records never become movement targets"))return false;
    constexpr uint32_t registry=0xB000,buckets=0xC000,node=0xD000,input=0xE000,handle=0x12345678;
    for(const auto callback:{0x827B58D0u,0x82A69948u}){
        const auto offset=PPC_IMAGE_BASE+PPC_IMAGE_SIZE+(uint64_t(callback-PPC_CODE_BASE)*2);
        if(!VirtualAlloc(base+(offset&~uint64_t(4095)),4096,MEM_COMMIT,PAGE_READWRITE))return false;
    }
    PPC_LOOKUP_FUNC(base,0x827B58D0)=__imp__sub_827B58D0;
    PPC_LOOKUP_FUNC(base,0x82A69948)=__imp__sub_82A69948;
    PPC_STORE_U32(0x833DA7E4,registry);PPC_STORE_U32(registry+4,0x827B58D0);PPC_STORE_U32(registry+8,0x82A69948);
    PPC_STORE_U32(registry+16,buckets);PPC_STORE_U32(registry+20,1);PPC_STORE_U32(buckets,node);
    PPC_STORE_U32(node,handle);PPC_STORE_U32(node+4,entity);PPC_STORE_U32(node+8,0);
    PPC_STORE_U32(motion+80,input);PPC_STORE_U32(input+176,input+184);PPC_STORE_U8(input+145,1);
    PPC_STORE_U8(types,255);
    PPC_STORE_U32(manager+20,handle);PPC_STORE_U32(manager+28,camera);PPC_STORE_U32(camera,0x82112778);
    PPC_STORE_U32(camera+564,1);PPC_STORE_U32(camera+392,0xB000);PPC_STORE_U32(camera+24,0xB000);
    const auto lookup_context=ctx;const auto lookup_csr=_mm_getcsr();
    UpdateGameplayModTargets(ctx,base);
    if(!check(FindBinding(motion,base).actor==0 && FindBinding(motion,base).entity==entity && FindBinding(motion,base).user==0,"retail handle-registry lookup resolves a controlled player while unused P2 is still a sentinel"))return false;
    if(!check(!std::memcmp(&ctx,&lookup_context,sizeof(ctx)) && _mm_getcsr()==lookup_csr,"native player resolution preserves caller registers and host FP state"))return false;
    PPC_STORE_U32(manager+28,1);PPC_STORE_U32(manager+32,3);UpdateGameplayModTargets(ctx,base);
    if(!check(FindBinding(motion,base).user==0,"2D controllers do not require the 3D camera class or control mode"))return false;
    PPC_STORE_U8(input+145,0);UpdateGameplayModTargets(ctx,base);
    if(!check(FindBinding(motion,base).user<0,"native disabled input blocks gameplay abilities"))return false;
    PPC_STORE_U8(input+145,1);PPC_STORE_U32(manager+20,handle+1);UpdateGameplayModTargets(ctx,base);
    if(!check(FindBinding(motion,base).actor<0,"missing/stale entity handles clear cached movement targets"))return false;
    PPC_STORE_U32(manager+20,handle);PPC_STORE_U32(registry+4,0);UpdateGameplayModTargets(ctx,base);
    if(!check(FindBinding(motion,base).actor<0,"partially initialized native registries are not called"))return false;
    PPC_STORE_U32(0x833DAB60,0x300000);UpdateGameplayModTargets(ctx,base);
    if(!check(FindBinding(motion,base).actor<0,"uncommitted manager records clear the cached targets"))return false;
    if(!check(MotionForEntity(entity,base)==motion,"original inherited BasicMotion type lookup resolves the owning component"))return false;
    {PPCContext lookup{};lookup.r3.u64=entity;__imp__sub_82740440(lookup,base);if(!check(lookup.r3.u32==motion,"component lookup agrees with the original retail instructions"))return false;}
    PPC_STORE_U8(types+17,255);if(!check(!MotionForEntity(entity,base),"missing motion components are rejected"))return false;
    {
        struct RestorePipeline {
            GameplayTuning config=GetGameplayTuning();
            std::array<ActorState,2> states=actors;
            uint64_t epoch=simulation_steps.load();
            PPCContext& context;PPCContext saved;
            std::array<int,6> bindings{};int menu=0;
            RestorePipeline(PPCContext& c):context(c),saved(c){
                ControlOption(2201,-1,menu);
                for(unsigned i=0;i<6;++i)ControlOption(2000+(i/3)*32+25+i%3,-1,bindings[i]);
            }
            ~RestorePipeline(){
                {std::lock_guard lock(tuning_mutex);tuning=config;}
                {std::lock_guard lock(state_mutex);actors=states;}
                simulation_steps.store(epoch);context=saved;ClearControlKeys();
                int result;for(unsigned i=0;i<6;++i)ControlOption(2000+(i/3)*32+25+i%3,bindings[i],result);
                ControlOption(2201,menu,result);
            }
        } restore_pipeline(ctx);
        auto movement_config=config;movement_config.native_animations=false;movement_config.pound_damage=false;movement_config.pound_knockback=false;
        {std::lock_guard lock(tuning_mutex);tuning=movement_config;}
        {std::lock_guard lock(state_mutex);actors={};}
        int result;ControlOption(2201,0,result);SetExternalControlCapture(false);SetControlUIContext(false);ClearControlKeys();
        const std::array<int,6> bindings={VK_SHIFT,VK_CONTROL,'R',264,257,259};
        for(unsigned i=0;i<bindings.size();++i)ControlOption(2000+(i/3)*32+25+i%3,bindings[i],result);
        PPC_STORE_U32(0x833DAB60,manager);PPC_STORE_U32(registry+4,0x827B58D0);PPC_STORE_U32(manager+20,handle);PPC_STORE_U8(types+17,0);
        PPC_STORE_U32(motion+108,0x7000);put(0x7000+72,5);
        PPC_STORE_U32(motion+548,table);PPC_STORE_U32(motion+552,0);PPC_STORE_U32(motion+556,4);
        PPC_STORE_U8(motion+640,1);PPC_STORE_U32(motion+208,1);put(motion+728,1);put(motion+736,0);
        auto tick=[&](unsigned key,uint16_t buttons,unsigned native_state,bool reset_input=true){
            simulation_steps.fetch_add(1);if(reset_input)ClearControlKeys();if(key)ControlKey(key,true);
            rex::input::X_INPUT_STATE physical{};physical.gamepad.buttons=buttons;RemapLocalControlState(0,physical);
            PPC_STORE_U32(motion+384,native_state);UpdateGameplayModTargets(ctx,base);
            PrepareMotion(ctx,base,motion,1./30);
            put(motion+240,1);put(motion+244,2);put(motion+248,3);
            ctx.r3.u64=motion;sub_8278BF80(ctx,base);
        };
        tick(0,0,0); // Release controls suppressed by leaving a menu.
        tick(VK_SHIFT,0,0);
        if(!check(actors[0].model.sprinting && PPC_LOAD_U32(motion+552)==1 && get(table+4)==8,"keyboard sprint reaches native priority storage through resolved player ownership"))return false;
        tick(0,0,0);
        if(!check(!actors[0].model.sprinting && !PPC_LOAD_U32(motion+552),"releasing keyboard sprint removes its native speed override"))return false;
        tick(VK_CONTROL,0,0);
        if(!check(actors[0].model.dashing && get(motion+240)==12 && get(motion+248)==0,"keyboard dash reaches the actual post-force velocity hook"))return false;
        PPC_STORE_U32(node,handle+2);PPC_STORE_U32(manager+20,handle+2);UpdateGameplayModTargets(ctx,base);
        put(motion+240,1);put(motion+244,2);put(motion+248,3);ctx.r3.u64=motion;sub_8278BF80(ctx,base);
        if(!check(get(motion+240)==1,"the force hook rejects a stale actor identity before its next preparation callback"))return false;
        tick(0,0,0);
        if(!check(!actors[0].model.dashing && get(motion+240)==1,"a new entity handle at recycled entity/motion addresses cannot inherit the old dash"))return false;
        PPC_STORE_U32(node,handle);PPC_STORE_U32(manager+20,handle);tick(0,0,0);
        tick('R',0,2);
        if(!check(actors[0].model.pounding && get(motion+244)==-20 && get(motion+240)==0,"keyboard airborne pound reaches native vertical velocity"))return false;
        SetControlUIContext(true);tick(VK_CONTROL,0,0);
        if(!check(!actors[0].model.dashing && get(motion+240)==1 && get(motion+244)==2,"native UI ownership cancels abilities before the force adapter"))return false;
        SetControlUIContext(false);tick(0,0,0);
        tick(0,0x40,0);
        if(!check(actors[0].model.sprinting && get(table+4)==8,"controller LS starts native sprint through the physical input bridge"))return false;
        tick(0,0,0);
        if(!check(actors[0].model.sprinting,"controller sprint persists after releasing the toggle button"))return false;
        tick(0,0x40,0);
        if(!check(!actors[0].model.sprinting && !PPC_LOAD_U32(motion+552),"a second controller click removes the sprint override"))return false;
        tick(0,0,0);tick(0,0x2000,0);
        if(!check(!actors[0].model.dashing,"B press does not dash before a companion hold can be distinguished"))return false;
        tick(0,0,0,false);
        if(!check(actors[0].model.dashing && get(motion+240)==12,"controller B tap drives native dash on release without keyboard input"))return false;
        tick(0,0x8000,2);
        if(!check(actors[0].model.pounding && get(motion+244)==-20,"controller Y drives the airborne pound velocity hook"))return false;
        PPC_STORE_U8(input+145,0);tick(VK_CONTROL,0,0);
        if(!check(!actors[0].model.dashing && get(motion+240)==1,"native disabled input cancels and blocks ability velocity"))return false;
        PPC_STORE_U8(input+145,1);PPC_STORE_U32(manager+20,handle+1);tick(VK_CONTROL,0,0);
        if(!check(get(motion+240)==1 && get(motion+244)==2,"level-transition target loss cannot write the old actor's velocity"))return false;
        REXLOG_INFO("Native movement pipeline tests passed: physical KBM/pad bindings -> retail handle lookup -> movement preparation -> post-force hook; UI, input-disable and transition gates");
    }
    if(!check(TestNativeEngine() && TestNativeEngineMemory(ctx,base,0xB000),"native animation event queue, caller state preservation and invalid combat targets"))return false;
    PPC_STORE_U32(motion+108,0x7000);put(0x7000+72,5);
    PPC_STORE_U32(motion+548,table);PPC_STORE_U32(motion+552,2);PPC_STORE_U32(motion+556,4);
    PPC_STORE_U32(table,2);put(table+4,2);PPC_STORE_U32(table+8,uint32_t(sprint_priority));put(table+12,5);
    const auto saved=ctx;const auto csr=_mm_getcsr();SetSpeed(ctx,base,motion,sprint_priority,8);
    if(!check(get(table+12)==8 && PPC_LOAD_U32(table)==2 && get(table+4)==2 && std::memcmp(&ctx,&saved,sizeof(ctx))==0 && _mm_getcsr()==csr,"native speed updates preserve scripted priorities, guest registers and FP mode"))return false;
    ctx.r3.u64=motion;__imp__sub_82787440(ctx,base);if(!check(ctx.f1.f64==2,"authored higher-priority speed constraints win over sprint"))return false;
    if(!check(BaseMoveSpeed(motion,base)==5,"scripted slowdown does not contaminate the sprint's authored baseline"))return false;
    put(0x7000+72,6);if(!check(BaseMoveSpeed(motion,base)==6,"live movement-stat changes update a held sprint's baseline"))return false;
    RemoveSpeed(ctx,base,motion,sprint_priority);if(!check(PPC_LOAD_U32(motion+552)==1 && PPC_LOAD_U32(table)==2 && get(table+4)==2,"releasing sprint removes only its own native speed override"))return false;
    put(vector,4);put(vector+4,5);put(vector+8,6);put(vector+12,0);ctx.r3.u64=motion;ctx.r4.u64=vector;ctx.f1.f64=.5;__imp__sub_82786E88(ctx,base);
    if(!check(get(motion+912)==4 && get(motion+916)==5 && get(motion+920)==6 && get(motion+928)==.5f,"console impulses use the original native external-velocity setter"))return false;
    PPC_STORE_U8(motion+640,1);PPC_STORE_U32(motion+208,1);put(motion+240,1);put(motion+244,2);put(motion+248,3);ctx.r3.u64=motion;__imp__sub_8278BF80(ctx,base);
    if(!check(get(motion+240)==5 && get(motion+244)==7 && get(motion+248)==9 && get(motion+912)==2 && get(motion+916)==2.5f && get(motion+920)==3,"original force accumulator and per-tick decay execute before the movement adapter"))return false;
    ActorState actor;actor.model.dashing=true;actor.dash_x=1;actor.dash_z=0;
    ApplyRequestedVelocity(motion,actor,config,base);
    if(!check(get(motion+240)==12 && get(motion+248)==0 && get(motion+244)==7 && get(motion+912)==2,"dash changes native horizontal velocity intent and retains vertical/externally authored state"))return false;
    actor.model.dashing=false;actor.model.pounding=true;ApplyRequestedVelocity(motion,actor,config,base);
    if(!check(get(motion+244)==-20 && get(motion+240)==0 && get(motion+248)==0 && get(motion+912)==2,"pound cancels lateral motion and targets native vertical velocity without moving scene transforms"))return false;
    REXLOG_INFO("Gameplay movement model tests passed; original level collision, animation and landing effects still require gameplay validation");return true;
}
