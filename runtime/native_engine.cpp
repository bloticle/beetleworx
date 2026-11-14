#include "online_actor_input.h"
#include "online_actor_jump.h"
#include "online_actor_roster.h"
#include "online_actor_frames.h"
#include "online_actor_reference.h"
#include "online_owned_probe.h"
#include "online_local_movement.h"
#include "online_local_jump.h"
#include "online_movement_frame.h"
#include "guest_read_batch.h"
#include "native_engine.h"
#include "native_player_spawn.h"
#include "native_ability_events.h"
#include "engine_workbench.h"
#include "console_tools.h"
#include "online.h"
#include "online_runtime.h"
#include "players.h"
#include "ppc_config.h"
#include "ppc_context.h"
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <chrono>
#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(std::string,em2_user_data_directory);
REXCVAR_DEFINE_INT32(em2_probe_extra_native_players,0,"Debug","Private one-shot: construct, admit and destroy 1..14 additional native characters without assigning controls or replacing retail slots");
REXCVAR_DEFINE_BOOL(em2_probe_owned_native_player_reject,false,"Debug","Private owned-player probe: force pre-admission rejection to verify native rollback");
REXCVAR_DEFINE_BOOL(em2_probe_owned_native_player,false,"Debug","Private single-human probe: quarantine broadcast input and attach a native owned movement reference; no movement injection");
REXCVAR_DEFINE_INT32(em2_probe_owned_native_player_character,0,"Debug","Private owned-player probe character: 0 Mickey, 1 human Oswald; never a retail player slot");
REXCVAR_DECLARE(bool,em2_probe_owned_native_player_input);
REXCVAR_DEFINE_BOOL(em2_probe_online_local_movement,false,"Debug","Private read-only Movement binding/math probe; never dispatches controls or advances actors");
REXCVAR_DEFINE_INT32(em2_probe_extra_native_players_hold_seconds,0,"Debug","Private extra-character demo lifetime, 0..900 seconds; zero runs the immediate cleanup test");

PPC_EXTERN_FUNC(__imp__sub_825430D8);
PPC_EXTERN_FUNC(__imp__sub_82843878);
PPC_EXTERN_FUNC(__imp__sub_8283E658);
PPC_EXTERN_FUNC(__imp__sub_8283E778);
PPC_EXTERN_FUNC(__imp__sub_82393300);
PPC_EXTERN_FUNC(__imp__sub_8275C160);
PPC_EXTERN_FUNC(__imp__sub_82786E88);
PPC_EXTERN_FUNC(__imp__sub_8295CC38);
PPC_EXTERN_FUNC(__imp__sub_82AAB408);
PPC_EXTERN_FUNC(__imp__sub_827FD850);
PPC_EXTERN_FUNC(__imp__sub_82821AC8);
PPC_EXTERN_FUNC(__imp__sub_822F1C10);
PPC_EXTERN_FUNC(sub_8283E658);
PPC_EXTERN_FUNC(sub_8283E778);
PPC_EXTERN_FUNC(__imp__sub_8276A828);
PPC_EXTERN_FUNC(__imp__sub_827403B0);
PPC_EXTERN_FUNC(__imp__sub_82732A18);
PPC_EXTERN_FUNC(__imp__sub_82744E90);
PPC_EXTERN_FUNC(__imp__sub_82742F70);
PPC_EXTERN_FUNC(__imp__sub_827CAE68);
PPC_EXTERN_FUNC(__imp__sub_8275C728);
PPC_EXTERN_FUNC(__imp__sub_8275C970);
PPC_EXTERN_FUNC(__imp__sub_827CA5F0);
PPC_EXTERN_FUNC(__imp__sub_82747128);
PPC_EXTERN_FUNC(__imp__sub_8308B3E8);
PPC_EXTERN_FUNC(__imp__sub_827AC868);

namespace {
struct SpawnContactTrace {uint32_t body=0,other=0,contacts=0;};
thread_local SpawnContactTrace spawn_contact_trace;
bool Readable(uint32_t address,size_t count,uint8_t* base){
    if(address<4096 || !count || uint64_t(address)+count>0x100000000ull)return false;
    return em2::GuestReadBatch::Readable(base+address,count);
}
struct Scope {
    PPCContext& ctx;PPCContext saved;unsigned csr;uint32_t stack;
    explicit Scope(PPCContext& c):ctx(c),saved(c),csr(_mm_getcsr()),stack(c.r1.u32-2048){ctx.r1.u64=stack;}
    ~Scope(){ctx=saved;_mm_setcsr(csr);}
};
// NiFixedString references for reflected properties and stimulus aliases.
// Animation events and variables use the separate numeric registries below.
std::unordered_map<std::string,uint32_t> identifiers;
struct AnimationOwner {std::atomic<uint32_t> animation{0},owned{0},interrupt{0};};
std::array<AnimationOwner,2> animation_owners;
std::array<std::atomic<uint32_t>,5> locomotion_ids{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
std::array<std::atomic<uint32_t>,17> interrupt_ids{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
std::atomic<uint32_t> land_id{UINT32_MAX};
std::unordered_map<std::string,uint32_t> event_ids,variable_ids;
bool event_registry_loaded=false,variable_registry_loaded=false;
constexpr uint32_t event_registry=0x833DCDC0,variable_registry=0x833DD858;
constexpr std::array<std::string_view,14> native_commands={"entity.inspect","entity.components","entity.getfloat","entity.setfloat","entity.push","anim.snapshot","anim.events","anim.vars","anim.event","anim.float","anim.int","anim.overrides","anim.release","combat.sphere"};
std::atomic<int> console_target{-1};
std::atomic<std::shared_ptr<const em2::NativeGraphSnapshot>> graph_snapshot;
struct CompletionMetadata {std::vector<std::string> events,variables;};
std::atomic<std::shared_ptr<const CompletionMetadata>> completion_metadata{std::make_shared<const CompletionMetadata>()};
bool completion_metadata_published=false;
void PublishCompletionMetadata(){
    auto value=std::make_shared<CompletionMetadata>();
    for(const auto& [name,id]:event_ids)value->events.push_back(name);
    for(const auto& alias:em2::kNativeAbilityEvents)if(event_ids.contains(alias.registered))value->events.emplace_back(alias.alias);
    for(const auto& [name,id]:variable_ids)value->variables.push_back(name);
    for(auto* names:{&value->events,&value->variables}){std::sort(names->begin(),names->end());names->erase(std::unique(names->begin(),names->end()),names->end());}
    completion_metadata.store(std::move(value),std::memory_order_release);
}
std::optional<std::string> GuestString(uint32_t pointer,uint8_t* base){
    if(!Readable(pointer,1,base))return {};std::string text;const bool contiguous=Readable(pointer,160,base);
    for(unsigned i=0;i<160;++i){if(!contiguous && !Readable(pointer+i,1,base))return {};const auto c=base[pointer+i];if(!c)return text;if(c<32 || c>=127)return {};text.push_back(char(c));}
    return {};
}
std::optional<uint32_t> AnimationIdentifier(const std::string& input,bool variable,uint8_t* base){
    std::string name=input;if(!variable)for(const auto& alias:em2::kNativeAbilityEvents)if(input==alias.alias){name=alias.registered;break;}
    auto& cache=variable?variable_ids:event_ids;if(const auto found=cache.find(name);found!=cache.end())return found->second;
    auto& loaded=variable?variable_registry_loaded:event_registry_loaded;if(loaded)return {};
    const uint32_t registry=variable?variable_registry:event_registry,count=variable?122:675;
    if(!Readable(registry,count*4,base))return {};
    uint32_t valid=0;for(uint32_t i=0;i<count;++i)if(const auto text=GuestString(PPC_LOAD_U32(registry+i*4),base);text && !text->empty()){cache.emplace(*text,i);++valid;}
    loaded=valid==count;if(const auto found=cache.find(name);found!=cache.end())return found->second;
    return {};
}
struct AnimationOverride {unsigned actor;uint32_t identifier;std::string name;float value;bool integer;};
using Overrides=std::vector<AnimationOverride>;
std::atomic<std::shared_ptr<const Overrides>> overrides{std::make_shared<const Overrides>()};
std::atomic<bool> has_overrides{false};
std::optional<float> OverrideValue(uint32_t animation,uint32_t identifier,bool integer){
    if(!has_overrides.load(std::memory_order_acquire) || em2::net::SynchronizedSessionActive())return {};
    for(unsigned actor=0;actor<2;++actor)if(animation && animation_owners[actor].animation.load(std::memory_order_acquire)==animation){
        const auto snapshot=overrides.load(std::memory_order_acquire);
        for(const auto& entry:*snapshot)if(entry.actor==actor && entry.identifier==identifier && entry.integer==integer)return entry.value;
    }
    return {};
}
bool SetAnimationOverride(unsigned actor,uint32_t identifier,const std::string& name,float value,bool integer){
    auto next=std::make_shared<Overrides>(*overrides.load(std::memory_order_acquire));
    const auto old=std::find_if(next->begin(),next->end(),[&](const auto& entry){return entry.actor==actor && entry.identifier==identifier;});
    if(old!=next->end())*old={actor,identifier,name,value,integer};else{if(next->size()>=64)return false;next->push_back({actor,identifier,name,value,integer});}
    overrides.store(next,std::memory_order_release);has_overrides.store(!next->empty(),std::memory_order_release);return true;
}
bool PassAnimationEvent(uint32_t animation,uint32_t id){
    for(auto& owner:animation_owners)if(animation && owner.animation.load(std::memory_order_acquire)==animation && owner.owned.load(std::memory_order_acquire)){
        if(id!=UINT32_MAX && id==land_id.load(std::memory_order_relaxed)){owner.owned.store(0,std::memory_order_release);return true;}
        for(const auto& interrupt:interrupt_ids)if(id!=UINT32_MAX && id==interrupt.load(std::memory_order_relaxed)){owner.owned.store(0,std::memory_order_release);owner.interrupt.fetch_add(1,std::memory_order_release);return true;}
        for(unsigned i=0;i<locomotion_ids.size();++i)if(id!=UINT32_MAX && id==locomotion_ids[i].load(std::memory_order_relaxed)){
            // A sprint owns only grounded locomotion. Jump/fall must retain
            // their original transitions immediately, including the first tick.
            if(owner.owned.load(std::memory_order_relaxed)==3 && (i==0 || i==1 || i==4)){
                owner.owned.store(0,std::memory_order_release);return true;
            }
            return false;
        }
    }
    return true;
}
uint32_t Identifier(PPCContext& ctx,uint8_t* base,const std::string& name){
    if(name.empty() || name.size()>127 || !std::all_of(name.begin(),name.end(),[](unsigned char c){return c>=32 && c<127;}))return 0;
    if(const auto found=identifiers.find(name);found!=identifiers.end())return found->second;
    if(identifiers.size()>=256)return 0;
    Scope scope(ctx);const auto text=scope.stack+512;if(!Readable(scope.stack,2048,base))return 0;
    std::memcpy(base+text,name.c_str(),name.size()+1);ctx.r3.u64=text;__imp__sub_825430D8(ctx,base);
    const auto id=ctx.r3.u32;if(!Readable(id,1,base))return 0;identifiers.emplace(name,id);return id;
}
void PrimeAnimationEvents(PPCContext& ctx,uint8_t* base){
    if(land_id.load(std::memory_order_acquire)!=UINT32_MAX)return;
    constexpr const char* locomotion[]={"EVENT_Start_InAir","EVENT_Start_Fall","EVENT_Start_Running_On_Ground","EVENT_Start_Walking_On_Ground","EVENT_Start_Jump"};
    constexpr const char* interrupts[]={"EVENT_Start_Damaged_Leap","EVENT_Start_Ledge_Grab","EVENT_Start_Death_Mickey","EVENT_Start_Death_Oswald","EVENT_Start_Death_Fall_Rescue","EVENT_Start_Death_Mickey_Drown","EVENT_Start_Death_Oswald_Drown","EVENT_Start_Knockback_FallDown","EVENT_Start_Death_Mickey_Knockback","EVENT_Start_Death_Oswald_Knockback","EVENT_Start_Dynamic","EVENT_Start_DynamicCutscene_01","EVENT_Start_DynamicCutscene_02","EVENT_Start_DynamicCutscene_03","EVENT_Force_Idle","EVENT_Start_DoubleJump","EVENT_Start_HitReaction"};
    for(unsigned i=0;i<locomotion_ids.size();++i)locomotion_ids[i].store(AnimationIdentifier(locomotion[i],false,base).value_or(UINT32_MAX),std::memory_order_relaxed);
    for(unsigned i=0;i<interrupt_ids.size();++i)interrupt_ids[i].store(AnimationIdentifier(interrupts[i],false,base).value_or(UINT32_MAX),std::memory_order_relaxed);
    land_id.store(AnimationIdentifier("EVENT_Start_Land",false,base).value_or(UINT32_MAX),std::memory_order_release);
}
bool AnimationReady(uint32_t animation,uint8_t* base){
    em2::GuestReadBatch inspection;
    if(!Readable(animation,212,base))return false;
    const auto graph=PPC_LOAD_U32(animation+192),context=PPC_LOAD_U32(animation+208);
    if(!Readable(graph,128,base) || !Readable(context,16,base))return false;
    auto queue=PPC_LOAD_U32(context+12);
    if(!queue){const auto table=PPC_LOAD_U32(context);if(!Readable(table,96,base))return false;queue=PPC_LOAD_U32(table+92);}
    return Readable(queue,20,base) && PPC_LOAD_U32(queue+4)<=65536 && PPC_LOAD_U32(queue+16)<=PPC_LOAD_U32(queue+4);
}
bool AnimationVariable(PPCContext& ctx,uint8_t* base,uint32_t animation,const std::string& name,float value,bool integer){
    if(!std::isfinite(value) || !AnimationReady(animation,base))return false;
    const auto graph=PPC_LOAD_U32(animation+192),mapping=PPC_LOAD_U32(graph+116);
    if(!Readable(mapping,36,base))return false;
    const auto id=AnimationIdentifier(name,true,base);if(!id)return false;Scope scope(ctx);
    ctx.r3.u64=mapping+20;ctx.r4.u64=*id;ctx.r5.s64=-1;__imp__sub_82393300(ctx,base);
    if(ctx.r3.s32<0 || ctx.r3.s32>4096)return false;
    const auto values=PPC_LOAD_U32(graph+124);if(!Readable(values,12,base) || !Readable(PPC_LOAD_U32(values+8),size_t(ctx.r3.u32)*4+4,base))return false;
    ctx.r3.u64=animation;ctx.r4.u64=*id;
    if(integer){ctx.r5.s64=int32_t(value);__imp__sub_8283E778(ctx,base);}
    else{ctx.f1.f64=value;__imp__sub_8283E658(ctx,base);}return true;
}
bool VirtualMethod(uint32_t object,unsigned offset,uint32_t& function,uint8_t* base){
    if(!Readable(object,4,base))return false;const auto table=PPC_LOAD_U32(object);
    if(!Readable(table,offset+4,base))return false;function=PPC_LOAD_U32(table+offset);
    return function>=0x822F0000 && function<0x83350000 && !(function&3);
}
bool PropertyFloat(PPCContext& ctx,uint8_t* base,uint32_t entity,const std::string& name,float& value,bool write){
    const auto id=Identifier(ctx,base,name);uint32_t query=0,method=0;
    if(!id || !VirtualMethod(entity,88,query,base) || !VirtualMethod(entity,write?276:212,method,base))return false;
    Scope scope(ctx);const auto property=scope.stack+512,type=property+4,result=type+4;
    PPC_STORE_U32(property,id);PPC_STORE_U32(type,0);PPC_STORE_U32(result,0);
    ctx.r3.u64=entity;ctx.r4.u64=property;ctx.r5.u64=type;PPC_CALL_INDIRECT_FUNC(query);
    const auto actual=PPC_LOAD_U32(type);const bool matches=ctx.r3.u32 && actual==PPC_LOAD_U32(0x833CD850);
    if(actual){ctx.r3.u64=type;__imp__sub_822F1C10(ctx,base);}
    if(!matches)return false;
    ctx.r3.u64=entity;ctx.r4.u64=property;ctx.r5.u64=write?0:result;ctx.r6.u64=0;ctx.f1.f64=value;
    PPC_CALL_INDIRECT_FUNC(method);
    if(!write)value=std::bit_cast<float>(PPC_LOAD_U32(result));return true;
}
enum class Kind {Inspect,Components,Snapshot,Event,Float,Int,GetFloat,SetFloat,Sphere,Events,Variables,Release,Overrides,Push};
struct Command {Kind kind;int actor=-1;std::string name;float a=0,b=0;unsigned type=0;std::array<float,4> impulse{};};
bool PushMotion(PPCContext& ctx,uint8_t* base,uint32_t motion,const std::array<float,4>& impulse){
    if((motion&15) || !Readable(motion,932,base))return false;
    for(unsigned i=0;i<4;++i)if(!std::isfinite(impulse[i]) || (i<3?std::abs(impulse[i])>500:impulse[i]<0 || impulse[i]>1))return false;
    MEMORY_BASIC_INFORMATION memory{};VirtualQuery(base+motion,&memory,sizeof(memory));
    if(!(memory.Protect&(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return false;
    Scope scope(ctx);const auto vector=scope.stack+512;
    if((vector&15) || !Readable(vector,16,base))return false;
    for(unsigned i=0;i<4;++i)PPC_STORE_U32(vector+i*4,i==3?0:std::bit_cast<uint32_t>(impulse[i]));
    ctx.r3.u64=motion;ctx.r4.u64=vector;ctx.f1.f64=impulse[3];__imp__sub_82786E88(ctx,base);return true;
}
std::mutex mutex;std::deque<Command> commands;std::array<em2::NativeActor,2> actors;
em2::NativeConsoleWorldStatus console_world;
void InvalidateConsoleWorld(em2::NativeConsoleWorldStatus& world){
    world.ready=false;++world.version;++world.lifecycle;
}
void UpdateConsoleWorld(em2::NativeConsoleWorldStatus& world,const std::array<em2::NativeActor,2>& before,const std::array<em2::NativeActor,2>& after){
    for(unsigned i=0;i<2;++i)if(before[i].entity!=after[i].entity || before[i].motion!=after[i].motion || before[i].identity!=after[i].identity){world.ready=false;++world.version;break;}
    if(std::any_of(after.begin(),after.end(),[](const auto& actor){return actor.ready && actor.entity && actor.motion;}))world.ready=true;
    if(std::none_of(after.begin(),after.end(),[](const auto& actor){return actor.entity && actor.motion;}))world.ready=false;
}
std::optional<float> Number(const std::string& text){float value=0;const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);if(error!=std::errc{} || end!=text.data()+text.size() || !std::isfinite(value))return {};return value;}
std::optional<int> Actor(const std::string& name){if(name=="mickey" || name=="0")return 0;if(name=="oswald" || name=="1")return 1;return {};}

void ProbeOnlineLocalMovement(PPCContext& ctx,uint8_t* base,const std::array<em2::NativeActor,2>& snapshot){
    if(!REXCVAR_GET(em2_probe_online_local_movement) || !REXCVAR_GET(em2_private_profile) ||
       REXCVAR_GET(em2_user_data_directory).empty() || em2::net::SynchronizedSessionActive())return;
    // Preserve padding too: memberwise PPCContext copies cannot provide a
    // byte-exact baseline or restoration for this observational probe.
    struct Preserve {PPCContext& ctx;std::array<uint8_t,sizeof(PPCContext)> saved;
        unsigned csr=_mm_getcsr();DWORD error=GetLastError();
        explicit Preserve(PPCContext& value):ctx(value){std::memcpy(saved.data(),&ctx,sizeof(ctx));}
        ~Preserve(){std::memcpy(&ctx,saved.data(),sizeof(ctx));_mm_setcsr(csr);SetLastError(error);}} preserve(ctx);
    static thread_local struct Probe {uint32_t entity=0;uint64_t identity=0;bool armed=false,done=false;
        std::chrono::steady_clock::time_point stable{},first_attempt{},next_attempt{};
        unsigned attempts=0,failure_stage=0;} probe;
    if(probe.done)return;
    const auto now=std::chrono::steady_clock::now();
    if(!snapshot[0].ready || !snapshot[0].entity)return;
    if(probe.entity!=snapshot[0].entity || probe.identity!=snapshot[0].identity){
        if(probe.armed)em2::net::DisarmNativeLocalMovement();
        probe={};probe.entity=snapshot[0].entity;probe.identity=snapshot[0].identity;probe.stable=now;return;
    }
    if(!probe.armed){
        if(now-probe.stable<std::chrono::seconds(3) || now<probe.next_attempt)return;
        if(!probe.attempts)probe.first_attempt=now;
        if(!em2::net::ArmNativeLocalMovement(1,PPC_LOAD_U32(0x833DA7E4),base,0))return;
        probe.armed=true;++probe.attempts;
    }
    em2::net::NativeLocalMovementBinding binding;
    if(!em2::net::DiscoverNativeLocalMovementBinding(ctx,base,binding)){
        const auto diagnostic=em2::net::LocalMovementDiagnostics();
        em2::net::DisarmNativeLocalMovement();probe.armed=false;
        probe.next_attempt=now+std::chrono::milliseconds(200);
        if(probe.attempts==1 || probe.failure_stage!=diagnostic.discovery_stage){
            probe.failure_stage=diagnostic.discovery_stage;
            REXLOG_WARN("Private online Movement discovery pending: stage {}, event {:08X}, count {}, index {}, UID {:08X}, candidates {}, inspect stage {}; observations {}, captures {}",
                diagnostic.discovery_stage,diagnostic.discovery_event,diagnostic.discovery_count,diagnostic.discovery_index,
                diagnostic.discovery_uid,diagnostic.discovery_candidates,diagnostic.discovery_inspect_stage,diagnostic.observations,diagnostic.captured);
        }
        if(now-probe.first_attempt<std::chrono::seconds(15))return;
        probe.done=true;
        REXLOG_ERROR("Private online Movement probe: pre-tick discovery failed after {} attempts at stage {}",probe.attempts,diagnostic.discovery_stage);return;
    }
    probe.done=true;
    struct Disarm {~Disarm(){em2::net::DisarmNativeLocalJump();em2::net::DisarmNativeLocalMovement();}} disarm;
    if(binding.entity!=snapshot[0].entity || !Readable(binding.controller,800,base)){
        REXLOG_ERROR("Private online Movement probe: selected actor changed before evaluation");return;
    }
    std::array<uint8_t,800> before;std::memcpy(before.data(),base+binding.controller,before.size());
    const std::array<em2::net::Pad,5> samples={em2::net::Pad{},em2::net::Pad{0,0,0,32767,0},
        em2::net::Pad{0,0,0,0,32767},em2::net::Pad{0,0,0,-32768,-32768},em2::net::Pad{0,0,0,8689,-8689}};
    const auto diagnostic=em2::net::LocalMovementDiagnostics();
    bool success=diagnostic.observations==0 && diagnostic.captured==0;
    constexpr uint64_t logical_actor_epoch=65; // Deliberately differs from the local producer lease.
    for(const auto& sample:samples){
        em2::net::RoomMovement result;
        const bool produced=em2::net::ProduceNativeLocalRoomMovement(binding,sample,logical_actor_epoch,ctx,base,result,0);
        const bool context_unchanged=!std::memcmp(&ctx,preserve.saved.data(),sizeof(ctx));
        const bool controller_unchanged=!std::memcmp(before.data(),base+binding.controller,before.size());
        success &= produced && em2::net::ValidRoomMovement(result) && result.pad==sample && result.actor_epoch==logical_actor_epoch &&
            context_unchanged && controller_unchanged;
        if(!sample.left_x && !sample.left_y)success &= result.x==0 && result.y==0;
        REXLOG_INFO("Private online Movement command: raw {},{} -> native {},{}; forward {},{}; actor epoch {}; produced {}, context unchanged {}, controller unchanged {}",
            sample.left_x,sample.left_y,result.x,result.y,result.forward_x,result.forward_z,result.actor_epoch,produced,context_unchanged,controller_unchanged);
        if(!context_unchanged){const auto* a=preserve.saved.data();const auto* b=reinterpret_cast<const uint8_t*>(&ctx);
            for(size_t i=0;i<sizeof(ctx);++i)if(a[i]!=b[i]){REXLOG_WARN("Private Movement context first difference byte {}: {} -> {}",i,a[i],b[i]);break;}}
        if(!controller_unchanged)for(size_t i=0;i<before.size();++i)if(before[i]!=base[binding.controller+i]){
            REXLOG_WARN("Private Movement controller first difference byte {}: {} -> {}",i,before[i],base[binding.controller+i]);break;}
    }
    em2::net::NativeLocalJumpBinding jump;
    const bool jump_discovered=em2::net::DiscoverNativeLocalJumpBinding(binding,ctx,base,jump);
    const bool jump_armed=jump_discovered && em2::net::ArmNativeLocalJump(jump,logical_actor_epoch,100,ctx,base);
    bool jump_success=jump_armed;
    if(jump_armed){
        // Observe native digital math on private history only. These synthetic
        // samples never enter the live device, actors or network transport.
        std::array<uint8_t,640> button_before{};
        std::memcpy(button_before.data(),base+jump.buttons,jump.button_count*20);
        std::array<std::array<uint8_t,436>,2> action_before{};
        for(unsigned i=0;i<2;++i)std::memcpy(action_before[i].data(),base+jump.actions[i].action,436);
        em2::net::RoomJumpStream history;
        const std::array<bool,6> levels{false,true,true,false,true,false};
        const std::array<unsigned,6> expected_count{0,1,0,1,1,1};
        uint64_t expected_sequence=0;
        for(unsigned i=0;i<levels.size();++i){
            em2::net::Pad pad{};if(levels[i])pad.buttons=jump.button_mask;
            em2::net::NativeLocalJumpEvents events;em2::net::RoomMovement command;
            const bool produced=em2::net::ProduceNativeLocalJump(jump,pad,logical_actor_epoch,100+i,ctx,base,events,0) &&
                em2::net::ProduceNativeLocalRoomMovement(binding,pad,logical_actor_epoch,ctx,base,command,0);
            command.source=em2::net::RoomMovementSource::NativeActionV2;
            command.jump_button_mask=events.button_mask;command.jump_count=events.count;command.jump_events=events.events;
            bool unchanged=!std::memcmp(&ctx,preserve.saved.data(),sizeof(ctx)) &&
                !std::memcmp(before.data(),base+binding.controller,before.size()) &&
                !std::memcmp(button_before.data(),base+jump.buttons,jump.button_count*20);
            for(unsigned j=0;j<2;++j)unchanged &= !std::memcmp(action_before[j].data(),base+jump.actions[j].action,436);
            const bool accepted=produced && em2::net::AdvanceRoomJumpStream(history,command);
            const bool edge_ok=events.count==expected_count[i] && (!events.count ||
                (events.events[0].sequence==++expected_sequence && events.events[0].edge==
                    (levels[i]?em2::net::RoomJumpEdge::Pressed:em2::net::RoomJumpEdge::Released)));
            jump_success &= accepted && edge_ok && unchanged;
            REXLOG_INFO("Private online Jump command: source tick {}, mask {:04X}, held {}, count {}, sequence {}; produced {}, accepted {}, native state unchanged {}",
                100+i,events.button_mask,levels[i],events.count,events.events[0].sequence,produced,accepted,unchanged);
        }
    }
    success &= jump_success;
    REXLOG_INFO("Private online Jump probe completed: discovered {}, armed {}, success {}; original digital interpreter and private history only; no live action dispatch or network submission",jump_discovered,jump_armed,jump_success);
    REXLOG_INFO("Private online Movement probe completed: success {}, registry {:08X}, actor {:08X}, listener {:08X}, event {:08X}, action {:08X}, prior observations {}, prior captures {}; pre-tick discovery and native math/reference only, no action dispatch, actor/control writes or network submission",
        success,binding.registry,binding.entity,binding.listener,binding.event,binding.action,diagnostic.observations,diagnostic.captured);
}
// Factory/lifetime prerequisite for extra online actors. Never run this in a
// normal profile or synchronized session. Native admission is required before
// native destruction; neither proves input ownership or multiplayer capacity.
void ProbeExtraNativePlayers(PPCContext& ctx,uint8_t* base,const std::array<em2::NativeActor,2>& snapshot){
    const auto requested=REXCVAR_GET(em2_probe_extra_native_players);
    if(requested<1 || requested>14 || !REXCVAR_GET(em2_private_profile) ||
       REXCVAR_GET(em2_user_data_directory).empty() || em2::net::SynchronizedSessionActive())return;
    const bool owned=REXCVAR_GET(em2_probe_owned_native_player);
    const unsigned owned_character=unsigned(std::clamp(REXCVAR_GET(em2_probe_owned_native_player_character),0,1));
    if(owned && REXCVAR_GET(em2_probe_owned_native_player_input) && requested!=1)return;
    const auto character_for=[&](unsigned i){return owned && requested==1?owned_character:i&1u;};
    struct Probe {uint64_t peer=0;em2::net::OwnedActorRoster roster;
        unsigned ready=0,age=0;bool attempted=false,failed=false,queued=false,finished=false,fresh_cleanup=false;
        uint32_t registry=0,manager=0;std::array<uint32_t,2> original{},original_entities{};std::vector<uint32_t> handles,entities;
        std::chrono::steady_clock::time_point created{},observed{};};
    static Probe probe;
    if(probe.finished)return;
    const auto registry=PPC_LOAD_U32(0x833DA7E4),manager=PPC_LOAD_U32(0x833DAB60);
    if(!Readable(registry,228,base) || !Readable(manager,640,base))return;
    auto originals=[&]{return std::array<uint32_t,2>{PPC_LOAD_U32(manager+20),PPC_LOAD_U32(manager+340)};};
    if(!probe.attempted){
        if(!snapshot[0].ready || !snapshot[1].ready || !snapshot[0].entity || !snapshot[1].entity){probe.ready=0;return;}
        if(probe.registry!=registry || probe.manager!=manager || probe.original!=originals()){
            probe.ready=0;probe.registry=registry;probe.manager=manager;probe.original=originals();
            probe.original_entities={snapshot[0].entity,snapshot[1].entity};
        }
        // Permit normal Continue and the subsequent native CleanLoadLevel to
        // finish; require the same two identities for four simulation seconds.
        const unsigned settled=REXCVAR_GET(em2_probe_extra_native_players_hold_seconds)>0?600u:120u;
        if(++probe.ready<settled)return;
    }else if(probe.registry!=registry || probe.manager!=manager || probe.original!=originals() ||
             probe.original_entities!=std::array<uint32_t,2>{snapshot[0].entity,snapshot[1].entity}){
        if(owned && registry==probe.registry){
            // A retail respawn is not permission to unquarantine a still-live
            // extra actor. Stop the hold and clean it through the same registry.
            if(probe.peer)probe.roster.BeginRetirement(ctx,base);
            probe.failed=true;probe.manager=manager;probe.original=originals();
            probe.original_entities={snapshot[0].entity,snapshot[1].entity};
            REXLOG_ERROR("Owned native player probe: retail identities changed; disabled extra input and beginning verified cleanup");
        }else{
            if(probe.peer){
                if(REXCVAR_GET(em2_probe_owned_native_player_input))em2::net::StopOwnedActorProbe(probe.peer);
                if(!probe.roster.AbandonDestroyedWorld(ctx,base))probe.failed=true;
                if(REXCVAR_GET(em2_probe_owned_native_player_input))em2::net::RetireOwnedActorProbe(probe.peer,ctx,base);
            }
            probe.peer=0;
            probe.finished=true;REXLOG_ERROR("Extra native player probe: world changed before cleanup verification; no stale calls issued");return;
        }
    }
    if(ctx.r1.u32<32768 || !Readable(ctx.r1.u32-32768,32768,base))return;
    // Native calls may allocate/change mappings; do not retain GuestReadBatch
    // caches across them. Preserve the interrupted native machine context.
    const auto error=GetLastError();Scope scope(ctx);
    struct LastError {DWORD value;~LastError(){SetLastError(value);}} last_error{error};
    const auto handle_scratch=scope.stack+512;
    auto resolve=[&](uint32_t handle){
        PPC_STORE_U32(handle_scratch,handle);ctx.r3.u64=registry;ctx.r4.u64=handle_scratch;
        __imp__sub_82732A18(ctx,base);return ctx.r3.u32;
    };
    // This token exists only in the synchronous fresh-factory branch below.
    // Never call this after entering admission, including partial admission.
    auto discard_fresh=[&](uint32_t handle,uint32_t entity){
        const auto owner=PPC_LOAD_U32(registry+152);
        if(!handle || resolve(handle)!=entity || !Readable(entity-8,104,base) ||
           entity==snapshot[0].entity || entity==snapshot[1].entity || !Readable(owner,112,base))return false;
        const auto table=PPC_LOAD_U32(entity);
        if(table!=0x8208D908 || !Readable(table,368,base) || PPC_LOAD_U32(table)!=0x827C8E30 ||
           PPC_LOAD_U32(table+4)!=0x827C8E58 || PPC_LOAD_U32(table+364)!=0x827C8C70 || !PPC_LOAD_U32(entity-4))return false;
        const auto pending=PPC_LOAD_U32(registry+192),pending_count=PPC_LOAD_U32(registry+196);
        if(pending_count>65536 || (pending_count && !Readable(pending,size_t(pending_count)*4,base)))return false;
        for(uint32_t i=0;i<pending_count;++i)if(PPC_LOAD_U32(pending+4*i)==entity)return false;
        const auto children=PPC_LOAD_U32(owner+16),count=PPC_LOAD_U32(owner+24);
        if(!count || count>65536 || !Readable(children,size_t(count)*4,base))return false;
        uint32_t index=0,matches=0;
        for(uint32_t i=0;i<count;++i)if(PPC_LOAD_U32(children+4*i)==entity){index=i;++matches;}
        if(matches!=1)return false;
        const auto hold=scope.stack+896;PPC_STORE_U32(hold,0);
        ctx.r3.u64=hold;ctx.r4.u64=entity;__imp__sub_8308B3E8(ctx,base);
        // Native owner removal erases child/name indexes and NiPointers. It
        // never initializes or deactivates unadmitted component listeners.
        ctx.r3.u64=owner;ctx.r4.u64=index;__imp__sub_82747128(ctx,base);
        ctx.r3.u64=hold;ctx.r4.u64=0;__imp__sub_8308B3E8(ctx,base);
        const auto remaining=resolve(handle);
        REXLOG_INFO("Owned native player rollback: default owner count {} -> {}, handle {:08X} remaining {:08X}; no admission, no component deactivation, retail handles unchanged {}",count,PPC_LOAD_U32(owner+24),handle,remaining,originals()==probe.original);
        return !remaining && PPC_LOAD_U32(owner+24)==count-1 && originals()==probe.original;
    };
    auto rollback_fresh_group=[&]{
        bool complete=true;
        if(!probe.roster.actors().empty())complete=probe.roster.BeginRetirement(ctx,base);
        for(size_t n=probe.handles.size();n-->0;){
            const auto entity=resolve(probe.handles[n]);
            if(!entity)continue;
            if(entity!=probe.entities[n]){complete=false;continue;}
            complete=discard_fresh(probe.handles[n],entity) && complete;
        }
        if(!probe.roster.actors().empty())complete=probe.roster.CompleteRetirement(ctx,base) && complete;
        probe.finished=complete;
        if(complete)REXLOG_INFO("Owned native player group rollback: requested {} constructed {} registered {} complete true; no actor admitted",requested,probe.handles.size(),probe.roster.actors().size());
        else if(++probe.age==1 || probe.age%120==0)REXLOG_ERROR("Owned native player group rollback pending: requested {} constructed {} registered {}; ownership remains quarantined",requested,probe.handles.size(),probe.roster.actors().size());
    };
    if(probe.fresh_cleanup){rollback_fresh_group();return;}
    // Exact GetHealth export path (827CF240): entity int-property getter
    // vtable+204 with descriptor833DBB14. Read only; never restore/set health.
    auto health=[&](uint32_t entity){
        std::optional<int32_t> result;
        uint32_t method=0;
        if(!Readable(0x833DBB14,4,base) || !VirtualMethod(entity,204,method,base))return result;
        const auto descriptor=PPC_LOAD_U32(0x833DBB14),out=scope.stack+768;
        if(!Readable(descriptor,8,base))return result;
        PPC_STORE_U32(out,0xCDCDCDCD);ctx.r3.u64=entity;ctx.r4.u64=descriptor+4;ctx.r5.u64=out;ctx.r6.u64=0;
        PPC_CALL_INDIRECT_FUNC(method);
        if(ctx.r3.u32)result=int32_t(PPC_LOAD_U32(out));return result;
    };
    auto observe_players=[&](const char* phase,int instance){
        for(unsigned player=0;player<2;++player){
            const auto& actor=snapshot[player];const auto hp=health(actor.entity);const auto point=scope.stack+704;
            if(REXCVAR_GET(em2_probe_owned_native_player_input) && instance==-1 && std::strcmp(phase,"running")){
                uint32_t task_flags=UINT32_MAX;
                const bool task_read=em2::net::ReadNativeJumpTaskFlags(actor.entity,base,task_flags);
                REXLOG_INFO("Owned actor retail task lifecycle: phase {} player {} entity {:08X} Jump flags {:08X} readable {}",phase,player,actor.entity,task_flags,task_read);
            }
            ctx.r3.u64=actor.entity;ctx.r4.u64=point;__imp__sub_8275C160(ctx,base);
            REXLOG_INFO("Extra native player original health: phase {} instance {} player {} entity {:08X} present {} health {} position {},{},{} state {} enabled {}",phase,instance,player,actor.entity,bool(hp),hp.value_or(INT32_MIN),
                std::bit_cast<float>(PPC_LOAD_U32(point)),std::bit_cast<float>(PPC_LOAD_U32(point+4)),std::bit_cast<float>(PPC_LOAD_U32(point+8)),
                Readable(actor.motion,976,base)?PPC_LOAD_U32(actor.motion+384):UINT32_MAX,Readable(actor.motion,976,base)?PPC_LOAD_U8(actor.motion+640):0);
        }
    };
    if(!probe.attempted){
        probe.attempted=true;
        observe_players("before",-1);
        // Retail 8276CCF0 passes a Vector4 position in r6 to the factory.
        // 8276A828 forwards it unchanged; 82743AD8 dereferences it when
        // assigning the cloned entity transform, even before activation.
        const auto position=scope.stack+576;
        ctx.r3.u64=snapshot[0].entity;ctx.r4.u64=position;__imp__sub_8275C160(ctx,base);
        const auto rotation=scope.stack+608;
        ctx.r3.u64=snapshot[0].entity;ctx.r4.u64=rotation;ctx.r5.u64=2;__imp__sub_8275C728(ctx,base);
        const std::array<uint32_t,4> origin={PPC_LOAD_U32(position),PPC_LOAD_U32(position+4),PPC_LOAD_U32(position+8),PPC_LOAD_U32(position+12)};
        const bool demo=REXCVAR_GET(em2_probe_extra_native_players_hold_seconds)>0;
        std::vector<std::array<float,3>> spawns;
        if(demo) {
            // Plan the complete group before constructing any actors. Reject
            // blocked/unsupported sites with native hull sweeps, and reserve
            // spacing so the candidates do not overlap one another.
            std::string why;
            const unsigned spawn_rings=std::max(4u,(unsigned(requested)+2u)/2u);
            for(int i=0;i<requested;++i) {
                bool found=false;unsigned rejected=0;
                for(unsigned ring=1;ring<=spawn_rings && !found;++ring)for(unsigned point=0;point<24 && !found;++point) {
                    const float angle=(float(point)+float(ring&1)*.5f)*6.28318530718f/24.f;
                    std::array<float,3> preferred{std::bit_cast<float>(origin[0])+float(ring)*2.f*std::cos(angle),
                        std::bit_cast<float>(origin[1]),std::bit_cast<float>(origin[2])+float(ring)*2.f*std::sin(angle)};
                    if(std::any_of(spawns.begin(),spawns.end(),[&](const auto& p){const auto x=p[0]-preferred[0],z=p[2]-preferred[2];return x*x+z*z<1.5f*1.5f;}))continue;
                    std::array<float,3> grounded;
                    // The character hull may come from retail AI Oswald, but
                    // an added HUMAN needs the live human collision profile.
                    if(!em2::FindNativePlayerSpawn(ctx,base,snapshot[character_for(unsigned(i))].entity,preferred,grounded,why,snapshot[0].entity)){++rejected;continue;}
                    spawns.push_back(grounded);found=true;
                    REXLOG_INFO("Extra native player safe spawn: instance {} position {},{},{} rejected {} native hull/floor sweeps",i,grounded[0],grounded[1],grounded[2],rejected);
                }
                if(!found){probe.failed=probe.finished=true;REXLOG_ERROR("Extra native player safe spawn: could not place full group at instance {}: {}; no extra actors constructed",i,why);return;}
            }
        }
        REXLOG_INFO("Extra native player probe: begin {} instances, registry {:08X}, manager {:08X}, retail handles {:08X}/{:08X}",requested,registry,manager,probe.original[0],probe.original[1]);
        for(int i=0;i<requested;++i){
            const unsigned character=character_for(unsigned(i));
            if(demo){
                for(unsigned axis=0;axis<3;++axis)PPC_STORE_U32(position+axis*4,std::bit_cast<uint32_t>(spawns[size_t(i)][axis]));
                PPC_STORE_U32(position+12,origin[3]);
            }
            // Call only the retail factory (not the two-slot replacement
            // manager). Its human prefabs are Player and PlayerTwo.
            ctx.r3.u64=manager;ctx.r4.u64=character;ctx.r5.u64=0;ctx.r6.u64=position;__imp__sub_8276A828(ctx,base);
            const auto entity=ctx.r3.u32;
            if(!Readable(entity,96,base)){probe.failed=true;REXLOG_ERROR("Extra native player probe: factory failed at instance {}",i);break;}
            // Complete the transform setup used by retail 8276CCF0, including
            // a valid quaternion before physics/components are admitted.
            ctx.r3.u64=entity;ctx.r4.u64=rotation;ctx.f1.f64=0;__imp__sub_8275C970(ctx,base);
            ctx.r3.u64=handle_scratch;ctx.r4.u64=registry;ctx.r5.u64=entity;__imp__sub_827403B0(ctx,base);
            const auto handle=PPC_LOAD_U32(handle_scratch);
            const auto types=PPC_LOAD_U32(entity+88),components=PPC_LOAD_U32(entity+36);
            const auto type=PPC_LOAD_U8(0x833DAF54);
            uint32_t motion=0;
            if(Readable(types,unsigned(type)+1,base)){
                const auto slot=PPC_LOAD_U8(types+type);
                if(slot!=255 && Readable(components+unsigned(slot)*4,4,base))motion=PPC_LOAD_U32(components+unsigned(slot)*4)-8;
            }
            const bool unique=entity!=snapshot[0].entity && entity!=snapshot[1].entity &&
                handle && handle!=probe.original[0] && handle!=probe.original[1] &&
                std::find(probe.entities.begin(),probe.entities.end(),entity)==probe.entities.end() &&
                std::find(probe.handles.begin(),probe.handles.end(),handle)==probe.handles.end();
            const bool valid=unique && Readable(motion,976,base) && resolve(handle)==entity && originals()==probe.original;
            REXLOG_INFO("Extra native player probe: instance {} character {} handle {:08X} entity {:08X} motion {:08X} unique {} valid {}",i,character,handle,entity,motion,unique,valid);
            // Never put an aliased retail actor in our destruction list, even
            // if a future factory change violates the fresh-instance contract.
            if(unique){probe.entities.push_back(entity);probe.handles.push_back(handle);}
            const auto hp=health(entity);
            REXLOG_INFO("Extra native player spawned health: instance {} character {} entity {:08X} present {} health {}",i,character,entity,bool(hp),hp.value_or(INT32_MIN));
            observe_players("constructed",i);
            if(!valid){
                probe.failed=true;
                break;
            }
            if(owned){
                const bool forced=REXCVAR_GET(em2_probe_owned_native_player_reject) && i==requested-1;
                if(forced || !probe.roster.Add(uint8_t(i+3),1000+unsigned(i),handle,character,ctx,base)){
                    probe.failed=true;
                    REXLOG_INFO("Owned native player probe: registration rejected at {} forced {} reason '{}'",i,forced,probe.roster.error());
                    break;
                }
                const auto input=probe.roster.actors().back().actor;if(!probe.peer)probe.peer=input.peer;
                REXLOG_INFO("Owned native player probe: registered peer {} handle {:08X} controller {:08X}, listener {:08X}, native action user {}; room peer {} GUID [{:08X} {:08X} {:08X} {:08X}]; ordinary broadcasts quarantined",input.peer,input.handle,input.controller,input.listener,input.action_user,i+3,
                    PPC_LOAD_U32(input.entity+4),PPC_LOAD_U32(input.entity+8),PPC_LOAD_U32(input.entity+12),PPC_LOAD_U32(input.entity+16));
            }
        }
        if(owned && probe.failed){
            probe.fresh_cleanup=true;rollback_fresh_group();
            return;
        }
        for(const auto entity:probe.entities){
            // Match retail time-mode propagation, then native admission. Admission
            // activates component listeners; the owned actor is quarantined
            // before entering it, including its partial-failure boundary.
            ctx.r3.u64=entity-8;ctx.r4.u64=0;__imp__sub_827CAE68(ctx,base);
            ctx.r3.u64=registry;ctx.r4.u64=entity;ctx.r5.u64=0;__imp__sub_82742F70(ctx,base);
            REXLOG_INFO("Extra native player probe: admitted entity {:08X}, retail handles unchanged {}",entity,originals()==probe.original);
            if(originals()!=probe.original)probe.failed=true;
        }
        if(owned && probe.peer && !probe.failed){
            for(size_t n=0;n<spawns.size();++n){
                std::array<float,3> actual_ground;std::string actual_error;
                const bool actual_support=em2::FindNativePlayerSpawn(ctx,base,probe.entities[n],spawns[n],actual_ground,actual_error,0,true);
                REXLOG_INFO("Owned spawn actual hull comparison: accepted {} planned {}/{}/{} actual {}/{}/{} reason '{}'",actual_support,
                    spawns[n][0],spawns[n][1],spawns[n][2],actual_support?actual_ground[0]:0.f,actual_support?actual_ground[1]:0.f,actual_support?actual_ground[2]:0.f,actual_error);
                if(!actual_support || std::abs(actual_ground[1]-spawns[n][1])>.1f)probe.failed=true;
            }
            const auto camera_controller=PPC_LOAD_U32(manager+28);
            const auto camera_handle=Readable(camera_controller,24,base)?PPC_LOAD_U32(camera_controller+20):0;
            uint16_t expected=0;for(unsigned n=0;n<unsigned(requested);++n)expected|=uint16_t(1u<<(n+2));
            if(!probe.failed && !probe.roster.Activate(expected,camera_handle,ctx,base))probe.failed=true;
            bool bound=false;
            if(!probe.failed){
                em2::net::OwnedMovementFrames frames;std::array<uint64_t,16> epochs{};
                const auto full=uint16_t(expected|3);for(unsigned n=0;n<16;++n)if(full&(1u<<n))epochs[n]=1;
                bound=frames.Configure(full,3,epochs,1,registry) && probe.roster.BindFrames(frames,ctx,base);
                em2::net::RoomFrame neutral;neutral.tick=1;neutral.players_mask=full;
                for(unsigned n=0;n<16;++n)if(full&(1u<<n)){neutral.players[n].actor_epoch=1;neutral.players[n].source=em2::net::RoomMovementSource::NativeActionV1;}
                bound=bound && frames.Preflight(neutral,ctx,base)==em2::net::OwnedFrameResult::Ready;
                if(!bound)probe.failed=true;
            }
            REXLOG_INFO("Owned native player roster admission: requested {} registered {} mask {:04X} active {} frame bindings {} reason '{}'; no network frame delivered",requested,probe.roster.actors().size(),probe.roster.mask(),probe.roster.active(),bound,probe.roster.error());
            if(!probe.failed && REXCVAR_GET(em2_probe_owned_native_player_input) && !em2::net::ArmOwnedActorProbe(probe.peer,snapshot[0].entity,ctx,base))probe.failed=true;
        }
        observe_players("admitted",-1);
        probe.created=probe.observed=std::chrono::steady_clock::now();
        if(demo)REXLOG_INFO("Extra native player demo: {} additional actors admitted near the player; lifetime {} seconds; independent controls/network not assigned",probe.entities.size(),std::clamp(REXCVAR_GET(em2_probe_extra_native_players_hold_seconds),0,900));
    }
    if(!probe.queued){
        if(probe.peer && REXCVAR_GET(em2_probe_owned_native_player_input) && em2::net::OwnedActorProbeFailed(probe.peer))probe.failed=true;
        const auto now=std::chrono::steady_clock::now();
        const auto hold=std::chrono::seconds(std::clamp(REXCVAR_GET(em2_probe_extra_native_players_hold_seconds),0,900));
        if(!probe.failed && now-probe.created<hold){
            const auto interval=now-probe.created<std::chrono::seconds(4)?std::chrono::milliseconds(250):std::chrono::milliseconds(2000);
            if(now-probe.observed>=interval){
                observe_players("running",-1);
                unsigned alive=0,roots=0,animated=0;
                for(const auto handle:probe.handles){
                    const auto entity=resolve(handle);if(!Readable(entity,96,base))continue;++alive;
                    animated+=em2::DiscoverNativeAnimation(entity,base)!=0;
                    const auto types=PPC_LOAD_U32(entity+88),components=PPC_LOAD_U32(entity+36);
                    const auto hp=health(entity);
                    REXLOG_INFO("Extra native player active health: handle {:08X} entity {:08X} present {} health {}",handle,entity,bool(hp),hp.value_or(INT32_MIN));
                    if(now-probe.created<std::chrono::seconds(4)){
                        const auto point=scope.stack+640;
                        ctx.r3.u64=entity;ctx.r4.u64=point;__imp__sub_8275C160(ctx,base);
                        uint32_t motion=0;const auto motion_type=PPC_LOAD_U8(0x833DAF54);
                        if(Readable(types,unsigned(motion_type)+1,base)){
                            const auto slot=PPC_LOAD_U8(types+motion_type);
                            if(slot!=255 && Readable(components+unsigned(slot)*4,4,base))motion=PPC_LOAD_U32(components+unsigned(slot)*4)-8;
                        }
                        REXLOG_INFO("Extra native player demo motion: handle {:08X} entity {:08X} position {},{},{} motion state {} enabled {}",handle,entity,
                            std::bit_cast<float>(PPC_LOAD_U32(point)),std::bit_cast<float>(PPC_LOAD_U32(point+4)),std::bit_cast<float>(PPC_LOAD_U32(point+8)),
                            Readable(motion,976,base)?PPC_LOAD_U32(motion+384):UINT32_MAX,Readable(motion,976,base)?PPC_LOAD_U8(motion+640):0);
                    }
                    const auto type=PPC_LOAD_U8(0x833DB0EC);
                    if(!Readable(types,unsigned(type)+1,base))continue;
                    const auto slot=PPC_LOAD_U8(types+type);if(slot==255 || !Readable(components+unsigned(slot)*4,4,base))continue;
                    const auto face=PPC_LOAD_U32(components+unsigned(slot)*4);
                    if(face>=8 && Readable(face-8,92,base))roots+=PPC_LOAD_U32(face-8+88)!=0;
                }
                probe.observed=now;
                REXLOG_INFO("Extra native player demo: alive {} scene roots {} animation graphs {} of {}; retail handles unchanged {}",alive,roots,animated,probe.handles.size(),originals()==probe.original);
            }
            return;
        }
        // Original destruction queues and retains the world-owned entity until
        // the engine removes it. No manual free/refcount/manager pointer edits.
        if(probe.peer){
            if(REXCVAR_GET(em2_probe_owned_native_player_input) && !em2::net::StopOwnedActorProbe(probe.peer))probe.failed=true;
            const bool released=probe.roster.BeginRetirement(ctx,base);
            REXLOG_INFO("Owned native player roster: references released before native destruction {}",released);
            if(!released){probe.failed=true;return;}
        }
        for(size_t i=0;i<probe.entities.size();++i){
            const auto entity=resolve(probe.handles[i]);
            if(!entity)continue;
            if(entity!=probe.entities[i] || entity==snapshot[0].entity || entity==snapshot[1].entity){
                probe.failed=true;REXLOG_ERROR("Extra native player cleanup rejected a changed or retail entity identity");continue;
            }
            ctx.r3.u64=registry;ctx.r4.u64=entity;__imp__sub_82744E90(ctx,base);
        }
        probe.queued=true;
        REXLOG_INFO("Extra native player probe: queued native destruction for {} instances, retail handles unchanged {}",probe.entities.size(),originals()==probe.original);
        return;
    }
    ++probe.age;
    unsigned remaining=0;for(const auto handle:probe.handles)remaining+=resolve(handle)!=0;
    if(remaining && probe.age<120)return;
    if(!remaining && probe.peer){if(!probe.roster.CompleteRetirement(ctx,base))probe.failed=true;
        if(REXCVAR_GET(em2_probe_owned_native_player_input) && !em2::net::RetireOwnedActorProbe(probe.peer,ctx,base))probe.failed=true;
        probe.peer=0;}
    probe.finished=true;
    const bool success=!probe.failed && probe.entities.size()==size_t(requested) && !remaining && originals()==probe.original;
    REXLOG_INFO("Extra native player probe: completed success {} requested {} created {} remaining {} retail handles unchanged {}; construction/admission/lifetime only, no input/replication proof",success,requested,probe.entities.size(),remaining,originals()==probe.original);
}
}

PPC_FUNC_IMPL(sub_827CA5F0) {
    // Private read-only diagnostic. Observe the actual entity Health write,
    // including its native callers; do not repair health or suppress death.
    if(REXCVAR_GET(em2_probe_extra_native_players)>0 && REXCVAR_GET(em2_private_profile)) {
        struct Restore {unsigned csr=_mm_getcsr();DWORD error=GetLastError();
            ~Restore(){_mm_setcsr(csr);SetLastError(error);}} restore;
        try {
            if(Readable(0x833DBB14,4,base) && Readable(ctx.r4.u32,4,base)) {
                const auto descriptor=PPC_LOAD_U32(0x833DBB14);
                if(Readable(descriptor,8,base) && PPC_LOAD_U32(ctx.r4.u32)==PPC_LOAD_U32(descriptor+4)) {
                    static std::atomic<unsigned> writes{0};
                    if(writes.fetch_add(1,std::memory_order_relaxed)<256) {
                        REXLOG_INFO("Private native health write: entity {:08X} value {} index {} caller {:08X}",ctx.r3.u32,ctx.r5.s32,ctx.r6.u32,uint32_t(ctx.lr));
                        if(spawn_contact_trace.body && Readable(spawn_contact_trace.other,233,base)) {
                            const auto setting=PPC_LOAD_U32(0x833DB6A0);
                            REXLOG_INFO("Private native fatal contact: body {:08X} other {:08X} other motion type {} contacts {:08X} last distance {} penetration tolerance {}",
                                spawn_contact_trace.body,spawn_contact_trace.other,PPC_LOAD_U8(spawn_contact_trace.other+232),spawn_contact_trace.contacts,ctx.f0.f64,
                                Readable(setting,4,base)?std::bit_cast<float>(PPC_LOAD_U32(setting)):0.f);
                        }
                        auto stack=ctx.r1.u32;
                        for(unsigned i=0;i<14 && Readable(stack,4,base);++i) {
                            const auto next=PPC_LOAD_U32(stack);
                            if(next<=stack || next-stack>65536 || !Readable(next-8,8,base))break;
                            REXLOG_INFO("Private native health write frame {}: saved return {:08X}",i,PPC_LOAD_U32(next-8));
                            stack=next;
                        }
                    }
                }
            }
        }catch(...){}
    }
    __imp__sub_827CA5F0(ctx,base);
}

PPC_FUNC_IMPL(sub_827AC868) {
    if(REXCVAR_GET(em2_probe_extra_native_players)>0 && REXCVAR_GET(em2_private_profile)) {
        struct Restore {SpawnContactTrace previous=spawn_contact_trace;~Restore(){spawn_contact_trace=previous;}} restore;
        spawn_contact_trace={ctx.r3.u32,ctx.r4.u32,ctx.r5.u32};
        __imp__sub_827AC868(ctx,base);
    }else __imp__sub_827AC868(ctx,base);
}

uint32_t em2::DiscoverNativeAnimation(uint32_t entity,uint8_t* base){
    GuestReadBatch inspection;
    // Exact 829AD330 type lookup, guarded before touching transient entities.
    if(!Readable(entity,92,base))return 0;const auto types=PPC_LOAD_U32(entity+88),components=PPC_LOAD_U32(entity+36);
    const auto type=PPC_LOAD_U8(0x833DCCF8);if(!Readable(types,unsigned(type)+1,base))return 0;const auto slot=PPC_LOAD_U8(types+type);
    if(slot==255 || !Readable(components,unsigned(slot)*4+4,base))return 0;const auto entry=PPC_LOAD_U32(components+unsigned(slot)*4);
    return entry>=8 && AnimationReady(entry-8,base)?entry-8:0;
}
bool em2::NativeAnimationGraphHasEvent(uint32_t animation,uint32_t event,uint8_t* base){
    GuestReadBatch inspection;
    if(!AnimationReady(animation,base))return false;
    // hkbBehaviorGraph stores separate event and variable symbol maps. Each
    // map's local-to-global array begins at +8; the reverse hash is at +20.
    const auto graph=PPC_LOAD_U32(animation+192),map=PPC_LOAD_U32(graph+108);
    if(!Readable(map,32,base))return false;
    const auto entries=PPC_LOAD_U32(map+8),count=PPC_LOAD_U32(map+12),capacity=PPC_LOAD_U32(map+16)&0x3FFFFFFF;
    if(!count || count>capacity || count>16384 || !Readable(entries,size_t(count)*4,base))return false;
    for(unsigned i=0;i<count;++i)if(PPC_LOAD_U32(entries+i*4)==event)return true;
    return false;
}
bool em2::NativeAnimationEvent(PPCContext& ctx,uint8_t* base,uint32_t animation,const std::string& name){
    if(!AnimationReady(animation,base))return false;const auto id=AnimationIdentifier(name,false,base);if(!id)return false;
    const bool pound=name=="EVENT_PC_GroundPound",dash=name=="EVENT_PC_DashGround" || name=="EVENT_PC_DashAir";
    const bool sprint=name=="EVENT_Start_AttackChargeLoop",sprint_stop=name=="EVENT_Start_AttackChargeEnd";
    if(sprint && (!NativeAnimationGraphHasEvent(animation,*id,base) || !NativeAnimationGraphHasEvent(animation,406,base)))return false;
    if(pound || dash || sprint)PrimeAnimationEvents(ctx,base);
    if(!PassAnimationEvent(animation,*id))return false;
    Scope scope(ctx);ctx.r3.u64=animation;ctx.r4.u64=*id;__imp__sub_82843878(ctx,base);
    for(auto& owner:animation_owners)if(owner.animation.load(std::memory_order_acquire)==animation){
        if(pound || dash || sprint)owner.owned.store(pound?2u:sprint?3u:1u,std::memory_order_release);
        else if(sprint_stop || name=="EVENT_PC_GroundPoundCancel" || name=="EVENT_PC_DashStopGround" || name=="EVENT_PC_DashStopAir")owner.owned.store(0,std::memory_order_release);
    }
    return true;
}
uint32_t em2::NativeAnimationInterruptSerial(uint32_t animation){for(const auto& owner:animation_owners)if(animation && owner.animation.load(std::memory_order_acquire)==animation)return owner.interrupt.load(std::memory_order_acquire);return 0;}
namespace {
bool ValidStimuli(std::span<const em2::NativeStimulus> stimuli){
    return !stimuli.empty() && stimuli.size()<=2 && std::all_of(stimuli.begin(),stimuli.end(),[](const auto& value){return value.type<=31 && std::isfinite(value.amount) && value.amount>=0 && value.amount<=1000;});
}
bool PackStimulusSphere(PPCContext& ctx,uint8_t* base,uint32_t stack,uint32_t entity,uint32_t alias,std::span<const em2::NativeStimulus> stimuli,float radius){
    if(!Readable(stack,2048,base) || !ValidStimuli(stimuli) || !std::isfinite(radius) || radius<.1f || radius>50)return false;
    const auto position=stack+512,direction=position+16,source=direction+16;
    const auto types=stack+560,amounts=stack+576,type_data=stack+608,amount_data=stack+624;
    // 8295CD60 constructs these three-word arrays before calling 8295CC38.
    // The latter borrows its inputs; its caller normally frees them as soon
    // as it returns. Stack buffers have the same lifetime without allocation.
    PPC_STORE_U32(types,type_data);PPC_STORE_U32(types+4,unsigned(stimuli.size()));PPC_STORE_U32(types+8,2);
    PPC_STORE_U32(amounts,amount_data);PPC_STORE_U32(amounts+4,unsigned(stimuli.size()));PPC_STORE_U32(amounts+8,2);
    std::memset(base+type_data,0,32);
    for(unsigned i=0;i<stimuli.size();++i){PPC_STORE_U32(type_data+i*4,stimuli[i].type);PPC_STORE_U32(amount_data+i*4,std::bit_cast<uint32_t>(stimuli[i].amount));}
    PPC_STORE_U32(source,alias);
    PPC_STORE_U8(stack+87,1);PPC_STORE_U8(stack+95,0);PPC_STORE_U32(stack+100,0);PPC_STORE_U8(stack+111,0);PPC_STORE_U32(stack+116,source);
    ctx.r3.u64=types;ctx.r4.u64=amounts;ctx.r5.u64=position;ctx.r6.u64=direction;ctx.r7.u64=0x3AFB4EDF;ctx.r8.u64=0;ctx.r9.u64=entity;ctx.r10.u64=0;ctx.f1.f64=radius;
    return true;
}
}
bool em2::NativeStimuliSphere(PPCContext& ctx,uint8_t* base,uint32_t entity,std::span<const NativeStimulus> stimuli,float radius){
    if(!Readable(entity,96,base) || !ValidStimuli(stimuli) || !std::isfinite(radius) || radius<.1f || radius>50)return false;
    const auto alias=Identifier(ctx,base,"PCGroundPound");if(!alias)return false;
    Scope scope(ctx);const auto position=scope.stack+512,direction=position+16,source=direction+16;
    if(!Readable(scope.stack,2048,base))return false;
    std::memset(base+position,0,48);PPC_STORE_U32(source,alias);
    ctx.r3.u64=entity;ctx.r4.u64=position;__imp__sub_8275C160(ctx,base);
    for(unsigned i=0;i<3;++i)if(!std::isfinite(std::bit_cast<float>(PPC_LOAD_U32(position+i*4))))return false;
    if(!PackStimulusSphere(ctx,base,scope.stack,entity,alias,stimuli,radius))return false;
    __imp__sub_8295CC38(ctx,base);return true;
}
bool em2::NativeStimulusSphere(PPCContext& ctx,uint8_t* base,uint32_t entity,unsigned type,float amount,float radius){
    const NativeStimulus stimulus{type,amount};return NativeStimuliSphere(ctx,base,entity,std::span(&stimulus,1),radius);
}
void em2::UpdateNativeEngineActors(const std::array<NativeActor,2>& value){std::lock_guard lock(mutex);const auto previous=actors;UpdateConsoleWorld(console_world,previous,value);actors=value;
    if(const auto view=graph_snapshot.load();view && (!value[view->actor].ready || value[view->actor].identity!=view->identity || previous[view->actor].animation!=value[view->actor].animation))graph_snapshot.store({});
    for(unsigned i=0;i<2;++i)if(animation_owners[i].animation.load(std::memory_order_relaxed)!=value[i].animation || previous[i].identity!=value[i].identity || previous[i].entity!=value[i].entity || previous[i].motion!=value[i].motion){animation_owners[i].owned.store(0,std::memory_order_release);animation_owners[i].interrupt.fetch_add(1,std::memory_order_release);animation_owners[i].animation.store(value[i].animation,std::memory_order_release);}
}
std::optional<std::string> em2::QueueNativeEngineCommand(const std::vector<std::string>& args){
    if(args.empty())return {};const auto& name=args[0];Command cmd{};size_t count=0;
    if(name=="entity.inspect"){cmd.kind=Kind::Inspect;count=1;}
    else if(name=="entity.push"){cmd.kind=Kind::Push;count=5;}
    else if(name=="entity.components"){cmd.kind=Kind::Components;count=1;}
    else if(name=="anim.snapshot"){cmd.kind=Kind::Snapshot;count=1;}
    else if(name=="anim.events" || name=="anim.vars"){cmd.kind=name=="anim.events"?Kind::Events:Kind::Variables;count=1;}
    else if(name=="anim.release"){cmd.kind=Kind::Release;count=2;}
    else if(name=="anim.overrides"){cmd.kind=Kind::Overrides;count=1;}
    else if(name=="anim.event"){cmd.kind=Kind::Event;count=2;}
    else if(name=="anim.float" || name=="anim.int"){cmd.kind=name=="anim.float"?Kind::Float:Kind::Int;count=3;}
    else if(name=="entity.getfloat"){cmd.kind=Kind::GetFloat;count=2;}
    else if(name=="entity.setfloat"){cmd.kind=Kind::SetFloat;count=3;}
    else if(name=="combat.sphere"){cmd.kind=Kind::Sphere;count=4;}
    else return {};
    const bool list=cmd.kind==Kind::Events || cmd.kind==Kind::Variables;
    if(list){if(args.size()>3)return "Use anim.events/anim.vars [filter] [actor].";if(args.size()>1){if(const auto actor=Actor(args.back()))cmd.actor=*actor;else cmd.name=args.back();}if(args.size()==3){if(cmd.actor<0)return "The final argument must select mickey or oswald.";cmd.name=args[1];}}
    else{
        if(args.size()!=count && args.size()!=count+1){for(const auto& info:ConsoleCommandCatalog())if(name==info.name)return std::string("Usage: ")+info.usage+"\nExample: "+info.example;return "Wrong arguments; use help "+name;}
        if(args.size()==count+1){const auto actor=Actor(args.back());if(!actor)return "Actor must be mickey, oswald, 0, or 1.";cmd.actor=*actor;}
    }
    if(cmd.kind==Kind::Push){
        for(unsigned i=0;i<4;++i){const auto v=Number(args[i+1]);if(!v || (i<3?std::abs(*v)>500:*v<0 || *v>1))return "Use entity.push <X -500..500> <Y up -500..500> <Z -500..500> <decay 0..1> [actor].";cmd.impulse[i]=*v;}
        cmd.name="external motion impulse";
    }else{
        if(count>=2){cmd.name=args[1];if(cmd.name.empty() || cmd.name.size()>127)return "Names must contain 1..127 characters.";}
        if(count>=3){const auto a=Number(args[2]);if(!a || std::abs(*a)>1000000)return "Value must be finite and between -1000000 and 1000000.";cmd.a=*a;if(cmd.kind==Kind::Int && std::floor(cmd.a)!=cmd.a)return "An animation integer requires a whole number.";}
    }
    if(cmd.kind==Kind::Sphere){const auto type=Number(args[1]),radius=Number(args[3]);if(!type || *type<0 || *type>31 || std::floor(*type)!=*type || !radius || *radius<.1f || *radius>50 || cmd.a<0 || cmd.a>1000)return "Use combat.sphere <native type 0..31> <amount 0..1000> <radius 0.1..50> [actor].";cmd.type=unsigned(*type);cmd.b=*radius;}
    const bool explicit_actor=list?args.size()>1 && Actor(args.back()).has_value():args.size()==count+1;
    if(!explicit_actor)cmd.actor=console_target.load();
    if(net::SynchronizedSessionActive())return "Native engine tools require offline gameplay.";
    const auto target=cmd.actor<0?"selected character":cmd.actor?"Oswald":"Mickey";
    std::lock_guard lock(mutex);if(commands.size()>=32)return "Engine command queue is full.";commands.push_back(std::move(cmd));return "Queued "+name+" for "+target+" on the game thread.";
}
void em2::RunNativeEngineCommands(PPCContext& ctx,uint8_t* base){
    std::deque<Command> pending;std::array<NativeActor,2> snapshot;
    {std::lock_guard lock(mutex);pending.swap(commands);snapshot=actors;}
    ProbeExtraNativePlayers(ctx,base,snapshot);
    ProbeOnlineLocalMovement(ctx,base,snapshot);
    if(!completion_metadata_published && std::any_of(snapshot.begin(),snapshot.end(),[](const auto& actor){return actor.ready;})){
        AnimationIdentifier("",false,base);AnimationIdentifier("",true,base);
        if(event_registry_loaded && variable_registry_loaded){PublishCompletionMetadata();completion_metadata_published=true;}
    }
    for(const auto& command:pending){const auto actor=command.actor<0?(OswaldIsSelected()?1u:0u):unsigned(command.actor);const auto& target=snapshot[actor];
        if(net::SynchronizedSessionActive() || !target.ready){EngineConsoleMessage("This command requires a loaded offline player.");continue;}
        bool ok=false;float value=command.a;
        switch(command.kind){
        case Kind::Push:ok=PushMotion(ctx,base,target.motion,command.impulse);break;
        case Kind::Components:{
            const auto types=PPC_LOAD_U32(target.entity+88),components=PPC_LOAD_U32(target.entity+36);
            if(!Readable(0x833DBAF0,1,base))break;const auto type_count=PPC_LOAD_U8(0x833DBAF0);if(!type_count || !Readable(types,type_count,base))break;
            for(unsigned type=0;type<type_count;++type){const auto slot=PPC_LOAD_U8(types+type);if(slot==255 || !Readable(components+unsigned(slot)*4,4,base))continue;const auto entry=PPC_LOAD_U32(components+unsigned(slot)*4);if(entry<8 || !Readable(entry-8,4,base))continue;
                std::ostringstream text;text<<"type "<<type<<" / slot "<<unsigned(slot)<<" / component 0x"<<std::hex<<entry-8<<" / vtable 0x"<<PPC_LOAD_U32(entry-8);if(type==PPC_LOAD_U8(0x833DAF54))text<<" (BasicMotion)";if(type==PPC_LOAD_U8(0x833DCCF8))text<<" (animation)";EngineConsoleMessage(text.str());}
            continue;
        }
        case Kind::Snapshot:{
            graph_snapshot.store({});if(!AnimationReady(target.animation,base))break;
            const auto graph=PPC_LOAD_U32(target.animation+192);
            auto view=std::make_shared<NativeGraphSnapshot>();view->actor=actor;view->identity=target.identity;
            Scope scope(ctx);bool valid=true;
            for(bool variable:{false,true}){const auto symbols=PPC_LOAD_U32(graph+(variable?116:108));if(!Readable(symbols,32,base)){valid=false;break;}const auto map=symbols+20;const auto entries=PPC_LOAD_U32(map),count=PPC_LOAD_U32(map+4),mask=PPC_LOAD_U32(map+8);if(mask>16383 || (mask&(mask+1)) || count>mask+1 || !Readable(entries,size_t(mask+1)*8,base)){valid=false;break;}
                for(unsigned i=0;i<=mask;++i){const auto id=PPC_LOAD_U32(entries+i*8);if(id>=(variable?122u:675u))continue;const auto name=GuestString(PPC_LOAD_U32((variable?variable_registry:event_registry)+id*4),base);if(!name)continue;
                    if(!variable){view->events.push_back(*name);for(const auto& alias:kNativeAbilityEvents)if(*name==alias.registered)view->events.emplace_back(alias.alias);continue;}
                    ctx.r3.u64=map;ctx.r4.u64=id;ctx.r5.s64=-1;__imp__sub_82393300(ctx,base);const auto index=ctx.r3.s32;
                    const auto values=PPC_LOAD_U32(graph+124);if(index<0 || index>4096 || !Readable(values,12,base))continue;const auto data=PPC_LOAD_U32(values+8);if(!Readable(data,size_t(index)*4+4,base))continue;
                    const auto raw=PPC_LOAD_U32(data+unsigned(index)*4);view->variables.push_back({*name,std::bit_cast<float>(raw),std::bit_cast<int32_t>(raw)});
                }
            }
            if(!valid)break;
            std::sort(view->events.begin(),view->events.end());view->events.erase(std::unique(view->events.begin(),view->events.end()),view->events.end());
            std::sort(view->variables.begin(),view->variables.end(),[](const auto& a,const auto& b){return a.name<b.name;});
            EngineConsoleMessage(std::string(actor?"Oswald":"Mickey")+": loaded graph snapshot refreshed ("+std::to_string(view->events.size())+" events, "+std::to_string(view->variables.size())+" variables).");graph_snapshot.store(std::move(view));continue;
        }
        case Kind::Release:{
            auto next=std::make_shared<Overrides>(*overrides.load(std::memory_order_acquire));
            const auto before=next->size();std::erase_if(*next,[&](const auto& entry){return entry.actor==actor && (command.name=="all" || command.name==entry.name);});
            overrides.store(next,std::memory_order_release);has_overrides.store(!next->empty(),std::memory_order_release);
            EngineConsoleMessage("Released "+std::to_string(before-next->size())+" animation override(s).");continue;
        }
        case Kind::Overrides:{
            const auto snapshot=overrides.load(std::memory_order_acquire);
            for(const auto& entry:*snapshot)if(entry.actor==actor)EngineConsoleMessage(entry.name+" = "+std::to_string(entry.value)+(entry.integer?" (integer)":" (float)"));
            continue;
        }
        case Kind::Events:case Kind::Variables:{
            if(!AnimationReady(target.animation,base))break;
            const auto graph=PPC_LOAD_U32(target.animation+192),maps=PPC_LOAD_U32(graph+116);
            const auto map=maps+(command.kind==Kind::Variables?20:0);if(!Readable(map,12,base))break;
            const auto entries=PPC_LOAD_U32(map),mask=PPC_LOAD_U32(map+8);if(mask>16383 || !Readable(entries,size_t(mask+1)*8,base))break;
            std::vector<std::string> names;
            for(unsigned i=0;i<=mask;++i){const auto name=PPC_LOAD_U32(entries+i*8);if(name==UINT32_MAX)continue;
                const bool variable=command.kind==Kind::Variables;const auto count=variable?122u:675u;if(name>=count)continue;
                const auto text=GuestString(PPC_LOAD_U32((variable?variable_registry:event_registry)+name*4),base);
                if(text && (command.name.empty() || text->find(command.name)!=text->npos))names.push_back(*text);
                if(text && !variable)for(const auto& alias:kNativeAbilityEvents)if(*text==alias.registered && (command.name.empty() || std::string_view(alias.alias).find(command.name)!=std::string_view::npos))names.push_back(std::string(alias.alias)+" -> "+alias.registered);
            }
            std::sort(names.begin(),names.end());for(const auto& name:names)EngineConsoleMessage(name);
            EngineConsoleMessage(std::to_string(names.size())+" native graph names matched.");continue;
        }
        case Kind::Inspect:{Scope scope(ctx);const auto point=scope.stack+512;ctx.r3.u64=target.entity;ctx.r4.u64=point;__imp__sub_8275C160(ctx,base);
            std::ostringstream out;out<<(actor?"Oswald":"Mickey")<<" entity=0x"<<std::hex<<target.entity<<" motion=0x"<<target.motion<<" animation=0x"<<target.animation<<std::dec<<" position=(";
            for(unsigned i=0;i<3;++i)out<<(i?", ":"")<<std::bit_cast<float>(PPC_LOAD_U32(point+i*4));out<<") velocity=(";
            for(unsigned i=0;i<3;++i)out<<(i?", ":"")<<std::bit_cast<float>(PPC_LOAD_U32(target.motion+240+i*4));out<<") motion state="<<PPC_LOAD_U32(target.motion+384);EngineConsoleMessage(out.str());continue;}
        case Kind::Event:
            for(auto& owner:animation_owners)if(owner.animation.load(std::memory_order_acquire)==target.animation){owner.owned.store(0,std::memory_order_release);owner.interrupt.fetch_add(1,std::memory_order_release);}
            ok=NativeAnimationEvent(ctx,base,target.animation,command.name);break;
        case Kind::Float:case Kind::Int:
            ok=AnimationVariable(ctx,base,target.animation,command.name,value,command.kind==Kind::Int);
            if(ok)ok=SetAnimationOverride(actor,*AnimationIdentifier(command.name,true,base),command.name,value,command.kind==Kind::Int);break;
        case Kind::GetFloat:case Kind::SetFloat:ok=PropertyFloat(ctx,base,target.entity,command.name,value,command.kind==Kind::SetFloat);break;
        case Kind::Sphere:ok=NativeStimulusSphere(ctx,base,target.entity,command.type,command.a,command.b);break;
        }
        const std::string who=actor?"Oswald: ":"Mickey: ";
        if(ok){if(command.kind==Kind::GetFloat)EngineConsoleMessage(who+command.name+" = "+std::to_string(value));else EngineConsoleMessage(who+"submitted "+command.name+(command.kind==Kind::Float||command.kind==Kind::Int?" = "+std::to_string(value):""));}
        else EngineConsoleMessage(who+"operation unavailable for '"+command.name+"'. Use anim.snapshot to inspect the loaded graph; reflected properties must exist and be floats.");
    }
}
std::span<const std::string_view> em2::NativeEngineCommandNames(){return native_commands;}
int em2::NativeConsoleTarget(){return console_target.load();}
bool em2::NativeConsoleWorldReady(){return NativeConsoleWorldState().ready;}
em2::NativeConsoleWorldStatus em2::NativeConsoleWorldState(){std::lock_guard lock(mutex);return console_world;}
void em2::InvalidateNativeConsoleWorld(){std::lock_guard lock(mutex);InvalidateConsoleWorld(console_world);}
void em2::SetNativeConsoleTarget(int actor){if(actor>=-1 && actor<=1)console_target.store(actor);}
std::shared_ptr<const em2::NativeGraphSnapshot> em2::NativeAnimationSnapshot(){return graph_snapshot.load();}
std::optional<std::vector<std::string>> em2::NativeEngineArgumentCompletions(std::string_view command,unsigned argument,std::string_view prefix){
    if(std::find(native_commands.begin(),native_commands.end(),command)==native_commands.end())return {};
    std::vector<std::string> result;
    auto add=[&](std::string_view name){if(name.starts_with(prefix))result.emplace_back(name);};
    const auto metadata=completion_metadata.load(std::memory_order_acquire);
    if(argument==1){
        if(command=="anim.event" || command=="anim.events")for(const auto& name:metadata->events)add(name);
        if(command=="anim.float" || command=="anim.int" || command=="anim.vars" || command=="anim.release")for(const auto& name:metadata->variables)add(name);
        if(command=="anim.release")add("all");
        if(command=="combat.sphere")for(const auto name:{"0","1","2","3","8"})add(name);
    }
    const unsigned actor_argument=command=="entity.inspect" || command=="entity.components" || command=="anim.snapshot" || command=="anim.overrides"?1:
        command=="entity.push"?5:command=="combat.sphere"?4:command=="entity.setfloat" || command=="anim.float" || command=="anim.int"?3:2;
    if(argument==actor_argument){add("mickey");add("oswald");}
    std::sort(result.begin(),result.end());result.erase(std::unique(result.begin(),result.end()),result.end());return result;
}
bool em2::TestNativeEngine(){
    // Disabled movement during an existing cutscene is not a missing world.
    // New/retired identities must still invalidate queued gameplay actions.
    NativeConsoleWorldStatus world;std::array<NativeActor,2> empty{},active{};active[0]={0x5000,0x1000,0,true,1};
    UpdateConsoleWorld(world,empty,active);if(!world.ready)return false;
    auto disabled=active;disabled[0].ready=false;const auto version=world.version;
    UpdateConsoleWorld(world,active,disabled);if(!world.ready || world.version!=version)return false;
    auto replaced=disabled;++replaced[0].identity;UpdateConsoleWorld(world,disabled,replaced);if(world.ready || world.version==version)return false;
    replaced[0].ready=true;UpdateConsoleWorld(world,disabled,replaced);if(!world.ready)return false;
    UpdateConsoleWorld(world,replaced,empty);if(world.ready)return false;
    // Scene-owned scripts survive ordinary player/camera/physics replacement,
    // but explicit native transitions always invalidate their lifecycle.
    if(world.lifecycle!=0)return false;
    const auto actor_version=world.version;InvalidateConsoleWorld(world);
    if(world.ready || world.lifecycle!=1 || world.version!=actor_version+1)return false;
    UpdateConsoleWorld(world,empty,active);
    if(!world.ready || world.lifecycle!=1)return false;
    InvalidateConsoleWorld(world);if(world.lifecycle!=2 || world.ready)return false;
    // Parsing must reject dangerous numeric narrowing before any guest call.
    if(Number("nan") || Number("1oops") || Number("inf") || Actor("2"))return false;
    if(QueueNativeEngineCommand({"unrelated"}) || !QueueNativeEngineCommand({"combat.sphere","2","1","0"}) || !QueueNativeEngineCommand({"anim.int","VAR_MotionState","1.2"}))return false;
    for(const auto& input:std::array<std::vector<std::string>,4>{{
        {"entity.push","nan","0","0","0"},{"entity.push","0","501","0","0"},
        {"entity.push","0","0","0","-0.1"},{"entity.push","0","0","0","1.1"}}}){
        const auto result=QueueNativeEngineCommand(input);if(!result || !result->starts_with("Use entity.push"))return false;
    }
    const auto saved_events=event_ids,saved_variables=variable_ids;const auto saved_metadata=completion_metadata.load();
    event_ids={{"EVENT_Start_Fall",0},{"EVENT_Start_Jump",2},{"EVENT_SlamAttack",594}};
    variable_ids={{"VAR_Forward_Speed",1},{"VAR_MotionState",0}};PublishCompletionMetadata();
    const auto held_metadata=completion_metadata.load();event_ids.clear();variable_ids.clear();
    auto equal=[](const auto& result,const std::vector<std::string>& expected){return result && *result==expected;};
    bool complete=equal(NativeEngineArgumentCompletions("anim.event",1,"EVENT_Start_"),{"EVENT_Start_Fall","EVENT_Start_Jump"}) &&
        equal(NativeEngineArgumentCompletions("anim.event",1,"EVENT_PC_GroundPound"),{"EVENT_PC_GroundPound"}) &&
        equal(NativeEngineArgumentCompletions("anim.float",1,"VAR_For"),{"VAR_Forward_Speed"}) &&
        equal(NativeEngineArgumentCompletions("anim.release",1,"a"),{"all"}) &&
        equal(NativeEngineArgumentCompletions("anim.int",2,"VAR_"),{}) &&
        equal(NativeEngineArgumentCompletions("entity.inspect",1,"os"),{"oswald"}) &&
        !NativeEngineArgumentCompletions("unrelated",1,"");
    PublishCompletionMetadata();complete&=held_metadata->events.size()==4 && completion_metadata.load()->events.empty();
    event_ids=saved_events;variable_ids=saved_variables;completion_metadata.store(saved_metadata);
    if(complete)REXLOG_INFO("Native console completions passed: immutable registry snapshot, ability aliases, variable/actor context, numeric argument exclusion and snapshot replacement");
    complete&=equal(NativeEngineArgumentCompletions("entity.push",5,"os"),{"oswald"}) && equal(NativeEngineArgumentCompletions("entity.push",2,""),{});
    return complete;
}
namespace {
bool TestStimulusPacking(PPCContext& ctx,uint8_t* base,uint32_t scratch){
    const auto saved=ctx;const auto csr=_mm_getcsr();bool valid=true;
    {
        Scope scope(ctx);
        const auto reference=scratch+1536,reference_amounts=reference+16;
        const auto reference_types=reference+32,reference_values=reference+48,temp=reference+64;
        const auto event=scratch+2048,event_types=event+128,event_amounts=event+144;
        const std::array<em2::NativeStimulus,2> stimuli={{{8,2},{3,1}}};
        for(unsigned count:{1u,2u}){
            for(const auto header:{reference,reference_amounts,event+80,event+92}){PPC_STORE_U32(header+4,0);PPC_STORE_U32(header+8,2);}
            PPC_STORE_U32(reference,reference_types);PPC_STORE_U32(reference_amounts,reference_values);
            PPC_STORE_U32(event+80,event_types);PPC_STORE_U32(event+92,event_amounts);
            for(unsigned i=0;i<count;++i){
                PPC_STORE_U32(temp,stimuli[i].type);PPC_STORE_U32(temp+4,std::bit_cast<uint32_t>(stimuli[i].amount));
                ctx.r3.u64=reference;ctx.r4.u64=i;ctx.r5.u64=temp;__imp__sub_82AAB408(ctx,base);
                ctx.r3.u64=reference_amounts;ctx.r4.u64=i;ctx.r5.u64=temp+4;__imp__sub_827FD850(ctx,base);
            }
            valid&=PackStimulusSphere(ctx,base,scope.stack,0x5000,0x1234,std::span(stimuli.data(),count),3);
            const auto types=ctx.r3.u32,amounts=ctx.r4.u32;
            valid&=PPC_LOAD_U32(types+4)==PPC_LOAD_U32(reference+4) && PPC_LOAD_U32(amounts+4)==PPC_LOAD_U32(reference_amounts+4) &&
                PPC_LOAD_U32(types+8)==2 && PPC_LOAD_U32(amounts+8)==2 && PPC_LOAD_U32(reference)==reference_types && PPC_LOAD_U32(reference_amounts)==reference_values &&
                !std::memcmp(base+PPC_LOAD_U32(types),base+reference_types,count*4) && !std::memcmp(base+PPC_LOAD_U32(amounts),base+reference_values,count*4);
            valid&=ctx.r5.u32==scope.stack+512 && ctx.r6.u32==scope.stack+528 && ctx.r7.u32==0x3AFB4EDF && !ctx.r8.u32 && ctx.r9.u32==0x5000 && !ctx.r10.u32 && ctx.f1.f64==3 &&
                PPC_LOAD_U8(scope.stack+87)==1 && !PPC_LOAD_U8(scope.stack+95) && !PPC_LOAD_U32(scope.stack+100) && !PPC_LOAD_U8(scope.stack+111) &&
                PPC_LOAD_U32(scope.stack+116)==scope.stack+544 && PPC_LOAD_U32(scope.stack+544)==0x1234;
            // 8295C318 copies each pair into an event with this exact retail
            // builder. Verify that one event retains both melee and impulse.
            for(unsigned i=0;i<count;++i){
                ctx.r3.u64=event;ctx.r4.u64=PPC_LOAD_U32(PPC_LOAD_U32(types)+i*4);
                ctx.f1.f64=std::bit_cast<float>(PPC_LOAD_U32(PPC_LOAD_U32(amounts)+i*4));__imp__sub_82821AC8(ctx,base);
            }
            valid&=PPC_LOAD_U32(event+84)==count && PPC_LOAD_U32(event+96)==count && PPC_LOAD_U32(event+80)==event_types && PPC_LOAD_U32(event+92)==event_amounts &&
                !std::memcmp(base+event_types,base+reference_types,count*4) && !std::memcmp(base+event_amounts,base+reference_values,count*4);
        }
        const auto arguments=ctx;
        std::array<uint8_t,128> memory{};std::memcpy(memory.data(),base+scope.stack+512,memory.size());
        const std::array<em2::NativeStimulus,3> oversized={{{8,2},{3,1},{2,1}}};
        const std::array<em2::NativeStimulus,1> invalid={{{32,1}}};
        valid&=!PackStimulusSphere(ctx,base,scope.stack,0x5000,0x1234,{},3) &&
            !PackStimulusSphere(ctx,base,scope.stack,0x5000,0x1234,oversized,3) &&
            !PackStimulusSphere(ctx,base,scope.stack,0x5000,0x1234,invalid,3) &&
            !PackStimulusSphere(ctx,base,1,0x5000,0x1234,stimuli,3) &&
            !PackStimulusSphere(ctx,base,scope.stack,0x5000,0x1234,stimuli,0) &&
            !std::memcmp(&arguments,&ctx,sizeof(ctx)) && !std::memcmp(memory.data(),base+scope.stack+512,memory.size());
    }
    valid&=!std::memcmp(&saved,&ctx,sizeof(ctx)) && _mm_getcsr()==csr;
    if(valid)REXLOG_INFO("Native impact packing passed retail integer/float array and event-builder comparisons: single/batched damage, flags, source, radius, bounds, context and FP state; no world query or live damage executed");
    else REXLOG_ERROR("Native impact packing disagrees with retail array/event layout or adapter arguments");
    return valid;
}
}
bool em2::TestNativeEngineMemory(PPCContext& ctx,uint8_t* base,uint32_t scratch){
    if(!TestStimulusPacking(ctx,base,scratch))return false;
    // Exercise the original 82786E88 body, not a substitute memory writer.
    const auto push_saved=ctx;const auto push_csr=_mm_getcsr();
    std::memset(base+scratch,0xA5,1024);
    if(!PushMotion(ctx,base,scratch,{3,-12,7,.75f}) ||
       PPC_LOAD_U32(scratch+912)!=std::bit_cast<uint32_t>(3.f) || PPC_LOAD_U32(scratch+916)!=std::bit_cast<uint32_t>(-12.f) ||
       PPC_LOAD_U32(scratch+920)!=std::bit_cast<uint32_t>(7.f) || PPC_LOAD_U32(scratch+924)!=0 ||
       PPC_LOAD_U32(scratch+928)!=std::bit_cast<uint32_t>(.75f) || PPC_LOAD_U32(scratch+908)!=0xA5A5A5A5 || PPC_LOAD_U32(scratch+932)!=0xA5A5A5A5 ||
       PushMotion(ctx,base,scratch+1,{0,0,0,0}) || PushMotion(ctx,base,scratch,{501,0,0,0}) ||
       PushMotion(ctx,base,scratch,{0,0,0,2}) || PushMotion(ctx,base,1,{0,0,0,0}) ||
       std::memcmp(&push_saved,&ctx,sizeof(ctx)) || push_csr!=_mm_getcsr())return false;
    const auto animation=scratch,graph=scratch+256,context=scratch+512,queue=scratch+544,buffer=scratch+640,text=scratch+800;
    constexpr uint32_t id=0;
    std::memset(base+scratch,0,1536);PPC_STORE_U32(animation+192,graph);PPC_STORE_U32(animation+208,context);PPC_STORE_U32(context+12,queue);
    PPC_STORE_U32(queue,buffer);PPC_STORE_U32(queue+4,8);
    const auto old_event=PPC_LOAD_U32(event_registry),old_variable=PPC_LOAD_U32(variable_registry);const auto saved_event_cache=event_ids,saved_variable_cache=variable_ids;
    const bool saved_events_loaded=event_registry_loaded,saved_variables_loaded=variable_registry_loaded;
    std::memcpy(base+text,"PCFixtureEvent",15);std::memcpy(base+text+32,"PCFixtureVariable",18);
    PPC_STORE_U32(event_registry,text);PPC_STORE_U32(variable_registry,text+32);
    event_ids.clear();variable_ids.clear();event_registry_loaded=variable_registry_loaded=false;
    const auto saved=ctx;const auto csr=_mm_getcsr();const bool sent=NativeAnimationEvent(ctx,base,animation,"PCFixtureEvent");
    auto& owner=animation_owners[0];const auto old_animation=owner.animation.load(),old_owned=owner.owned.load(),old_interrupt=owner.interrupt.load();
    std::array<NativeActor,2> previous_actors;{std::lock_guard lock(mutex);previous_actors=actors;}
    auto replacement=previous_actors;replacement[0]={0x5000,0x1000,animation,true,1};UpdateNativeEngineActors(replacement);
    owner.owned=2;const auto identity_serial=owner.interrupt.load();UpdateNativeEngineActors(replacement);
    bool identity_safe=owner.owned.load()==2 && owner.interrupt.load()==identity_serial;
    replacement[0].identity=2;UpdateNativeEngineActors(replacement);
    identity_safe&=!owner.owned.load() && owner.interrupt.load()==identity_serial+1;
    UpdateNativeEngineActors(previous_actors);owner.interrupt=old_interrupt;
    const auto old_locomotion=locomotion_ids[0].load(),old_run=locomotion_ids[2].load(),old_hit=interrupt_ids[0].load(),old_land=land_id.load();
    owner.animation=animation;owner.owned=2;locomotion_ids[0]=0;interrupt_ids[0]=12;land_id=13;
    bool filtering=!PassAnimationEvent(animation,0) && PassAnimationEvent(animation+4,0) && PassAnimationEvent(animation,13) && !owner.owned.load() && owner.interrupt.load()==old_interrupt;
    owner.owned=1;filtering&=PassAnimationEvent(animation,12) && !owner.owned.load() && owner.interrupt.load()==old_interrupt+1;
    owner.owned=3;locomotion_ids[2]=14;
    filtering&=!PassAnimationEvent(animation,14) && owner.owned.load()==3;
    filtering&=PassAnimationEvent(animation,0) && !owner.owned.load() && owner.interrupt.load()==old_interrupt+1;
    const auto old_overrides=overrides.load();const bool old_has_overrides=has_overrides.load();
    const auto maps=scratch+1024,entries=scratch+1088,values=scratch+1152,word=scratch+1184;
    PPC_STORE_U32(graph+116,maps);PPC_STORE_U32(graph+124,values);PPC_STORE_U32(values+8,word);
    PPC_STORE_U32(maps+20,entries);PPC_STORE_U32(maps+24,1);PPC_STORE_U32(maps+28,3);
    for(unsigned i=0;i<4;++i)PPC_STORE_U32(entries+i*8,UINT32_MAX);
    const auto slot=((id>>4)*0x9E3779B1u)&3u;PPC_STORE_U32(entries+slot*8,id);PPC_STORE_U32(entries+slot*8+4,0);
    filtering&=AnimationVariable(ctx,base,animation,"PCFixtureVariable",2.5f,false) && std::bit_cast<float>(PPC_LOAD_U32(word))==2.5f;
    filtering&=SetAnimationOverride(0,id,"PCFixtureVariable",7.5f,false);
    {Scope call(ctx);ctx.r3.u64=animation;ctx.r4.u64=id;ctx.f1.f64=1;sub_8283E658(ctx,base);}
    filtering&=std::bit_cast<float>(PPC_LOAD_U32(word))==7.5f && !OverrideValue(animation,id,true);
    // Read the exact word written by the retail setter through the queued
    // snapshot path used by the console UI, including registry identifier0.
    const auto event_map=scratch+1216,event_array=scratch+1264;
    PPC_STORE_U32(graph+108,event_map);
    PPC_STORE_U32(event_map+8,event_array);PPC_STORE_U32(event_map+12,1);PPC_STORE_U32(event_map+16,4);
    PPC_STORE_U32(event_array,id);
    PPC_STORE_U32(event_map+20,entries);PPC_STORE_U32(event_map+24,1);PPC_STORE_U32(event_map+28,3);
    const auto previous_view=graph_snapshot.load();const bool previous_published=completion_metadata_published;
    std::deque<Command> previous_commands;{std::lock_guard lock(mutex);previous_commands.swap(commands);Command snapshot;snapshot.kind=Kind::Snapshot;snapshot.actor=0;commands.push_back(snapshot);}
    completion_metadata_published=true;UpdateNativeEngineActors(replacement);RunNativeEngineCommands(ctx,base);
    const auto view=NativeAnimationSnapshot();bool graph_valid=view && view->actor==0 && view->events==std::vector<std::string>{"PCFixtureEvent"} && view->variables.size()==1 && view->variables[0].name=="PCFixtureVariable" && view->variables[0].floating==7.5f && view->variables[0].integer==std::bit_cast<int32_t>(7.5f);
    ++replacement[0].identity;UpdateNativeEngineActors(replacement);graph_valid&=!NativeAnimationSnapshot();UpdateNativeEngineActors(previous_actors);
    completion_metadata_published=previous_published;graph_snapshot.store(previous_view);{std::lock_guard lock(mutex);commands.swap(previous_commands);}owner.animation=animation;
    filtering&=graph_valid && NativeAnimationGraphHasEvent(animation,id,base) && !NativeAnimationGraphHasEvent(animation,405,base);
    filtering&=SetAnimationOverride(0,id,"PCFixtureVariable",11,true);
    {Scope call(ctx);ctx.r3.u64=animation;ctx.r4.u64=id;ctx.r5.s64=1;sub_8283E778(ctx,base);}
    filtering&=PPC_LOAD_U32(word)==11 && !OverrideValue(animation,id,false);
    overrides.store(old_overrides);has_overrides.store(old_has_overrides);
    PPC_STORE_U32(event_registry,old_event);PPC_STORE_U32(variable_registry,old_variable);event_ids=saved_event_cache;variable_ids=saved_variable_cache;event_registry_loaded=saved_events_loaded;variable_registry_loaded=saved_variables_loaded;
    owner.animation=old_animation;owner.owned=old_owned;owner.interrupt=old_interrupt;locomotion_ids[0]=old_locomotion;locomotion_ids[2]=old_run;interrupt_ids[0]=old_hit;land_id=old_land;
    return identity_safe && filtering && sent && PPC_LOAD_U32(buffer)==id && PPC_LOAD_U32(queue+16)==1 && PPC_LOAD_U32(queue+12)==1 && !std::memcmp(&saved,&ctx,sizeof(ctx)) && _mm_getcsr()==csr && !NativeAnimationEvent(ctx,base,1,"EVENT_Start_Stomping") && !NativeStimulusSphere(ctx,base,1,2,1,3) && !NativeStimulusSphere(ctx,base,scratch,2,1,0);
}

// The native controller continues to supply graph variables. While a custom
// ability owns the pose, its routine locomotion events must not replace that
// pose on an ascending-to-falling transition. This hook performs no guest
// pointer traversal; landing, damage, death and scripted sequences still pass.
PPC_FUNC_IMPL(sub_82843878){if(PassAnimationEvent(ctx.r3.u32,ctx.r4.u32))__imp__sub_82843878(ctx,base);}
PPC_FUNC_IMPL(sub_8283E658){if(const auto value=OverrideValue(ctx.r3.u32,ctx.r4.u32,false))ctx.f1.f64=*value;__imp__sub_8283E658(ctx,base);}
PPC_FUNC_IMPL(sub_8283E778){if(const auto value=OverrideValue(ctx.r3.u32,ctx.r4.u32,true))ctx.r5.s64=int32_t(*value);__imp__sub_8283E778(ctx,base);}
