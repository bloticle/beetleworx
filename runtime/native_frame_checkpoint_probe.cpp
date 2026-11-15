#include "native_frame_checkpoint_probe.h"
#include "native_retained_script_checkpoint.h"
#include "native_simulation_clock_checkpoint.h"
#include "native_gameplay_component_checkpoint.h"
#include "allocation_trace.h"
#include "native_entity_probe.h"
#include "online_runtime.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <windows.h>
#include <memory>
#include <array>
#include <algorithm>
#include <map>
#include <tuple>
#include <sstream>
#include <immintrin.h>
REXCVAR_DEFINE_BOOL(em2_probe_native_frame_checkpoint,false,"Debug","Private one-real-update native component restore; exits before partial simulation can resume; not full rollback");
REXCVAR_DEFINE_BOOL(em2_probe_native_frame_scripts,false,"Debug","With private frame checkpoint, retain trusted Lua helpers and restore live scripts across one real update; not full rollback");
REXCVAR_DEFINE_BOOL(em2_probe_native_frame_replay,false,"Debug","Private one-update replay comparison; incomplete state and side-effect coverage, never online rollback activation");
REXCVAR_DECLARE(bool,em2_private_profile);
namespace em2 {namespace {
thread_local std::unique_ptr<NativeGameplayComponentCheckpoint> pending,expected_replay;
thread_local bool replay_running=false;
struct PhaseSample {unsigned phase=0;std::vector<NativePhysicsMotionSample> bodies;};
thread_local std::vector<PhaseSample> physics_phases;
thread_local size_t physics_phase_index=0;
thread_local std::string physics_phase_error;
struct PoseSample{uint32_t object=0;unsigned phase=0,word_count=0;uint32_t parent_entity=0,parent_transform=0,parent_flags=0;std::array<uint32_t,16> parent_world{};std::array<uint32_t,106> words{};};
thread_local std::vector<PoseSample> pose_samples;
thread_local size_t pose_replay_index=0;
thread_local unsigned pose_changed=0,pose_order_changed=0,pose_unexpected=0;
thread_local size_t pose_used=0;
using PoseKey=std::pair<uint32_t,unsigned>;
thread_local std::map<PoseKey,std::vector<size_t>> pose_groups;
thread_local std::map<PoseKey,size_t> pose_group_indices;
struct TranslationWrite{uint32_t object=0,caller=0;unsigned offset=0;std::array<uint32_t,3> target{},previous{};std::array<uint32_t,6> parents{};};
thread_local std::vector<TranslationWrite> translation_writes;
thread_local size_t translation_replay_index=0;
thread_local unsigned translation_changed=0,translation_order_changed=0,translation_unexpected=0;
thread_local size_t translation_used=0;
using TranslationKey=std::tuple<uint32_t,uint32_t,unsigned>;
thread_local std::map<TranslationKey,std::vector<size_t>> translation_groups;
thread_local std::map<TranslationKey,size_t> translation_group_indices;
struct BodyCall {uint32_t body=0,caller=0,method=0;uint64_t scalar=0;std::array<uint32_t,8> arguments{};std::array<uint32_t,6> parents{};std::array<uint32_t,3> velocity{};std::array<uint32_t,6> userdata{};uint32_t source_table=0;std::array<uint32_t,16> source_pose{};};
thread_local std::vector<BodyCall> original_body_calls,replay_body_calls;
void DescribeBodyCalls(uint32_t body,const std::vector<BodyCall>& calls,const char* branch){
    unsigned count=0;for(const auto& call:calls)if(call.body==body){
        if(++count>12)break;
        std::ostringstream text;text<<std::hex<<call.caller<<" parents";for(auto pc:call.parents)text<<" "<<pc;
        text<<" velocity";for(auto word:call.velocity)text<<" "<<word;
        text<<" method "<<call.method<<" scalar "<<call.scalar<<" arguments";for(auto word:call.arguments)text<<" "<<word;
        text<<" userdata";for(auto word:call.userdata)text<<" "<<word;
        text<<" source class "<<call.source_table<<" pose";for(auto word:call.source_pose)text<<" "<<word;
        REXLOG_INFO("Private replay body driver {} {:08X} call {}: {}",branch,body,count,text.str());
    }
    if(!count)REXLOG_INFO("Private replay body driver {} {:08X}: no activation calls observed",branch,body);
}
thread_local std::unique_ptr<NativeRetainedScriptCheckpoint> scripts;
thread_local uint64_t epoch=0,tick=0,ready_epoch=0;
thread_local unsigned ready_frames=0;
bool Enabled(){return REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_probe_native_frame_checkpoint);}
[[noreturn]]void Finish(PPCContext& ctx,uint8_t* base,bool success,const std::string& error){
    const bool restored_scripts=scripts && REXCVAR_GET(em2_probe_native_frame_scripts);
    if(scripts && scripts->Faulted(base)){REXLOG_ERROR("Private native frame script VM faulted; no native cleanup or simulation resumed");spdlog::shutdown();ExitProcess(1);}
    if(scripts && !scripts->Release(ctx,base)){success=false;REXLOG_ERROR("Private native frame script roots did not release");}
    scripts.reset();
    if(expected_replay && !expected_replay->Release(ctx,base)){success=false;REXLOG_ERROR("Private replay expectation references did not release");}
    expected_replay.reset();
    if(pending && !pending->Release(ctx,base)){success=false;REXLOG_ERROR("Private native frame checkpoint reference release failed");}
    pending.reset();
    if(success)REXLOG_INFO("Private native frame component checkpoint complete: restored after one real update; component payloads/clocks/ownership checked; scripts restored {}, hidden state not fully covered, replay diagnostic {}",restored_scripts,replay_running);
    else REXLOG_ERROR("Private native frame component checkpoint failed: {}; no simulation resumed",error);
    spdlog::shutdown();ExitProcess(success?0:1);
}
}
void ObservePrivateNativeReplayTranslation(PPCContext& ctx,uint8_t* base,unsigned offset){
    if(!pending || (!replay_running && expected_replay) || !Enabled() || !REXCVAR_GET(em2_probe_native_frame_replay))return;
    struct Host{DWORD error=GetLastError();unsigned csr=_mm_getcsr();~Host(){SetLastError(error);_mm_setcsr(csr);}}host;
    GuestReadBatch reads;
    auto readable=[&](uint32_t at,size_t size){return at>=4096 && uint64_t(at)+size<=0x100000000ull && GuestReadBatch::Readable(base+at,size);};
    TranslationWrite now;now.object=ctx.r3.u32;now.caller=uint32_t(ctx.lr);now.offset=offset;
    if((offset!=72 && offset!=172) || !readable(now.object,offset+12) || !readable(ctx.r4.u32,12)){physics_phase_error="Private translation trace extent unavailable";return;}
    for(unsigned i=0;i<3;++i){now.target[i]=PPC_LOAD_U32(ctx.r4.u32+i*4);now.previous[i]=PPC_LOAD_U32(now.object+offset+i*4);}
    auto frame=ctx.r1.u32;
    for(auto& pc:now.parents){if((frame&15) || !readable(frame,4))break;const auto parent=PPC_LOAD_U32(frame);if(parent<=frame || uint64_t(parent)-frame>1024*1024 || !readable(parent-8,4))break;pc=PPC_LOAD_U32(parent-8);frame=parent;}
    const TranslationKey key{now.object,now.caller,offset};
    if(!replay_running){if(translation_writes.size()>=4096){physics_phase_error="Private translation trace bound exceeded";return;}translation_groups[key].push_back(translation_writes.size());translation_writes.push_back(now);return;}
    const auto ordinal=translation_group_indices[key]++;const auto actual_index=translation_replay_index++;
    const auto group=translation_groups.find(key);
    if(group==translation_groups.end() || ordinal>=group->second.size()){
        if(++translation_unexpected<=16)REXLOG_INFO("Private replay unexpected translation write {:08X} offset {} caller {:08X}",now.object,offset,now.caller);return;
    }
    const auto expected_index=group->second[ordinal];++translation_used;translation_order_changed+=expected_index!=actual_index;
    const auto& expected=translation_writes[expected_index];
    if(expected.target==now.target && expected.previous==now.previous)return;
    if(++translation_changed>32)return;
    std::ostringstream values;values<<std::hex<<"target";for(unsigned i=0;i<3;++i)values<<" "<<expected.target[i]<<"/"<<now.target[i];values<<" previous";for(unsigned i=0;i<3;++i)values<<" "<<expected.previous[i]<<"/"<<now.previous[i];values<<" parents";for(auto pc:now.parents)values<<" "<<pc;
    REXLOG_INFO("Private replay translation write {:08X} offset {} caller {:08X}: {}",now.object,offset,now.caller,values.str());
}
void ObservePrivateNativeReplayPose(PPCContext& ctx,uint8_t* base,unsigned phase,uint32_t requested_object){
    if(!pending || (!replay_running && expected_replay) || !Enabled() || !REXCVAR_GET(em2_probe_native_frame_replay))return;
    struct Host{DWORD error=GetLastError();unsigned csr=_mm_getcsr();~Host(){SetLastError(error);_mm_setcsr(csr);}}host;
    GuestReadBatch reads;
    const auto object=requested_object?requested_object:ctx.r3.u32;
    const unsigned bytes=phase>=2?424:320;
    if(object<4096 || uint64_t(object)+bytes>0x100000000ull || !GuestReadBatch::Readable(base+object,bytes)){physics_phase_error="Private pose diagnostic extent unavailable";return;}
    PoseSample now;now.object=object;now.phase=phase;now.word_count=bytes/4;for(unsigned i=0;i<now.word_count;++i)now.words[i]=PPC_LOAD_U32(object+i*4);
    if(phase>=2){
        auto readable=[&](uint32_t at,size_t size){return at>=4096 && uint64_t(at)+size<=0x100000000ull && GuestReadBatch::Readable(base+at,size);};
        // Original827D78D0 passes parent primary+8 to entity component lookups.
        const auto primary=now.words[268/4];now.parent_entity=primary && primary<=UINT32_MAX-8?primary+8:0;
        if(readable(now.parent_entity,92) && PPC_LOAD_U32(now.parent_entity)==0x8208D908){
            const auto list=PPC_LOAD_U32(now.parent_entity+36),count=PPC_LOAD_U32(now.parent_entity+44);
            if(count<=255 && (!count || readable(list,count*4)))for(unsigned i=0;i<count;++i){const auto face=PPC_LOAD_U32(list+i*4);
                if(face>=8 && readable(face-8,424) && PPC_LOAD_U32(face)==0x82090340 && PPC_LOAD_U32(face-8)==0x820904D0){
                    now.parent_transform=face-8;unsigned at=0;
                    for(unsigned offset:{172u,176u,180u,208u,212u,216u,220u,224u,228u,232u,236u,240u,244u,248u,252u,260u})now.parent_world[at++]=PPC_LOAD_U32(now.parent_transform+offset);
                    now.parent_flags=uint32_t(PPC_LOAD_U16(now.parent_transform+412))<<8|PPC_LOAD_U8(now.parent_transform+414);break;
                }
            }
        }
    }
    const PoseKey key{object,phase};
    if(!replay_running){if(pose_samples.size()>=2048){physics_phase_error="Private pose diagnostic trace bound exceeded";return;}pose_groups[key].push_back(pose_samples.size());pose_samples.push_back(now);return;}
    const auto ordinal=pose_group_indices[key]++,actual_index=pose_replay_index++;const auto group=pose_groups.find(key);
    if(group==pose_groups.end() || ordinal>=group->second.size()){
        if(++pose_unexpected<=16)REXLOG_INFO("Private replay unexpected pose sample {:08X} phase {}",object,phase);return;
    }
    const auto expected_index=group->second[ordinal];++pose_used;pose_order_changed+=expected_index!=actual_index;
    const auto& expected=pose_samples[expected_index];unsigned changed=0;std::ostringstream delta;
    for(unsigned i=0;i<now.word_count;++i)if(expected.words[i]!=now.words[i]){if(changed++<24)delta<<" +"<<std::dec<<i*4<<":"<<std::hex<<expected.words[i]<<"/"<<now.words[i];}
    pose_changed+=changed!=0;
    if(changed)REXLOG_INFO("Private replay pose entry {:08X} class {:08X} phase {}: {} differing words {}",object,now.words[0],phase,changed,delta.str());
    if(phase>=2 && changed){std::ostringstream source;
        for(unsigned i=0;i<16;++i)source<<" "<<std::hex<<expected.parent_world[i]<<"/"<<now.parent_world[i];
        REXLOG_INFO("Private replay parent source {:08X} phase {}: entity {:08X}/{:08X} transform {:08X}/{:08X} flags {:08X}/{:08X}, world expected/actual {}",object,phase,expected.parent_entity,now.parent_entity,expected.parent_transform,now.parent_transform,expected.parent_flags,now.parent_flags,source.str());
    }
}
void ObservePrivateNativeReplayController(PPCContext& ctx,uint8_t* base){
    if(!pending || (!replay_running && expected_replay) || !Enabled() || !REXCVAR_GET(em2_probe_native_frame_replay))return;
    struct Host{DWORD error=GetLastError();unsigned csr=_mm_getcsr();~Host(){SetLastError(error);_mm_setcsr(csr);}}host;
    GuestReadBatch reads;
    // 8278D4D8 reads through +956. Snapshot words only for diagnostics; no
    // ownership interpretation and no restoring pointer-bearing controller data.
    struct Sample{uint32_t object=0,state=0,table=0,compute=0,update=0;std::array<uint32_t,240> words{};std::array<uint32_t,16> machine{};};
    static thread_local std::vector<Sample> original;
    const auto object=ctx.r3.u32;
    if(object<4096 || uint64_t(object)+960>0x100000000ull || !GuestReadBatch::Readable(base+object,960)){physics_phase_error="Private controller diagnostic extent unavailable";return;}
    Sample now;now.object=object;for(unsigned i=0;i<240;++i)now.words[i]=PPC_LOAD_U32(object+i*4);
    auto readable=[&](uint32_t at,size_t size){return at>=4096 && uint64_t(at)+size<=0x100000000ull && GuestReadBatch::Readable(base+at,size);};
    const auto machine=now.words[92/4];
    if(readable(machine,64)){
        for(unsigned i=0;i<16;++i)now.machine[i]=PPC_LOAD_U32(machine+i*4);
        const auto factory=now.machine[3],state=now.machine[4];
        if(state<=255 && uint64_t(factory)+(state+2)*4<=UINT32_MAX && readable(factory+(state+2)*4,4)){
            now.state=PPC_LOAD_U32(factory+(state+2)*4);
            if(readable(now.state,4)){now.table=PPC_LOAD_U32(now.state);if(readable(now.table,28)){now.compute=PPC_LOAD_U32(now.table+20);now.update=PPC_LOAD_U32(now.table+24);}}
        }
    }
    if(!replay_running){if(original.size()>=32){physics_phase_error="Private controller diagnostic trace bound exceeded";return;}original.push_back(now);return;}
    const auto found=std::find_if(original.begin(),original.end(),[&](const auto& sample){return sample.object==object;});
    if(found==original.end()){physics_phase_error="Replay controller was absent from original update";return;}
    unsigned changed=0;std::ostringstream delta;
    for(unsigned i=0;i<240;++i)if(found->words[i]!=now.words[i]){if(changed++<48)delta<<" +"<<std::dec<<i*4<<":"<<std::hex<<found->words[i]<<"/"<<now.words[i];}
    REXLOG_INFO("Private replay controller entry {:08X} table {:08X}: {} changed words expected/actual {}",object,now.words[0],changed,delta.str());
    unsigned machine_changed=0;std::ostringstream machine_delta;
    for(unsigned i=0;i<16;++i)if(found->machine[i]!=now.machine[i]){++machine_changed;machine_delta<<" +"<<std::dec<<i*4<<":"<<std::hex<<found->machine[i]<<"/"<<now.machine[i];}
    REXLOG_INFO("Private replay motion state machine {:08X}: selected id {}, object {:08X}/{:08X}, class {:08X}/{:08X}, compute {:08X}, update {:08X}, {} changed header words {}",object,now.machine[4],found->state,now.state,found->table,now.table,now.compute,now.update,machine_changed,machine_delta.str());
}
void ObservePrivateNativeReplayBodyCall(PPCContext& ctx,uint8_t* base,uint32_t method,uint32_t body,uint32_t vector_a,uint32_t vector_b){
    if(!pending || (!replay_running && expected_replay) || !Enabled() || !REXCVAR_GET(em2_probe_native_frame_replay))return;
    struct Host{DWORD error=GetLastError();unsigned csr=_mm_getcsr();~Host(){SetLastError(error);_mm_setcsr(csr);}}host;
    GuestReadBatch reads;
    try{
        auto readable=[&](uint32_t at,size_t size){return at>=4096 && uint64_t(at)+size<=0x100000000ull && GuestReadBatch::Readable(base+at,size);};
        BodyCall call;call.body=body?body:ctx.r3.u32;call.caller=uint32_t(ctx.lr);call.method=method;call.scalar=ctx.f1.u64;
        if(method==0x8288AE38){if(!readable(call.body,20))return;call.body=PPC_LOAD_U32(call.body+16);}
        if(!readable(call.body,544) || PPC_LOAD_U32(call.body)!=0x82030384)return;
        for(unsigned i=0;i<4;++i){if(vector_a && readable(vector_a,16))call.arguments[i]=PPC_LOAD_U32(vector_a+i*4);if(vector_b && readable(vector_b,16))call.arguments[i+4]=PPC_LOAD_U32(vector_b+i*4);}
        for(unsigned i=0;i<3;++i)call.velocity[i]=PPC_LOAD_U32(call.body+432+i*4);
        const auto userdata=PPC_LOAD_U32(call.body+12);
        if(readable(userdata,24)){
            for(unsigned i=0;i<6;++i)call.userdata[i]=PPC_LOAD_U32(userdata+i*4);
            const auto source=call.userdata[1];
            if(source && readable(source,208)){call.source_table=PPC_LOAD_U32(source);for(unsigned i=0;i<16;++i)call.source_pose[i]=PPC_LOAD_U32(source+144+i*4);}
        }
        auto frame=ctx.r1.u32;
        for(auto& pc:call.parents){
            if((frame&15) || !readable(frame,4))break;
            const auto parent=PPC_LOAD_U32(frame);
            if(parent<=frame || uint64_t(parent)-frame>1024*1024 || !readable(parent-8,4))break;
            pc=PPC_LOAD_U32(parent-8);frame=parent;
        }
        auto& calls=replay_running?replay_body_calls:original_body_calls;
        if(calls.size()>=4096){physics_phase_error="Private body driver trace bound exceeded";return;}
        calls.push_back(call);
    }catch(const std::exception& e){physics_phase_error=e.what();}
}
void ObservePrivateNativeReplayPhysics(uint8_t* base,uint32_t object,unsigned phase){
    if(!pending || (!replay_running && expected_replay) || !Enabled() || !REXCVAR_GET(em2_probe_native_frame_replay))return;
    struct Host{DWORD error=GetLastError();unsigned csr=_mm_getcsr();~Host(){SetLastError(error);_mm_setcsr(csr);}}host;
    try{
        PhaseSample now;now.phase=phase;std::string why;
        if(!pending->SamplePhysicsPhase(base,object,phase,now.bodies,why)){physics_phase_error=why;return;}
        if(now.bodies.empty())return;
        if(!replay_running){if(physics_phases.size()>=256){physics_phase_error="Private physics phase trace bound exceeded";return;}physics_phases.push_back(std::move(now));return;}
        if(physics_phase_index>=physics_phases.size() || physics_phases[physics_phase_index].phase!=phase){physics_phase_error="Original/replay physics phase sequence differs";return;}
        const auto& expected=physics_phases[physics_phase_index++];size_t changed=0,metadata_changed=0,island_changed=0;std::string first;
        if(now.bodies.size()!=expected.bodies.size()){physics_phase_error="Original/replay phase body inventory differs";return;}
        for(size_t i=0;i<now.bodies.size();++i)if(now.bodies[i].metadata!=expected.bodies[i].metadata){
            if(++metadata_changed>8)continue;std::ostringstream values;
            for(unsigned byte=0;byte<96;byte+=4){auto word=[&](const auto& sample){const auto* b=sample.metadata.data()+byte;return uint32_t(b[0])<<24|uint32_t(b[1])<<16|uint32_t(b[2])<<8|b[3];};const auto a=word(expected.bodies[i]),b=word(now.bodies[i]);if(a!=b)values<<" +"<<std::dec<<128+byte<<":"<<std::hex<<a<<"/"<<b;}
            REXLOG_INFO("Private replay body metadata {:08X} phase {} differs:{}",now.bodies[i].body,phase,values.str());
        }
        std::map<uint32_t,bool> seen_islands;
        for(size_t i=0;i<now.bodies.size();++i)if(now.bodies[i].island!=expected.bodies[i].island || now.bodies[i].island_metadata!=expected.bodies[i].island_metadata){
            if(!seen_islands.emplace(now.bodies[i].island,true).second)continue;
            if(++island_changed>8)continue;std::ostringstream values;
            for(unsigned byte=0;byte<96;byte+=4){auto word=[&](const auto& sample){const auto* b=sample.island_metadata.data()+byte;return uint32_t(b[0])<<24|uint32_t(b[1])<<16|uint32_t(b[2])<<8|b[3];};const auto a=word(expected.bodies[i]),b=word(now.bodies[i]);if(a!=b)values<<" +"<<std::dec<<byte<<":"<<std::hex<<a<<"/"<<b;}
            REXLOG_INFO("Private replay island metadata {:08X}/{:08X} phase {} differs:{}",expected.bodies[i].island,now.bodies[i].island,phase,values.str());
        }
        for(size_t i=0;i<now.bodies.size();++i)if(now.bodies[i].body!=expected.bodies[i].body || now.bodies[i].bytes!=expected.bodies[i].bytes){
            ++changed;
            if((phase==1 || phase==3) && changed<=8){
                std::ostringstream values;
                for(unsigned offset:{208u,212u,216u}){auto word=[&](const auto& sample){const auto* b=sample.bytes.data()+offset;return uint32_t(b[0])<<24|uint32_t(b[1])<<16|uint32_t(b[2])<<8|b[3];};values<<" "<<std::hex<<word(expected.bodies[i])<<"/"<<word(now.bodies[i]);}
                DescribeBodyCalls(now.bodies[i].body,original_body_calls,"original");
                DescribeBodyCalls(now.bodies[i].body,replay_body_calls,"replay");
                REXLOG_INFO("Private replay divergent body {:08X} at physics phase {}: {}, velocity expected/actual {}",now.bodies[i].body,phase,pending->DescribePhysicsBody(base,now.bodies[i].body),values.str());
            }
            if(!first.empty())continue;unsigned byte=0;while(byte<320 && now.bodies[i].bytes[byte]==expected.bodies[i].bytes[byte])++byte;
            std::ostringstream message;message<<"body 0x"<<std::hex<<now.bodies[i].body<<" type "<<std::dec<<unsigned(now.bodies[i].bytes[8])<<" byte "<<byte;
            if(byte<320)message<<" expected "<<unsigned(expected.bodies[i].bytes[byte])<<" actual "<<unsigned(now.bodies[i].bytes[byte]);first=message.str();
        }
        REXLOG_INFO("Private native replay physics phase {}: {} of {} body motions differ; {}; {} body metadata payloads and {} island headers differ",phase,changed,now.bodies.size(),first,metadata_changed,island_changed);
        if((metadata_changed || island_changed) && physics_phase_error.empty())physics_phase_error="Original/replay body/island metadata differs; owned structures are not restored yet";
    }catch(const std::exception& e){physics_phase_error=e.what();}
}
bool PreparePrivateNativeFrameScriptRoots(PPCContext& ctx,uint8_t* base,uint32_t state,uint32_t values,unsigned count,std::string& error){
    if(!Enabled() || !REXCVAR_GET(em2_probe_native_frame_scripts) || scripts){error="Private retained script probe not enabled or already prepared";return false;}
    scripts=std::make_unique<NativeRetainedScriptCheckpoint>();
    return scripts->Prepare(ctx,base,state,values,count,CurrentNativeLoadGeneration(),error);
}
void TickPrivateNativeFrameCheckpoint(PPCContext& ctx,uint8_t* base,uint64_t completed_tick){
    if(!Enabled())return;
    std::string error;
    if(replay_running){
        if(completed_tick!=tick+1 || CurrentNativeLoadGeneration()!=epoch || !FinishPrivateNativeClockReplay(base,epoch,error))Finish(ctx,base,false,"Replay clock/frame scope: "+error);
        if(!physics_phase_error.empty() || physics_phase_index!=physics_phases.size())Finish(ctx,base,false,"Physics phase observation: "+physics_phase_error);
        if(pose_samples.empty())Finish(ctx,base,false,"Pose trace was absent across original/replay");
        REXLOG_INFO("Private native replay pose trace: {} original/{} replay samples, {} paired by object/phase/ordinal, {} reordered, {} extra, {} missing, {} differing observed-word samples; animation wrapper320 bytes, parent-basis phases424 bytes, pointed-to state not covered",pose_samples.size(),pose_replay_index,pose_used,pose_order_changed,pose_unexpected,pose_samples.size()-pose_used,pose_changed);
        REXLOG_INFO("Private native replay translation trace: {} original/{} replay writes, {} paired by object/caller/ordinal, {} reordered, {} extra, {} missing, {} differing input/prior-position samples; original setters always executed",translation_writes.size(),translation_replay_index,translation_used,translation_order_changed,translation_unexpected,translation_writes.size()-translation_used,translation_changed);
        if(!expected_replay || !expected_replay->Matches(ctx,base,epoch,error))Finish(ctx,base,false,"Replayed native components diverged: "+error);
        if(translation_used!=translation_writes.size() || translation_unexpected || translation_order_changed)Finish(ctx,base,false,"Native translation side-effect call sequence differs despite component equality");
        if(pose_used!=pose_samples.size() || pose_unexpected || pose_order_changed)Finish(ctx,base,false,"Native pose side-effect call sequence differs despite component equality");
        if(!scripts || !scripts->MatchesReplaySchedule(ctx,base,epoch,error))Finish(ctx,base,false,"Replayed native scheduler diverged: "+error);
        REXLOG_INFO("Private native frame component replay matched: one original engine update and finalization re-executed, captured components/clocks and scheduler metadata equal; hidden VM/gameplay state and side effects not fully compared");
        Finish(ctx,base,true,{});
    }
    if(pending){
        if(CurrentNativeLoadGeneration()!=epoch || completed_tick!=tick+1 || net::SynchronizedSessionActive())Finish(ctx,base,false,"Owner epoch/frame/session changed across update");
        const bool unchanged=pending->Matches(ctx,base,epoch,error);
        REXLOG_INFO("Private native frame component observation: previous tick {}, current {}, {} fields/{} bodies, captured state unchanged {}, comparison '{}'",tick,completed_tick,pending->fields(),pending->bodies(),unchanged,error);
        if(unchanged)Finish(ctx,base,false,"No captured state changed across actual update; restoration was not exercised");
        if(REXCVAR_GET(em2_probe_native_frame_replay)){
            if(!SealPrivateNativeClockRecord(base,epoch,error))Finish(ctx,base,false,error);
            expected_replay=std::make_unique<NativeGameplayComponentCheckpoint>();
            if(!expected_replay->Capture(ctx,base,epoch,error) || !scripts || !scripts->RememberReplaySchedule(ctx,base,epoch,error))Finish(ctx,base,false,"Replay expectation capture: "+error);
        }
        if(REXCVAR_GET(em2_probe_native_frame_scripts)){
            if(!scripts || scripts->Restore(ctx,base,epoch+1,error))Finish(ctx,base,false,"Retained scripts accepted another epoch or lost roots");
            if(!scripts->Restore(ctx,base,epoch,error))Finish(ctx,base,false,error);
        }
        if(!pending->Restore(ctx,base,epoch,error))Finish(ctx,base,false,error);
        if(scripts){
            if(!scripts->Verify(ctx,base,epoch,error))Finish(ctx,base,false,error);
            REXLOG_INFO("Private native frame scripts restored: {} tasks, registry/global roots and native schedule/owner metadata checked after component reconstruction",scripts->tasks());
        }
        if(REXCVAR_GET(em2_probe_native_frame_replay)){
            if(!BeginPrivateNativeClockReplay(base,epoch,error))Finish(ctx,base,false,error);
            replay_running=true;
            if(!RunPrivateNativeReplayUpdate(ctx,base,error))Finish(ctx,base,false,error);
            Finish(ctx,base,false,"Private replay returned unexpectedly");
        }
        Finish(ctx,base,true,{});
    }
    const auto current=CurrentNativeLoadGeneration();
    if((REXCVAR_GET(em2_probe_native_frame_scripts) && !scripts) || current<3 || net::SynchronizedSessionActive()){ready_frames=0;return;}
    NativeEntityOwnership owners;const char* why=nullptr;uint32_t where=0;
    if(!ReadNativeEntityOwnership(base,owners,why,where)){ready_frames=0;return;}
    if(current!=ready_epoch){ready_epoch=current;ready_frames=0;}
    if(++ready_frames<10)return;
    pending=std::make_unique<NativeGameplayComponentCheckpoint>();epoch=current;tick=completed_tick;
    if(!pending->Capture(ctx,base,epoch,error) || !pending->TestRandomRoundTrip(ctx,base,epoch,error) || !pending->TestInputRoundTrip(ctx,base,epoch,error) || !pending->TestPhysicsBridgeRoundTrip(base,epoch,error))Finish(ctx,base,false,error);
    if(scripts && (!scripts->Capture(ctx,base,epoch,error) || !pending->Matches(ctx,base,epoch,error)))Finish(ctx,base,false,error);
    if(REXCVAR_GET(em2_probe_native_frame_replay) && !BeginPrivateNativeClockRecord(base,epoch,error))Finish(ctx,base,false,error);
    REXLOG_INFO("Private native frame component checkpoint armed: tick {}, world {}, {} fields/{} bodies; next normal update owns all callbacks, no scripted input",tick,epoch,pending->fields(),pending->bodies());
}
void CancelPrivateNativeFrameCheckpoint(PPCContext& ctx,uint8_t* base){
    if(!pending && !scripts)return;
    Finish(ctx,base,false,"World unload began with a retained frame component checkpoint");
}
}
