#include "em1_preview_runtime.h"
#include "em1_level_entry.h"
#include "demo_setup.h"
#include "em1_model_validation.h"
#include "em1_runtime_validation.h"
#include "em1_native_globals.h"
#include "em1_spherical_lighting.h"
#include "em1_native_effect_trace.h"
#include "em1_single_gsa.h"
#include "em1_havok_collision.h"
#include "em1_havok_collision_world.h"
#include "em1_jigsaw_havok.h"
#include "em1_player_bootstrap.h"
#include "em1_preview_capture.h"
#include "em1_loader_diagnostics.h"
#include "em1_native_preview.h"
#include "em1_vfs.h"
#include "native_engine.h"
#include "game_console.h"
#include "online_runtime.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/kernel/kernel_state.h>
#include <rex/logging.h>
#include <windows.h>
#include <map>
#include <immintrin.h>
#include <cstring>
#include <atomic>

REXCVAR_DEFINE_BOOL(em1_preview_courtyard,false,"Debug",
    "Developer original courtyard entity admission and deferred cleanup test; no player/camera changes");
REXCVAR_DEFINE_BOOL(em1_enter_courtyard,false,"Debug","Enter original courtyard architecture through the native isolated scene loader");
REXCVAR_DEFINE_BOOL(em1_verify_native_globals,false,"Debug","Validate authored definitions in an isolated native global store; active progression unchanged");
REXCVAR_DEFINE_BOOL(em1_verify_native_trigger_activation,false,"Debug","Validate adapted original trigger admission and deferred removal");
REXCVAR_DEFINE_BOOL(em1_verify_native_trigger,false,"Debug","Instantiate original gear trigger without world activation, then release it");
REXCVAR_DECLARE(bool,em1_validate_gear_assets);
REXCVAR_DEFINE_BOOL(em1_verify_native_gear_activation,false,"Debug","Validate full original gear native admission and removal");
REXCVAR_DEFINE_BOOL(em1_verify_gear_unload,false,"Debug","Return private EM1 courtyard to main menu after observing persistent gear pair");
REXCVAR_DEFINE_BOOL(em1_verify_particle_activation,false,"Debug","Admit original courtyard steam actors and verify native level unload");
REXCVAR_DEFINE_BOOL(em1_verify_gear_rotation,false,"Debug","Probe original native gear rotation and stop without gameplay input");
namespace em2::em1 {
namespace {
struct Restore {
    PPCContext& context; PPCContext saved; DWORD error; unsigned csr;
    explicit Restore(PPCContext& c):context(c),saved(c),error(GetLastError()),csr(_mm_getcsr()){}
    ~Restore(){context=saved;SetLastError(error);_mm_setcsr(csr);}
};
enum class Phase { Waiting, Active, Cleanup, Done };
Phase phase=Phase::Waiting;
NativeModelResource model;
NativeArchitectureEntity entity;
unsigned frames=0;
uint64_t world_version=0;
bool created=false;
bool gameplay_loaded=false;
std::atomic<std::shared_ptr<const PreviewLightingSample>> preview_lighting;
}
std::shared_ptr<const PreviewLightingSample> CurrentPreviewLighting(){return preview_lighting.load(std::memory_order_acquire);}
SingleGsaEntry& ActiveSingleGsaEntry(){static SingleGsaEntry entry;return entry;}
bool TryEnterArchitectureLevel(PPCContext& ctx,uint8_t* base,uint32_t app,std::string_view destination) {
    static bool attempted=false;
    auto& entry=ActiveSingleGsaEntry();
    // The title scene initializes the resource/profile systems needed by the
    // courtyard. Redirect the subsequent native gameplay load, never this
    // initial scene (a fresh profile otherwise reaches incomplete resources).
    if(!REXCVAR_GET(em1_enter_courtyard) || attempted || entry.loading ||
       destination.starts_with("mainmenu.") || destination.starts_with("front"))return false;
    attempted=true;
    std::string error;
    auto* kernel=rex::kernel::kernel_state();
    auto source=kernel && kernel->file_system()?MountedRvz(*kernel->file_system()):nullptr;
    if(!source || !PrepareSingleGsaEntry(source,entry,error)) {
        REXLOG_ERROR("[em1] isolated courtyard preparation failed: {}",error);ctx.r3.u64=0;return true;
    }
    // Match the existing native QA redirection: later app initialization reads
    // this same startup name, after the ordinary profile/Continue flow.
    if(entry.scene_name.size()>47){ctx.r3.u64=0;return true;}
    char previous_startup_name[48];
    std::memcpy(previous_startup_name,base+app+0x1067c,sizeof(previous_startup_name));
    std::memcpy(base+app+0x1067c,entry.scene_name.c_str(),entry.scene_name.size()+1);
    const bool loaded=EnterSingleGsa(ctx,base,app,entry,error);
    if(!loaded)std::memcpy(base+app+0x1067c,previous_startup_name,sizeof(previous_startup_name));
    ctx.r3.u64=loaded?1:0;
    REXLOG_INFO("[em1] isolated courtyard entry result={} error='{}'; player/camera acceptance pending",loaded,error);
    return true;
}
void NotifyArchitecturePreviewLevelLoaded(std::string_view name) {
    gameplay_loaded=!name.empty() && !name.starts_with("mainmenu.") && !name.starts_with("front");
}
namespace {
NativeCollisionAttachment courtyard_collision;
NativePlayerBootstrap courtyard_player;
NativeStandingObservation courtyard_standing;
bool courtyard_runtime_failed=false,courtyard_collision_ready=false,courtyard_player_ready=false;
struct CourtyardGears {
    std::array<NativeArchitectureEntity,2> entities;
    std::array<uint32_t,2> source_ids{};
    std::array<std::string,2> source_names;
    NativeModelResource model;
    NativeHavokResource physics;
    bool attempted=false,cleanup=false;
    unsigned observation_ticks=0;
    unsigned rotation_phase=0,rotation_ticks=0;
    std::array<std::array<float,18>,2> rotation_samples{};
} courtyard_gears;
struct CourtyardParticles {
    std::array<NativeModelResource,2> models;
    std::array<NativeArchitectureEntity,8> entities;
    std::array<uint32_t,8> source_ids{};
    unsigned emission_ticks=0,emission_phase=0;
    bool attempted=false,cleanup=false;
} courtyard_particles;
bool ReleaseCourtyardParticles(PPCContext& ctx,uint8_t* base,bool& complete,std::string& error){
    complete=false;bool all=true;
    ResetNativeParticleTrace();
    courtyard_particles.source_ids.fill(0);
    for(auto& actor:courtyard_particles.entities){
        if(!UnbindOriginalScriptEntity(ActiveSingleGsaEntry().script_state,actor.handle)){error="Could not revoke particle script identity";return false;}
        bool done=false;if(!DestroyNativeArchitectureEntity(ctx,base,actor,done,error))return false;
        all=all && done;
    }
    if(!all)return true;
    for(auto& resource:courtyard_particles.models)if(!ReleaseNativeModel(ctx,base,resource,error))return false;
    complete=true;return true;
}
void TickCourtyardParticles(PPCContext& ctx,uint8_t* base,SingleGsaEntry& entry){
    auto& state=courtyard_particles;std::string error;
    if(!state.attempted && REXCVAR_GET(em1_verify_particle_activation)){
        state.attempted=true;
        std::map<uint32_t,unsigned> actors;
        bool ready=true;
        for(const auto& member:entry.scene.group_members){
            const auto& g=member.group;
            int kind=-1;
            if(g=="dbc_scaletest.Steam_BlastGear 01" || g=="dbc_scaletest.Steam_BlastGear 02")kind=0;
            if(g=="dbc_scaletest.Steam_Jet_01 01" || g=="dbc_scaletest.Steam_Jet_01 02")kind=1;
            if(kind>=0){auto [it,inserted]=actors.emplace(member.source_entity,unsigned(kind));if(!inserted && it->second!=unsigned(kind))ready=false;}
        }
        if(actors.size()!=8 || !ready){ready=false;error="Unexpected original steam group membership";}
        constexpr const char* paths[]={"effects/_shared/common_steam_02.nif_wii","effects/_shared/steam_jet_01.nif_wii"};
        for(unsigned i=0;ready && i<2;++i){
            ModelClosure closure;
            ready=ReadModelClosure(entry.model.source,"packfiles/dbc_1st_zonec.pak",paths[i],closure,error) &&
                PublishNativeModel(ctx,base,entry.model.manager,closure,state.models[i],error);
        }
        unsigned slot=0;
        for(const auto& [id,kind]:actors){
            if(!ready)break;
            ArchitectureScene projection;auto& resource=state.models[kind];auto& actor=state.entities[slot++];
            ready=ReadCourtyardArchitectureScene(entry.model.source,resource.native_name,projection,error,{},{},1,id);
            projection.scene=std::move(projection.particle_prototype);
            ready=ready && CreateNativeArchitectureEntity(ctx,base,projection,resource,actor,error,true,NativePrototype::Particle) &&
                ActivateNativeParticle(ctx,base,actor,error);
            if(ready){
                ready=BindOriginalScriptEntity(entry.script_state,actor.handle);
                if(ready)state.source_ids[slot-1]=id;
                else error="Original Lua particle identity registration failed";
            }
            if(ready){
                ready=NativeHandleForAuthoredEntity(id)==actor.handle && AuthoredEntityForNativeHandle(actor.handle)==id;
                if(!ready)error="Particle authored/native identity roundtrip failed";
            }
            REXLOG_INFO("[em1] courtyard steam actor authored={} ready={} handle={} error='{}'",id,ready,actor.handle,error);
        }
        if(ready){
            unsigned memberships=0;
            for(const auto& member:entry.scene.group_members){
                const auto owned=actors.find(member.source_entity);
                if(owned==actors.end())continue;
                const auto handle=NativeHandleForAuthoredEntity(member.source_entity);
                if(!handle || AuthoredEntityForNativeHandle(handle)!=member.source_entity){ready=false;error="Particle group identity unavailable";break;}
                ++memberships;
                REXLOG_INFO("[em1] native steam group identity group='{}' authored={} handle={}",member.group,member.source_entity,handle);
            }
            REXLOG_INFO("[em1] native steam group identity verification={} memberships={}",ready,memberships);
        }
        if(ready){
            for(const auto& actor:state.entities)ready=SetNativeParticleEmission(ctx,base,actor,false,error) && ready;
            if(ready)state.emission_ticks=30;
        }
        state.cleanup=!ready;
        REXLOG_INFO("[em1] courtyard steam admission ready={} actors={}; emitter execution/rendering unverified",ready,slot);
    }
    if(state.emission_ticks && !state.cleanup && --state.emission_ticks==0){
        const auto phase=state.emission_phase;
        const bool running=phase==1 || phase==2;
        bool valid=true;
        REXLOG_INFO("[em1] particle lifecycle sample phase={} running={}",phase,running);
        for(const auto& actor:state.entities)valid=ObserveNativeParticleGraph(ctx,base,actor,running) && valid;
        // Start after baseline, observe at30 and120 ticks, stop, then observe
        // drain after120 and240 more ticks. No direct native update is forced.
        if(phase==0 || phase==2){
            for(const auto* group:{"dbc_scaletest.Steam_BlastGear 01","dbc_scaletest.Steam_BlastGear 02","dbc_scaletest.Steam_Jet_01 01","dbc_scaletest.Steam_Jet_01 02"})
                valid=QueueOriginalParticleGroup(entry.script_state,group,phase==0) && valid;
        }
        ++state.emission_phase;
        if(valid && phase<4)state.emission_ticks=phase==0?30:phase==1?90:120;
        if(!valid)state.cleanup=true;
        REXLOG_INFO("[em1] particle lifecycle sample completed phase={} valid={}; visible output unverified",phase,valid);
    }

    if(state.cleanup){
        bool done=false;
        if(!ReleaseCourtyardParticles(ctx,base,done,error))REXLOG_ERROR("[em1] steam cleanup retained ownership: {}",error);
        if(done)state.cleanup=false;
    }
}
bool ReleaseCourtyardGears(PPCContext& ctx,uint8_t* base,bool& complete,std::string& error){
    complete=false;bool all=true;
    courtyard_gears.source_ids.fill(0);
    for(auto& gear:courtyard_gears.entities){
        if(!UnbindOriginalScriptEntity(ActiveSingleGsaEntry().script_state,gear.handle)){error="Could not revoke native gear script identity";return false;}
        bool done=false;
        if(!DestroyNativeArchitectureEntity(ctx,base,gear,done,error))return false;
        all=all && done;
    }
    if(!all)return true;
    if(!ReleaseNativeHavok(ctx,base,courtyard_gears.physics,error) ||
       !ReleaseNativeModel(ctx,base,courtyard_gears.model,error))return false;
    complete=true;return true;
}

void TickSingleGsaRuntime(PPCContext& ctx,uint8_t* base) {
    auto& entry=ActiveSingleGsaEntry();
    if(!entry.entered || entry.loading || courtyard_runtime_failed){UpdateOriginalScriptPlayer(entry.script_state,0);preview_lighting.store({},std::memory_order_release);return;}
    Restore restore(ctx);
    std::string error;
    if(courtyard_player_ready){
        TickCourtyardParticles(ctx,base,entry);
        auto& gears=courtyard_gears;
        if(!gears.attempted){
            gears.attempted=true;ModelClosure closure;ConvertedConvexBodyResource body;
            auto source=entry.model.source;
            auto pak=PakSource::Open(source,"packfiles/dbc_1st_zonec.pak",error);
            std::vector<uint8_t> original;
            bool ready=pak && ReadModelClosure(source,"packfiles/dbc_1st_zonec.pak",
              "environments/thepark/darkbeautycastle/zones/dbc_zoneb_trebgeara1.nif_wii",closure,error) &&
              pak->ReadOriginal("environments/thepark/darkbeautycastle/zones/dbc_zoneb_trebgeara1_dynamic_hull.hkx_wii",original,error) &&
              ConvertConvexBodyResource(original,body,error) &&
              PublishNativeModel(ctx,base,entry.model.manager,closure,gears.model,error) &&
              PublishNativeHavok(ctx,base,entry.model.manager,source,pak->OriginKey()+":gear-body",body.pack,gears.physics,error);
            const bool probe=REXCVAR_GET(em1_validate_gear_assets);
            const bool activate=!probe || REXCVAR_GET(em1_verify_native_gear_activation);
            for(unsigned i=0;ready && i<gears.entities.size();++i){
                ArchitectureScene prototype;
                ready=ReadCourtyardArchitectureScene(source,entry.model.native_name,prototype,error,gears.model.native_name,gears.physics.owner.native_name,i+1);
                prototype.scene=std::move(prototype.gear_prototype);
                ready=ready && CreateNativeArchitectureEntity(ctx,base,prototype,gears.model,gears.entities[i],error,true,NativePrototype::Gear);
                if(ready && activate)ready=ActivateNativeGear(ctx,base,gears.entities[i],error);
                if(ready && activate){
                    ready=BindOriginalScriptEntity(entry.script_state,gears.entities[i].handle);
                    if(!ready)error="Original Lua native gear identity registration failed";
                    if(ready){
                        gears.source_ids[i]=prototype.gear_source_entity_id;
                        gears.source_names[i]=prototype.gear_source_entity_name;
                        if(!gears.source_ids[i] || (i && gears.source_ids[i]==gears.source_ids[0])){
                            ready=false;error="Duplicate or missing authored gear identity";
                        }
                    }
                    REXLOG_INFO("[em1] original Lua gear identity bound={} handle={}",ready,gears.entities[i].handle);
                }
                REXLOG_INFO("[em1] courtyard gear {} ready={} admitted={} entity={:08X} error='{}'",i+1,ready,gears.entities[i].admitted,gears.entities[i].entity,error);
            }
            gears.cleanup=probe || !ready;gears.observation_ticks=ready && activate?(REXCVAR_GET(em1_verify_particle_activation)?450u:30u):0;
            REXLOG_INFO("[em1] courtyard gear pair ready={} persistent={} model='{}' physics='{}'",ready,ready && !probe,gears.model.native_name,gears.physics.owner.native_name);
        }
        if(gears.observation_ticks){
            if(--gears.observation_ticks==0){
                bool valid=true;std::array<uint32_t,2> bodies{};
                for(unsigned i=0;i<gears.entities.size();++i)valid=ObserveNativeGear(base,gears.entities[i],&bodies[i]) && valid;
                valid=valid && bodies[0]!=bodies[1];
                REXLOG_INFO("[em1] courtyard gear pair world observation={} distinct_bodies={}",valid,bodies[0]!=bodies[1]);
                for(unsigned i=0;i<gears.entities.size();++i){
                    const auto authored=AuthoredEntityForNativeHandle(gears.entities[i].handle);
                    const bool identity=authored && authored==gears.source_ids[i] && NativeHandleForAuthoredEntity(authored)==gears.entities[i].handle;
                    unsigned damage_links=0,tutorial_links=0;bool names=true;
                    for(const auto& link:entry.scene.event_links)if(link.source_entity==authored && link.output=="OnDamage"){
                        ++damage_links;names=names && link.source_name==gears.source_names[i];
                        if(link.input=="DBC_1st_ZoneC_DoorGear")++tutorial_links;
                    }
                    REXLOG_INFO("[em1] gear authored event identity={} authored={} native={} name='{}' damage_links={} tutorial_links={}; routing only, no callback dispatched",identity && names && tutorial_links==1,authored,gears.entities[i].handle,gears.source_names[i],damage_links,tutorial_links);
                }
                if(valid && !gears.cleanup && REXCVAR_GET(em1_verify_gear_rotation)){
                    bool sampled=true;for(unsigned i=0;i<2;++i)sampled=ReadNativeGearRotations(base,gears.entities[i],gears.rotation_samples[i]) && sampled;
                    const bool started=sampled && QueueOriginalCutsceneInput(entry.script_state,true) &&
                        QueueOriginalCutsceneInput(entry.script_state,true) && QueueOriginalCutsceneInput(entry.script_state,false) &&
                        QueueOriginalCutsceneInput(entry.script_state,false) &&
                        (!REXCVAR_GET(em1_verify_gear_unload) || QueueOriginalCutsceneInput(entry.script_state,true)) && QueueOriginalGearRotation(entry.script_state,gears.entities[0].handle,360);
                    REXLOG_INFO("[em1] native gear rotation probe started={} error='{}'",started,error);
                    if(started){gears.rotation_phase=1;gears.rotation_ticks=30;}
                }else if(valid && !gears.cleanup && REXCVAR_GET(em1_verify_gear_unload))
                    REXLOG_INFO("[em1] gear unload probe queued: {}",em2::QueueGameLua("CleanLoadLevel('mainmenu.start')",true));
            }
        }else if(gears.cleanup){
            bool complete=false;
            if(!ReleaseCourtyardGears(ctx,base,complete,error))REXLOG_ERROR("[em1] courtyard gear release refused: {}",error);
            if(complete){gears.cleanup=false;REXLOG_INFO("[em1] courtyard gear pair cleanup completed");}
        }
        if(gears.rotation_ticks && --gears.rotation_ticks==0){
            std::array<std::array<float,18>,2> current{};bool sampled=true;
            for(unsigned i=0;i<2;++i)sampled=ReadNativeGearRotations(base,gears.entities[i],current[i]) && sampled;
            auto difference=[&](unsigned gear,unsigned start){float largest=0;for(unsigned i=start;i<start+9;++i)largest=std::max(largest,std::abs(current[gear][i]-gears.rotation_samples[gear][i]));return largest;};
            const auto body_change=difference(0,0),model_change=difference(0,9);
            const bool other_stable=difference(1,0)<0.0001f && difference(1,9)<0.0001f;
            if(gears.rotation_phase==1){
                const bool input_cleanup=ReleaseNativeCutsceneInput(ctx,base,entry.script_state);
                const bool reacquired=!REXCVAR_GET(em1_verify_gear_unload) || QueueOriginalCutsceneInput(entry.script_state,true);
                REXLOG_INFO("[em1] native cutscene explicit cleanup={} reacquire_queued={}",input_cleanup,reacquired);
                const bool stopped=input_cleanup && reacquired && QueueOriginalGearRotation(entry.script_state,gears.entities[0].handle,0);
                REXLOG_INFO("[em1] native gear rotation observed={} body_delta={} model_delta={} other_stable={} stop_set={} error='{}'",sampled && body_change>0.05f && model_change>0.05f && other_stable,body_change,model_change,other_stable,stopped,error);
                gears.rotation_samples=current;gears.rotation_phase=2;gears.rotation_ticks=30;
            }else if(gears.rotation_phase==2){
                REXLOG_INFO("[em1] native gear stop settling sampled={} body_delta={} model_delta={} other_stable={}",sampled,body_change,model_change,other_stable);
                gears.rotation_samples=current;gears.rotation_phase=3;gears.rotation_ticks=30;
            }else{
                REXLOG_INFO("[em1] native gear stopped observation={} body_delta={} model_delta={} other_stable={}",sampled && body_change<0.0001f && model_change<0.0001f && other_stable,body_change,model_change,other_stable);
                gears.rotation_phase=4;
                if(REXCVAR_GET(em1_verify_gear_unload))REXLOG_INFO("[em1] gear unload probe queued: {}",em2::QueueGameLua("CleanLoadLevel('mainmenu.start')",true));
            }
        }
        static bool trigger_checked=false,trigger_cleanup=false;
        static unsigned trigger_observation_ticks=0;
        static NativeArchitectureEntity trigger;
        if(!trigger_checked && (REXCVAR_GET(em1_verify_native_trigger) || REXCVAR_GET(em1_verify_native_trigger_activation))){
            trigger_checked=true;ArchitectureScene prototype;
            const bool activate=REXCVAR_GET(em1_verify_native_trigger_activation);
            prototype.scene=activate?entry.scene.trigger_native_prototype:entry.scene.trigger_prototype;
            std::string error;
            const bool created=CreateNativeArchitectureEntity(ctx,base,prototype,entry.model,trigger,error,true);
            REXLOG_INFO("[em1] native trigger prototype instantiated={} entity={:08X} error='{}'",created,trigger.entity,error);
            bool admitted=false;
            if(created && activate){admitted=ActivateNativeTrigger(ctx,base,trigger,error);REXLOG_INFO("[em1] native trigger admission={} error='{}'",admitted,error);}
            trigger_observation_ticks=admitted?30:0;trigger_cleanup=true;
        }
        if(trigger_cleanup){
            if(trigger_observation_ticks){
                if(--trigger_observation_ticks==0)ObserveNativeTrigger(base,trigger);
            }else{
                bool released=false;std::string error;
                if(!DestroyNativeArchitectureEntity(ctx,base,trigger,released,error))REXLOG_ERROR("[em1] native trigger release failed: {}",error);
                if(released){trigger_cleanup=false;REXLOG_INFO("[em1] native trigger prototype released");}
            }
        }
        static bool globals_checked=false;
        if(!globals_checked && REXCVAR_GET(em1_verify_native_globals)){globals_checked=true;VerifyNativeGlobalDefinitions(ctx,base,entry.script_state);}
        try {
            uint32_t actor=0,script_identity=0;LightVector position{};
            if(SampleNativeCourtyardLightPosition(ctx,base,actor,position,&script_identity)) {
                UpdateOriginalScriptPlayer(entry.script_state,script_identity);
                const auto previous=CurrentPreviewLighting();
                if(!previous || previous->entity!=actor || previous->position!=position) {
                    auto sample=std::make_shared<PreviewLightingSample>();sample->entity=actor;sample->position=position;
                    // EM1's evaluator flips view X/Z. Cancel those flips for
                    // EM2's WorldInverseTranspose-normal material basis.
                    const LightRotation world_basis{{{-1,0,0},{0,1,0},{0,0,-1}}};
                    sample->coefficients=EvaluateAuthoredLighting(entry.scene.runtime_lights,position,world_basis);
                    preview_lighting.store(std::move(sample),std::memory_order_release);
                }
            } else {UpdateOriginalScriptPlayer(entry.script_state,0);preview_lighting.store({},std::memory_order_release);}
        }catch(const std::exception&){UpdateOriginalScriptPlayer(entry.script_state,0);preview_lighting.store({},std::memory_order_release);}
        if(!courtyard_standing.complete){
            if(!ObserveNativeCourtyardStanding(ctx,base,courtyard_player,
                courtyard_standing.samples,courtyard_standing,error))
                REXLOG_WARN("[em1] standing observation unavailable: {}",error);
            if(courtyard_standing.complete){
                ObserveArchitectureCameraSnapshot(ctx,base,courtyard_player.camera,"standing-observation-complete");ObserveEm1FinalMaterialFlags(base);
                // Read-only first integration check. Do not send EM1's basis to
                // EM2 material constants until their spaces/layouts are matched.
                try {
                    const auto index=(courtyard_standing.tail_next+courtyard_standing.tail.size()-1)%courtyard_standing.tail.size();
                    const auto position=courtyard_standing.tail[index].position;
                    const LightRotation identity{{{1,0,0},{0,1,0},{0,0,1}}};
                    const auto coefficients=EvaluateAuthoredLighting(entry.scene.runtime_lights,position,identity);
                    REXLOG_INFO("[em1] authored SH reference sample at native player ({},{},{}) lights={} identity camera; rendering uses the separate world-basis sample",position[0],position[1],position[2],entry.scene.runtime_lights.size());
                    for(unsigned row=0;row<coefficients.size();++row)REXLOG_INFO("[em1] authored SH coefficient {}: {}/{}/{}",row,coefficients[row][0],coefficients[row][1],coefficients[row][2]);
                }catch(const std::exception& failure){REXLOG_WARN("[em1] authored SH observation unavailable: {}",failure.what());}
            }
        }
        return;
    }
    if(!courtyard_collision_ready) {
        const auto world=CurrentNativeCollisionWorld(base);
        if(!world)return;
        auto pak=PakSource::Open(entry.closure.source,"packfiles/dbc_1st_zonec.pak",error);
        std::vector<uint8_t> original;
        AuthoredStaticCollision collision;
        const bool ready=pak && pak->ReadOriginal("environments/thepark/darkbeautycastle/zones/dbc_scaletest2_static_mesh.hkx_wii",original,error) &&
            ReconstructAuthoredStaticCollision(original,collision,error) &&
            ConstructNativeAuthoredCollision(ctx,base,collision,kIdentityCollisionPlacement,courtyard_collision,error) &&
            AdmitNativeAuthoredCollision(ctx,base,world,courtyard_collision,error) &&
            ProbeNativeCollisionFloorRay(ctx,base,courtyard_collision,error);
        if(!ready){courtyard_runtime_failed=true;REXLOG_ERROR("[em1] isolated courtyard collision failed: {}",error);return;}
        courtyard_collision_ready=true;
    }
    const auto result=BootstrapNativeCourtyardPlayer(ctx,base,courtyard_player,error);
    if(result==NativePlayerBootstrapStatus::Failed){courtyard_runtime_failed=true;REXLOG_ERROR("[em1] isolated courtyard player failed: {}",error);}
    if(result==NativePlayerBootstrapStatus::Ready){courtyard_player_ready=true;NotifyArchitecturePreviewCaptureReady();
        ObserveNativeCourtyardStanding(ctx,base,courtyard_player,0,courtyard_standing,error);
        ObserveArchitectureCameraSnapshot(ctx,base,courtyard_player.camera,"bootstrap-ready");
        REXLOG_INFO("[em1] isolated courtyard player/camera registered; standing and visual acceptance pending");}
}
}
bool BeforeArchitectureLevelChange(PPCContext& ctx,uint8_t* base,std::string_view name) {
    auto& entry=ActiveSingleGsaEntry();
    preview_lighting.store({},std::memory_order_release);
    if(!entry.entered || entry.loading || name==entry.scene_name)return true;
    UpdateOriginalScriptPlayer(entry.script_state,0);
    CancelOriginalScripts(entry.script_state);
    if(!ReleaseNativeCutsceneInput(ctx,base,entry.script_state))return false;
    ResetNativeParticleTrace();
    const auto particle_ids=courtyard_particles.source_ids;
    courtyard_particles.source_ids.fill(0);
    bool particles_revoked=true;
    for(unsigned i=0;i<courtyard_particles.entities.size();++i){
        const auto handle=courtyard_particles.entities[i].handle;
        if(!UnbindOriginalScriptEntity(entry.script_state,handle))return false;
        particles_revoked=particles_revoked && !NativeHandleForAuthoredEntity(particle_ids[i]) && !AuthoredEntityForNativeHandle(handle);
    }
    if(courtyard_particles.attempted)REXLOG_INFO("[em1] native steam identities revoked before unload={}",particles_revoked);
    if(!particles_revoked)return false;
    courtyard_gears.source_ids.fill(0);
    for(const auto& gear:courtyard_gears.entities)if(!UnbindOriginalScriptEntity(entry.script_state,gear.handle))return false;
    std::string error;
    if(!ReleaseNativeAuthoredCollision(ctx,base,courtyard_collision,error)) {
        REXLOG_ERROR("[em1] courtyard transition retained physics ownership: {}",error);return false;
    }
    return true;
}
void AfterArchitectureLevelChange(PPCContext& ctx,uint8_t* base,std::string_view name) {
    auto& entry=ActiveSingleGsaEntry();
    if(!entry.entered || entry.loading || name==entry.scene_name)return;
    std::string error;
    bool particles_released=false;
    if(!ReleaseCourtyardParticles(ctx,base,particles_released,error) || !particles_released){
        REXLOG_ERROR("[em1] unloaded courtyard retained particle ownership: {}",error);return;
    }
    courtyard_particles={};
    REXLOG_INFO("[em1] courtyard particle owners released after native level unload");
    bool gears_released=false;
    if(!ReleaseCourtyardGears(ctx,base,gears_released,error) || !gears_released){
        REXLOG_ERROR("[em1] unloaded courtyard retained gear ownership: {}",error);return;
    }
    courtyard_gears={};
    REXLOG_INFO("[em1] courtyard gear owners released after native level unload");
    if(!RetainNativeGlobalScripts(ctx,base,entry.script_state))return;
    if(!ReleaseSingleGsaOwners(ctx,base,entry,error)) {
        REXLOG_ERROR("[em1] unloaded courtyard resource release refused: {}",error);return;
    }
    courtyard_player={};courtyard_standing={};courtyard_runtime_failed=false;courtyard_collision_ready=false;courtyard_player_ready=false;
}
void TickArchitecturePreview(PPCContext& ctx,uint8_t* base) {
    TickSingleGsaRuntime(ctx,base);
    if(gameplay_loaded && em2::NativeConsoleWorldState().ready)MaybeValidateNativeLevel(ctx,base,true);
    if(gameplay_loaded && em2::NativeConsoleWorldState().ready)MaybeValidateNativeModel(ctx,base,true);
    if(phase==Phase::Done || (phase==Phase::Waiting && !REXCVAR_GET(em1_preview_courtyard)))return;
    Restore restore(ctx);
    const auto world=em2::NativeConsoleWorldState();
    std::string error;
    if(phase==Phase::Waiting) {
        if(!gameplay_loaded || !world.ready || em2::net::SynchronizedSessionActive())return;
        phase=Phase::Cleanup; // Even a partially created object must be cleaned up.
        auto* kernel=rex::kernel::kernel_state();
        auto source=kernel && kernel->file_system()?MountedRvz(*kernel->file_system()):nullptr;
        ModelClosure closure;
        if(!source || !ReadModelClosure(source,"packfiles/dbc_1st_zonec.pak",
            "environments/thepark/darkbeautycastle/Zones/DBC_scaletest2.nif_wii",closure,error)) {
            REXLOG_ERROR("[em1] architecture preview source unavailable: {}",error);return;
        }
        const auto manager=PPC_LOAD_U32(0x833da818);
        if(!PublishNativeModel(ctx,base,manager,closure,model,error)) {
            REXLOG_ERROR("[em1] architecture preview resource failed: {}",error);return;
        }
        ArchitectureScene scene;
        if(!ReadCourtyardArchitectureScene(source,model.native_name,scene,error) ||
           !CreateNativeArchitectureEntity(ctx,base,scene,model,entity,error)) {
            REXLOG_ERROR("[em1] architecture preview admission failed: {}",error);return;
        }
        world_version=world.version;frames=0;created=true;phase=Phase::Active;
        REXLOG_INFO("[em1] authored architecture entity admitted: entity={:08X} scenegraph={:08X}; 180-frame lifecycle test, no collision/player/camera changes",
            entity.entity,entity.scenegraph);
    } else if(phase==Phase::Active) {
        if(++frames>=180 || !REXCVAR_GET(em1_preview_courtyard) || world.version!=world_version || !world.ready)
            phase=Phase::Cleanup;
    }
    if(phase==Phase::Cleanup) {
        bool complete=false;
        if(!DestroyNativeArchitectureEntity(ctx,base,entity,complete,error)) {
            REXLOG_ERROR("[em1] architecture preview cleanup refused: {}; retained ownership for diagnosis",error);
            phase=Phase::Done;return;
        }
        if(!complete)return;
        if(!ReleaseNativeModel(ctx,base,model,error)) {
            REXLOG_ERROR("[em1] architecture preview resource release refused: {}",error);
            phase=Phase::Done;return;
        }
        phase=Phase::Done;
        REXLOG_INFO("[em1] architecture lifecycle completed: admitted={} frames={}; native entity and model released; no visual or gameplay acceptance yet",created,frames);
    }
}
uint32_t NativeHandleForAuthoredEntity(uint32_t authored){
    const auto& entry=ActiveSingleGsaEntry();
    if(!authored || !entry.entered || entry.loading || courtyard_runtime_failed)return 0;
    for(unsigned i=0;i<courtyard_gears.entities.size();++i){
        const auto& gear=courtyard_gears.entities[i];
        if(courtyard_gears.source_ids[i]==authored && gear.admitted && !gear.destroy_requested)return gear.handle;
    }
    for(unsigned i=0;i<courtyard_particles.entities.size();++i){
        const auto& actor=courtyard_particles.entities[i];
        if(courtyard_particles.source_ids[i]==authored && actor.admitted && !actor.destroy_requested)return actor.handle;
    }
    return 0;
}
uint32_t AuthoredEntityForNativeHandle(uint32_t handle){
    const auto& entry=ActiveSingleGsaEntry();
    if(!handle || !entry.entered || entry.loading || courtyard_runtime_failed)return 0;
    for(unsigned i=0;i<courtyard_gears.entities.size();++i){
        const auto& gear=courtyard_gears.entities[i];
        if(gear.handle==handle && gear.admitted && !gear.destroy_requested)return courtyard_gears.source_ids[i];
    }
    for(unsigned i=0;i<courtyard_particles.entities.size();++i){
        const auto& actor=courtyard_particles.entities[i];
        if(actor.handle==handle && actor.admitted && !actor.destroy_requested)return courtyard_particles.source_ids[i];
    }
    return 0;
}
bool SetOwnedArchitectureParticleEmission(PPCContext& ctx,uint8_t* base,uint32_t handle,bool enabled){
    const auto& entry=ActiveSingleGsaEntry();
    if(!entry.entered || entry.loading || courtyard_runtime_failed || !handle)return false;
    for(unsigned i=0;i<courtyard_particles.entities.size();++i){
        const auto& actor=courtyard_particles.entities[i];
        if(courtyard_particles.source_ids[i] && actor.handle==handle && actor.admitted && !actor.destroy_requested){
            std::string error;return SetNativeParticleEmission(ctx,base,actor,enabled,error);
        }
    }
    return false;
}
bool SetOwnedArchitectureGearSpeed(PPCContext& ctx,uint8_t* base,uint32_t handle,float speed){
    auto& entry=ActiveSingleGsaEntry();
    if(!entry.entered || entry.loading || courtyard_runtime_failed || !handle)return false;
    for(const auto& gear:courtyard_gears.entities)if(gear.handle==handle && gear.admitted && !gear.destroy_requested){
        std::string error;return SetNativeGearRotationSpeed(ctx,base,gear,speed,error);
    }
    return false;
}
bool IsActiveArchitectureScriptState(const NativeScriptState& state){
    const auto& entry=ActiveSingleGsaEntry();
    return &entry.script_state==&state && entry.entered && !entry.loading && !courtyard_runtime_failed && courtyard_player_ready;
}
void TickArchitectureScripts(PPCContext& ctx,uint8_t* base,double simulation_delta) {
    TickRegisteredLevelScripts(ctx,base,simulation_delta);
    auto& entry=ActiveSingleGsaEntry();
    if(!entry.entered || entry.loading || courtyard_runtime_failed || !courtyard_player_ready)return;
    struct FloatingState {unsigned saved=_mm_getcsr();~FloatingState(){_mm_setcsr(saved);}} floating_state;
    TickNativeGlobalScripts(ctx,base,entry.script_state,simulation_delta);
}
}
