#include "online_actor_death.h"
#include "native_player_spawn.h"
#include "native_character_physics.h"
#include <bit>
#include <cmath>
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <windows.h>
#include <immintrin.h>
PPC_EXTERN_FUNC(__imp__sub_82A010E8);
PPC_EXTERN_FUNC(__imp__sub_82A38FC8);
PPC_EXTERN_FUNC(__imp__sub_8276CC90);
PPC_EXTERN_FUNC(__imp__sub_82769518);
PPC_EXTERN_FUNC(sub_82A010E8);
namespace {
using Actor=em2::net::OwnedActorInput;
using Ticket=em2::net::OwnedActorDeathTicket;
struct Environment {unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(csr);SetLastError(error);}};
struct Context {PPCContext& ctx;PPCContext saved;Environment environment;explicit Context(PPCContext& value):ctx(value),saved(value){}~Context(){ctx=saved;}};
struct Queue {
    uint32_t registry=0;DWORD owner=0;uint64_t epoch=0,serial=0;bool armed=false;
    std::array<Ticket,16> pending{};
} queue;
std::mutex queue_mutex;
thread_local unsigned native_event_depth=0;
bool Readable(uint32_t address,size_t size,uint8_t* base){return base && address>=4096 && uint64_t(address)+size<=0x100000000ull && em2::GuestReadBatch::Readable(base+address,size);}
bool Owner(){return queue.owner && queue.owner==GetCurrentThreadId();}
bool SameActor(const Actor& a,const Actor& b){return a.peer==b.peer && a.generation==b.generation && a.registry==b.registry && a.handle==b.handle && a.entity==b.entity && a.controller==b.controller && a.listener==b.listener && a.action_user==b.action_user;}
bool SameTicket(const Ticket& a,const Ticket& b){return a.sequence==b.sequence && a.consumer_epoch==b.consumer_epoch && a.actor.enabled==b.actor.enabled && SameActor(a.actor,b.actor);}
bool ReloadParameter(uint32_t parameter,uint8_t* base){
    constexpr char name[]="ReloadCheckpoint";
    // Event61 +16 is a NiFixedString* wrapper, not the character buffer.
    // Original8250D888 dereferences wrapper+0 before comparing characters.
    if(!Readable(parameter,4,base))return false;
    const auto characters=PPC_LOAD_U32(parameter);
    return Readable(characters,sizeof(name),base) && !std::memcmp(base+characters,name,sizeof(name));
}
bool Intercept(PPCContext& ctx,uint8_t* base){
    Environment preserve;std::lock_guard lock(queue_mutex);
    if(!Owner() || !queue.armed || ctx.lr!=0x82A39504 || !base ||
       PPC_LOAD_U32(0x833DA7E4)!=queue.registry || ctx.r26.u32<84 || ctx.r4.u32!=0 || ctx.r6.u32!=1 ||
       !Readable(ctx.r25.u32,20,base) || PPC_LOAD_U16(ctx.r25.u32+4)!=61 ||
       !ReloadParameter(PPC_LOAD_U32(ctx.r25.u32+16),base))return false;
    const auto fade=PPC_LOAD_U32(0x833E1F2C); // Native singleton used by this exact caller.
    if(!fade || ctx.r3.u32!=fade)return false;
    Actor actor;
    if(!em2::net::OwnedActorInputForController(ctx.r26.u32-84,ctx,base,actor) || actor.registry!=queue.registry ||
       actor.listener+4!=ctx.r26.u32 || PPC_LOAD_U32(actor.controller+84)!=0x8211DCC4 ||
       PPC_LOAD_U32(0x8211DCC4)!=0x82A38FC8)return false;
    for(const auto& ticket:queue.pending)if(ticket.sequence && SameActor(ticket.actor,actor))return true;
    const auto free=std::find_if(queue.pending.begin(),queue.pending.end(),[](const Ticket& value){return !value.sequence;});
    if(free==queue.pending.end() || queue.serial==std::numeric_limits<uint64_t>::max())return false;
    *free={queue.epoch,++queue.serial,actor};
    return true;
}
void RouteFade(PPCContext& ctx,uint8_t* base,PPCFunc* original){
    bool intercepted=false;
    {Context preserve(ctx);intercepted=Intercept(ctx,base);}
    if(intercepted){ctx.r3.u64=0;return;}
    original(ctx,base);
}
void NoCurrentState(PPCContext& ctx,uint8_t*){ctx.r3.u64=0;}
void ObserveEventState(PPCContext& ctx,uint8_t* base){PPC_STORE_U32(0x47A00,PPC_LOAD_U32(0x47A00)+1);}
void CurrentEventState(PPCContext& ctx,uint8_t*){ctx.r3.u64=0x47900;}
}
bool em2::net::ArmOwnedActorDeaths(uint32_t registry,uint64_t epoch,uint64_t anchor_peer,PPCContext& ctx,uint8_t* base){
    Context preserve(ctx);std::lock_guard lock(queue_mutex);
    if(native_event_depth || !epoch || !registry || !base || PPC_LOAD_U32(0x833DA7E4)!=registry || (queue.owner && !Owner()))return false;
    Actor anchor;if(!OwnedActorInputFor(anchor_peer,ctx,base,anchor) || anchor.registry!=registry)return false;
    const bool pending=std::any_of(queue.pending.begin(),queue.pending.end(),[](const Ticket& value){return value.sequence!=0;});
    if(pending && (queue.epoch!=epoch || queue.registry!=registry))return false;
    queue.registry=registry;queue.epoch=epoch;queue.owner=GetCurrentThreadId();queue.armed=true;return true;
}
bool em2::net::DisarmOwnedActorDeaths(uint64_t epoch){
    Environment preserve;std::lock_guard lock(queue_mutex);if(native_event_depth || !Owner() || !epoch || epoch!=queue.epoch)return false;queue.armed=false;return true;
}
bool em2::net::PeekOwnedActorDeath(uint64_t epoch,PPCContext& ctx,uint8_t* base,Ticket& output,OwnedActorDeathState& state){
    Context preserve(ctx);std::lock_guard lock(queue_mutex);output={};state=OwnedActorDeathState::Retired;
    if(native_event_depth || !Owner() || !epoch || epoch!=queue.epoch)return false;
    const auto found=std::min_element(queue.pending.begin(),queue.pending.end(),[](const Ticket& a,const Ticket& b){
        if(!a.sequence)return false;if(!b.sequence)return true;return a.sequence<b.sequence;});
    if(found==queue.pending.end() || !found->sequence)return false;
    output=*found;
    if(base && PPC_LOAD_U32(0x833DA7E4)==output.actor.registry){
        Actor current;if(OwnedActorInputForController(output.actor.controller,ctx,base,current) && SameActor(current,output.actor))state=OwnedActorDeathState::Current;
    }
    return true;
}
bool em2::net::ConfirmOwnedActorDeath(uint64_t epoch,const Ticket& ticket){
    Environment preserve;std::lock_guard lock(queue_mutex);if(native_event_depth || !Owner() || !epoch || epoch!=queue.epoch || ticket.consumer_epoch!=epoch)return false;
    for(auto& pending:queue.pending)if(pending.sequence && SameTicket(pending,ticket)){pending={};return true;}return false;
}
// Original respawn 82A37518 resolves a two-slot PlayerManager index, then
// uses it unchecked to read the checkpoint matrix and select character tasks.
// Extra humans have no retail record: the native lookup returns -1, producing
// an invalid/uninitialized matrix and NaN physics. Resolve only this audited
// actor-owned query to its pinned native character profile; no retail record
// or global player identity is installed or changed.
PPC_FUNC_IMPL(sub_8276CC90){
    Actor actor;bool owned=false;
    {
        Context preserve(ctx);
        owned=ctx.lr==0x82A37578 && Readable(0x833DAB60,4,base) &&
            ctx.r3.u32==PPC_LOAD_U32(0x833DAB60) &&
            em2::net::OwnedActorInputForController(ctx.r27.u32,ctx,base,actor) &&
            actor.enabled && actor.entity==ctx.r4.u32 && actor.action_user<2;
    }
    if(owned){
        ctx.r3.u64=actor.action_user;
        REXLOG_INFO("Native extra respawn profile resolved: peer {} generation {} entity {:08X} profile {}; original checkpoint matrix and recovery tasks",actor.peer,actor.generation,actor.entity,actor.action_user);
        return;
    }
    __imp__sub_8276CC90(ctx,base);
}
PPC_FUNC_IMPL(sub_82769518){
    Actor actor;bool owned=false;const auto output=ctx.r5.u32;
    {
        Context preserve(ctx);
        owned=ctx.lr==0x82A375A4 && Readable(0x833DAB60,4,base) &&
            ctx.r3.u32==PPC_LOAD_U32(0x833DAB60) && Readable(output,64,base) &&
            em2::net::OwnedActorInputForController(ctx.r27.u32,ctx,base,actor) &&
            actor.enabled && actor.action_user<2 && ctx.r4.u32==actor.action_user;
    }
    __imp__sub_82769518(ctx,base);
    if(!owned || !ctx.r3.u32)return;
    Context preserve(ctx);
    std::array<float,3> checkpoint;for(unsigned i=0;i<3;++i)checkpoint[i]=std::bit_cast<float>(PPC_LOAD_U32(output+48+i*4));
    if(!std::all_of(checkpoint.begin(),checkpoint.end(),[](float value){return std::isfinite(value);}))return;
    // Keep the authored checkpoint orientation and region, while finding an
    // independent supported position. Spawning directly on the retail actor
    // lets its collision hull push the extra human and corrupt fresh controls.
    const auto place=[&](const std::array<float,3>& preferred){
        std::array<float,3> grounded;std::string why;
        if(!em2::FindNativePlayerSpawn(ctx,base,actor.entity,preferred,grounded,why,0,true))return false;
        for(unsigned i=0;i<3;++i)PPC_STORE_U32(output+48+i*4,std::bit_cast<uint32_t>(grounded[i]));
        REXLOG_INFO("Native extra respawn supported placement: peer {} generation {} position {}/{}/{}; authored checkpoint and native collision query",actor.peer,actor.generation,grounded[0],grounded[1],grounded[2]);
        return true;
    };
    // Prefer an already separated supported point close to this checkpoint.
    // Avoid needlessly moving a stationary dead human onto a different edge.
    // Fallen actors and actors far from the checkpoint take the search below.
    em2::net::GameplayObservation::CharacterPhysics sample;
    if(em2::ReadNativeActorPhysics(base,actor.entity,sample) && sample.active){
        std::array<float,3> current;for(unsigned i=0;i<3;++i)current[i]=std::bit_cast<float>(sample.position[i]);
        if(std::hypot(current[0]-checkpoint[0],current[2]-checkpoint[2])<=4.f &&
           std::abs(current[1]-checkpoint[1])<=1.f && place(current))return;
    }
    for(unsigned ring=1;ring<=4;++ring)for(unsigned point=0;point<24;++point){
        const float angle=(float(point)+float(ring&1)*.5f)*6.28318530718f/24.f;
        const std::array<float,3> preferred{checkpoint[0]+float(ring)*2.f*std::cos(angle),checkpoint[1],checkpoint[2]+float(ring)*2.f*std::sin(angle)};
        if(place(preferred))return;
    }
    REXLOG_WARN("Native extra respawn could not separate a supported checkpoint placement; retaining original native checkpoint");
}
PPC_FUNC_IMPL(sub_82A010E8){RouteFade(ctx,base,__imp__sub_82A010E8);}
// Traversal-depth guard only: original event forwarding and actor animation
// execute exactly once. Lifecycle consumers cannot drain from a native listener.
PPC_FUNC_IMPL(sub_82A38FC8){
    struct Traversal {Traversal(){++native_event_depth;}~Traversal(){--native_event_depth;}} traversal;
    __imp__sub_82A38FC8(ctx,base);
}
bool em2::net::TestOwnedActorDeaths(PPCContext& ctx,uint8_t* base,uint64_t first_peer,uint64_t second_peer){
    Context preserve_test_context(ctx);
    struct Restore {Queue saved=queue;unsigned depth=native_event_depth;~Restore(){queue=saved;native_event_depth=depth;}}restore;
    queue={};Actor a,b;if(!OwnedActorInputFor(first_peer,ctx,base,a) || !OwnedActorInputFor(second_peer,ctx,base,b))return false;
    constexpr uint32_t fade=0x47000,event=0x47500,parameter_wrapper=0x47580,parameter=0x47600,fsm=0x47700,state_vt=0x47800;
    unsigned checks=0;auto check=[&](bool value,const char* why){++checks;if(!value)REXLOG_ERROR("Owned actor death: {}",why);return value;};
    const auto fade_global=PPC_LOAD_U32(0x833E1F2C);
    std::array<uint8_t,800> saved_a,saved_b;std::memcpy(saved_a.data(),base+a.controller,800);std::memcpy(saved_b.data(),base+b.controller,800);
    struct NativeRestore {uint8_t* base;Actor a,b;const std::array<uint8_t,800>& aa;const std::array<uint8_t,800>& bb;uint32_t fade;
        ~NativeRestore(){std::memcpy(base+a.controller,aa.data(),800);std::memcpy(base+b.controller,bb.data(),800);PPC_STORE_U32(0x833E1F2C,fade);}}
        native_restore{base,a,b,saved_a,saved_b,fade_global};
    struct DispatchRestore {
        struct Entry {uint32_t address=0;PPCFunc* function=nullptr;};
        uint8_t* base;std::array<Entry,3> entries{};unsigned count=0;
        ~DispatchRestore(){for(unsigned i=0;i<count;++i)PPC_LOOKUP_FUNC(base,entries[i].address)=entries[i].function;}
    } dispatch_restore{base};
    auto map=[&](uint32_t address,PPCFunc* function){auto* dispatch=&PPC_LOOKUP_FUNC(base,address);const auto page=reinterpret_cast<uintptr_t>(dispatch)&~uintptr_t(4095);
        if(!VirtualAlloc(reinterpret_cast<void*>(page),4096,MEM_COMMIT,PAGE_READWRITE))return false;
        dispatch_restore.entries[dispatch_restore.count++]={address,*dispatch};*dispatch=function;return true;};
    if(!map(uint32_t(PPC_CODE_BASE)+40,NoCurrentState) || !map(uint32_t(PPC_CODE_BASE)+44,CurrentEventState) || !map(uint32_t(PPC_CODE_BASE)+48,ObserveEventState))return false;
    PPC_STORE_U32(fsm+28,uint32_t(PPC_CODE_BASE)+44);PPC_STORE_U32(0x47900,state_vt);PPC_STORE_U32(state_vt+44,uint32_t(PPC_CODE_BASE)+48);
    for(const auto actor:{a,b}){PPC_STORE_U32(actor.controller+84,0x8211DCC4);PPC_STORE_U32(actor.controller+104,fsm);PPC_STORE_U32(actor.controller+172,0);PPC_STORE_U32(actor.controller+176,0);PPC_STORE_U32(actor.controller+192,0);PPC_STORE_U32(actor.controller+196,0);}
    PPC_STORE_U32(0x833E1F2C,fade);PPC_STORE_U8(fade+328,1);PPC_STORE_U8(fade+329,1); // Actual native early-return path, no shared fade writes.
    PPC_STORE_U16(event+4,61);std::memcpy(base+parameter,"ReloadCheckpoint",17);
    PPC_STORE_U32(parameter_wrapper,parameter);PPC_STORE_U32(event+16,parameter_wrapper);
    const auto registry=a.registry;
    auto call=[&](const Actor& actor){Context preserve(ctx);ctx.r3.u64=actor.controller+84;ctx.r4.u64=event;
        sub_82A38FC8(ctx,base);};
    const auto unchanged=ctx;const auto csr=_mm_getcsr();const auto error=GetLastError();
    Ticket ticket;OwnedActorDeathState state;
    call(a);
    if(!check(!PeekOwnedActorDeath(17,ctx,base,ticket,state) && !queue.armed,"unarmed production path leaves original native fade and events untouched"))return false;
    if(!check(!ArmOwnedActorDeaths(registry,0,a.peer,ctx,base) && !ArmOwnedActorDeaths(registry,17,999999,ctx,base) &&
        ArmOwnedActorDeaths(registry,17,a.peer,ctx,base),"lifecycle opt-in requires real registered game-thread anchor and nonzero consumer epoch"))return false;
    const auto fade_flag=PPC_LOAD_U8(a.controller+673);const auto fade_mode=PPC_LOAD_U32(a.controller+676);
    const auto before_events=PPC_LOAD_U32(0x47A00);call(a);call(a);call(b);
    if(!check(PeekOwnedActorDeath(17,ctx,base,ticket,state) && state==OwnedActorDeathState::Current && ticket.actor.peer==a.peer &&
        PPC_LOAD_U32(0x47A00)>before_events && PPC_LOAD_U8(a.controller+673)==fade_flag && PPC_LOAD_U32(a.controller+676)==fade_mode && !std::memcmp(&unchanged,&ctx,sizeof(ctx)) && _mm_getcsr()==csr && GetLastError()==error,
        "ORIGINAL Mickey base/derived event forwarding survives interception and restores context/FP/LastError"))return false;
    const auto first=ticket;
    if(!check(std::count_if(queue.pending.begin(),queue.pending.end(),[](const Ticket& value){return value.sequence!=0;})==2 &&
        PeekOwnedActorDeath(17,ctx,base,ticket,state) && SameTicket(first,ticket),"repeated death notifications deduplicate by peer/generation and remain pending without acknowledgement"))return false;
    if(!check(!ConfirmOwnedActorDeath(18,first) && !ArmOwnedActorDeaths(registry,18,a.peer,ctx,base) && DisarmOwnedActorDeaths(17) &&
        PeekOwnedActorDeath(17,ctx,base,ticket,state),"consumer mismatch rejects and disarming retains deferred lifecycle tickets"))return false;
    if(!check(ConfirmOwnedActorDeath(17,first) && !ConfirmOwnedActorDeath(17,first) && PeekOwnedActorDeath(17,ctx,base,ticket,state) && ticket.actor.peer==b.peer,
        "only matching explicit consumer confirmation drains an exact ticket once"))return false;
    {
        const auto owner=PPC_LOAD_U32(b.controller+64);PPC_STORE_U32(b.controller+64,0);
        const bool retired=PeekOwnedActorDeath(17,ctx,base,ticket,state) && state==OwnedActorDeathState::Retired && ticket.actor.peer==b.peer;
        PPC_STORE_U32(b.controller+64,owner);
        auto forged=ticket;forged.actor.entity+=8;auto forged_enabled=ticket;forged_enabled.actor.enabled=!forged_enabled.actor.enabled;
        auto forged_profile=ticket;forged_profile.actor.action_user^=1;
        if(!check(retired && !ConfirmOwnedActorDeath(17,forged) && !ConfirmOwnedActorDeath(17,forged_enabled) && !ConfirmOwnedActorDeath(17,forged_profile) && PeekOwnedActorDeath(17,ctx,base,ticket,state) && state==OwnedActorDeathState::Current,
            "stale controller ownership is reported retired without dropping or retargeting the pending ticket; forged confirmation rejects"))return false;
    }
    {
        const auto global=PPC_LOAD_U32(0x833DA7E4);PPC_STORE_U32(0x833DA7E4,global+4);
        const bool retired=PeekOwnedActorDeath(17,ctx,base,ticket,state) && state==OwnedActorDeathState::Retired;
        PPC_STORE_U32(0x833DA7E4,global);
        if(!check(retired && ConfirmOwnedActorDeath(17,ticket),"changed world cannot retarget captured death ownership and still permits lifecycle acknowledgement"))return false;
    }
    if(!ArmOwnedActorDeaths(registry,19,a.peer,ctx,base))return false;
    auto candidate=ctx;candidate.lr=0x82A39504;candidate.r26.u64=a.controller+84;candidate.r25.u64=event;candidate.r3.u64=fade;candidate.r4.u64=0;candidate.r6.u64=1;
    const auto valid=candidate;
    auto rejected=[&](PPCContext altered){Context preserve(altered);return !Intercept(altered,base);};
    auto wrong=valid;wrong.lr+=4;bool guarded=rejected(wrong);wrong=valid;wrong.r26.u64+=4;guarded &= rejected(wrong);
    wrong=valid;wrong.r3.u64+=4;guarded &= rejected(wrong);wrong=valid;wrong.r6.u64=0;guarded &= rejected(wrong);
    PPC_STORE_U16(event+4,62);guarded &= rejected(valid);PPC_STORE_U16(event+4,61);
    PPC_STORE_U32(event+16,parameter);guarded &= rejected(valid);PPC_STORE_U32(event+16,parameter_wrapper);
    PPC_STORE_U32(parameter_wrapper,0);guarded &= rejected(valid);PPC_STORE_U32(parameter_wrapper,parameter);
    std::memcpy(base+parameter,"FallRescue",11);guarded &= rejected(valid);std::memcpy(base+parameter,"ReloadCheckpoint",17);
    wrong=valid;wrong.r26.u64=0x28000;guarded &= rejected(wrong);
    if(!check(guarded && !PeekOwnedActorDeath(19,ctx,base,ticket,state),"other callers, event types, fade identities, rescue parameters and unregistered actors retain exact original behavior"))return false;
    const auto saved_owner=PPC_LOAD_U32(a.controller+64);PPC_STORE_U32(a.controller+64,0);
    const bool stale=rejected(valid);PPC_STORE_U32(a.controller+64,saved_owner);
    const auto saved_event_vt=PPC_LOAD_U32(a.controller+84);PPC_STORE_U32(a.controller+84,0);
    const bool replaced_listener=rejected(valid);PPC_STORE_U32(a.controller+84,saved_event_vt);
    if(!check(stale && replaced_listener && !PeekOwnedActorDeath(19,ctx,base,ticket,state),"stale owner or replaced native event listener cannot gain death ownership"))return false;
    std::memcpy(base+parameter,"FallRescue",11);call(a);std::memcpy(base+parameter,"ReloadCheckpoint",17);
    if(!check(!PeekOwnedActorDeath(19,ctx,base,ticket,state),"ORIGINAL FallRescue event executes native fallback and never queues owned reload cleanup"))return false;
    const auto manager=PPC_LOAD_U32(0x833DAB60),retail_handle=PPC_LOAD_U32(manager+20);
    PPC_STORE_U32(manager+20,a.handle);const bool retail_rejected=rejected(valid);call(a);PPC_STORE_U32(manager+20,retail_handle);
    if(!check(retail_rejected && !PeekOwnedActorDeath(19,ctx,base,ticket,state),"retail manager identity takes original native death/fade route even if its controller address was formerly registered"))return false;
    native_event_depth=1;const bool traversal=!PeekOwnedActorDeath(19,ctx,base,ticket,state) && !DisarmOwnedActorDeaths(19);native_event_depth=0;
    if(!check(traversal,"lifecycle draining/rearming is forbidden while native actor event traversal is active"))return false;
    // The original fade itself is exercised with its real early-return guard,
    // comparing native fallback context, rather than a replacement stub.
    auto direct=valid;direct.lr+=4;auto expected=direct;RouteFade(direct,base,__imp__sub_82A010E8);__imp__sub_82A010E8(expected,base);
    if(!check(!std::memcmp(&direct,&expected,sizeof(direct)) && PPC_LOAD_U8(fade+328)==1 && PPC_LOAD_U8(fade+329)==1,
        "unowned fade calls execute the exact original native fallback without global health/fade repair"))return false;
    REXLOG_INFO("Owned actor death passed {} native event/fade and explicit consumer queue checks; no live damage/respawn assertion",checks);
    return true;
}
