#include "online_behavior_trace.h"
#include "simulation.h"
#include "guest_read_batch.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(bool,em2_trace_online_observations);
REXCVAR_DECLARE(std::string,em2_room_probe_role);
REXCVAR_DEFINE_UINT32(em2_behavior_trace_start_tick,0,"Debug","Private Room chooser trace begins at this agreed tick; earlier ticks retain the tiny inactive path");
REXCVAR_DEFINE_UINT32(em2_behavior_trace_dump_tick,0xFFFFFFFFu,"Debug","Private Room chooser history snapshot after this completed tick; maximum disables scheduled snapshots");
PPC_EXTERN_FUNC(__imp__sub_828D00F0);
PPC_EXTERN_FUNC(__imp__sub_829886D8);
PPC_EXTERN_FUNC(__imp__sub_828CB0E0);
PPC_EXTERN_FUNC(__imp__sub_829A5140);
namespace {
struct HostState {unsigned csr=_mm_getcsr();DWORD error=GetLastError();~HostState(){_mm_setcsr(csr);SetLastError(error);}};
struct Preserve {
    PPCContext* ctx;std::array<uint8_t,sizeof(PPCContext)> saved{};unsigned csr=_mm_getcsr();DWORD error=GetLastError();
    explicit Preserve(PPCContext* value=nullptr):ctx(value){if(ctx)std::memcpy(saved.data(),ctx,sizeof(*ctx));}
    ~Preserve(){if(ctx)std::memcpy(ctx,saved.data(),sizeof(*ctx));_mm_setcsr(csr);SetLastError(error);}
};
bool Enabled(){return REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_trace_online_observations) && !REXCVAR_GET(em2_room_probe_role).empty();}
bool Readable(uint32_t address,size_t size,uint8_t* base){return base && address>=4096 && size && uint64_t(address)+size<=0x100000000ull && em2::GuestReadBatch::Readable(base+address,size);}
uint32_t Word(uint32_t address,uint8_t* base){return Readable(address,4,base)?PPC_LOAD_U32(address):0;}
template<size_t N>bool Words(uint32_t address,std::array<uint32_t,N>& out,uint8_t* base){
    if(!Readable(address,N*4,base))return false;
    for(unsigned i=0;i<N;++i)out[i]=PPC_LOAD_U32(address+i*4);return true;
}
constexpr std::array<unsigned,10> gate_offsets{0,4,20,24,28,32,36,48,64,84};
struct Action {uint32_t address=0,table=0;bool readable=false;std::array<uint32_t,24> words{};std::array<uint32_t,10> gates{};};
Action ReadAction(uint32_t address,uint8_t* base){
    Action action;action.address=address;action.readable=Words(address,action.words,base);action.table=action.words[0];
    for(unsigned i=0;i<gate_offsets.size();++i)if(uint64_t(action.table)+gate_offsets[i]<=UINT32_MAX)action.gates[i]=Word(action.table+gate_offsets[i],base);
    return action;
}
struct Record {
    uint32_t caller=0,selector=0,context=0,entry_stack=0,list=0,count=0;
    bool entered=false,selected=false,returned=false,scores_valid=false;
    uint32_t native_stack=0,eligible_count=0,threshold=0,current_score=0,previous_action=0;
    std::array<uint32_t,6> before{},after{};
    std::array<uint32_t,33> context_before{},context_after{};
    std::array<uint32_t,8> scene{};
    uint32_t entity=0;std::array<uint32_t,4> entity_guid{};
    std::array<Action,32> actions_before{},actions_after{};
    std::array<uint32_t,32> scores{},eligible{};
};
struct StateRecord {
    uint32_t caller=0,action=0,context=0,entity=0,motion=0,motion_state=0;
    uint64_t delta=0;bool returned=false;
    std::array<uint32_t,4> guid{};
    std::array<uint32_t,32> before{},after{};
    // Scaled game counter60, raw previous counter68, native frequency and reciprocal.
    std::array<uint32_t,7> clocks_before{},clocks_after{};
};
struct RangeRecord {uint32_t caller=0,action=0;uint64_t minimum=0,maximum=0,result=0;bool returned=false;};
struct ChildView {
    uint32_t manager=0,entry=0,status_object=0,status=0xFFFFFFFF,buffer=0;
    std::array<uint32_t,6> buffer_words{};
};
struct ChildRecord {
    uint32_t caller=0,action=0,context=0,controller=0,dt=0,entity=0,result=0;
    uint64_t f1=0;bool returned=false;
    std::array<uint32_t,4> guid{};std::array<uint32_t,56> before{},after{};
    std::array<uint32_t,7> clocks_before{},clocks_after{};ChildView view_before{},view_after{};
};
struct Frame {uint64_t tick=0;unsigned count=0,dropped=0,state_count=0,state_dropped=0,range_count=0,range_dropped=0,child_count=0,child_dropped=0;bool valid=false,published=false;
    std::array<Record,32> records{};std::array<StateRecord,32> states{};std::array<RangeRecord,64> ranges{};std::array<ChildRecord,64> children{};};
struct Ring {std::array<Frame,4> frames{};unsigned next=0;Frame* current=nullptr;};
thread_local std::unique_ptr<Ring> history;
thread_local Record* active=nullptr;
thread_local uint32_t active_state_action=0;
void Entity(uint32_t context,uint32_t& entity,std::array<uint32_t,4>& guid,uint8_t* base){
    const auto object=Word(context+60,base);
    // ORIGINAL829885F0/82988038 convert the NiEntity base to its +8 face.
    if(object && uint64_t(object)+108<=UINT32_MAX && Word(object+8,base)==0x8208D908 && Words(object+12,guid,base))entity=object+8;
}
void StateClocks(std::array<uint32_t,7>& out,uint8_t* base){
    constexpr std::array<uint32_t,7> addresses{0x833FCA60,0x833FCA64,0x833FCA68,0x833FCA6C,0x833D04D8,0x833D04DC,0x833D04E0};
    for(unsigned i=0;i<addresses.size();++i)out[i]=Word(addresses[i],base);
}
ChildView ReadChildView(const std::array<uint32_t,56>& words,uint8_t* base){
    ChildView view;view.buffer=words[49];Words(view.buffer,view.buffer_words,base);
    // ORIGINAL828D9718 -> 82870358 -> 8237F5F0/8237F410. Read the
    // existing live entry only; never invoke its profiler or service methods.
    const auto reference=words[47];
    const auto world=Word(Word(0x833DF668,base),base);
    view.manager=Word(world+4,base);
    if(!reference || !view.manager)return view;
    const auto count=Word(view.manager+172,base),array=Word(view.manager+168,base);
    if(count>512 || !count || !Readable(array,size_t(count)*28,base))return view;
    for(uint32_t i=0;i<count;++i){const auto entry=array+i*28;
        if(Word(entry+20,base)!=reference || !Readable(entry+24,1,base) || PPC_LOAD_U8(entry+24))continue;
        view.entry=entry;view.status_object=Word(entry+12,base);
        if(!view.status_object)view.status=0;
        else if(uint64_t(view.status_object)+40<=UINT32_MAX && Readable(view.status_object+40,1,base))view.status=PPC_LOAD_U8(view.status_object+40);
        break;
    }
    return view;
}
void Before(Record& record,PPCContext& ctx,uint8_t* base){
    em2::GuestReadBatch reads;
    record.caller=uint32_t(ctx.lr);record.selector=ctx.r3.u32;record.context=ctx.r4.u32;record.entry_stack=ctx.r1.u32;
    Words(record.selector,record.before,base);Words(record.context,record.context_before,base);
    // ORIGINAL8294CD78 resolves a chooser context wrapper through context+60;
    // 827EC438 passes that same native controller context to this chooser.
    Entity(record.context,record.entity,record.entity_guid,base);
    record.list=record.before[1];record.count=record.before[2];
    const std::array<uint32_t,8> clocks{0x833FCA4C,0x833FCA54,0x833FCA70,0x833FCA74,0x833FCA38,0x833FCA3C,0x833FCA60,0x833FCA64};
    for(unsigned i=0;i<clocks.size();++i)record.scene[i]=Word(clocks[i],base);
    if(record.count<=record.actions_before.size() && Readable(record.list,size_t(record.count)*4,base))
        for(unsigned i=0;i<record.count;++i)record.actions_before[i]=ReadAction(Word(record.list+i*4,base),base);
}
void After(Record& record,uint8_t* base,bool returned){
    em2::GuestReadBatch reads;record.returned=returned;Words(record.selector,record.after,base);Words(record.context,record.context_after,base);
    const auto count=record.after[2],list=record.after[1];
    if(count<=record.actions_after.size() && Readable(list,size_t(count)*4,base))
        for(unsigned i=0;i<count;++i)record.actions_after[i]=ReadAction(Word(list+i*4,base),base);
    // The exact native frame is proven only by its first rand return. Normal
    // return leaves its score scratch intact; busy/reentrant early exits do not.
    if(returned && record.entered && record.count==count && count<=record.scores.size() &&
       uint64_t(record.native_stack)+736==record.entry_stack && Readable(record.native_stack+112,size_t(count)*4,base)){
        for(unsigned i=0;i<count;++i)record.scores[i]=Word(record.native_stack+112+i*4,base);
        record.scores_valid=true;
    }
}
std::string Hex(const auto& words){std::string text;for(auto word:words){if(!text.empty())text+=' ';text+=fmt::format("{:08X}",word);}return text;}
}
void em2::net::BeginNativeBehaviorTraceTick(uint64_t tick) noexcept{
    HostState preserve;
    try{if(!Enabled() || tick<REXCVAR_GET(em2_behavior_trace_start_tick)){if(history)history->current=nullptr;return;}
        const auto dump_tick=REXCVAR_GET(em2_behavior_trace_dump_tick);
        if(dump_tick!=0xFFFFFFFFu && tick && tick-1==dump_tick)DumpNativeBehaviorTrace();
        if(!history)history=std::make_unique<Ring>();
        auto& frame=history->frames[history->next++%history->frames.size()];frame.tick=tick;frame.count=frame.dropped=frame.state_count=frame.state_dropped=frame.range_count=frame.range_dropped=frame.child_count=frame.child_dropped=0;frame.valid=true;frame.published=false;history->current=&frame;
    }catch(...){}
}
void em2::net::ResetNativeBehaviorTrace() noexcept{HostState preserve;history.reset();active=nullptr;active_state_action=0;}
void em2::net::ObserveNativeBehaviorRandom(uint32_t caller,PPCContext& ctx,uint8_t* base) noexcept{
    if(!active || (caller!=0x828D0134 && caller!=0x828D037C))return;
    Preserve preserve(&ctx);
    try{
        if(ctx.r31.u32!=active->selector || ctx.r19.u32!=active->context || uint64_t(ctx.r1.u32)+736!=active->entry_stack)return;
        if(caller==0x828D0134){active->entered=true;active->native_stack=ctx.r1.u32;return;}
        active->selected=true;active->eligible_count=ctx.r26.u32;active->threshold=ctx.r23.u32;active->current_score=ctx.r21.u32;active->previous_action=ctx.r20.u32;
        if(active->eligible_count<=active->eligible.size() && Readable(ctx.r1.u32+368,size_t(active->eligible_count)*4,base))
            for(unsigned i=0;i<active->eligible_count;++i)active->eligible[i]=Word(ctx.r1.u32+368+i*4,base);
    }catch(...){}
}
namespace {
bool CaptureAllowed() noexcept{
    HostState preserve;
    try{uint64_t tick=0;return Enabled() && em2::CurrentAgreedSimulationTick(tick) && history && history->current && history->current->tick==tick;}catch(...){return false;}
}
void CommitRecord(const Record& record){if(history && history->current){auto& frame=*history->current;
    if(frame.count<frame.records.size())frame.records[frame.count++]=record;else ++frame.dropped;}}
// Keep large diagnostic scratch out of the default native wrapper's frame.
__declspec(noinline) void TraceChooser(PPCContext& ctx,uint8_t* base){
    Record record;bool capture=true;
    {Preserve preserve(&ctx);try{
        if(history->current->count==history->current->records.size()){++history->current->dropped;capture=false;}else Before(record,ctx,base);
    }catch(...){capture=false;}}
    const auto previous=active;if(capture)active=&record;
    struct Restore {Record* previous;~Restore(){active=previous;}}restore{previous};
    try{__imp__sub_828D00F0(ctx,base);}catch(...){
        if(capture){Preserve preserve(&ctx);try{After(record,base,false);CommitRecord(record);}catch(...){}}
        throw;
    }
    if(capture){Preserve preserve(&ctx);try{After(record,base,true);CommitRecord(record);}catch(...){}}
}
__declspec(noinline) void TraceState(PPCContext& ctx,uint8_t* base){
    StateRecord record;bool capture=false;
    {Preserve preserve(&ctx);try{em2::GuestReadBatch reads;auto& frame=*history->current;
        if(frame.state_count==frame.states.size())++frame.state_dropped;
        else{capture=true;record.caller=uint32_t(ctx.lr);record.action=ctx.r3.u32;record.context=ctx.r4.u32;record.delta=ctx.f1.u64;
            Words(record.action,record.before,base);Entity(Word(record.context,base),record.entity,record.guid,base);StateClocks(record.clocks_before,base);
            // The original admission resolves this exact Motion typed component
            // through833DAF54, then reads base+384 (82786850/82787098).
            const auto index_address=uint32_t(0x833DAF54);
            if(record.entity && Readable(index_address,1,base)){
                const auto map=Word(record.entity+88,base);const auto type=PPC_LOAD_U8(index_address);
                if(map && uint64_t(map)+type<=UINT32_MAX && Readable(map+type,1,base)){const auto index=PPC_LOAD_U8(map+type);const auto array=Word(record.entity+36,base);
                    if(index!=255 && array && uint64_t(array)+4ull*index<=UINT32_MAX){const auto face=Word(array+4*index,base);
                        if(face>=8 && Readable(face-8,388,base)){record.motion=face-8;record.motion_state=Word(record.motion+384,base);}}}
            }
        }}catch(...){capture=false;}}
    const auto previous=active_state_action;active_state_action=record.action;
    struct Restore{uint32_t previous;~Restore(){active_state_action=previous;}}restore{previous};
    const auto commit=[&](bool returned){if(!capture)return;Preserve preserve(&ctx);try{em2::GuestReadBatch reads;
        record.returned=returned;Words(record.action,record.after,base);StateClocks(record.clocks_after,base);
        auto& frame=*history->current;if(frame.state_count<frame.states.size())frame.states[frame.state_count++]=record;else ++frame.state_dropped;
    }catch(...){}};
    try{__imp__sub_829886D8(ctx,base);}catch(...){commit(false);throw;}commit(true);
}
__declspec(noinline) void TraceRange(PPCContext& ctx,uint8_t* base){
    RangeRecord record;bool capture=false;
    {Preserve preserve(&ctx);if(history->current->range_count<history->current->ranges.size()){
        capture=true;record.caller=uint32_t(ctx.lr);record.action=active_state_action;record.minimum=ctx.f1.u64;record.maximum=ctx.f2.u64;
    }else ++history->current->range_dropped;}
    const auto commit=[&](bool returned){if(!capture)return;Preserve preserve(&ctx);record.returned=returned;record.result=ctx.f1.u64;
        auto& frame=*history->current;if(frame.range_count<frame.ranges.size())frame.ranges[frame.range_count++]=record;else ++frame.range_dropped;};
    try{__imp__sub_828CB0E0(ctx,base);}catch(...){commit(false);throw;}commit(true);
}
__declspec(noinline) void TraceChild(PPCContext& ctx,uint8_t* base){
    ChildRecord record;bool capture=false;
    {Preserve preserve(&ctx);try{em2::GuestReadBatch reads;auto& frame=*history->current;
        if(frame.child_count==frame.children.size())++frame.child_dropped;
        else{capture=true;record.caller=uint32_t(ctx.lr);record.action=ctx.r3.u32;record.context=ctx.r4.u32;record.f1=ctx.f1.u64;
            record.controller=Word(record.context,base);record.dt=Word(record.context+4,base);
            Entity(record.controller,record.entity,record.guid,base);Words(record.action,record.before,base);
            StateClocks(record.clocks_before,base);record.view_before=ReadChildView(record.before,base);
        }}catch(...){capture=false;}}
    const auto commit=[&](bool returned){if(!capture)return;Preserve preserve(&ctx);try{em2::GuestReadBatch reads;
        record.returned=returned;record.result=ctx.r3.u32;Words(record.action,record.after,base);
        StateClocks(record.clocks_after,base);record.view_after=ReadChildView(record.after,base);
        auto& frame=*history->current;if(frame.child_count<frame.children.size())frame.children[frame.child_count++]=record;else ++frame.child_dropped;
    }catch(...){}};
    try{__imp__sub_829A5140(ctx,base);}catch(...){commit(false);throw;}commit(true);
}
}
PPC_FUNC_IMPL(sub_828D00F0){
    if(!CaptureAllowed()){__imp__sub_828D00F0(ctx,base);return;}
    TraceChooser(ctx,base);
}
PPC_FUNC_IMPL(sub_829886D8){if(!CaptureAllowed()){__imp__sub_829886D8(ctx,base);return;}TraceState(ctx,base);}
PPC_FUNC_IMPL(sub_828CB0E0){if(!CaptureAllowed()){__imp__sub_828CB0E0(ctx,base);return;}TraceRange(ctx,base);}
PPC_FUNC_IMPL(sub_829A5140){if(!CaptureAllowed()){__imp__sub_829A5140(ctx,base);return;}TraceChild(ctx,base);}
void em2::net::DumpNativeBehaviorTrace() noexcept{
    HostState preserve;
    try{if(!history)return;std::array<Frame*,4> frames{};for(unsigned i=0;i<frames.size();++i)frames[i]=&history->frames[i];
        std::sort(frames.begin(),frames.end(),[](const auto* a,const auto* b){return a->tick<b->tick;});
        for(auto* frame:frames)if(frame->valid && !frame->published){frame->published=true;
            REXLOG_INFO("Native behavior failure history: tick {}, records {}, dropped {}",frame->tick,frame->count,frame->dropped);
            REXLOG_INFO("Native behavior state history: tick {}, states {}, dropped {}, ranges {}, dropped {}",frame->tick,frame->state_count,frame->state_dropped,frame->range_count,frame->range_dropped);
            REXLOG_INFO("Native behavior child history: tick {}, children {}, dropped {}",frame->tick,frame->child_count,frame->child_dropped);
            for(unsigned n=0;n<frame->child_count;++n){const auto& r=frame->children[n];
                REXLOG_INFO("Native behavior child: tick {} index {} caller {:08X} action {:08X} context {:08X} controller {:08X} dt {:08X} entity {:08X} GUID [{}] f1 {:016X} returned {} result {:08X} before [{}] after [{}] clocks-before [{}] clocks-after [{}]",
                    frame->tick,n,r.caller,r.action,r.context,r.controller,r.dt,r.entity,Hex(r.guid),r.f1,r.returned,r.result,Hex(r.before),Hex(r.after),Hex(r.clocks_before),Hex(r.clocks_after));
                for(unsigned phase=0;phase<2;++phase){const auto& v=phase?r.view_after:r.view_before;
                    REXLOG_INFO("Native behavior child service: tick {} index {} phase {} manager {:08X} entry {:08X} status-object {:08X} status {:08X} buffer {:08X} words [{}]",
                        frame->tick,n,phase,v.manager,v.entry,v.status_object,v.status,v.buffer,Hex(v.buffer_words));}}
            for(unsigned n=0;n<frame->state_count;++n){const auto& r=frame->states[n];
                REXLOG_INFO("Native behavior state: tick {} index {} caller {:08X} action {:08X} context {:08X} entity {:08X} GUID [{}] motion {:08X} motion-state {} delta {:016X} returned {} before [{}] after [{}] clocks-before [{}] clocks-after [{}]",
                    frame->tick,n,r.caller,r.action,r.context,r.entity,Hex(r.guid),r.motion,r.motion_state,r.delta,r.returned,Hex(r.before),Hex(r.after),Hex(r.clocks_before),Hex(r.clocks_after));}
            for(unsigned n=0;n<frame->range_count;++n){const auto& r=frame->ranges[n];
                REXLOG_INFO("Native behavior range: tick {} index {} caller {:08X} action {:08X} minimum {:016X} maximum {:016X} result {:016X} returned {}",
                    frame->tick,n,r.caller,r.action,r.minimum,r.maximum,r.result,r.returned);}
            for(unsigned n=0;n<frame->count;++n){const auto& r=frame->records[n];
                REXLOG_INFO("Native behavior chooser: tick {} index {} caller {:08X} selector {:08X} context {:08X} entrySP {:08X} entered {} selected {} returned {} nativeSP {:08X} scores-valid {} count {} eligible {} threshold {:08X} current-score {:08X} previous {:08X}",
                    frame->tick,n,r.caller,r.selector,r.context,r.entry_stack,r.entered,r.selected,r.returned,r.native_stack,r.scores_valid,r.count,r.eligible_count,r.threshold,r.current_score,r.previous_action);
                REXLOG_INFO("Native behavior entity: tick {} index {} entity {:08X} GUID [{}]",frame->tick,n,r.entity,Hex(r.entity_guid));
                REXLOG_INFO("Native behavior selector words: tick {} index {} before [{}] after [{}]",frame->tick,n,Hex(r.before),Hex(r.after));
                REXLOG_INFO("Native behavior context words: tick {} index {} context {:08X} before [{}] after [{}]",frame->tick,n,r.context,Hex(r.context_before),Hex(r.context_after));
                REXLOG_INFO("Native behavior clocks: tick {} index {} words [{}]",frame->tick,n,Hex(r.scene));
                for(unsigned i=0;i<std::min<unsigned>(r.count,r.actions_before.size());++i){const auto& a=r.actions_before[i];
                    REXLOG_INFO("Native behavior candidate-before: tick {} index {} candidate {} address {:08X} table {:08X} readable {} gates [{}] words [{}]",frame->tick,n,i,a.address,a.table,a.readable,Hex(a.gates),Hex(a.words));}
                for(unsigned i=0;i<std::min<unsigned>(r.after[2],r.actions_after.size());++i){const auto& a=r.actions_after[i];
                    REXLOG_INFO("Native behavior candidate-after: tick {} index {} candidate {} address {:08X} table {:08X} readable {} score-valid {} score {:08X} gates [{}] words [{}]",frame->tick,n,i,a.address,a.table,a.readable,r.scores_valid,r.scores[i],Hex(a.gates),Hex(a.words));}
                if(r.selected)REXLOG_INFO("Native behavior eligible: tick {} index {} addresses [{}]",frame->tick,n,Hex(r.eligible));
            }
        }
    }catch(...){}
}
