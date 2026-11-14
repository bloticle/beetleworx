#include "online_actor_input.h"
#include "online_retail_input.h"
#include "online_actor_reference.h"
#include "online_actor_frames.h"
#include "online_actor_death.h"
#include "online_actor_jump.h"
#include "online_actor_roster.h"
#include "online_behavior_trace.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <rex/cvar.h>
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(bool,em2_probe_owned_button_tasks);
REXCVAR_DEFINE_BOOL(em2_owned_actor_policy_refresh,true,"Online","Propagate authored per-character task restriction changes to extra native humans at the next safe input boundary");
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <windows.h>
#include <immintrin.h>
PPC_EXTERN_FUNC(__imp__sub_82A3C930);
PPC_EXTERN_FUNC(__imp__sub_82783CE8);
PPC_EXTERN_FUNC(__imp__sub_8276B968);
PPC_EXTERN_FUNC(__imp__sub_8276BA50);
PPC_EXTERN_FUNC(__imp__sub_82A26B60);
PPC_EXTERN_FUNC(__imp__sub_8285D768);
PPC_EXTERN_FUNC(__imp__sub_82A27E20);
PPC_EXTERN_FUNC(__imp__sub_82A27F80);
PPC_EXTERN_FUNC(__imp__sub_82853E88);
PPC_EXTERN_FUNC(__imp__sub_82853E48);
PPC_EXTERN_FUNC(__imp__sub_82853C60);
PPC_EXTERN_FUNC(__imp__sub_822F1C10);
PPC_EXTERN_FUNC(__imp__sub_825430D8);
PPC_EXTERN_FUNC(__imp__sub_82732A18);
PPC_EXTERN_FUNC(__imp__sub_827B58D0);
PPC_EXTERN_FUNC(__imp__sub_82A69948);
PPC_EXTERN_FUNC(__imp__sub_82783208);
PPC_EXTERN_FUNC(__imp__sub_827874B0);
PPC_EXTERN_FUNC(__imp__sub_8278A790);
PPC_EXTERN_FUNC(__imp__sub_827835A8);
PPC_EXTERN_FUNC(__imp__sub_827CA160);
PPC_EXTERN_FUNC(sub_827835A8);

namespace {
using Actor=em2::net::OwnedActorInput;
using Result=em2::net::ActorInputResult;
using Action=em2::net::NativeActorAction;
std::mutex mutex;
std::array<Actor,16> actors{};
DWORD owner_thread=0;
uint64_t generation=0;
struct BasisClock {uint32_t registry=0;uint64_t tick=0;bool valid=false;};
struct BasisEntry {Actor actor;em2::net::ActorLookBasis basis;uint32_t motion=0,reference=0;bool staged=false,consumed=false,input_claimed=false;};
BasisClock basis_clock;
std::array<BasisEntry,16> basis_entries{};
struct BasisUpdate {BasisEntry entry;};
thread_local const BasisUpdate* basis_update=nullptr;
// Only isolated native controller fixtures may omit the coordinator input step.
thread_local bool basis_fixture_without_input=false;
struct PolicyDelta {
    unsigned profile=0;bool add=false;
    std::array<char,96> name{},reason{};
    std::array<uint64_t,16> generations{};
};
struct PolicyQueue {std::array<PolicyDelta,64> entries{};unsigned count=0;bool failed=false;};
thread_local PolicyQueue pending_policy;
struct Environment {unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(csr);SetLastError(error);}};
struct ContextScope {PPCContext& ctx;PPCContext saved;Environment environment;explicit ContextScope(PPCContext& c):ctx(c),saved(c){}~ContextScope(){ctx=saved;}};
bool Readable(uint32_t address,size_t size,uint8_t* base){return base && address>=4096 && uint64_t(address)+size<=0x100000000ull && em2::GuestReadBatch::Readable(base+address,size);}
std::optional<Actor> ByPeer(uint64_t peer){std::lock_guard lock(mutex);for(const auto& actor:actors)if(actor.peer==peer && peer)return actor;return {};}
std::optional<Actor> ByListener(uint32_t listener){std::lock_guard lock(mutex);for(const auto& actor:actors)if(actor.peer && actor.listener==listener)return actor;return {};}
bool Owner(){std::lock_guard lock(mutex);return owner_thread && owner_thread==GetCurrentThreadId();}
bool PolicyString(uint32_t ref,std::array<char,96>& text,uint8_t* base){
    if(!Readable(ref,4,base))return false;const auto value=PPC_LOAD_U32(ref);
    if(!value)return true;
    for(unsigned i=0;i<text.size();++i){if(!Readable(value+i,1,base))return false;
        text[i]=char(PPC_LOAD_U8(value+i));if(!text[i])return true;}
    return false;
}
void NoteHumanTaskPolicy(uint32_t manager,unsigned profile,uint32_t name,uint32_t reason,bool add,uint8_t* base){
    if(!REXCVAR_GET(em2_owned_actor_policy_refresh) || profile>1 || !Owner() ||
       !Readable(0x833DAB60,4,base) || manager!=PPC_LOAD_U32(0x833DAB60))return;
    PolicyDelta event;event.profile=profile;event.add=add;bool targeted=false;
    {std::lock_guard lock(mutex);for(unsigned i=0;i<actors.size();++i){const auto& actor=actors[i];
        if(actor.peer && actor.enabled && actor.action_user==profile){event.generations[i]=actor.generation;targeted=true;}}}
    if(!targeted)return; // Later admissions read the current native policy once.
    if(pending_policy.count==pending_policy.entries.size() || !PolicyString(name,event.name,base) || !event.name[0] ||
       (add && reason && !PolicyString(reason,event.reason,base))){pending_policy.failed=true;return;}
    pending_policy.entries[pending_policy.count++]=event;
}

bool Resolve(uint32_t registry,uint32_t handle,PPCContext& ctx,uint8_t* base,uint32_t& entity){
    ContextScope restore(ctx);const auto top=ctx.r1.u32&~15u;
    if(!registry || !handle || top<8192 || !Readable(registry,24,base) || !Readable(top-4096,4096,base))return false;
    std::array<uint8_t,4096> saved;std::memcpy(saved.data(),base+top-4096,saved.size());
    struct Scratch {uint8_t* destination;const std::array<uint8_t,4096>& saved;~Scratch(){std::memcpy(destination,saved.data(),saved.size());}}scratch{base+top-4096,saved};
    const auto key=top-512;PPC_STORE_U32(key,handle);ctx.r1.u64=top-2048;ctx.r3.u64=registry;ctx.r4.u64=key;
    __imp__sub_82732A18(ctx,base);entity=ctx.r3.u32;return entity!=0;
}
bool Inspect(Actor& actor,PPCContext& ctx,uint8_t* base){
    if(actor.action_user>1)return false;
    const auto registry=PPC_LOAD_U32(0x833DA7E4),manager=PPC_LOAD_U32(0x833DAB60);
    if(registry!=actor.registry || !Readable(manager,640,base) || !actor.handle ||
        PPC_LOAD_U32(manager+20)==actor.handle || PPC_LOAD_U32(manager+340)==actor.handle)return false;
    uint32_t entity=0;if(!Resolve(registry,actor.handle,ctx,base,entity) || !Readable(entity,96,base))return false;
    for(unsigned offset:{20u,340u}){const auto handle=PPC_LOAD_U32(manager+offset);uint32_t retail=0;
        if(handle && Resolve(registry,handle,ctx,base,retail) && retail==entity)return false;}
    const auto types=PPC_LOAD_U32(entity+88),list=PPC_LOAD_U32(entity+36);
    const auto kind=PPC_LOAD_U8(0x833E2188);
    if(!Readable(types,unsigned(kind)+1,base))return false;
    const auto slot=PPC_LOAD_U8(types+kind);if(slot==255 || !Readable(list,unsigned(slot)*4+4,base))return false;
    const auto face=PPC_LOAD_U32(list+unsigned(slot)*4);
    // Exact layout from native camera helper82A28250 and human input82A3C930.
    if(face<24 || !Readable(face-24,800,base) || PPC_LOAD_U32(face)!=0x8211DCE0)return false;
    const auto controller=face-24,listener=controller+80;
    if(PPC_LOAD_U32(listener)!=0x8211DCD0 || PPC_LOAD_U32(0x8211DCD0)!=0x82A3C930 ||
        PPC_LOAD_U32(controller+64)!=entity-8)return false;
    if(actor.entity && (actor.entity!=entity || actor.controller!=controller || actor.listener!=listener))return false;
    actor.entity=entity;actor.controller=controller;actor.listener=listener;return true;
}
bool Current(const Actor& actor,PPCContext& ctx,uint8_t* base){auto checked=actor;return Inspect(checked,ctx,base);}
struct Replay {Actor actor;};
thread_local const Replay* replay=nullptr;
class ReplayScope {const Replay* saved=replay;public:explicit ReplayScope(const Replay& next){replay=&next;}~ReplayScope(){replay=saved;}};
bool Quarantine(PPCContext& ctx,uint8_t* base){
    const auto actor=ByListener(ctx.r3.u32);if(!actor)return false;
    Environment environment;
    // Never claim a reused retail listener or a stale world as an owned extra.
    if(!Current(*actor,ctx,base))return false;
    return !actor->enabled || !replay || replay->actor.peer!=actor->peer || replay->actor.generation!=actor->generation;
}
bool BasisValid(const Actor& actor,const em2::net::ActorLookBasis& basis){
    if(basis.peer!=actor.peer || basis.generation!=actor.generation)return false;
    double right=0,forward=0,dot=0;
    for(unsigned i=0;i<3;++i){if(!std::isfinite(basis.right[i]) || !std::isfinite(basis.forward[i]))return false;
        right+=double(basis.right[i])*basis.right[i];forward+=double(basis.forward[i])*basis.forward[i];dot+=double(basis.right[i])*basis.forward[i];}
    return std::abs(right-1)<1e-4 && std::abs(forward-1)<1e-4 && std::abs(dot)<1e-4;
}
bool BasisWorldReady(uint32_t registry,uint8_t* base){
    if(!base || !registry || PPC_LOAD_U32(0x833DA7E4)!=registry)return false;
    const auto manager=PPC_LOAD_U32(0x833DAB60),profile=PPC_LOAD_U32(0x833DAD98);
    return Readable(manager,640,base) && PPC_LOAD_U8(manager+357) && Readable(profile,103,base) && !PPC_LOAD_U8(profile+102);
}
bool NativeMotionFor(const Actor& actor,uint8_t* base,uint32_t& motion){
    const auto types=PPC_LOAD_U32(actor.entity+88),list=PPC_LOAD_U32(actor.entity+36);
    const auto kind=PPC_LOAD_U8(0x833DAF54);
    if(!Readable(types,unsigned(kind)+1,base))return false;
    const auto slot=PPC_LOAD_U8(types+kind);if(slot==255 || !Readable(list,unsigned(slot)*4+4,base))return false;
    const auto face=PPC_LOAD_U32(list+unsigned(slot)*4);if(face<8 || !Readable(face-8,896,base))return false;
    motion=face-8;
    return PPC_LOAD_U32(motion+48)==actor.entity-8;
}
Result BasisContext(const Actor& actor,PPCContext& ctx,uint8_t* base,uint32_t& motion,uint32_t& reference){
    if(!Current(actor,ctx,base))return Result::Stale;
    if(!BasisWorldReady(actor.registry,base))return Result::InvalidContext;
    const auto component=actor.controller+24;
    // Original per-frame controller's ordinary-reference path ONLY. Captured
    // transition bases and scripted/alternate references retain native rules.
    if((PPC_LOAD_U8(component+44)&0x80) || PPC_LOAD_U8(component+233) ||
       PPC_LOAD_U32(component+284) || PPC_LOAD_U8(component+289))return Result::InvalidContext;
    reference=PPC_LOAD_U32(component+72);
    if(!Readable(reference,96,base) || PPC_LOAD_U32(reference)!=0x8208D908 ||
       PPC_LOAD_U32(0x8208D908+172)!=0x827CA160)return Result::ReferenceRequired;
    if(!NativeMotionFor(actor,base,motion))return Result::InvalidContext;
    return Result::Applied;
}
std::optional<BasisEntry> BasisForComponent(uint32_t component,PPCContext& ctx,uint8_t* base,bool require_input=true){
    BasisEntry entry;BasisClock clock;
    {std::lock_guard lock(mutex);clock=basis_clock;for(const auto& candidate:basis_entries)
        if(candidate.staged && !candidate.consumed && candidate.actor.controller+24==component){entry=candidate;break;}}
    if(!entry.staged || !Owner())return {};
    auto retire=[&](){std::lock_guard lock(mutex);for(auto& pending:basis_entries)
        if(pending.staged && pending.actor.peer==entry.actor.peer && pending.actor.generation==entry.actor.generation && pending.basis.tick==entry.basis.tick)pending.consumed=true;};
    if(!clock.valid || clock.registry!=entry.actor.registry || clock.tick!=entry.basis.tick ||
       (require_input && !entry.input_claimed && !basis_fixture_without_input)){retire();return {};}
    const auto actor=ByPeer(entry.actor.peer);
    if(!actor || !actor->enabled || actor->generation!=entry.actor.generation){retire();return {};}
    uint32_t motion=0,reference=0;
    if(BasisContext(*actor,ctx,base,motion,reference)!=Result::Applied || motion!=entry.motion || reference!=entry.reference){retire();return {};}
    return entry;
}
void UpdateBasisController(PPCContext& ctx,uint8_t* base,PPCFunc* native){
    if(basis_update){
        struct Suspend {const BasisUpdate* saved=basis_update;Suspend(){basis_update=nullptr;}~Suspend(){basis_update=saved;}}suspend;
        native(ctx,base);return;
    }
    std::optional<BasisEntry> admitted;
    {Environment preserve;admitted=BasisForComponent(ctx.r3.u32,ctx,base);}
    if(!admitted){native(ctx,base);return;}
    const BasisUpdate update{*admitted};
    struct Scope {const BasisUpdate* saved=basis_update;explicit Scope(const BasisUpdate& value){basis_update=&value;}~Scope(){basis_update=saved;}}scope{update};
    native(ctx,base);
}
bool ClaimBasisBoundary(PPCContext& ctx,uint8_t* base,BasisEntry& output){
    if(!basis_update || ctx.lr!=0x827837E8 || ctx.r27.u32!=basis_update->entry.actor.controller+24 ||
       ctx.r3.u32!=basis_update->entry.motion || ctx.r31.u32!=ctx.r1.u32 || ctx.r6.u32!=ctx.r1.u32+96 || !Readable(ctx.r6.u32,16,base))return false;
    const auto current=BasisForComponent(ctx.r27.u32,ctx,base);if(!current || current->actor.generation!=basis_update->entry.actor.generation ||
        current->basis.tick!=basis_update->entry.basis.tick)return false;
    std::lock_guard lock(mutex);
    for(auto& entry:basis_entries)if(entry.staged && !entry.consumed && entry.actor.peer==current->actor.peer &&
        entry.actor.generation==current->actor.generation && entry.basis.tick==current->basis.tick){entry.consumed=true;output=*current;return true;}
    return false;
}
void ApplyBasisMotion(PPCContext& ctx,uint8_t* base,PPCFunc* native){
    BasisEntry entry;bool apply=false;
    {Environment preserve;apply=ClaimBasisBoundary(ctx,base,entry);}
    if(!apply){native(ctx,base);return;}
    const auto argument=ctx.r6.u32;
    std::array<uint8_t,16> saved;std::memcpy(saved.data(),base+argument,saved.size());
    struct RestoreArgument {uint8_t* base;uint32_t at;const std::array<uint8_t,16>& bytes;~RestoreArgument(){std::memcpy(base+at,bytes.data(),bytes.size());}}restore{base,argument,saved};
    // Original caller's local vector at frame+96, never a human input cache,
    // NiPointer, authoritative transform or camera array entry.
    for(unsigned i=0;i<3;++i)PPC_STORE_U32(argument+i*4,std::bit_cast<uint32_t>(entry.basis.forward[i]));
    PPC_STORE_U32(argument+12,0);
    native(ctx,base);
}
Result AdmitNativeActorAction(uint64_t peer,const Action& action,const em2::net::ActorLookBasis& basis,PPCContext& ctx,uint8_t* base,Actor& actor){
    auto found=ByPeer(peer);if(!found)return Result::Unregistered;actor=*found;
    if(!Owner())return Result::WrongThread;
    if(!Current(actor,ctx,base))return Result::Stale;
    if(!actor.enabled)return Result::Disabled;
    if(replay)return Result::Nested;
    if(!Readable(action.action,4,base) || !std::isfinite(action.x) || !std::isfinite(action.y) || std::abs(action.x)>1 || std::abs(action.y)>1)return Result::InvalidInput;
    if(!BasisValid(actor,basis))return Result::InvalidBasis;
    return Result::Applied;
}
Result MovementAdmission(uint64_t peer,const Action& action,PPCContext& ctx,uint8_t* base,Actor& actor,uint32_t& motion,uint32_t& reference){
    const auto found=ByPeer(peer);if(!found)return Result::Unregistered;actor=*found;
    if(!Owner())return Result::WrongThread;
    if(replay || basis_update)return Result::Nested;
    if(!Current(actor,ctx,base))return Result::Stale;
    if(!actor.enabled)return Result::Disabled;
    if(!Readable(action.action,4,base) || !std::isfinite(action.x) || !std::isfinite(action.y) ||
       std::abs(action.x)>1 || std::abs(action.y)>1)return Result::InvalidInput;
    const auto uid=PPC_LOAD_U32(action.action),movement=PPC_LOAD_U32(0x833DAEB8+actor.action_user*12);
    if(!movement || uid!=movement)return Result::UnsupportedAction;
    // These native Mickey tool/brush branches precede its ordinary Movement
    // branch and may affect shared resources. Never admit aliased authored IDs.
    for(unsigned offset:{280u,284u,288u,292u,296u,300u})
        if(PPC_LOAD_U32(actor.listener+offset)==uid)return Result::UnsupportedAction;
    return BasisContext(actor,ctx,base,motion,reference);
}
// Test-only ingress to prove original native callback isolation before a
// production motion/basis route exists. It is not exposed in the public API.
Result NativeCallbackFixture(uint64_t peer,const Action& action,const em2::net::ActorLookBasis& basis,PPCContext& ctx,uint8_t* base,PPCFunc* callback){
    ContextScope preserve(ctx);Actor actor;const auto admitted=AdmitNativeActorAction(peer,action,basis,ctx,base,actor);
    if(admitted!=Result::Applied)return admitted;
    Replay state{actor};ReplayScope scoped(state);
    ctx.r3.u64=actor.listener;ctx.r4.u64=action.event;ctx.r5.u64=action.action;ctx.f1.f64=action.x;ctx.f2.f64=action.y;
    callback(ctx,base);return Result::Applied;
}
}
bool em2::net::RegisterOwnedActorInput(uint64_t peer,uint32_t handle,PPCContext& ctx,uint8_t* base,Actor& output,unsigned action_user){
    Environment preserve;output={};if(!peer || !base || action_user>1 || basis_update || replay)return false;
    {std::lock_guard lock(mutex);if(owner_thread && owner_thread!=GetCurrentThreadId())return false;}
    Actor candidate;candidate.peer=peer;candidate.handle=handle;candidate.action_user=action_user;candidate.registry=PPC_LOAD_U32(0x833DA7E4);
    if(!Inspect(candidate,ctx,base))return false;
    std::lock_guard lock(mutex);
    for(const auto& actor:actors)if(actor.peer && (actor.peer==peer || actor.handle==handle || actor.entity==candidate.entity || actor.controller==candidate.controller || actor.listener==candidate.listener))return false;
    const auto free=std::find_if(actors.begin(),actors.end(),[](const Actor& value){return !value.peer;});if(free==actors.end())return false;
    if(generation==UINT64_MAX)return false;
    // The registry itself persists across title/level reloads. Its address is
    // not a session epoch: after the last actor is retired, the next admitted
    // group owns a fresh input clock. Never rewind a group with live members.
    if(std::none_of(actors.begin(),actors.end(),[](const Actor& value){return value.peer!=0;})){
        basis_clock={};basis_entries={};
    }
    owner_thread=GetCurrentThreadId();candidate.generation=++generation;candidate.enabled=true;*free=candidate;output=candidate;return true;
}
bool em2::net::SetOwnedActorInputEnabled(uint64_t peer,bool enabled){std::lock_guard lock(mutex);if(owner_thread!=GetCurrentThreadId())return false;for(auto& actor:actors)if(actor.peer==peer && peer){actor.enabled=enabled;for(auto& entry:basis_entries)if(entry.actor.peer==peer)entry={};return true;}return false;}
bool em2::net::SetOwnedActorInputEnabled(uint64_t peer,uint64_t actor_generation,bool enabled){
    std::lock_guard lock(mutex);if(owner_thread!=GetCurrentThreadId() || !peer || !actor_generation)return false;
    for(auto& actor:actors)if(actor.peer==peer){if(actor.generation!=actor_generation)return false;
        actor.enabled=enabled;for(auto& entry:basis_entries)if(entry.actor.peer==peer && entry.actor.generation==actor_generation)entry={};return true;}
    return false;
}
void em2::net::UnregisterOwnedActorInput(uint64_t peer){std::lock_guard lock(mutex);if(owner_thread!=GetCurrentThreadId())return;for(auto& actor:actors)if(actor.peer==peer && peer)actor={};for(auto& entry:basis_entries)if(entry.actor.peer==peer)entry={};}
bool em2::net::UnregisterOwnedActorInput(uint64_t peer,uint64_t actor_generation){
    std::lock_guard lock(mutex);if(owner_thread!=GetCurrentThreadId() || !peer || !actor_generation)return false;
    for(auto& actor:actors)if(actor.peer==peer){if(actor.generation!=actor_generation)return false;actor={};break;}
    for(auto& entry:basis_entries)if(entry.actor.peer==peer && entry.actor.generation==actor_generation)entry={};return true;
}
void em2::net::ResetOwnedActorInputs(){std::lock_guard lock(mutex);if(owner_thread && owner_thread!=GetCurrentThreadId())return;actors={};basis_entries={};basis_clock={};pending_policy={};owner_thread=0;}
bool em2::net::OwnedActorInputFor(uint64_t peer,PPCContext& ctx,uint8_t* base,Actor& output){Environment preserve;output={};auto found=ByPeer(peer);if(!found || !Owner() || !Current(*found,ctx,base))return false;output=*found;return true;}
bool em2::net::OwnedActorInputForController(uint32_t controller,PPCContext& ctx,uint8_t* base,Actor& output){
    ContextScope preserve(ctx);output={};if(!Owner())return false;
    std::optional<Actor> found;
    {std::lock_guard lock(mutex);for(const auto& actor:actors)if(actor.peer && actor.controller==controller){found=actor;break;}}
    if(!found || !Current(*found,ctx,base))return false;output=*found;return true;
}
bool em2::net::BeginOwnedActorBasisTick(uint32_t registry,uint64_t tick,PPCContext&,uint8_t* base){
    Environment preserve;if(!Owner() || basis_update || !BasisWorldReady(registry,base))return false;
    std::lock_guard lock(mutex);
    if(basis_clock.valid && (basis_clock.registry!=registry || tick<=basis_clock.tick))return false;
    basis_clock={registry,tick,true};basis_entries={};return true;
}
em2::net::ActorInputResult em2::net::PrepareOwnedActorBasis(uint64_t peer,const ActorLookBasis& basis,PPCContext& ctx,uint8_t* base){
    Environment preserve;const auto actor=ByPeer(peer);if(!actor)return Result::Unregistered;
    if(!Owner())return Result::WrongThread;
    if(basis_update)return Result::Nested;
    if(!actor->enabled)return Result::Disabled;
    if(!BasisValid(*actor,basis) || basis.forward[1]!=0 || basis.right[1]!=0 ||
        std::abs(basis.right[0]-basis.forward[2])>1e-4f || std::abs(basis.right[2]+basis.forward[0])>1e-4f)return Result::InvalidBasis;
    BasisClock clock;{std::lock_guard lock(mutex);clock=basis_clock;}
    if(!clock.valid || clock.registry!=actor->registry || clock.tick!=basis.tick)return Result::InvalidTick;
    uint32_t motion=0,reference=0;const auto result=BasisContext(*actor,ctx,base,motion,reference);if(result!=Result::Applied)return result;
    std::lock_guard lock(mutex);
    for(const auto& entry:basis_entries)if(entry.staged && entry.actor.peer==peer)return Result::InvalidTick;
    const auto free=std::find_if(basis_entries.begin(),basis_entries.end(),[](const BasisEntry& value){return !value.staged;});
    if(free==basis_entries.end())return Result::InvalidContext;
    *free={*actor,basis,motion,reference,true,false};return Result::Applied;
}
em2::net::ActorInputResult em2::net::ValidateOwnedActorMovement(uint64_t peer,const Action& action,PPCContext& ctx,uint8_t* base){
    ContextScope preserve(ctx);Actor actor;uint32_t motion=0,reference=0;
    return MovementAdmission(peer,action,ctx,base,actor,motion,reference);
}
em2::net::ActorInputResult em2::net::ReplayOwnedActorMovement(uint64_t peer,const Action& action,uint64_t tick,PPCContext& ctx,uint8_t* base){
    ContextScope preserve(ctx);Actor actor;uint32_t motion=0,reference=0;
    const auto admission=MovementAdmission(peer,action,ctx,base,actor,motion,reference);if(admission!=Result::Applied)return admission;
    const auto stage=BasisForComponent(actor.controller+24,ctx,base,false);
    if(!stage || stage->basis.tick!=tick || stage->motion!=motion || stage->reference!=reference)return Result::InvalidTick;
    {
        std::lock_guard lock(mutex);bool claimed=false;
        for(auto& entry:basis_entries)if(entry.staged && entry.actor.peer==peer &&
            entry.actor.generation==actor.generation && entry.basis.tick==tick && !entry.consumed && !entry.input_claimed){
            entry.input_claimed=true;claimed=true;break;
        }
        if(!claimed)return Result::InvalidTick;
    }
    // Claim before invoking native code: exceptions must never permit duplicate
    // native action/FSM side effects. Original derived handler retains global,
    // individual, lock, task-consumption and special-mode input gates.
    const Replay state{actor};ReplayScope scope(state);
    ctx.r3.u64=actor.listener;ctx.r4.u64=action.event;ctx.r5.u64=action.action;
    ctx.f1.f64=action.x;ctx.f2.f64=action.y;
    try{__imp__sub_82A3C930(ctx,base);}
    catch(...){
        std::lock_guard lock(mutex);
        for(auto& entry:basis_entries)if(entry.staged && entry.actor.peer==peer && entry.actor.generation==actor.generation && entry.basis.tick==tick)entry.consumed=true;
        throw;
    }
    return Result::Applied;
}
bool em2::net::OwnedActorBasisConsumed(uint64_t peer,uint64_t actor_generation,uint64_t tick){
    if(!Owner())return false;std::lock_guard lock(mutex);
    if(!basis_clock.valid || basis_clock.tick!=tick)return false;
    for(const auto& entry:basis_entries)if(entry.staged && entry.actor.peer==peer && entry.actor.generation==actor_generation && entry.basis.tick==tick)
        return entry.input_claimed && entry.consumed;
    return false;
}
bool em2::net::OwnedActorBasisDeliveryState(uint64_t peer,uint64_t actor_generation,uint64_t tick,bool& consumed){
    if(!Owner() || basis_update)return false;std::lock_guard lock(mutex);
    if(!basis_clock.valid || basis_clock.tick!=tick)return false;
    for(const auto& entry:basis_entries)if(entry.staged && entry.actor.peer==peer && entry.actor.generation==actor_generation && entry.basis.tick==tick){
        if(!entry.input_claimed)return false;consumed=entry.consumed;return true;
    }
    return false;
}
em2::net::ActorInputResult em2::net::detail::DispatchOwnedActorJump(uint64_t peer,const em2::net::NativeActorJump& command,PPCContext& ctx,uint8_t* base){
    // Preserve the exact caller bytes, including ABI padding. This backend is
    // callable only during the Jump module's private, exact-command permit.
    struct Caller {PPCContext& ctx;std::array<uint8_t,sizeof(PPCContext)> saved;Environment environment;
        explicit Caller(PPCContext& value):ctx(value){std::memcpy(saved.data(),&ctx,sizeof(ctx));}
        ~Caller(){std::memcpy(&ctx,saved.data(),sizeof(ctx));}} preserve(ctx);
    if(!OwnedJumpDispatchAuthorized(peer,command))return Result::InvalidContext;
    if(!Owner())return Result::WrongThread;
    if(replay || basis_update)return Result::Nested;
    const auto actor=ByPeer(peer);if(!actor)return Result::Unregistered;
    if(!command.generation || command.generation!=actor->generation || !Current(*actor,ctx,base))return Result::Stale;
    if(!actor->enabled)return Result::Disabled;
    const auto targets=ValidateOwnedJumpTargets(peer,command,ctx,base);if(targets!=Result::Applied)return targets;
    uint32_t motion=0,reference=0;const auto context=BasisContext(*actor,ctx,base,motion,reference);if(context!=Result::Applied)return context;
    // A premature Jump must leave a valid pending stage available for Movement.
    const auto stage=BasisForComponent(actor->controller+24,ctx,base,false);
    if(!stage || !stage->input_claimed || stage->consumed || stage->basis.tick!=command.tick ||
       stage->actor.generation!=command.generation || stage->motion!=motion || stage->reference!=reference)return Result::InvalidTick;
    const Replay state{*actor};ReplayScope scope(state);
    ctx.r3.u64=actor->listener;ctx.r4.u64=command.action.event;ctx.r5.u64=command.action.action;
    ctx.f1.f64=command.action.x;ctx.f2.f64=command.action.y;
    try{__imp__sub_82A3C930(ctx,base);}
    catch(...){
        std::lock_guard lock(mutex);
        for(auto& entry:basis_entries)if(entry.staged && entry.actor.peer==peer && entry.actor.generation==command.generation && entry.basis.tick==command.tick)entry.consumed=true;
        throw;
    }
    // Jump input does not consume the independently staged view/motion update.
    return Result::Applied;
}
em2::net::ActorInputResult em2::net::detail::DispatchOwnedActorButton(uint64_t peer,uint64_t actor_generation,uint64_t tick,const Action& action,PPCContext& ctx,uint8_t* base,uint32_t& native_result){
    ContextScope preserve(ctx);native_result=0;
    if(!OwnedButtonDispatchAuthorized(peer,actor_generation,tick,action))return Result::InvalidContext;
    if(!Owner())return Result::WrongThread;
    if(replay || basis_update)return Result::Nested;
    const auto actor=ByPeer(peer);if(!actor)return Result::Unregistered;
    if(actor->generation!=actor_generation || !Current(*actor,ctx,base))return Result::Stale;
    if(!actor->enabled)return Result::Disabled;
    if(!Readable(action.action,436,base) || PPC_LOAD_U32(action.action+80)>3 || !std::isfinite(action.x) || !std::isfinite(action.y))return Result::InvalidInput;
    const auto uid=PPC_LOAD_U32(action.action);
    if(uid==PPC_LOAD_U32(0x833DAEB8+actor->action_user*12) || uid==PPC_LOAD_U32(0x833DAEBC+actor->action_user*12) || uid==PPC_LOAD_U32(0x833DAEC0+actor->action_user*12))return Result::UnsupportedAction;
    uint32_t motion=0,reference=0;const auto context=BasisContext(*actor,ctx,base,motion,reference);if(context!=Result::Applied)return context;
    const auto stage=BasisForComponent(actor->controller+24,ctx,base,false);
    if(!stage || !stage->input_claimed || stage->consumed || stage->basis.tick!=tick || stage->actor.generation!=actor_generation || stage->motion!=motion || stage->reference!=reference)return Result::InvalidTick;
    const Replay state{*actor};ReplayScope scope(state);
    ctx.r3.u64=actor->listener;ctx.r4.u64=action.event;ctx.r5.u64=action.action;ctx.f1.f64=action.x;ctx.f2.f64=action.y;
    __imp__sub_82A3C930(ctx,base);native_result=ctx.r3.u32;
    return Result::Applied;
}
em2::net::ActorInputResult em2::net::ReplayOwnedActorAction(uint64_t peer,const Action& action,const ActorLookBasis& basis,PPCContext& ctx,uint8_t* base){
    Environment preserve;Actor actor;const auto admitted=AdmitNativeActorAction(peer,action,basis,ctx,base,actor);
    if(admitted!=Result::Applied)return admitted;
    if(PPC_LOAD_U32(action.action)!=PPC_LOAD_U32(0x833DAEB8+actor.action_user*12))return Result::UnsupportedAction;
    // Even an orthonormal peer basis is NOT permission to let the later native
    // NavigationInput task resolve its view through a retail two-slot camera.
    // Only ReplayOwnedActorMovement can authorize the proved staged-tick route.
    // Generic action replay has no complete native gameplay contract and never
    // changes action caches.
    return Result::BasisRouteUnverified;
}
bool em2::net::SuppressOwnedActorInput(PPCContext& ctx,uint8_t* base){if(!Quarantine(ctx,base))return false;ctx.r3.u64=0;return true;}
bool em2::net::ApplyOwnedActorTaskPermissions(uint64_t peer,PPCContext& ctx,uint8_t* base){
    ContextScope preserve(ctx);
    auto reject=[&](const char* reason,uint32_t value=0){REXLOG_ERROR("Owned actor task policy preflight: peer {} rejected {} value {:08X}",peer,reason,value);return false;};
    if(!Owner() || replay || basis_update)return reject("owner or active dispatch");
    const auto actor=ByPeer(peer);
    if(!actor || !actor->enabled || !Current(*actor,ctx,base))return reject("actor identity");
    const auto manager=PPC_LOAD_U32(0x833DAB60),collection=actor->controller+156;
    if(!Readable(manager,640,base) || actor->action_user>1)return reject("manager or action profile",manager);
    const auto buckets=PPC_LOAD_U32(collection+16),count=PPC_LOAD_U32(collection+20);
    if(!count || count>4096 || !Readable(buckets,size_t(count)*4,base))return reject("bucket bounds",count);
    std::array<uint32_t,256> visited{};unsigned seen=0;
    for(unsigned i=0;i<count;++i){auto node=PPC_LOAD_U32(buckets+i*4);
        while(node){
            if(seen==visited.size() || !Readable(node,12,base) || std::find(visited.begin(),visited.begin()+seen,node)!=visited.begin()+seen)return false;
            visited[seen++]=node;
            const auto task=PPC_LOAD_U32(node+4);
            if(!Readable(task,12,base) || PPC_LOAD_U32(task+4)!=actor->entity)return reject("task owner",task);
            const auto table=PPC_LOAD_U32(task);
            if(!Readable(table,72,base))return reject("task vtable",table);
            const auto name_method=PPC_LOAD_U32(table+68);
            if(name_method<0x82300000 || name_method>=0x83300000)return reject("task name method",name_method);
            node=PPC_LOAD_U32(node+8);
        }
    }
    // Retail human initialization82A2725C uses this exact existing-policy
    // reader.82A26990 instead REGISTERS permissions and is deliberately absent.
    ctx.r3.u64=manager;ctx.r4.u64=actor->action_user;ctx.r5.u64=collection;
    ctx.r6.u64=1;ctx.r7.u64=0;
    __imp__sub_82A26B60(ctx,base);
    const auto current=ByPeer(peer);
    return current && current->enabled && current->generation==actor->generation &&
        current->entity==actor->entity && Current(*current,ctx,base);
}
bool em2::net::RefreshOwnedActorTaskPermissions(PPCContext& ctx,uint8_t* base){
    ContextScope preserve(ctx);
    if((!pending_policy.count && !pending_policy.failed) || !REXCVAR_GET(em2_owned_actor_policy_refresh))return true;
    if(pending_policy.failed || !Owner() || replay || basis_update)return false;
    const auto top=ctx.r1.u32&~15u;
    if(top<65536 || !Readable(top-32768,32768,base))return false;
    std::array<uint8_t,32768> saved;std::memcpy(saved.data(),base+top-32768,saved.size());
    struct Stack{uint8_t* at;const std::array<uint8_t,32768>& bytes;~Stack(){std::memcpy(at,bytes.data(),bytes.size());}}stack{base+top-32768,saved};
    auto work=pending_policy;pending_policy.count=0;pending_policy.failed=false;
    ctx.r1.u64=top-4096;PPC_STORE_U32(ctx.r1.u32,preserve.saved.r1.u32);
    const auto name=top-512,reason=name+4,text=top-1024;
    // Replay ordered single-task native deltas. Reinitializing the full policy
    // does not clear a counter that became zero and can erase unrelated locks.
    for(unsigned n=0;n<work.count;++n){const auto& event=work.entries[n];
        std::memcpy(base+text,event.name.data(),event.name.size());ctx.r3.u64=text;__imp__sub_825430D8(ctx,base);PPC_STORE_U32(name,ctx.r3.u32);
        std::memcpy(base+text,event.reason.data(),event.reason.size());ctx.r3.u64=text;__imp__sub_825430D8(ctx,base);PPC_STORE_U32(reason,ctx.r3.u32);
        bool okay=true;
        for(unsigned i=0;i<event.generations.size();++i)if(event.generations[i]){
            Actor actor;{std::lock_guard lock(mutex);actor=actors[i];}
            if(!actor.peer || actor.generation!=event.generations[i] || !actor.enabled)continue;
            if(actor.action_user!=event.profile || !Current(actor,ctx,base)){okay=false;break;}
            ctx.r3.u64=actor.controller+156;ctx.r4.u64=name;__imp__sub_82853C60(ctx,base);const auto task=ctx.r3.u32;
            if(!Readable(task,12,base) || PPC_LOAD_U32(task+4)!=actor.entity){okay=false;break;}
            const auto before=(PPC_LOAD_U32(task+8)>>1)&7;
            ctx.r3.u64=actor.controller+156;ctx.r4.u64=name;ctx.r5.u64=reason;
            if(event.add)__imp__sub_82853E48(ctx,base);else __imp__sub_82853E88(ctx,base);
            const auto expected=event.add?std::min(7u,before+1):before?before-1:0;
            if(!Current(actor,ctx,base) || !Readable(task,12,base) || ((PPC_LOAD_U32(task+8)>>1)&7)!=expected){okay=false;break;}
            if(REXCVAR_GET(em2_private_profile))REXLOG_INFO("Owned actor native task delta: peer {} profile {} task '{}' operation {}; original single-task counter, generation {}",actor.peer,actor.action_user,event.name.data(),event.add?"add":"remove",actor.generation);
        }
        ctx.r3.u64=reason;__imp__sub_822F1C10(ctx,base);ctx.r3.u64=name;__imp__sub_822F1C10(ctx,base);
        if(!okay){pending_policy.failed=true;return false;}
    }
    return !pending_policy.failed;
}
PPC_FUNC_IMPL(sub_82A27E20){
    const auto manager=ctx.r3.u32,profile=ctx.r4.u32,name=ctx.r5.u32,reason=ctx.r6.u32;
    __imp__sub_82A27E20(ctx,base);
    ContextScope preserve(ctx);NoteHumanTaskPolicy(manager,profile,name,reason,true,base);
}
PPC_FUNC_IMPL(sub_82A27F80){
    const auto manager=ctx.r3.u32,profile=ctx.r4.u32,name=ctx.r5.u32;
    __imp__sub_82A27F80(ctx,base);
    ContextScope preserve(ctx);NoteHumanTaskPolicy(manager,profile,name,0,false,base);
}

PPC_FUNC_IMPL(sub_8285D768){
    const auto task=ctx.r3.u32,previous=ctx.r4.u32,caller=uint32_t(ctx.lr),stack=ctx.r1.u32;
    __imp__sub_8285D768(ctx,base);
    ContextScope preserve(ctx);
    if(!REXCVAR_GET(em2_probe_owned_button_tasks) || !REXCVAR_GET(em2_private_profile) ||
       !Readable(task,12,base) || !Readable(previous,4,base))return;
    const auto table=PPC_LOAD_U32(task),before=PPC_LOAD_U32(previous),after=PPC_LOAD_U32(task+8);
    if((table!=0x82163AC0 && table!=0x82165840 && table!=0x82164EA8 && table!=0x82161560) || (before&14)==(after&14))return;
    static thread_local unsigned reports=0;if(reports++>=512)return;
    std::array<uint32_t,4> callers{caller,0,0,0};auto frame=stack;
    for(unsigned i=1;i<callers.size();++i){
        if(!Readable(frame,4,base))break;const auto prior=PPC_LOAD_U32(frame);
        if(prior<=frame || prior-frame>65536 || !Readable(prior-8,8,base))break;
        callers[i]=PPC_LOAD_U32(prior-8);frame=prior;
    }
    REXLOG_INFO("Private task permission transition: entity {:08X} table {:08X} before {:08X} after {:08X} callers {:08X}/{:08X}/{:08X}/{:08X}",PPC_LOAD_U32(task+4),table,before,after,callers[0],callers[1],callers[2],callers[3]);
}
PPC_FUNC_IMPL(sub_8276B968){
    // Only this exact base human movement query, while replaying this actor,
    // can resolve to its owned entity. All normal native routes stay untouched.
    if(replay && ctx.lr==0x82783EB0 && ctx.r29.u32==replay->actor.listener && ctx.r3.u32==PPC_LOAD_U32(0x833DAB60) && ctx.r4.u32<2 && Current(replay->actor,ctx,base)){
        ctx.r3.u64=replay->actor.entity;return;
    }
    __imp__sub_8276B968(ctx,base);
}
PPC_FUNC_IMPL(sub_8276BA50){
    // The shared human handler classifies its action names using the retail
    // P2-identity query. Extras have no retail record: during this exact owned
    // replay only, classify by the actor's pinned native action profile.
    if(!replay || uint32_t(ctx.lr)!=0x82A3C9C8 || ctx.r30.u32!=replay->actor.listener ||
       ctx.r4.u32!=replay->actor.entity || ctx.r5.u32!=1){__imp__sub_8276BA50(ctx,base);return;}
    const auto owned=replay->actor;const auto manager=ctx.r3.u32;
    __imp__sub_8276BA50(ctx,base);
    bool admitted=false;
    {ContextScope preserve(ctx);const auto current=ByPeer(owned.peer);
        admitted=current && current->enabled && current->generation==owned.generation &&
            current->action_user==owned.action_user && manager==PPC_LOAD_U32(0x833DAB60) && Current(*current,ctx,base);}
    if(admitted)ctx.r3.u64=owned.action_user==1;
}
namespace {void ContinueOwnedBasisController(PPCContext& ctx,uint8_t* base){UpdateBasisController(ctx,base,__imp__sub_827835A8);}}
PPC_FUNC_IMPL(sub_827835A8){em2::net::UpdateRetailBasisController(ctx,base,ContinueOwnedBasisController);}
namespace {void ContinueBasisMotion(PPCContext& ctx,uint8_t* base){if(!em2::net::ApplyRetailBasisMotion(ctx,base,__imp__sub_827874B0))ApplyBasisMotion(ctx,base,__imp__sub_827874B0);}}
PPC_FUNC_IMPL(sub_827874B0){em2::net::RunNativeMotionInputTrace(ctx,base,ContinueBasisMotion);}

namespace {
void FixtureBase(PPCContext& ctx,uint8_t* base){if(!em2::net::SuppressOwnedActorInput(ctx,base))__imp__sub_82783CE8(ctx,base);}
void FixtureMickey(PPCContext& ctx,uint8_t* base){if(!em2::net::SuppressOwnedActorInput(ctx,base))__imp__sub_82A3C930(ctx,base);}
void ThrowAfterBase(PPCContext& ctx,uint8_t* base){FixtureBase(ctx,base);throw std::runtime_error("owned actor fixture unwind");}
// Fixture boundary only: a NiEntity property source supplying its authored
// 3x3 transform. The projection, flattening, normalization and capture gate
// are executed by the ORIGINAL BasicPlayerController method below.
void FixtureCameraTransform(PPCContext& ctx,uint8_t* base){
    const auto entity=ctx.r3.u32,output=ctx.r5.u32;
    if(ctx.lr!=0x82783284 || ctx.r4.u32!=0x833DA808 || ctx.r6.u32!=0)
        throw std::runtime_error("unexpected retained-camera property ABI");
    PPC_STORE_U32(entity+52,PPC_LOAD_U32(entity+52)+1);
    std::memcpy(base+output,base+entity+256,48);
    ctx.r3.u64=1;
}
void FixtureNoActionState(PPCContext& ctx,uint8_t*){ctx.r3.u64=0;}
void FixtureCurrentActionState(PPCContext& ctx,uint8_t*){ctx.r3.u64=0x44000;}
void FixtureActionState(PPCContext& ctx,uint8_t* base){
    const auto object=ctx.r3.u32;
    PPC_STORE_U32(object+20,PPC_LOAD_U32(object+20)+1);
    PPC_STORE_U32(object+24,ctx.r4.u32);
    PPC_STORE_U32(object+28,std::bit_cast<uint32_t>(float(ctx.f1.f64)));
    PPC_STORE_U32(object+32,std::bit_cast<uint32_t>(float(ctx.f2.f64)));
    if(PPC_LOAD_U32(object+36))throw std::runtime_error("native action-state unwind");
    ctx.r3.u64=PPC_LOAD_U32(object+16);
}
struct WorldVectorCaptured {};
struct ControllerUpdateStopped {};
void FixtureBoundedOwnedController(PPCContext& ctx,uint8_t* base){
    ContextScope preserve(ctx);
    try{sub_827835A8(ctx,base);}
    catch(const ControllerUpdateStopped&){
        if(basis_update || replay)throw std::logic_error("bounded owned controller leaked native input/basis scope");
        return;
    }
    throw std::logic_error("bounded owned controller did not reach the native continuation sink");
}
void FixtureControllerFrameEnd(PPCContext& ctx,uint8_t*){
    if(ctx.lr!=0x82783834)throw std::runtime_error("unexpected native controller continuation");
    throw ControllerUpdateStopped{};
}
void FixtureNativeReferenceTransform(PPCContext& ctx,uint8_t* base){
    if(ctx.lr!=0x827CA1B4 || ctx.r4.u32!=0x833DA808 || ctx.r6.u32)
        throw std::runtime_error("unexpected native reference component property ABI");
    const auto reference=PPC_LOAD_U32(ctx.r3.u32+4);
    PPC_STORE_U32(reference+52,PPC_LOAD_U32(reference+52)+1);
    std::memcpy(base+ctx.r5.u32,base+reference+256,48);ctx.r3.u64=1;
}
void FixtureInspectBasisArgument(PPCContext& ctx,uint8_t* base){
    ctx.r3.u64=PPC_LOAD_U32(ctx.r6.u32)==std::bit_cast<uint32_t>(1.f) &&
        PPC_LOAD_U32(ctx.r6.u32+4)==0 && PPC_LOAD_U32(ctx.r6.u32+8)==0;
}
struct WorldVectorProbe {uint32_t motion=0;std::array<float,3> vector{};bool reached=false;};
thread_local WorldVectorProbe* world_vector_probe=nullptr;
void FixtureMotionWorldVector(PPCContext& ctx,uint8_t* base){
    auto* probe=world_vector_probe;
    if(!probe || ctx.lr!=0x8278A8C0 || ctx.r3.u32!=probe->motion)
        throw std::runtime_error("unexpected native movement-vector ABI");
    for(unsigned i=0;i<3;++i)probe->vector[i]=std::bit_cast<float>(PPC_LOAD_U32(ctx.r4.u32+i*4));
    probe->reached=true;
    // Stop at the real virtual motion sink: do not pretend to exercise a
    // Havok body, contacts, animation or the remaining turn/steering update.
    throw WorldVectorCaptured{};
}
}
bool em2::net::TestOwnedActorInput(){
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
    struct Restore {uint8_t* base;decltype(actors) prior;decltype(basis_entries) prior_bases;BasisClock prior_clock;DWORD thread;uint64_t serial;PolicyQueue policy=pending_policy;const Replay* scope=replay;const BasisUpdate* update=basis_update;Environment environment;
        explicit Restore(uint8_t* memory):base(memory){std::lock_guard lock(mutex);prior=actors;prior_bases=basis_entries;prior_clock=basis_clock;thread=owner_thread;serial=generation;actors={};basis_entries={};basis_clock={};pending_policy={};owner_thread=0;}
        ~Restore(){std::lock_guard lock(mutex);actors=prior;basis_entries=prior_bases;basis_clock=prior_clock;owner_thread=thread;generation=serial;pending_policy=policy;replay=scope;basis_update=update;if(base)VirtualFree(base,0,MEM_RELEASE);}
    }restore{base};
    const auto path=std::filesystem::path(__FILE__).parent_path().parent_path()/"private/image.bin";
    std::error_code error;const auto image_size=std::filesystem::file_size(path,error);
    if(!base || error || !VirtualAlloc(base,0x50000,MEM_COMMIT,PAGE_READWRITE) ||
        !VirtualAlloc(base+PPC_IMAGE_BASE,image_size,MEM_COMMIT,PAGE_READWRITE) ||
        !VirtualAlloc(base+PPC_IMAGE_BASE+PPC_IMAGE_SIZE,4096,MEM_COMMIT,PAGE_READWRITE))return false;
    std::ifstream image(path,std::ios::binary);if(!image.read(reinterpret_cast<char*>(base+PPC_IMAGE_BASE),image_size))return false;
    PPC_LOOKUP_FUNC(base,uint32_t(PPC_CODE_BASE))=__imp__sub_827B58D0;
    PPC_LOOKUP_FUNC(base,uint32_t(PPC_CODE_BASE)+4)=__imp__sub_82A69948;
    constexpr uint32_t registry=0x2000,manager=0x3000,buckets=0x5000,action=0x8000;
    constexpr std::array<uint32_t,2> entities{0x10000,0x12000},controllers{0x18000,0x1A000},types{0x22000,0x23000},lists{0x24000,0x25000};
    PPC_STORE_U32(0x833DA7E4,registry);PPC_STORE_U32(0x833DAB60,manager);
    PPC_STORE_U32(registry+4,uint32_t(PPC_CODE_BASE));PPC_STORE_U32(registry+8,uint32_t(PPC_CODE_BASE)+4);PPC_STORE_U32(registry+16,buckets);PPC_STORE_U32(registry+20,4);
    PPC_STORE_U32(manager+20,0x111);PPC_STORE_U32(manager+340,0x222);
    PPC_STORE_U8(0x833E2188,3);PPC_STORE_U8(0x833DAF54,4);PPC_STORE_U8(0x833DBB13,5);
    PPC_STORE_U32(0x833DAEB8,0x8207F5A4);PPC_STORE_U32(action,0x8207F5A4); // Native Movement UID.
    PPC_STORE_U32(0x833DAD98,0x26000);
    auto context=[] {PPCContext ctx{};ctx.fpscr.InitHost();ctx.r1.u64=0x4F000;ctx.r13.u64=0x13572468;ctx.lr=0x12345678;return ctx;};
    auto insert=[&](uint32_t handle,uint32_t entity,uint32_t node){
        const auto key=0x6000u;PPC_STORE_U32(key,handle);auto hash=context();hash.r3.u64=key;__imp__sub_827B58D0(hash,base);
        const auto bucket=buckets+(hash.r3.u32%4)*4;
        PPC_STORE_U32(node,handle);PPC_STORE_U32(node+4,entity);PPC_STORE_U32(node+8,PPC_LOAD_U32(bucket));PPC_STORE_U32(bucket,node);
    };
    for(unsigned i=0;i<2;++i){
        const auto entity=entities[i],controller=controllers[i];
        PPC_STORE_U32(entity+88,types[i]);PPC_STORE_U32(entity+36,lists[i]);std::memset(base+types[i],255,256);PPC_STORE_U8(types[i]+3,0);
        PPC_STORE_U32(lists[i],controller+24);PPC_STORE_U32(controller+24,0x8211DCE0);PPC_STORE_U32(controller+80,0x8211DCD0);
        PPC_STORE_U32(controller+64,entity-8);PPC_STORE_U8(controller+68,0x80); // Native base forwarding gate: avoid unrelated FSM actions.
        insert(i+1,entity,0x7000+i*16);
    }
    // Sentinels stand for the untouched retail records, health and view state.
    std::array<uint8_t,640> retail;std::memcpy(retail.data(),base+manager,retail.size());
    PPC_STORE_U32(0x27000,5);PPC_STORE_U32(0x27004,5);PPC_STORE_U32(0x27008,0x12340000);
    unsigned checks=0;auto check=[&](bool value,const char* why){++checks;if(!value)REXLOG_ERROR("Owned actor input: {}",why);return value;};
    auto ctx=context();Actor a,b;
    const auto saved=ctx;const auto csr=_mm_getcsr();const auto last_error=GetLastError();
    if(!check(RegisterOwnedActorInput(100,1,ctx,base,a) && RegisterOwnedActorInput(200,2,ctx,base,b),"two extras resolve through actual native registry and typed human-controller layout"))return false;
    if(!check(!std::memcmp(&saved,&ctx,sizeof(ctx)) && _mm_getcsr()==csr && GetLastError()==last_error,"registration preserves full caller context/TLS/FP/LastError"))return false;
    {
    const auto original=actors;actors[0].enabled=true;actors[1].enabled=true;actors[1].action_user=1;
    constexpr uint32_t name_ref=0x28000,name_text=0x28100;
    PPC_STORE_U32(name_ref,name_text);std::memcpy(base+name_text,"ElectricStream",15);
    NoteHumanTaskPolicy(manager+4,0,name_ref,0,true,base);NoteHumanTaskPolicy(manager,2,name_ref,0,true,base);
    if(!check(!pending_policy.count && !pending_policy.failed,"foreign manager/profile cannot queue native policy changes"))return false;
    NoteHumanTaskPolicy(manager,1,name_ref,0,true,base);NoteHumanTaskPolicy(manager,1,name_ref,0,true,base);
    NoteHumanTaskPolicy(manager,1,name_ref,0,false,base);
    if(!check(pending_policy.count==3 && pending_policy.entries[0].add && pending_policy.entries[1].add && !pending_policy.entries[2].add &&
        !pending_policy.entries[0].generations[0] && pending_policy.entries[0].generations[1]==b.generation,
        "nested add/add/remove order and exact affected actor generations are retained"))return false;
    std::memcpy(base+name_text,"Changed",8);
    if(!check(std::string_view(pending_policy.entries[0].name.data())=="ElectricStream","queued policy owns its text rather than retaining a borrowed guest string"))return false;
    while(pending_policy.count<64)NoteHumanTaskPolicy(manager,0,name_ref,0,true,base);
    NoteHumanTaskPolicy(manager,0,name_ref,0,true,base);
    if(!check(pending_policy.count==64 && pending_policy.failed,"policy queue overflow faults instead of dropping native task state"))return false;
    actors=original;pending_policy={};
    if(!check(RefreshOwnedActorTaskPermissions(ctx,nullptr) && !std::memcmp(&saved,&ctx,sizeof(ctx)) && _mm_getcsr()==csr && GetLastError()==last_error,
        "no pending policy is a no-read/no-dispatch operation with full caller/environment preservation"))return false;
    }
    Actor duplicate;
    if(!check(!RegisterOwnedActorInput(100,2,ctx,base,duplicate) && !RegisterOwnedActorInput(300,1,ctx,base,duplicate) && !RegisterOwnedActorInput(300,0x111,ctx,base,duplicate),"peer/actor aliases and retail player handles cannot be registered"))return false;
    NativeActorAction first{0,action,.5f,-.25f},second{0,action,-.75f,.125f};
    ActorLookBasis basis_a{a.peer,a.generation,1},basis_b{b.peer,b.generation,1};
    auto invoke=[&](const Actor& actor,const NativeActorAction& input){auto call=context();call.r3.u64=actor.listener;call.r5.u64=input.action;call.f1.f64=input.x;call.f2.f64=input.y;FixtureBase(call,base);return call;};
    for(const auto& actor:{a,b}){invoke(actor,first);if(!check(PPC_LOAD_U32(actor.listener+168)==0 && PPC_LOAD_U32(actor.listener+172)==0,"ordinary shared Movement broadcasts are quarantined for owned extras"))return false;}
    if(!check(ReplayOwnedActorAction(a.peer,first,basis_a,ctx,base)==Result::BasisRouteUnverified && PPC_LOAD_U32(a.listener+172)==0,
        "production movement rejects an unverified per-peer NativeMotion/look-basis route without changing action caches"))return false;
    if(!check(NativeCallbackFixture(a.peer,first,basis_a,ctx,base,FixtureBase)==Result::Applied &&
        NativeCallbackFixture(b.peer,second,basis_b,ctx,base,FixtureBase)==Result::Applied,
        "test-only explicit peer scopes execute the ORIGINAL native base movement callback separately"))return false;
    auto value=[&](uint32_t at){return std::bit_cast<float>(PPC_LOAD_U32(at));};
    if(!check(value(a.listener+168)==first.y && value(a.listener+172)==first.x && value(b.listener+168)==second.y && value(b.listener+172)==second.x,
        "opposite native movement samples reach only their own controller caches without manual cache writes"))return false;
    if(!TestOwnedActorMovementReferences(ctx,base,a.peer,b.peer))return false;
    {
    // The projection probe intentionally snapshots different action samples.
    // Restore the complete fixture controllers before testing subsequent
    // input lifecycle behavior against the earlier native callback results.
    struct ControllerFixtureScope {
        uint8_t* base;std::array<uint32_t,2> addresses;std::array<std::array<uint8_t,800>,2> saved;
        ControllerFixtureScope(uint8_t* memory,std::array<uint32_t,2> values):base(memory),addresses(values){
            for(unsigned i=0;i<2;++i)std::memcpy(saved[i].data(),base+addresses[i],saved[i].size());}
        ~ControllerFixtureScope(){for(unsigned i=0;i<2;++i)std::memcpy(base+addresses[i],saved[i].data(),saved[i].size());}
    }controller_fixture_scope{base,controllers};
    // Native camera transition code82A14018/82A15210 resolves the target's
    // BasicPlayerController through type833DAE58 face-8, then calls82783208.
    // On Mickey that subobject is full controller+16, NOT listener+0.
    // This capture is latched by byte241 until native code clears it; it is
    // deliberately not described as a once-per-world-tick movement route.
    PPC_LOOKUP_FUNC(base,uint32_t(PPC_CODE_BASE)+8)=FixtureCameraTransform;
    constexpr uint32_t camera_table=0x32000;
    PPC_STORE_U32(camera_table+172,uint32_t(PPC_CODE_BASE)+8);
    auto capture=[&](const Actor& actor){auto call=context();call.r3.u64=actor.controller+16;__imp__sub_82783208(call,base);
        return call.r1.u32==0x4F000 && call.lr==0x12345678;};
    auto vector_at=[&](uint32_t at){return std::array<float,3>{value(at),value(at+4),value(at+8)};};
    auto close=[](const std::array<float,3>& actual,const std::array<float,3>& expected){
        for(unsigned i=0;i<3;++i)if(std::abs(actual[i]-expected[i])>1e-6f)return false;return true;};
    for(unsigned i=0;i<2;++i){
        const auto controller=controllers[i],camera=0x30000u+i*0x1000;
        // Same typed face shared by the derived and Basic controller views.
        PPC_STORE_U8(0x833DAE58,6);PPC_STORE_U8(types[i]+6,0);
        if(!check(PPC_LOAD_U32(lists[i])-8==controller+16,"typed BasicPlayerController face resolves to the verified Mickey subobject"))return false;
        PPC_STORE_U32(camera,camera_table);PPC_STORE_U32(controller+16+80,camera);
        PPC_STORE_U32(controller+16+232,std::bit_cast<uint32_t>(.25f+i));
        PPC_STORE_U32(controller+16+236,std::bit_cast<uint32_t>(-.5f-i));
        // Distinct native reference entities: both transform matrices are
        // fixture data; neither is a retail camera/PlayerManager record.
        for(unsigned j=0;j<12;++j)PPC_STORE_U32(camera+256+j*4,std::bit_cast<uint32_t>(j%5==0?1.f:0.f));
        const std::array<float,3> pitched=i?std::array<float,3>{-3,12,-4}:std::array<float,3>{3,-12,4};
        for(unsigned j=0;j<3;++j)PPC_STORE_U32(camera+256+32+j*4,std::bit_cast<uint32_t>(pitched[j]));
        PPC_STORE_U8(controller+16+241,0);
    }
    if(!check(capture(a) && close(vector_at(a.controller+16+256),{.6f,0,.8f}) &&
        PPC_LOAD_U32(b.controller+16+256)==0 && capture(b) && close(vector_at(b.controller+16+256),{-.6f,0,-.8f}),
        "original retained-camera transform projection independently flattens and normalizes two opposite actor bases"))return false;
    if(!check(value(a.controller+16+244)==.25f && value(a.controller+16+248)==-.5f &&
        value(b.controller+16+244)==1.25f && value(b.controller+16+248)==-1.5f,
        "native transition capture snapshots each actor's own movement samples"))return false;
    const auto captured=vector_at(a.controller+16+256);
    PPC_STORE_U32(0x30000+256+32,std::bit_cast<uint32_t>(-1.f));
    PPC_STORE_U32(0x30000+256+40,0);
    if(!check(capture(a) && vector_at(a.controller+16+256)==captured && PPC_LOAD_U32(0x30000+52)==1 && PPC_LOAD_U32(0x31000+52)==1,
        "native byte241 retains captured orientation across repeated calls instead of resampling or sharing a peer's camera"))return false;
    PPC_STORE_U8(a.controller+16+241,0);
    if(!check(capture(a) && close(vector_at(a.controller+16+256),{-1,0,0}) && PPC_LOAD_U32(0x30000+52)==2 &&
        close(vector_at(b.controller+16+256),{-.6f,0,-.8f}),"only the actor whose native capture latch is cleared refreshes its basis"))return false;
    PPC_STORE_U32(a.controller+16+80,0);PPC_STORE_U8(a.controller+16+241,0);
    if(!check(capture(a) && close(vector_at(a.controller+16+256),{-1,0,0}) && PPC_LOAD_U32(0x30000+52)==2 && PPC_LOAD_U8(a.controller+16+241)==1,
        "original null retained-camera fallback preserves prior basis and latches without a property query"))return false;
    }
    {
        PPC_LOOKUP_FUNC(base,uint32_t(PPC_CODE_BASE)+12)=FixtureMotionWorldVector;
        constexpr uint32_t motion_table=0x37000,basis_at=0x38000,owner_at=0x39000;
        PPC_STORE_U32(motion_table+144,uint32_t(PPC_CODE_BASE)+12);
        auto put_float=[&](uint32_t at,float input){PPC_STORE_U32(at,std::bit_cast<uint32_t>(input));};
        auto prepare=[&](uint32_t motion){std::memset(base+motion,0,1024);PPC_STORE_U32(motion,motion_table);
            PPC_STORE_U32(motion+48,owner_at);PPC_STORE_U32(motion+568,0x1e);put_float(motion+876,1);};
        auto input=[&](uint32_t motion,float forward,float lateral,const std::array<float,3>& basis){
            for(unsigned i=0;i<3;++i)put_float(basis_at+i*4,basis[i]);
            auto call=context();call.r3.u64=motion;call.r6.u64=basis_at;call.f1.f64=forward;call.f2.f64=lateral;
            __imp__sub_827874B0(call,base);
            return call.r1.u32==0x4f000 && call.lr==0x12345678;
        };
        auto world=[&](uint32_t motion,WorldVectorProbe& probe){
            auto call=context();ContextScope restore_context(call);
            struct Scope {WorldVectorProbe* prior=world_vector_probe;explicit Scope(WorldVectorProbe& p){world_vector_probe=&p;}~Scope(){world_vector_probe=prior;}}scope{probe};
            probe.motion=motion;call.r3.u64=motion;call.r6.u64=1;
            try{__imp__sub_8278A790(call,base);}catch(const WorldVectorCaptured&){return probe.reached;}
            return false;
        };
        auto vector_matches=[](const std::array<float,3>& actual,const std::array<float,3>& expected){
            for(unsigned i=0;i<3;++i)if(!std::isfinite(actual[i]) || std::abs(actual[i]-expected[i])>2e-5f)return false;return true;};
        constexpr uint32_t first_motion=0x35000,second_motion=0x36000;
        prepare(first_motion);prepare(second_motion);
        const std::array<std::array<float,3>,4> bases{{{0,0,1},{1,0,0},{0,0,-1},{-1,0,0}}};
        // Native input sign and rotation, established through ORIGINAL
        // 827874B0 -> 8278A790 -> atan2/axis-angle math -> virtual144.
        // The positive f1 sample starts on -X for the +Z reference and turns
        // through +Z,+X,-Z. Positive f2 starts on -Z, then -X,+Z,+X.
        const std::array<std::array<float,3>,4> forward_world{{{-1,0,0},{0,0,1},{1,0,0},{0,0,-1}}};
        const std::array<std::array<float,3>,4> lateral_world{{{0,0,-1},{-1,0,0},{0,0,1},{1,0,0}}};
        for(unsigned i=0;i<bases.size();++i){
            WorldVectorProbe positive,opposite,lateral;
            if(!check(input(first_motion,1,0,bases[i]) && input(second_motion,-1,0,bases[i]) &&
                world(first_motion,positive) && world(second_motion,opposite) && vector_matches(positive.vector,forward_world[i]) &&
                vector_matches(opposite.vector,{-forward_world[i][0],0,-forward_world[i][2]}),
                "original native motion computes cardinal/quarter-turn/opposite world vectors independently for two actors"))return false;
            if(!check(input(first_motion,0,1,bases[i]) && world(first_motion,lateral) && vector_matches(lateral.vector,lateral_world[i]),
                "original native lateral sign follows the camera basis without a guessed cross-product convention"))return false;
        }
        prepare(first_motion);PPC_STORE_U32(first_motion+568,0);WorldVectorProbe disabled;
        if(!check(input(first_motion,1,1,bases[0]) && world(first_motion,disabled) && vector_matches(disabled.vector,{0,0,0}) &&
            value(first_motion+132)==-1 && value(first_motion+136)==1,
            "native movement permission mask zeros effective motion while preserving raw action records"))return false;
        prepare(first_motion);PPC_STORE_U32(first_motion+872,1);put_float(first_motion+608,6);
        const auto prior_delta=PPC_LOAD_U32(0x833FCA54);put_float(0x833FCA54,1.f/60.f);
        struct RestoreDelta {uint8_t* base;uint32_t bits;~RestoreDelta(){PPC_STORE_U32(0x833FCA54,bits);}}restore_delta{base,prior_delta};
        WorldVectorProbe smoothed;
        if(!check(input(first_motion,.6f,.8f,bases[0]) && world(first_motion,smoothed) && vector_matches(smoothed.vector,{-.06f,0,-.08f}),
            "original linear native acceleration/smoothing remains active before basis rotation"))return false;
        if(!check(!world_vector_probe && !std::memcmp(retail.data(),base+manager,retail.size()),
            "native vector interception restores its private scope and never writes the retail camera/player array"))return false;
    }
    {
        struct RestoreControllers {
            uint8_t* base;std::array<uint32_t,2> controllers,types;std::array<std::array<uint8_t,800>,2> bytes;
            uint8_t ready;BasisClock clock;decltype(basis_entries) entries;
            RestoreControllers(uint8_t* memory,std::array<uint32_t,2> objects,std::array<uint32_t,2> type_tables):base(memory),controllers(objects),types(type_tables),ready(PPC_LOAD_U8(manager+357)),clock(basis_clock),entries(basis_entries){
                for(unsigned i=0;i<2;++i)std::memcpy(bytes[i].data(),base+controllers[i],800);}
            ~RestoreControllers(){for(unsigned i=0;i<2;++i){std::memcpy(base+controllers[i],bytes[i].data(),800);PPC_STORE_U8(types[i]+4,255);}
                PPC_STORE_U8(manager+357,ready);basis_clock=clock;basis_entries=entries;}
        }restore_controllers{base,controllers,types};
        struct IsolatedBasisFixture {bool prior=basis_fixture_without_input;IsolatedBasisFixture(){basis_fixture_without_input=true;}~IsolatedBasisFixture(){basis_fixture_without_input=prior;}} isolated_basis_fixture;
        // Genuine NiEntity dispatch and original827CA160 component-property
        // iteration. Only the leaf property source and the first subsequent
        // FSM continuation are fixture sinks; the entire native controller
        // update and native Motion boundary execute normally.
        auto* native_dispatch=&PPC_LOOKUP_FUNC(base,0x827CA160);
        const auto page=reinterpret_cast<uintptr_t>(native_dispatch)&~uintptr_t(4095);
        if(!VirtualAlloc(reinterpret_cast<void*>(page),4096,MEM_COMMIT,PAGE_READWRITE))return false;
        *native_dispatch=__imp__sub_827CA160;
        PPC_LOOKUP_FUNC(base,uint32_t(PPC_CODE_BASE)+16)=FixtureNativeReferenceTransform;
        PPC_LOOKUP_FUNC(base,uint32_t(PPC_CODE_BASE)+20)=FixtureControllerFrameEnd;
        constexpr uint32_t reference=0x3A000,reference_list=0x3B000,reference_component=0x3B100,reference_table=0x3B200,fsm_table=0x42000;
        PPC_STORE_U32(reference,0x8208D908);PPC_STORE_U32(reference+36,reference_list);PPC_STORE_U32(reference+44,1);
        PPC_STORE_U32(reference_list,reference_component);PPC_STORE_U32(reference_component,reference_table);PPC_STORE_U32(reference_component+4,reference);
        PPC_STORE_U32(reference_table+172,uint32_t(PPC_CODE_BASE)+16);PPC_STORE_U32(fsm_table+12,uint32_t(PPC_CODE_BASE)+20);
        for(unsigned j=0;j<12;++j)PPC_STORE_U32(reference+256+j*4,std::bit_cast<uint32_t>(j%5==0?1.f:0.f));
        // Retained reference forward is +Z; staged peer views deliberately
        // disagree, making fallback or accidental shared-view use observable.
        PPC_STORE_U32(reference+256+32,0);PPC_STORE_U32(reference+256+36,0);PPC_STORE_U32(reference+256+40,std::bit_cast<uint32_t>(1.f));
        auto put=[&](uint32_t at,float input){PPC_STORE_U32(at,std::bit_cast<uint32_t>(input));};
        for(unsigned i=0;i<2;++i){
            const auto motion=0x35000u+i*0x1000;
            std::memset(base+motion,0,1024);PPC_STORE_U32(motion+48,entities[i]-8);PPC_STORE_U32(motion+568,0x1e);put(motion+876,1);
            PPC_STORE_U8(types[i]+4,1);PPC_STORE_U32(lists[i]+4,motion+8);
            PPC_STORE_U8(controllers[i]+68,0);PPC_STORE_U32(controllers[i]+96,reference);PPC_STORE_U32(controllers[i]+104,fsm_table);
            PPC_STORE_U8(controllers[i]+257,0);PPC_STORE_U32(controllers[i]+308,0);PPC_STORE_U8(controllers[i]+313,0);
        }
        PPC_STORE_U8(manager+357,1);PPC_STORE_U8(0x26000+102,0);
        auto update=[&](const Actor& actor){auto call=context();call.r3.u64=actor.controller+24;
            try{sub_827835A8(call,base);}catch(const ControllerUpdateStopped&){
                return !basis_update && PPC_LOAD_U32(call.r1.u32+96)==0 && PPC_LOAD_U32(call.r1.u32+100)==0 &&
                    PPC_LOAD_U32(call.r1.u32+104)==std::bit_cast<uint32_t>(1.f);}
            return false;
        };
        auto basis_value=[&](uint32_t motion){return std::array<float,3>{value(motion+176),value(motion+180),value(motion+184)};};
        ActorLookBasis peer_a{a.peer,a.generation,10,{0,0,-1},{1,0,0}},peer_b{b.peer,b.generation,10,{0,0,1},{-1,0,0}};
        if(!check(BeginOwnedActorBasisTick(registry,10,ctx,base) && !BeginOwnedActorBasisTick(registry,10,ctx,base) &&
            !BeginOwnedActorBasisTick(registry,9,ctx,base),"authoritative basis ticks reject duplicate/backward clock advancement"))return false;
        auto pitched=peer_a;pitched.right={1,0,0};pitched.forward={0,.6f,.8f};
        auto vertical=pitched;vertical.forward={0,1,0};
        auto tiny_pitch=pitched;tiny_pitch.forward={0,1e-8f,1};
        auto mirrored=peer_a;mirrored.right={0,0,1};
        if(!check(PrepareOwnedActorBasis(a.peer,pitched,ctx,base)==Result::InvalidBasis &&
            PrepareOwnedActorBasis(a.peer,vertical,ctx,base)==Result::InvalidBasis &&
            PrepareOwnedActorBasis(a.peer,tiny_pitch,ctx,base)==Result::InvalidBasis &&
            PrepareOwnedActorBasis(a.peer,mirrored,ctx,base)==Result::InvalidBasis,
            "staging requires explicit ground-projected native basis: pitched/vertical/tiny-Y/mirrored alternatives reject"))return false;
        if(!check(PrepareOwnedActorBasis(a.peer,peer_a,ctx,base)==Result::Applied && PrepareOwnedActorBasis(b.peer,peer_b,ctx,base)==Result::Applied &&
            PrepareOwnedActorBasis(a.peer,peer_b,ctx,base)==Result::InvalidBasis,
            "two owned actors stage immutable views identified by peer/generation/current world tick"))return false;
        const auto incoming=ctx;const auto input_csr=_mm_getcsr();const auto input_error=GetLastError();
        if(!check(PrepareOwnedActorBasis(a.peer,peer_a,ctx,base)==Result::InvalidTick && !std::memcmp(&incoming,&ctx,sizeof(ctx)) &&
            _mm_getcsr()==input_csr && GetLastError()==input_error,"staging cannot mutate an already admitted basis or caller context/FP/LastError"))return false;
        if(!check(update(a) && update(b) && basis_value(0x35000)==peer_a.forward && basis_value(0x36000)==peer_b.forward,
            "actual native controller callsites deliver independent staged bases through original Motion setter"))return false;
        if(!check(update(a) && basis_value(0x35000)==std::array<float,3>{0,0,1} && basis_value(0x36000)==peer_b.forward,
            "consumed tick cannot reapply a peer basis; exact native retained-reference fallback remains independent"))return false;
        // New tick invalidates every previous stage. Remote input is still
        // gated, so this fallback is a native behavior probe, not stale-input
        // gameplay admission.
        if(!check(BeginOwnedActorBasisTick(registry,11,ctx,base) && PrepareOwnedActorBasis(a.peer,peer_a,ctx,base)==Result::InvalidTick &&
            update(b) && basis_value(0x36000)==std::array<float,3>{0,0,1},"new simulation tick invalidates stale staged bases"))return false;
        peer_a.tick=11;peer_b.tick=11;
        auto rejects_context=[&](uint32_t at,uint32_t bits,bool byte,Result expected){const auto prior=byte?uint32_t(PPC_LOAD_U8(at)):PPC_LOAD_U32(at);
            if(byte)PPC_STORE_U8(at,uint8_t(bits));else PPC_STORE_U32(at,bits);
            const auto result=PrepareOwnedActorBasis(a.peer,peer_a,ctx,base);
            if(byte)PPC_STORE_U8(at,uint8_t(prior));else PPC_STORE_U32(at,prior);return result==expected;};
        if(!check(rejects_context(a.controller+257,1,true,Result::InvalidContext) && rejects_context(a.controller+308,123,false,Result::InvalidContext) &&
            rejects_context(a.controller+313,1,true,Result::InvalidContext),"captured camera transitions and alternate/scripted references cannot admit peer-basis replacement"))return false;
        if(!check(rejects_context(a.controller+68,0x80,true,Result::InvalidContext) && rejects_context(0x26000+102,1,true,Result::InvalidContext) &&
            rejects_context(manager+357,0,true,Result::InvalidContext),"disabled component, pause and unready world reject basis staging"))return false;
        if(!check(rejects_context(a.controller+96,0,false,Result::ReferenceRequired),"missing retained native reference is not fabricated"))return false;
        auto invalid=peer_a;invalid.generation++;
        if(!check(PrepareOwnedActorBasis(a.peer,invalid,ctx,base)==Result::InvalidBasis &&
            rejects_context(0x833DA7E4,registry+4,false,Result::Stale),"actor-generation and world-registry changes reject old basis ownership"))return false;
        if(!check(PrepareOwnedActorBasis(a.peer,peer_a,ctx,base)==Result::Applied,"valid native state can stage after rejected transitions without weakening guards"))return false;
        {
            ContextScope caller(ctx);ctx.r3.u64=a.controller+24;
            UpdateBasisController(ctx,base,[](PPCContext& call,uint8_t* memory){
                auto* base=memory; // PPC guest-load/store macros use this local name.
                if(!basis_update)throw std::logic_error("guard fixture missing owned update scope");
                const auto entry=basis_update->entry;
                const auto argument=call.r1.u32+96;std::array<uint8_t,32> saved;
                std::memcpy(saved.data(),memory+argument,saved.size());
                struct ScratchRestore {uint8_t* destination;const std::array<uint8_t,32>& saved;~ScratchRestore(){std::memcpy(destination,saved.data(),saved.size());}}scratch{memory+argument,saved};
                for(unsigned offset:{0u,16u}){PPC_STORE_U32(argument+offset,0);PPC_STORE_U32(argument+offset+4,0);PPC_STORE_U32(argument+offset+8,std::bit_cast<uint32_t>(1.f));PPC_STORE_U32(argument+offset+12,0);}
                const auto caller_state=call;unsigned passed=0;
                for(unsigned variant=0;variant<5;++variant){
                    call=caller_state;call.r27.u64=entry.actor.controller+24;call.r3.u64=entry.motion;call.r31=call.r1;call.r6.u64=argument;call.lr=0x827837E8;
                    if(variant==0)call.lr+=4;
                    if(variant==1)call.r27.u64+=4;
                    if(variant==2)call.r3.u64+=4096;
                    if(variant==3)call.r31.u64+=16;
                    if(variant==4)call.r6.u64+=16;
                    ApplyBasisMotion(call,memory,FixtureInspectBasisArgument);passed+=call.r3.u32==0;
                }
                call=caller_state;call.r3.u64=passed;
            });
            if(!check(ctx.r3.u32==5,"wrong native caller LR/component/motion/frame/vector identities preserve exact original basis arguments"))return false;
        }
        // The native stack vector must be restored even if Motion throws.
        // Calling the shared controller wrapper with a bounded fixture body
        // allows a deliberate native Motion exception without a physics body.
        const auto preserved_input=ctx;ctx.r3.u64=a.controller+24;
        bool thrown=false;
        try{UpdateBasisController(ctx,base,[](PPCContext& call,uint8_t* memory){
            auto* base=memory;
            if(!basis_update)throw std::logic_error("missing fixture basis scope");
            const auto entry=basis_update->entry;
            // Recursive native controller calls cannot inherit a peer scope.
            auto nested=call;UpdateBasisController(nested,memory,[](PPCContext&,uint8_t*){
                if(basis_update)throw std::logic_error("nested controller borrowed peer basis");});
            ContextScope restore(call);
            call.r27.u64=entry.actor.controller+24;call.r3.u64=entry.motion;call.r31=call.r1;call.r6.u64=call.r1.u32+96;call.lr=0x827837E8;
            const auto argument=call.r6.u32;std::array<uint8_t,16> previous;
            std::memcpy(previous.data(),memory+argument,previous.size());
            struct StackRestore {uint8_t* destination;const std::array<uint8_t,16>& saved;~StackRestore(){std::memcpy(destination,saved.data(),saved.size());}}stack_restore{memory+argument,previous};
            PPC_STORE_U32(argument,0);PPC_STORE_U32(argument+4,0);PPC_STORE_U32(argument+8,std::bit_cast<uint32_t>(1.f));PPC_STORE_U32(argument+12,0);
            try{ApplyBasisMotion(call,memory,[](PPCContext& native,uint8_t* memory){
                auto* base=memory;
                if(PPC_LOAD_U32(native.r6.u32)!=std::bit_cast<uint32_t>(1.f) || PPC_LOAD_U32(native.r6.u32+4) || PPC_LOAD_U32(native.r6.u32+8))
                    throw std::logic_error("native Motion never received scoped peer basis");
                throw std::runtime_error("basis motion unwind");});}
            catch(const std::runtime_error&){if(PPC_LOAD_U32(argument) || PPC_LOAD_U32(argument+4) || PPC_LOAD_U32(argument+8)!=std::bit_cast<uint32_t>(1.f) || PPC_LOAD_U32(argument+12))
                throw std::logic_error("native stack basis was not restored");throw;}
        });}
        catch(const std::runtime_error&){thrown=true;}
        ctx=preserved_input;
        if(!check(thrown && !basis_update,"nested controller suspension and native Motion exception restore scoped basis arguments/ownership"))return false;
        // Production admission MUST receive an agreed native input before
        // allowing peer-basis movement. Execute original derived Mickey input,
        // its original base handler, task/FSM gates and retained-reference
        // controller path; no direct action-cache writes authorize movement.
        basis_fixture_without_input=false;
        PPC_LOOKUP_FUNC(base,uint32_t(PPC_CODE_BASE)+24)=FixtureNoActionState;
        PPC_LOOKUP_FUNC(base,uint32_t(PPC_CODE_BASE)+28)=FixtureCurrentActionState;
        PPC_LOOKUP_FUNC(base,uint32_t(PPC_CODE_BASE)+32)=FixtureActionState;
        PPC_STORE_U32(fsm_table+20,uint32_t(PPC_CODE_BASE)+24);
        PPC_STORE_U32(fsm_table+28,uint32_t(PPC_CODE_BASE)+24);
        PPC_STORE_U32(0x833DF47C,0x43000);PPC_STORE_U32(0x43000+48,0x43100);PPC_STORE_U32(0x43100+24,0);
        PPC_STORE_U32(0x44000,0x44100);PPC_STORE_U32(0x44200,0x44100);PPC_STORE_U32(0x44100+40,uint32_t(PPC_CODE_BASE)+32);
        std::array<uint8_t,800> preflight_actor_a,preflight_actor_b;
        std::memcpy(preflight_actor_a.data(),base+a.controller,800);std::memcpy(preflight_actor_b.data(),base+b.controller,800);
        const auto preflight_context=ctx;const auto preflight_tick=basis_clock.tick;
        auto input_claim_count=[](){return std::count_if(basis_entries.begin(),basis_entries.end(),[](const BasisEntry& entry){return entry.input_claimed;});};
        const auto preflight_claims=input_claim_count();
        if(!check(ValidateOwnedActorMovement(a.peer,first,ctx,base)==Result::Applied && ValidateOwnedActorMovement(b.peer,second,ctx,base)==Result::Applied &&
            ValidateOwnedActorMovement(a.peer,first,ctx,base)==Result::Applied && preflight_tick==basis_clock.tick && preflight_claims==input_claim_count() &&
            !std::memcmp(&preflight_context,&ctx,sizeof(ctx)) && !std::memcmp(preflight_actor_a.data(),base+a.controller,800) &&
            !std::memcmp(preflight_actor_b.data(),base+b.controller,800),"all-peer Movement preflight is repeatable and does not stage, claim, deliver input or change native actor state"))return false;
        uint64_t input_tick=100;
        auto stage=[&](const Actor& actor,ActorLookBasis& basis){basis.tick=++input_tick;
            return BeginOwnedActorBasisTick(registry,input_tick,ctx,base) && PrepareOwnedActorBasis(actor.peer,basis,ctx,base)==Result::Applied;};
        auto deliver=[&](const Actor& actor,const NativeActorAction& input){return ReplayOwnedActorMovement(actor.peer,input,input_tick,ctx,base);};
        if(!check(stage(a,peer_a) && update(a) && basis_value(0x35000)==std::array<float,3>{0,0,1} &&
            deliver(a,first)==Result::InvalidTick,"native update without agreed input cannot borrow the staged peer view or accept late input"))return false;
        peer_a.tick=++input_tick;peer_b.tick=input_tick;
        if(!check(BeginOwnedActorBasisTick(registry,input_tick,ctx,base) && PrepareOwnedActorBasis(a.peer,peer_a,ctx,base)==Result::Applied &&
            PrepareOwnedActorBasis(b.peer,peer_b,ctx,base)==Result::Applied,"production input stages two distinct peers in the same agreed tick"))return false;
        const auto production_context=ctx;const auto production_csr=_mm_getcsr();const auto production_error=GetLastError();
        if(!check(deliver(a,first)==Result::Applied && deliver(b,second)==Result::Applied &&
            value(a.listener+168)==first.y && value(a.listener+172)==first.x && value(b.listener+168)==second.y && value(b.listener+172)==second.x &&
            !std::memcmp(&ctx,&production_context,sizeof(ctx)) && _mm_getcsr()==production_csr && GetLastError()==production_error && !replay,
            "ORIGINAL derived Mickey handler delivers opposite peer movement with complete caller/FP/LastError restoration"))return false;
        if(!check(deliver(a,second)==Result::InvalidTick && update(a) && update(b) &&
            basis_value(0x35000)==peer_a.forward && basis_value(0x36000)==peer_b.forward,
            "one native input claim per peer precedes independent original-controller motion and cannot be overwritten"))return false;
        NativeActorAction neutral{first.event,action,0,0};
        if(!check(stage(a,peer_a) && deliver(a,neutral)==Result::Applied && value(a.listener+168)==0 && value(a.listener+172)==0,
            "explicit agreed neutral uses original derived callback to stop held input on the next tick"))return false;
        NativeActorAction other=first;other.action=0x8100;PPC_STORE_U32(other.action,0x8207F5B0);
        if(!check(stage(a,peer_a) && deliver(a,other)==Result::UnsupportedAction && deliver(a,first)==Result::Applied,
            "unsupported action cannot consume the current Movement-only claim"))return false;
        bool aliases_rejected=true;
        for(unsigned offset:{280u,284u,288u,292u,296u,300u}){
            if(!stage(a,peer_a))return false;const auto previous=PPC_LOAD_U32(a.listener+offset);PPC_STORE_U32(a.listener+offset,PPC_LOAD_U32(action));
            aliases_rejected &= ValidateOwnedActorMovement(a.peer,first,ctx,base)==Result::UnsupportedAction && deliver(a,first)==Result::UnsupportedAction;PPC_STORE_U32(a.listener+offset,previous);
        }
        if(!check(aliases_rejected,"all six authored tool-action UID aliases reject before shared-resource branches"))return false;
        if(!stage(a,peer_a))return false;
        PPC_STORE_U8(0x26000+102,1);
        const auto pause_preflight=ValidateOwnedActorMovement(a.peer,first,ctx,base);
        const auto pause_result=deliver(a,first);PPC_STORE_U8(0x26000+102,0);
        if(!check(pause_preflight==Result::InvalidContext && pause_result==Result::InvalidContext && deliver(a,first)==Result::Applied,"paused world rejects production admission without consuming its pending input"))return false;
        if(!stage(a,peer_a))return false;PPC_STORE_U8(a.controller+700,1);
        const auto force_zero=deliver(a,first);PPC_STORE_U8(a.controller+700,0);
        if(!check(force_zero==Result::Applied && value(a.listener+168)==0 && value(a.listener+172)==0,
            "original Mickey-specific listener700 gate forces native Movement neutral"))return false;
        if(!stage(a,peer_a))return false;PPC_STORE_U8(types[0]+5,2);PPC_STORE_U32(lists[0]+8,0x45008);
        PPC_STORE_U32(0x45000+80,0);PPC_STORE_U8(0x45000+88,0);
        const auto individual_block=deliver(a,first);PPC_STORE_U8(types[0]+5,255);
        if(!check(individual_block==Result::Applied && value(a.listener+168)==0 && value(a.listener+172)==0,
            "original per-entity input blocker preserves native cache instead of bypassing admission"))return false;
        if(!stage(a,peer_a))return false;PPC_STORE_U32(0x43100+24,1);
        const auto special=deliver(a,first);PPC_STORE_U32(0x43100+24,0);
        if(!check(special==Result::Applied && value(a.listener+168)==-first.y && value(a.listener+172)==first.x,
            "original special movement context retains native vertical-axis inversion"))return false;
        if(!stage(a,peer_a))return false;PPC_STORE_U32(a.listener+112,0x44600);PPC_STORE_U32(a.listener+116,1);PPC_STORE_U32(0x44600,0x44200);PPC_STORE_U32(0x44200+16,1);
        const auto consumed=deliver(a,first);PPC_STORE_U32(a.listener+116,0);
        if(!check(consumed==Result::Applied && PPC_LOAD_U32(0x44200+20)==1 && PPC_LOAD_U32(0x44200+24)==action &&
            value(a.listener+168)==0 && value(a.listener+172)==0,"original native task consumption zeros Movement and receives the original action payload"))return false;
        if(!stage(a,peer_a))return false;PPC_STORE_U32(fsm_table+28,uint32_t(PPC_CODE_BASE)+28);
        const auto forwarded=deliver(a,first);PPC_STORE_U32(fsm_table+28,uint32_t(PPC_CODE_BASE)+24);
        if(!check(forwarded==Result::Applied && PPC_LOAD_U32(0x44000+20)==1 && PPC_LOAD_U32(0x44000+24)==action &&
            value(0x44000+28)==first.x && value(0x44000+32)==first.y,"original full68 forwarding delivers movement to its native current FSM state unchanged"))return false;
        if(!stage(a,peer_a))return false;PPC_STORE_U8(a.listener+176,1);
        const auto locked=deliver(a,second);PPC_STORE_U8(a.listener+176,0);
        if(!check(locked==Result::Applied && value(a.listener+168)==first.y && value(a.listener+172)==first.x,
            "original native movement lock blocks new cache values"))return false;
        if(!stage(a,peer_a))return false;PPC_STORE_U32(0x35000+384,9);PPC_STORE_U32(0x35000+100,0);
        const auto denied_motion=deliver(a,second);PPC_STORE_U32(0x35000+384,0);
        if(!check(denied_motion==Result::Applied && value(a.listener+168)==first.y && value(a.listener+172)==first.x,
            "scoped entity query preserves the owned Motion mode9 permission denial"))return false;
        if(!stage(a,peer_a))return false;PPC_STORE_U8(a.controller+68,0x80);
        const auto inactive=deliver(a,first);PPC_STORE_U8(a.controller+68,0);
        if(!check(inactive==Result::InvalidContext,"disabled native component rejects Movement delivery before native callbacks"))return false;
        if(!stage(a,peer_a))return false;PPC_STORE_U32(a.controller+96,0);
        const auto no_reference=deliver(a,first);PPC_STORE_U32(a.controller+96,reference);
        if(!check(no_reference==Result::ReferenceRequired,"production input requires the actual retained native movement reference"))return false;
        if(!stage(a,peer_a))return false;
        if(!check(ReplayOwnedActorMovement(a.peer,first,input_tick-1,ctx,base)==Result::InvalidTick &&
            deliver(a,first)==Result::Applied,"wrong tick cannot consume a current peer's native input claim"))return false;
        if(!stage(a,peer_a))return false;PPC_STORE_U32(a.listener+112,0x44600);PPC_STORE_U32(a.listener+116,1);PPC_STORE_U32(0x44200+16,0);PPC_STORE_U32(0x44200+36,1);
        const auto exception_context=ctx;bool input_thrown=false;
        try{deliver(a,first);}catch(const std::runtime_error&){input_thrown=true;}
        PPC_STORE_U32(0x44200+36,0);PPC_STORE_U32(a.listener+116,0);
        if(!check(input_thrown && !replay && !std::memcmp(&exception_context,&ctx,sizeof(ctx)) && deliver(a,first)==Result::InvalidTick &&
            update(a) && basis_value(0x35000)==std::array<float,3>{0,0,1},
            "derived native input exception restores context/scope and retires both input and prepared motion admission"))return false;
        {
            struct PreserveFixtureStages {BasisClock clock=basis_clock;decltype(basis_entries) entries=basis_entries;
                ~PreserveFixtureStages(){basis_clock=clock;basis_entries=entries;}} preserve_fixture_stages;
            if(!TestOwnedMovementFrames(ctx,base,a.peer,b.peer,FixtureBoundedOwnedController))return false;
        }
        if(!TestOwnedActorDeaths(ctx,base,a.peer,b.peer))return false;
        {
            struct PreserveJumpFixtureStages {BasisClock clock=basis_clock;decltype(basis_entries) entries=basis_entries;
                ~PreserveJumpFixtureStages(){basis_clock=clock;basis_entries=entries;}} preserve_jump_fixture_stages;
            if(!TestOwnedActorJumps(ctx,base,a.peer,b.peer,FixtureBoundedOwnedController))return false;
        }
        {
            struct PreserveProfileStages {BasisClock clock=basis_clock;decltype(basis_entries) entries=basis_entries;
                ~PreserveProfileStages(){basis_clock=clock;basis_entries=entries;}} preserve_profile_stages;
            const auto peer=b.peer,handle=uint64_t(b.handle);
            UnregisterOwnedActorInput(peer);
            Actor rejected;
            if(!check(!RegisterOwnedActorInput(peer,uint32_t(handle),ctx,base,rejected,2) &&
                RegisterOwnedActorInput(peer,uint32_t(handle),ctx,base,b,1) && b.action_user==1,
                "owned actors bind one of two native action profiles independently of room peer IDs"))return false;
            if(!check(ValidateOwnedActorMovement(b.peer,first,ctx,base)==Result::UnsupportedAction,
                "secondary owned profile cannot consume a primary Movement action"))return false;
            const auto old_secondary=PPC_LOAD_U32(0x833DAEC4);PPC_STORE_U32(0x833DAEC4,0x8207F5C4);
            const bool movement_ok=TestOwnedMovementFrames(ctx,base,a.peer,b.peer,FixtureBoundedOwnedController);
            PPC_STORE_U32(0x833DAEC4,old_secondary);
            if(!movement_ok || !TestOwnedActorJumps(ctx,base,a.peer,b.peer,FixtureBoundedOwnedController))return false;
            UnregisterOwnedActorInput(peer);
            if(!RegisterOwnedActorInput(peer,uint32_t(handle),ctx,base,b))return false;
        }
        // Restore initial per-peer cache values through ORIGINAL base callback
        // for the following historical lifecycle assertions, not manual writes.
        if(NativeCallbackFixture(a.peer,first,peer_a,ctx,base,FixtureBase)!=Result::Applied)return false;
        basis_fixture_without_input=true;
        SetOwnedActorInputEnabled(a.peer,false);
        if(!check(PrepareOwnedActorBasis(a.peer,peer_a,ctx,base)==Result::Disabled && update(a) && basis_value(0x35000)==std::array<float,3>{0,0,1},
            "disabling an actor retires its prepared basis and preserves native controller fallback"))return false;
        SetOwnedActorInputEnabled(a.peer,true);
    }
    if(!check(!std::memcmp(retail.data(),base+manager,retail.size()) && PPC_LOAD_U32(0x27000)==5 && PPC_LOAD_U32(0x27004)==5 && PPC_LOAD_U32(0x27008)==0x12340000,
        "retail player records/health/view sentinels remain untouched (no native physics/death/playthrough assertion)"))return false;
    SetOwnedActorInputEnabled(a.peer,false);invoke(a,second);
    if(!check(ReplayOwnedActorAction(a.peer,first,basis_a,ctx,base)==Result::Disabled && value(a.listener+172)==first.x,"disabled actor blocks both broadcast and explicit input"))return false;
    SetOwnedActorInputEnabled(a.peer,true);
    auto invalid=basis_a;invalid.right={0,0,0};if(!check(ReplayOwnedActorAction(a.peer,first,invalid,ctx,base)==Result::InvalidBasis,"degenerate or unrelated peer basis cannot authorize input"))return false;
    invalid=basis_b;if(!check(ReplayOwnedActorAction(a.peer,first,invalid,ctx,base)==Result::InvalidBasis,"peer and actor-generation basis identity cannot cross between actors"))return false;
    {
        Replay outer{a};ReplayScope scope(outer);
        if(!check(NativeCallbackFixture(a.peer,first,basis_a,ctx,base,FixtureBase)==Result::Nested,"nested replay cannot borrow another actor's context"))return false;
        auto foreign=context();foreign.r3.u64=b.listener;
        if(!check(SuppressOwnedActorInput(foreign,base),"nested broadcast to a different registered extra stays quarantined"))return false;
    }
    const auto before=ctx;try{NativeCallbackFixture(a.peer,first,basis_a,ctx,base,ThrowAfterBase);return false;}catch(const std::runtime_error&){}
    if(!check(!replay && !std::memcmp(&before,&ctx,sizeof(ctx)),"native callback exception restores complete caller and peer scope"))return false;
    const auto node=0x7000u;PPC_STORE_U32(node+4,entities[1]);Actor stale;
    if(!check(!OwnedActorInputFor(a.peer,ctx,base,stale) && ReplayOwnedActorAction(a.peer,first,basis_a,ctx,base)==Result::Stale,"reused handle resolving to another entity cannot retain controller ownership"))return false;
    PPC_STORE_U32(node+4,entities[0]);PPC_STORE_U32(0x833DA7E4,registry+4);
    if(!check(!OwnedActorInputFor(a.peer,ctx,base,stale),"world registry replacement invalidates old actor routing"))return false;
    PPC_STORE_U32(0x833DA7E4,registry);UnregisterOwnedActorInput(a.peer);
    if(!check(!OwnedActorInputFor(a.peer,ctx,base,stale),"native actor teardown removes its input ownership"))return false;
    // A retail/unowned native listener takes the exact original path. Its
    // game-profile pause guard makes the real derived callback a bounded probe.
    PPC_STORE_U8(0x26000+102,1);auto native=context();native.r3.u64=0x28000;auto actual=native;
    std::array<uint8_t,4096> original_stack;std::memcpy(original_stack.data(),base+native.r1.u32-4096,original_stack.size());
    __imp__sub_82A3C930(native,base);const auto native_csr=_mm_getcsr();
    std::array<uint8_t,4096> expected_stack;std::memcpy(expected_stack.data(),base+actual.r1.u32-4096,expected_stack.size());
    std::memcpy(base+actual.r1.u32-4096,original_stack.data(),original_stack.size());FixtureMickey(actual,base);
    if(!check(!std::memcmp(&native,&actual,sizeof(native)) && !std::memcmp(base+actual.r1.u32-4096,expected_stack.data(),expected_stack.size()) && _mm_getcsr()==native_csr,
        "unowned/retail derived listener retains exact native context/stack/FP behavior"))return false;
    ResetOwnedActorInputs();
    {
        OwnedActorRoster group;
        if(!check(group.Add(3,300,1,0,ctx,base) && group.Add(16,400,2,1,ctx,base) && group.mask()==0x8004 && group.actors().size()==2,
            "native roster maps lobby peers3/16 to independent real handles and two native profiles"))return false;
        OwnedActorInput first_group,second_group;
        if(!check(OwnedActorInputFor(300,ctx,base,first_group) && OwnedActorInputFor(400,ctx,base,second_group) &&
            !first_group.enabled && !second_group.enabled && !group.Add(3,500,1,0,ctx,base) && !group.Add(4,300,2,0,ctx,base),
            "all newly registered group actors stay disabled; duplicate lobby/actor ownership cannot enter admission"))return false;
        OwnedMovementFrames unconfigured;
        if(!check(!group.Activate(0x0004,1,ctx,base) && !group.BindFrames(unconfigured,ctx,base) &&
            !group.CompleteRetirement(ctx,base) && !group.AbandonDestroyedWorld(ctx,base),
            "incomplete roster masks, unactivated frame binding and live-world abandonment are rejected"))return false;
        if(!check(group.BeginRetirement(ctx,base) && !group.CompleteRetirement(ctx,base) &&
            !SetOwnedActorInputEnabled(300,first_group.generation+1,true) && !UnregisterOwnedActorInput(300,first_group.generation+1),
            "group retirement waits for real native handle removal and rejects a different generation"))return false;
        PPC_STORE_U32(0x833DA7E4,registry+4);
        if(!check(group.AbandonDestroyedWorld(ctx,base) && ForgetOwnedActorJump(300,first_group.generation,ctx,base),
            "world teardown forgets registered ownership and absent Jump history without stale native reference calls"))return false;
        PPC_STORE_U32(0x833DA7E4,registry);
        OwnedActorInput replacement;
        if(!RegisterOwnedActorInput(300,1,ctx,base,replacement))return false;
        if(!check(!SetOwnedActorInputEnabled(300,first_group.generation,false) && !UnregisterOwnedActorInput(300,first_group.generation) &&
            OwnedActorInputFor(300,ctx,base,replacement) && replacement.enabled,
            "late cleanup from the old group cannot disable or unregister a reused native peer"))return false;
    }
    OwnedActorInput after_group;
    if(!check(OwnedActorInputFor(300,ctx,base,after_group) && after_group.enabled,"retired group destruction preserves a later actor generation"))return false;
    {
        const auto old_ready=PPC_LOAD_U8(manager+357),old_pause=PPC_LOAD_U8(0x26000+102);
        struct RestoreReady{uint8_t* base;uint8_t ready,paused;~RestoreReady(){PPC_STORE_U8(manager+357,ready);PPC_STORE_U8(0x26000+102,paused);}}restore_ready{base,old_ready,old_pause};
        PPC_STORE_U8(manager+357,1);PPC_STORE_U8(0x26000+102,0);
        if(!check(BeginOwnedActorBasisTick(registry,500,ctx,base),"clock re-entry fixture has a ready native world and a completed prior clock"))return false;
        OwnedActorInput companion,fresh;
        if(!check(RegisterOwnedActorInput(301,2,ctx,base,companion) &&
            !BeginOwnedActorBasisTick(registry,1,ctx,base),"adding a peer cannot rewind a live group's authoritative clock"))return false;
        if(!check(UnregisterOwnedActorInput(300,after_group.generation) &&
            !BeginOwnedActorBasisTick(registry,1,ctx,base),"retiring one peer preserves the surviving group's clock"))return false;
        if(!check(UnregisterOwnedActorInput(301,companion.generation) &&
            RegisterOwnedActorInput(302,1,ctx,base,fresh) && fresh.generation>companion.generation &&
            BeginOwnedActorBasisTick(registry,1,ctx,base) && !BeginOwnedActorBasisTick(registry,1,ctx,base) &&
            !UnregisterOwnedActorInput(302,after_group.generation),
            "fully retired group permits a fresh session clock, with monotonic actor generations and duplicate/stale rejection"))return false;
    }
    ResetOwnedActorInputs();
    REXLOG_INFO("Owned actor input passed {} native registry/controller/quarantine, original action, camera projection and native motion/basis checks: independent actor samples/views, native world vectors/smoothing/permission flags, tick/generation/context guards, exact native callsite/stack restoration and retail fallback; exact Movement-only original derived dispatch available under staged tick contract; generic actions remain BLOCKED; not sixteen-player gameplay",checks);
    return true;
}
