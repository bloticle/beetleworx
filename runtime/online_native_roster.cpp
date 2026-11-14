#include "online_native_roster.h"
#include "native_player_spawn.h"
#include "camera_look.h"
#include "controls.h"
#include "online_room.h"
#include "players.h"
#include "allocation_trace.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <optional>
PPC_EXTERN_FUNC(__imp__sub_82732A18);
PPC_EXTERN_FUNC(__imp__sub_8275C160);
PPC_EXTERN_FUNC(__imp__sub_8275C728);
PPC_EXTERN_FUNC(__imp__sub_8275C970);
PPC_EXTERN_FUNC(__imp__sub_8276A828);
PPC_EXTERN_FUNC(__imp__sub_827403B0);
PPC_EXTERN_FUNC(__imp__sub_827CAE68);
PPC_EXTERN_FUNC(__imp__sub_82742F70);
PPC_EXTERN_FUNC(__imp__sub_82744E90);
PPC_EXTERN_FUNC(__imp__sub_82747128);
PPC_EXTERN_FUNC(__imp__sub_8308B3E8);
PPC_EXTERN_FUNC(__imp__sub_826550C8);
PPC_EXTERN_FUNC(__imp__sub_82A109C8);
PPC_EXTERN_FUNC(__imp__sub_82743D18);
PPC_EXTERN_FUNC(__imp__sub_82850D50);
PPC_EXTERN_FUNC(__imp__sub_82A104F8);
PPC_EXTERN_FUNC(__imp__sub_82A166D8);
PPC_EXTERN_FUNC(__imp__sub_829F20E8);
PPC_EXTERN_FUNC(__imp__sub_82A14BF8);
PPC_EXTERN_FUNC(__imp__sub_82783208);
PPC_EXTERN_FUNC(__imp__sub_82851018);
PPC_EXTERN_FUNC(__imp__sub_82850A40);
PPC_EXTERN_FUNC(__imp__sub_82A5C478);
PPC_EXTERN_FUNC(__imp__sub_82A5D278);
namespace em2::net {
namespace {
bool Readable(uint32_t at,size_t size,uint8_t* base){return base && at>=4096 && uint64_t(at)+size<=0x100000000ull && GuestReadBatch::Readable(base+at,size);}
struct Environment {unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(csr);SetLastError(error);}};
struct Calls {
    PPCContext& ctx;PPCContext saved;uint8_t* base;uint32_t stack=0,low=0;
    std::array<uint8_t,32768> scratch{};
    Calls(PPCContext& c,uint8_t* b):ctx(c),saved(c),base(b){const auto top=c.r1.u32&~15u;
        if(top>=65536 && Readable(top-32768,scratch.size(),b)){low=top-32768;stack=top-4096;std::memcpy(scratch.data(),b+low,scratch.size());ctx.r1.u64=stack;}}
    ~Calls(){if(stack)std::memcpy(base+low,scratch.data(),scratch.size());ctx=saved;}
    uint32_t Resolve(uint32_t registry,uint32_t handle){const auto key=stack+512;PPC_STORE_U32(key,handle);
        ctx.r3.u64=registry;ctx.r4.u64=key;__imp__sub_82732A18(ctx,base);return ctx.r3.u32;}
};
}
struct NativeRoomActors::Impl {
    struct Actor {uint8_t peer=0;unsigned character=0;uint32_t handle=0,entity=0;std::array<float,3> spawn{};bool admission_entered=false,destruction_queued=false;
        uint32_t camera=0,output=0,tracker=0;bool camera_constructed=false,camera_destroyed=false,camera_entities_queued=false,look_delivered=false,look_fixed=false;};
    LobbySnapshot roster;OwnedActorRoster bindings;std::vector<Actor> actors;
    std::array<uint32_t,2> retail_handles{},retail_entities{};
    uint32_t thread=0,registry=0,manager=0;uint64_t load=0;uint16_t mask=0;
    bool configured=false,attempted=false,active=false,failed=false,queued=false,retired=false,unloading=false;
    std::string error,placement_wait;
    uint64_t placement_wait_ticks=0;
    bool Fail(std::string why){if(!failed){error=std::move(why);REXLOG_ERROR("Native Room actor admission failed: {}",error);}failed=true;active=false;return false;}
    bool Owner()const{return thread==GetCurrentThreadId();}
    bool SameWorld(uint8_t* base)const{return load && em2::CurrentNativeLoadGeneration()==load && Readable(0x833DA7E4,4,base) && PPC_LOAD_U32(0x833DA7E4)==registry;}
    void Quarantine(){for(const auto& entry:bindings.actors())SetOwnedActorInputEnabled(entry.actor.peer,entry.actor.generation,false);active=false;}
    bool RetailUnchanged(uint8_t* base)const{return SameWorld(base) && Readable(0x833DAB60,4,base) && PPC_LOAD_U32(0x833DAB60)==manager && Readable(manager,640,base) &&
        PPC_LOAD_U32(manager+20)==retail_handles[0] && PPC_LOAD_U32(manager+340)==retail_handles[1];}
};
NativeRoomActors::NativeRoomActors():impl_(std::make_unique<Impl>()){}
NativeRoomActors::~NativeRoomActors()=default;
bool NativeRoomActors::Configure(const LobbySnapshot& roster){
    auto& s=*impl_;if(s.configured || !roster.locked || roster.players.size()<2 || roster.players.size()>16 ||
        roster.capacity<roster.players.size() || roster.capacity>16 || roster.players[0].peer!=1 || roster.players[1].peer!=2 ||
        roster.players[0].character!=LobbyCharacter::Mickey || roster.players[1].character!=LobbyCharacter::Oswald)return false;
    uint16_t mask=0;uint64_t previous=0;
    for(const auto& player:roster.players){if(!player.peer || player.peer>roster.capacity || player.peer<=previous ||
        (player.character!=LobbyCharacter::Mickey && player.character!=LobbyCharacter::Oswald))return false;
        previous=player.peer;if(player.peer>2)mask|=uint16_t(1u<<(player.peer-1));}
    s.roster=roster;s.mask=mask;s.thread=GetCurrentThreadId();s.configured=true;return true;
}
NativeRosterStep NativeRoomActors::Prepare(PPCContext& ctx,uint8_t* base){
    Environment environment;auto& s=*impl_;
    if(!s.configured || !s.Owner() || s.failed || s.retired)return NativeRosterStep::Failed;
    if(!s.mask){s.active=true;return NativeRosterStep::Ready;}
    if(s.attempted){
        if(!s.active || !s.RetailUnchanged(base)){s.Quarantine();if(s.SameWorld(base))s.bindings.BeginRetirement(ctx,base);s.Fail("Native world or retail player identity changed after extra-human admission");return NativeRosterStep::Failed;}
        return NativeRosterStep::Ready;
    }
    if(!em2::NativeCoopReady(base) || !Readable(0x833DA7E4,4,base) || !Readable(0x833DAB60,4,base))return NativeRosterStep::Waiting;
    s.registry=PPC_LOAD_U32(0x833DA7E4);s.manager=PPC_LOAD_U32(0x833DAB60);
    s.load=em2::CurrentNativeLoadGeneration();if(!s.load || s.load==UINT64_MAX)return NativeRosterStep::Waiting;
    if(!Readable(s.registry,228,base) || !Readable(s.manager,640,base))return NativeRosterStep::Waiting;
    Calls calls(ctx,base);if(!calls.stack)return NativeRosterStep::Waiting;
    for(unsigned i=0;i<2;++i){s.retail_handles[i]=PPC_LOAD_U32(s.manager+i*320+20);s.retail_entities[i]=calls.Resolve(s.registry,s.retail_handles[i]);
        if(!s.retail_handles[i] || !Readable(s.retail_entities[i],96,base))return NativeRosterStep::Waiting;}
    if(s.retail_handles[0]==s.retail_handles[1] || s.retail_entities[0]==s.retail_entities[1])return NativeRosterStep::Waiting;
    const auto position=calls.stack+576,rotation=calls.stack+608,key=calls.stack+512;
    ctx.r3.u64=s.retail_entities[0];ctx.r4.u64=position;__imp__sub_8275C160(ctx,base);
    std::array<float,3> origin;for(unsigned i=0;i<3;++i)origin[i]=std::bit_cast<float>(PPC_LOAD_U32(position+i*4));
    const auto w=PPC_LOAD_U32(position+12);
    ctx.r3.u64=s.retail_entities[0];ctx.r4.u64=rotation;ctx.r5.u64=2;__imp__sub_8275C728(ctx,base);
    // Plan every position before allocating. No actor is admitted at a guessed
    // height or overlapping another planned human. All peers use roster order.
    std::vector<Impl::Actor> planned;
    const unsigned rings=std::max(4u,(unsigned(s.roster.players.size()-2)+2u)/2u);
    for(const auto& player:s.roster.players)if(player.peer>2){
        Impl::Actor actor;actor.peer=uint8_t(player.peer);actor.character=player.character==LobbyCharacter::Mickey?0u:1u;
        bool found=false;std::string why;
        for(unsigned ring=1;ring<=rings && !found;++ring)for(unsigned point=0;point<24 && !found;++point){
            const float angle=(float(point)+float(ring&1)*.5f)*6.28318530718f/24.f;
            std::array<float,3> preferred{origin[0]+float(ring)*2.f*std::cos(angle),origin[1],origin[2]+float(ring)*2.f*std::sin(angle)};
            if(std::any_of(planned.begin(),planned.end(),[&](const auto& p){const auto x=p.spawn[0]-preferred[0],z=p.spawn[2]-preferred[2];return x*x+z*z<2.25f;}))continue;
            found=em2::FindNativePlayerSpawn(ctx,base,s.retail_entities[actor.character],preferred,actor.spawn,why,s.retail_entities[0]);
        }
        if(!found){
            // The original entrance can still be dropping/placing the retail
            // characters or publishing collision after co-op becomes available.
            // Nothing has been constructed yet: retain the raw admission gate
            // and retry against the next completed native world, never invent
            // a floor or release unsafe gameplay. Its absolute deadline remains.
            if(s.placement_wait!=why || (++s.placement_wait_ticks%30)==1){
                s.placement_wait=why;
                REXLOG_INFO("Native Room actor placement waiting: origin {}/{}/{}; {}",origin[0],origin[1],origin[2],why);
            }
            return NativeRosterStep::Waiting;
        }
        planned.push_back(actor);
    }
    s.placement_wait.clear();s.attempted=true;
    auto fail=[&](const char* why){s.Fail(why);Retire(ctx,base);return NativeRosterStep::Failed;};
    for(auto actor:planned){
        for(unsigned i=0;i<3;++i)PPC_STORE_U32(position+i*4,std::bit_cast<uint32_t>(actor.spawn[i]));PPC_STORE_U32(position+12,w);
        ctx.r3.u64=s.manager;ctx.r4.u64=actor.character;ctx.r5.u64=0;ctx.r6.u64=position;__imp__sub_8276A828(ctx,base);actor.entity=ctx.r3.u32;
        if(!Readable(actor.entity,96,base))return fail("Human factory returned no usable native entity");
        ctx.r3.u64=actor.entity;ctx.r4.u64=rotation;ctx.f1.f64=0;__imp__sub_8275C970(ctx,base);
        ctx.r3.u64=key;ctx.r4.u64=s.registry;ctx.r5.u64=actor.entity;__imp__sub_827403B0(ctx,base);actor.handle=PPC_LOAD_U32(key);
        const bool unique=actor.handle && actor.handle!=s.retail_handles[0] && actor.handle!=s.retail_handles[1] &&
            actor.entity!=s.retail_entities[0] && actor.entity!=s.retail_entities[1] &&
            std::none_of(s.actors.begin(),s.actors.end(),[&](const auto& other){return other.handle==actor.handle || other.entity==actor.entity;});
        // Never place an aliased retail entity into a native destruction list.
        if(!unique)return fail("Human factory returned a duplicate or retail identity");
        s.actors.push_back(actor);
        if(calls.Resolve(s.registry,actor.handle)!=actor.entity || !s.RetailUnchanged(base) ||
            !s.bindings.Add(actor.peer,0x524f4f4d00000000ull+actor.peer,actor.handle,actor.character,ctx,base))return fail("Could not register and quarantine every fresh native human");
    }
    // Crossing each actor's flag permanently forbids its fresh-owner rollback.
    // Admission can partially activate listeners even if a later step fails.
    for(auto& actor:s.actors){
        ctx.r3.u64=actor.entity-8;ctx.r4.u64=0;__imp__sub_827CAE68(ctx,base);
        actor.admission_entered=true;
        ctx.r3.u64=s.registry;ctx.r4.u64=actor.entity;ctx.r5.u64=0;__imp__sub_82742F70(ctx,base);
        if(!s.RetailUnchanged(base))return fail("Native admission changed an existing retail player");
    }
    for(const auto& actor:s.actors){std::array<float,3> grounded;std::string why;
        if(!em2::FindNativePlayerSpawn(ctx,base,actor.entity,actor.spawn,grounded,why,0,true) || std::abs(grounded[1]-actor.spawn[1])>.1f)
            return fail("Admitted human collision hull does not match its supported spawn");}
    std::vector<uint32_t> cameras;
    for(auto& actor:s.actors){
        // Same allocator/constructor as native829F1DF0, without installing
        // another controller in the two-entry global/PlayerManager arrays.
        ctx.r3.u64=960;ctx.r4.u64=0;__imp__sub_826550C8(ctx,base);actor.camera=ctx.r3.u32;
        if(!Readable(actor.camera,960,base))return fail("Could not allocate an independent native camera controller");
        ctx.r3.u64=actor.camera;ctx.r4.u64=actor.character;__imp__sub_82A109C8(ctx,base);actor.camera_constructed=true;
        if(PPC_LOAD_U32(actor.camera)!=0x82112778)return fail("Native camera constructor returned the wrong interface");
        // Scene manager8290F658 creates DefaultCamera the same way. Calling
        // virtual Init would select its shared retail output; give this
        // controller its own world-owned native camera entity instead.
        const auto zero=calls.stack+1024;for(unsigned i=0;i<4;++i)PPC_STORE_U32(zero+i*4,0);
        ctx.r3.u64=s.registry;ctx.r4.u64=0;ctx.r5.u64=0x820D91A8;ctx.r6.u64=zero;ctx.r7.u64=0;ctx.r8.u64=1;ctx.r9.u64=0;
        __imp__sub_82743D18(ctx,base);const auto root=ctx.r3.u32;
        if(!root || !Readable(root+8,96,base) || PPC_LOAD_U32(root+8)!=0x8208D908)return fail("Native DefaultCamera prefab was unavailable");
        ctx.r3.u64=key;ctx.r4.u64=s.registry;ctx.r5.u64=root+8;__imp__sub_827403B0(ctx,base);const auto output=PPC_LOAD_U32(key);
        if(!output || output==PPC_LOAD_U32(PPC_LOAD_U32(s.manager+28)+20) || output==PPC_LOAD_U32(PPC_LOAD_U32(s.manager+348)+20) ||
            std::any_of(s.actors.begin(),s.actors.end(),[&](const auto& other){return output==other.handle || (other.output && output==other.output) || (other.tracker && output==other.tracker);}))return fail("Independent camera aliased an existing native entity");
        // Only verified fresh outputs belong in our destruction list. In
        // particular, rejecting an alias must never queue a retail camera.
        actor.output=output;
        ctx.r3.u64=actor.camera;ctx.r4.u64=key;__imp__sub_82850D50(ctx,base);
        ctx.r3.u64=actor.camera;ctx.r4.u64=actor.entity;__imp__sub_82A104F8(ctx,base);actor.tracker=PPC_LOAD_U32(actor.camera+564);
        // The remainder of ordinary82A105F8 wires the freshly attached
        // tracker into Normal mode. Its full entry also reselects a retail
        // output and actor, so replay only this native target-initialization
        // subsequence against our independent controller.
        const auto normal=PPC_LOAD_U32(actor.camera+392);
        const auto table=Readable(normal,4,base)?PPC_LOAD_U32(normal):0;
        const auto set_target=Readable(table,68,base)?PPC_LOAD_U32(table+64):0;
        if(set_target<uint32_t(PPC_CODE_BASE) || uint64_t(set_target)>=uint64_t(PPC_CODE_BASE)+PPC_CODE_SIZE)return fail("Native Normal camera target interface is unavailable");
        ctx.r3.u64=normal;ctx.r4.u64=actor.camera+564;PPC_CALL_INDIRECT_FUNC(set_target);
        if(PPC_LOAD_U32(actor.camera+24)==normal)PPC_STORE_U32(actor.camera+560,actor.tracker);
        const auto world=PPC_LOAD_U32(s.registry+228);
        const auto context=Readable(world,8,base)?PPC_LOAD_U32(world+4):0;
        if(!context)return fail("Native camera scene context is unavailable");
        ctx.r3.u64=actor.camera;ctx.r4.u64=context;__imp__sub_82850A40(ctx,base);
        // Ordinary enable finishes by resetting Normal mode against its
        // newly bound tracker. Otherwise the constructor's zero-distance
        // state slowly eases out from inside the actor after joining.
        ctx.r3.u64=normal;__imp__sub_82A5C478(ctx,base);
        ctx.r3.u64=actor.camera+564;__imp__sub_82A166D8(ctx,base);
        if(!Readable(ctx.r3.u32,88,base) || PPC_LOAD_U32(ctx.r3.u32+84)!=actor.handle || !s.RetailUnchanged(base))return fail("Independent native camera target did not bind its own human");
        cameras.push_back(actor.output);
        ctx.r3.u64=actor.camera;__imp__sub_82851018(ctx,base);const auto node=ctx.r3.u32;
        if(!Readable(node,368,base) || PPC_LOAD_U32(node)!=0x8203E610)return fail("Independent DefaultCamera has no native NiCamera output");
        REXLOG_INFO("Native Room independent camera: room peer {} controller {:08X} output {:08X} tracker {:08X} own target {:08X} node {:08X}; native controller and DefaultCamera/MickeyTracker factories; actor entity {:08X}",actor.peer,actor.camera,actor.output,actor.tracker,actor.handle,node,actor.entity);
    }
    if(!s.bindings.Activate(s.mask,cameras,ctx,base))return fail("Could not activate every human input lease and movement reference");
    s.active=true;
    REXLOG_INFO("Native Room extra humans admitted: count {} mask {:04X}; original retail handles unchanged; local views and complete action support separate",s.actors.size(),s.mask);
    return NativeRosterStep::Ready;
}
bool NativeRoomActors::Retire(PPCContext& ctx,uint8_t* base){
    Environment environment;auto& s=*impl_;if(!s.configured || !s.Owner())return false;if(s.retired)return true;
    s.Quarantine();
    if(!s.attempted || s.actors.empty()){s.retired=true;return true;}
    if(!s.SameWorld(base)){if(!s.bindings.actors().empty() && !s.bindings.AbandonDestroyedWorld(ctx,base))return false;s.retired=true;return true;}
    Calls calls(ctx,base);if(!calls.stack)return false;
    if(!s.bindings.actors().empty() && !s.bindings.BeginRetirement(ctx,base))return false;
    for(auto& actor:s.actors){
        if(actor.camera_constructed && !actor.camera_destroyed){actor.camera_destroyed=true;ctx.r3.u64=actor.camera;ctx.r4.u64=1;__imp__sub_829F20E8(ctx,base);}
        if(!actor.camera_entities_queued){actor.camera_entities_queued=true;for(auto handle:{actor.tracker,actor.output})if(handle){const auto entity=calls.Resolve(s.registry,handle);if(entity){ctx.r3.u64=s.registry;ctx.r4.u64=entity;__imp__sub_82744E90(ctx,base);}}}
    }
    if(!s.queued){
        for(size_t n=s.actors.size();n-->0;){auto& actor=s.actors[n];const auto entity=calls.Resolve(s.registry,actor.handle);
            if(!entity)continue;
            if(entity!=actor.entity || entity==s.retail_entities[0] || entity==s.retail_entities[1])return false;
            if(actor.admission_entered){if(!actor.destruction_queued){actor.destruction_queued=true;ctx.r3.u64=s.registry;ctx.r4.u64=entity;__imp__sub_82744E90(ctx,base);}continue;}
            const auto owner=PPC_LOAD_U32(s.registry+152);
            if(!Readable(entity-8,104,base) || PPC_LOAD_U32(entity)!=0x8208D908 || !PPC_LOAD_U32(entity-4) || !Readable(owner,112,base))return false;
            const auto pending=PPC_LOAD_U32(s.registry+192),pending_count=PPC_LOAD_U32(s.registry+196);
            if(pending_count>65536 || (pending_count && !Readable(pending,size_t(pending_count)*4,base)))return false;
            for(unsigned i=0;i<pending_count;++i)if(PPC_LOAD_U32(pending+i*4)==entity)return false;
            const auto children=PPC_LOAD_U32(owner+16),count=PPC_LOAD_U32(owner+24);
            if(!count || count>65536 || !Readable(children,size_t(count)*4,base))return false;
            unsigned index=0,matches=0;for(unsigned i=0;i<count;++i)if(PPC_LOAD_U32(children+i*4)==entity){index=i;++matches;}
            if(matches!=1)return false;
            const auto hold=calls.stack+896;PPC_STORE_U32(hold,0);
            ctx.r3.u64=hold;ctx.r4.u64=entity;__imp__sub_8308B3E8(ctx,base);
            ctx.r3.u64=owner;ctx.r4.u64=index;__imp__sub_82747128(ctx,base);
            ctx.r3.u64=hold;ctx.r4.u64=0;__imp__sub_8308B3E8(ctx,base);
            if(calls.Resolve(s.registry,actor.handle) || PPC_LOAD_U32(owner+24)!=count-1)return false;
        }
        s.queued=true;
    }
    for(const auto& actor:s.actors)if(calls.Resolve(s.registry,actor.handle))return false;
    for(const auto& actor:s.actors)for(auto handle:{actor.tracker,actor.output})if(handle && calls.Resolve(s.registry,handle))return false;
    if(!s.bindings.actors().empty() && !s.bindings.CompleteRetirement(ctx,base))return false;
    s.retired=true;return true;
}
bool NativeRoomActors::PrepareWorldUnload(PPCContext& ctx,uint8_t* base){
    Environment environment;auto& s=*impl_;
    if(!s.configured || !s.Owner() || s.retired || (s.mask && !s.SameWorld(base)))return false;
    s.Quarantine();
    Calls calls(ctx,base);if(s.mask && !calls.stack)return false;
    for(auto& actor:s.actors)if(actor.camera_constructed && !actor.camera_destroyed){
        const auto mode=PPC_LOAD_U32(actor.camera+24);
        if(!mode)continue;
        if(!Readable(mode,4,base))return false;
        const auto table=PPC_LOAD_U32(mode);
        if(!Readable(table,44,base))return false;
        const auto deactivate=PPC_LOAD_U32(table+40);
        if(deactivate<uint32_t(PPC_CODE_BASE) || uint64_t(deactivate)>=uint64_t(PPC_CODE_BASE)+PPC_CODE_SIZE)return false;
        // Native mode deactivation unregisters the mode itself (8299E650).
        // The camera destructor only removes its controller interfaces; it
        // assumes its owner has already exited the active mode.
        ctx.r3.u64=mode;PPC_CALL_INDIRECT_FUNC(deactivate);
        REXLOG_INFO("Native reload camera mode deactivated: peer {} mode {:08X} method {:08X}",actor.peer,mode,deactivate);
    }
    if(!s.bindings.actors().empty() && !s.bindings.ReleaseForWorldUnload(ctx,base))return false;
    s.unloading=true;return true;
}
bool NativeRoomActors::FinishWorldUnload(PPCContext& ctx,uint8_t* base){
    Environment environment;auto& s=*impl_;
    if(!s.Owner() || !s.unloading || s.retired || em2::CurrentNativeLoadGeneration()==s.load)return false;
    // Controllers are host-adapter allocations; their tracked entities and
    // extra characters belong to the original world and its own unloader.
    // Destruct only at the completed native boundary: the load request itself
    // runs inside event dispatch, whose handler iteration must finish first.
    Calls calls(ctx,base);if(s.mask && !calls.stack)return false;
    constexpr uint32_t active_camera=0x833DDA40;
    if(!Readable(active_camera,4,base))return false;
    auto retained_camera=PPC_LOAD_U32(active_camera);
    // BGameCamera's base destructor unconditionally clears the singleton,
    // even when destroying an independent extra human's camera. The engine
    // still pumps its ordinary camera update while the level loader runs.
    // Preserve the live retail camera; never restore a destroyed extra one.
    for(const auto& actor:s.actors)if(retained_camera>=actor.camera && uint64_t(retained_camera)<uint64_t(actor.camera)+960){
        if(!Readable(0x833DAB60,4,base))return false;
        const auto manager=PPC_LOAD_U32(0x833DAB60);if(!Readable(manager,32,base))return false;
        retained_camera=PPC_LOAD_U32(manager+28);break;
    }
    if(retained_camera && !Readable(retained_camera,160,base))return false;
    for(auto& actor:s.actors)if(actor.camera_constructed && !actor.camera_destroyed){
        actor.camera_destroyed=true;ctx.r3.u64=actor.camera;ctx.r4.u64=1;__imp__sub_829F20E8(ctx,base);
        if(!PPC_LOAD_U32(active_camera))PPC_STORE_U32(active_camera,retained_camera);
    }
    s.retired=true;return true;
}
bool NativeRoomActors::BindFrames(OwnedMovementFrames& frames,PPCContext& ctx,uint8_t* base){auto& s=*impl_;return s.Owner() && s.active && (!s.mask || s.bindings.BindFrames(frames,ctx,base));}
uint16_t NativeRoomActors::mask()const{return impl_->mask;}
bool NativeRoomActors::ready()const{return impl_->active && !impl_->failed;}
std::span<const OwnedRosterActor> NativeRoomActors::actors()const{return impl_->bindings.actors();}
const std::string& NativeRoomActors::error()const{return impl_->error;}
uint32_t NativeRoomActors::CameraController(uint64_t peer)const{const auto& s=*impl_;if(!s.Owner() || !s.active)return 0;
    for(const auto& actor:s.actors)if(actor.peer==peer && actor.camera_constructed && !actor.camera_destroyed)return actor.camera;return 0;}
bool NativeRoomActors::OwnsCameraMode(uint32_t mode,uint8_t* base)const{const auto& s=*impl_;if(!s.Owner() || !s.SameWorld(base) || !Readable(mode,24,base))return false;
    const auto controller=PPC_LOAD_U32(mode+20);for(const auto& actor:s.actors)if(actor.camera==controller && actor.camera_constructed && !actor.camera_destroyed)return true;return false;}
bool NativeRoomActors::UpdateCameras(const RoomFrame& frame,PPCContext& ctx,uint8_t* base){Environment environment;auto& s=*impl_;if(!s.Owner() || !s.active || !s.RetailUnchanged(base) || (frame.players_mask&s.mask)!=s.mask)return false;
    const auto arguments=ctx;Calls calls(ctx,base);if(!calls.stack)return false;
    // Prepare every normal-camera action before dispatching any of them.
    struct Look {uint32_t mode=0;em2::NativeCameraLookResult result;em2::NativeCameraLookBinding binding;bool ready=false,fixed=false;};
    std::vector<Look> looks;
    for(const auto& actor:s.actors){Look look;const auto mode=PPC_LOAD_U32(actor.camera+24);const auto& pad=frame.players[actor.peer-1].pad;
        const bool fixed=mode==PPC_LOAD_U32(actor.camera+392) && em2::NativeOwnedCameraLookFixedByCutscene(mode,ctx,base);
        if(fixed!=actor.look_fixed){
            REXLOG_INFO("Native Room owned camera input context: peer {} fixed {}; native CutScene/RotationFixed gate, original camera update retained",actor.peer,fixed);
        }
        if(mode==PPC_LOAD_U32(actor.camera+392) && !fixed){
            em2::NativeCameraLookBinding binding;
            if(em2::NativeOwnedCameraLookBindingFor(mode,ctx,base,binding,actor.character)){
                look.binding=binding;look.mode=mode;look.ready=em2::ReplayNativeOwnedCameraLookAction(binding,{pad.right_x,pad.right_y},ctx,base,look.result);
                if(!look.ready)return false;
            }else if(pad.right_x || pad.right_y || actor.look_delivered){
                em2::TraceNativeOwnedLookDefinitions(mode,ctx,base,actor.character);
                REXLOG_ERROR("Native Room owned look binding unavailable: peer {} mode {:08X} table {:08X} profile {} axes {}/{}",actor.peer,mode,Readable(mode,4,base)?PPC_LOAD_U32(mode):0,actor.character,pad.right_x,pad.right_y);return false;
            }
        }
        look.fixed=fixed;looks.push_back(look);
    }
    for(size_t i=0;i<s.actors.size();++i){auto& actor=s.actors[i];const auto& look=looks[i];
        actor.look_fixed=look.fixed;
        if(look.ready){
            if(!em2::DispatchNativeOwnedCameraLook(look.binding,look.result,ctx,base))return false;
            if(!actor.look_delivered)REXLOG_INFO("Native Room owned look admitted: peer {} mode {:08X} action {:08X} profile {}; original action math and original OnLook",actor.peer,look.mode,look.result.action,actor.character);
            actor.look_delivered=true;
        }
        if(!actor.camera_constructed || actor.camera_destroyed || PPC_LOAD_U32(actor.camera+20)!=actor.output)return false;
        ctx.r3.u64=actor.camera;ctx.r4=arguments.r4;ctx.r5=arguments.r5;ctx.r6=arguments.r6;ctx.f1=arguments.f1;__imp__sub_82A14BF8(ctx,base);}
    return true;}
bool NativeRoomActors::ViewBasis(uint64_t peer,PPCContext& ctx,uint8_t* base,float& x,float& z)const{Environment environment;const auto& s=*impl_;
    if(!s.Owner() || !s.active || !s.RetailUnchanged(base))return false;
    for(const auto& entry:s.bindings.actors())if(entry.room_peer==peer){Calls calls(ctx,base);if(!calls.stack)return false;
        const auto copy=calls.stack+1536;std::memcpy(base+copy,base+entry.actor.controller+16,320);
        ctx.r3.u64=copy;__imp__sub_82783208(ctx,base);if(ctx.r3.u32!=1)return false;
        x=std::bit_cast<float>(PPC_LOAD_U32(copy+256));z=std::bit_cast<float>(PPC_LOAD_U32(copy+264));
        return std::isfinite(x) && std::isfinite(z) && std::abs(double(x)*x+double(z)*z-1)<1e-4;}
    return false;}
}

