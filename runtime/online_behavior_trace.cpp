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
#include <stdexcept>
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(bool,em2_trace_online_observations);
REXCVAR_DECLARE(std::string,em2_room_probe_role);
REXCVAR_DEFINE_UINT32(em2_behavior_trace_start_tick,0,"Debug","Private Room chooser trace begins at this agreed tick; earlier ticks retain the tiny inactive path");
REXCVAR_DEFINE_UINT32(em2_behavior_trace_dump_tick,0xFFFFFFFFu,"Debug","Private Room chooser history snapshot after this completed tick; maximum disables scheduled snapshots");
REXCVAR_DEFINE_STRING(em2_behavior_motion_guid,"","Debug","Optional private Motion trace GUID as exactly 32 hexadecimal digits; empty records bounded all actors");
PPC_EXTERN_FUNC(__imp__sub_828D00F0);
PPC_EXTERN_FUNC(__imp__sub_829886D8);
PPC_EXTERN_FUNC(__imp__sub_828CB0E0);
PPC_EXTERN_FUNC(__imp__sub_829A5140);
PPC_EXTERN_FUNC(__imp__sub_82788C00);
PPC_EXTERN_FUNC(__imp__sub_8278D158);
PPC_EXTERN_FUNC(__imp__sub_8278E8E0);
PPC_EXTERN_FUNC(__imp__sub_82477A88);
PPC_EXTERN_FUNC(__imp__sub_8294D008);
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
struct MotionView {
    bool readable=false;uint32_t entity=0,physics=0,activation=0xFFFFFFFF,controller=0,native_state=0xFFFFFFFF,attribute=0,attribute_value=0;
    uint32_t owner92=0xFFFFFFFF,owner93=0xFFFFFFFF;
    std::array<uint32_t,4> guid{};std::array<uint32_t,3> position{},body_velocity{};
    bool positioned=false,body_velocity_readable=false;
    std::array<uint32_t,100> words{};std::array<uint32_t,32> controller_words{};std::array<uint32_t,7> clocks{};
    // Motion byte52, list548/count552, flags704/957, timer884,
    // attribute108, selected speed-value bits and list-first-value bits.
    std::array<uint32_t,9> branch{};std::array<uint32_t,20> speed_words{};bool speed_readable=false,speed_value_readable=false;
};
enum class MotionStage:uint32_t {Ordinary=0,Physics=1,Dispatch=2,VelocityWrite=3,LaunchAction=4};
struct MotionRecord {
    uint64_t order=0;uint32_t caller=0,motion=0,thread=0;bool agreed=false,returned=false;
    MotionStage stage=MotionStage::Physics;
    uint64_t f1=0;std::array<uint64_t,4> args{};uint32_t result=0;
    std::array<uint32_t,4> input_velocity{};std::array<uint32_t,12> parents{};
    uint32_t action=0,action_delta=0;std::array<uint32_t,44> action_before{},action_after{};
    std::array<uint32_t,2> child_before{},child_after{};
    MotionView before{},after{};
};
struct MotionSetRecord {
    uint64_t order=0;uint32_t caller=0,motion=0,thread=0,next=0,previous=0;bool agreed=false,returned=false;
    MotionView before{},after{};
};
struct Frame {uint64_t tick=0,motion_order=0;unsigned count=0,dropped=0,state_count=0,state_dropped=0,range_count=0,range_dropped=0,child_count=0,child_dropped=0,motion_count=0,motion_dropped=0,set_count=0,set_dropped=0;bool valid=false,published=false;
    std::array<Record,32> records{};std::array<StateRecord,32> states{};std::array<RangeRecord,64> ranges{};std::array<ChildRecord,64> children{};
    std::array<MotionRecord,128> motions{};std::array<MotionSetRecord,64> sets{};};
// A filtered actor can leave the failure ring long before RNG notices its
// changed physical state. Retain original before/after views without logging
// in the running tick. Never allocate this extension for an all-actor trace.
struct MotionTrajectory {
    struct Entry {uint64_t tick=0;MotionRecord record;};
    std::array<Entry,2048> entries{};
    size_t next=0,count=0;bool published=false;
    void Push(uint64_t tick,const MotionRecord& record){
        entries[next]={tick,record};next=(next+1)%entries.size();count=std::min(count+1,entries.size());published=false;
    }
};
struct Ring {std::array<Frame,4> frames{};unsigned next=0;uint32_t filtered_motion=0;Frame* current=nullptr;std::unique_ptr<MotionTrajectory> trajectory;};
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
MotionView ReadMotionView(uint32_t motion,uint8_t* base){
    MotionView v;v.readable=Words(motion,v.words,base);StateClocks(v.clocks,base);
    if(!v.readable)return v;
    const auto owner=v.words[12];
    if(owner && uint64_t(owner)+108<=UINT32_MAX && Word(owner+8,base)==0x8208D908){
        v.entity=owner+8;Words(owner+12,v.guid,base);
        if(Readable(owner+92,2,base)){v.owner92=PPC_LOAD_U8(owner+92);v.owner93=PPC_LOAD_U8(owner+93);}
        if(Readable(0x833DBC3C,1,base)){
            const auto types=Word(v.entity+88,base),components=Word(v.entity+36,base);const auto type=PPC_LOAD_U8(0x833DBC3C);
            if(types && uint64_t(types)+type<=UINT32_MAX && Readable(types+type,1,base)){
                const auto slot=PPC_LOAD_U8(types+type);
                if(slot!=255 && components && uint64_t(components)+4ull*slot<=UINT32_MAX){const auto face=Word(components+4*slot,base);
                    if(face>=8 && uint64_t(face)+176<=UINT32_MAX)v.positioned=Words(face-8+172,v.position,base);}
            }
        }
    }
    v.physics=v.words[20];
    if(v.physics && uint64_t(v.physics)+180<=UINT32_MAX && Readable(v.physics,180,base)){
        v.activation=PPC_LOAD_U8(v.physics+145);v.controller=Word(v.physics+176,base);Words(v.controller,v.controller_words,base);
        // ORIGINAL82EB8D50 resolves this object's+16 then returns its+432 velocity.
        const auto body=Word(v.controller+16,base);
        if(body && uint64_t(body)+444<=UINT32_MAX)v.body_velocity_readable=Words(body+432,v.body_velocity,base);
    }
    const auto context=v.words[23];
    if(context && uint64_t(context)+20<=UINT32_MAX && Readable(context+16,4,base))v.native_state=PPC_LOAD_U32(context+16);
    v.attribute=v.words[29];
    if(v.attribute && uint64_t(v.attribute)+76<=UINT32_MAX)v.attribute_value=Word(v.attribute+72,base);
    if(uint64_t(motion)+958<=UINT32_MAX){
        v.branch[0]=Readable(motion+52,1,base)?PPC_LOAD_U8(motion+52):0xFFFFFFFF;
        v.branch[1]=Word(motion+548,base);v.branch[2]=Word(motion+552,base);
        v.branch[3]=Readable(motion+704,1,base)?PPC_LOAD_U8(motion+704):0xFFFFFFFF;
        v.branch[4]=Readable(motion+957,1,base)?PPC_LOAD_U8(motion+957):0xFFFFFFFF;
        v.branch[5]=Word(motion+884,base);v.branch[6]=v.words[27];
        v.speed_readable=Words(v.branch[6],v.speed_words,base);
        if(v.branch[1] && uint64_t(v.branch[1])+8<=UINT32_MAX)v.branch[8]=Word(v.branch[1]+4,base);
        // ORIGINAL82787440: positive override count selects list+4;
        // otherwise82886D68(attribute108) returns attribute+72, or native0.
        uint32_t speed_address=0x82002060;
        if(int32_t(v.branch[2])>0){if(v.branch[1] && uint64_t(v.branch[1])+8<=UINT32_MAX)speed_address=v.branch[1]+4;else speed_address=0;}
        else if(v.branch[6]){if(uint64_t(v.branch[6])+76<=UINT32_MAX)speed_address=v.branch[6]+72;else speed_address=0;}
        v.speed_value_readable=Readable(speed_address,4,base);v.branch[7]=Word(speed_address,base);
    }
    return v;
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
        auto& frame=history->frames[history->next++%history->frames.size()];frame.tick=tick;frame.motion_order=0;frame.count=frame.dropped=frame.state_count=frame.state_dropped=frame.range_count=frame.range_dropped=frame.child_count=frame.child_dropped=frame.motion_count=frame.motion_dropped=frame.set_count=frame.set_dropped=0;frame.valid=true;frame.published=false;history->current=&frame;
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
bool MotionGuidMatches(uint32_t motion,uint8_t* base){
    const auto& requested=REXCVAR_GET(em2_behavior_motion_guid);
    if(requested.empty())return true;
    struct Cached {std::string text;bool valid=false;std::array<uint32_t,4> guid{};};
    static thread_local Cached cached;
    if(cached.text!=requested){
        cached.text=requested;cached.valid=requested.size()==32;cached.guid={};
        for(size_t i=0;cached.valid && i<requested.size();++i){
            const auto c=requested[i];const int digit=c>='0' && c<='9'?c-'0':c>='a' && c<='f'?c-'a'+10:c>='A' && c<='F'?c-'A'+10:-1;
            if(digit<0){cached.valid=false;break;}cached.guid[i/8]=(cached.guid[i/8]<<4)|uint32_t(digit);
        }
    }
    if(!cached.valid || !motion || uint64_t(motion)+52>UINT32_MAX)return false;
    em2::GuestReadBatch reads;
    const auto owner=Word(motion+48,base);
    if(!owner || uint64_t(owner)+28>UINT32_MAX || Word(owner+8,base)!=0x8208D908)return false;
    std::array<uint32_t,4> guid{};
    return Words(owner+12,guid,base) && guid==cached.guid;
}
bool MotionCaptureAllowed(uint32_t motion,uint8_t* base) noexcept{
    HostState preserve;
    // A between-tick call belongs to the last ring slot ONLY on this same
    // thread. No history is created for workers or before the requested tick.
    try{return Enabled() && history && history->current && history->current->valid && !history->current->published && MotionGuidMatches(motion,base);}catch(...){return false;}
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
__declspec(noinline) void TraceMotion(PPCContext& ctx,uint8_t* base,PPCFunc* original,MotionStage stage,uint32_t observed_motion=0){
    MotionRecord record;bool capture=false;
    {Preserve preserve(&ctx);try{em2::GuestReadBatch reads;auto& frame=*history->current;
        if(frame.motion_count==frame.motions.size())++frame.motion_dropped;
        else{capture=true;record.stage=stage;record.order=++frame.motion_order;record.caller=uint32_t(ctx.lr);record.motion=observed_motion?observed_motion:ctx.r3.u32;record.thread=GetCurrentThreadId();
            record.agreed=em2::InAgreedSimulationTick();record.f1=ctx.f1.u64;record.args={ctx.r4.u64,ctx.r5.u64,ctx.r6.u64,ctx.r7.u64};record.before=ReadMotionView(record.motion,base);
            if(!REXCVAR_GET(em2_behavior_motion_guid).empty())history->filtered_motion=record.motion;
            if(stage==MotionStage::VelocityWrite){
                Words(ctx.r4.u32&~15u,record.input_velocity,base);auto stack=ctx.r1.u32;
                for(auto& address:record.parents){const auto parent=Word(stack,base);
                    if(parent<=stack || parent-stack>65536)break;address=Word(parent-8,base);stack=parent;}
            }
            if(stage==MotionStage::LaunchAction){
                record.action=ctx.r3.u32;record.action_delta=Word(ctx.r4.u32+4,base);Words(record.action,record.action_before,base);
                if(record.action_before[1])Words(record.action_before[1],record.child_before,base);
            }
        }
    }catch(...){capture=false;}}
    const auto commit=[&](bool returned){if(!capture)return;Preserve preserve(&ctx);try{em2::GuestReadBatch reads;record.returned=returned;record.result=ctx.r3.u32;record.after=ReadMotionView(record.motion,base);
        if(stage==MotionStage::LaunchAction){Words(record.action,record.action_after,base);if(record.action_after[1])Words(record.action_after[1],record.child_after,base);}
        auto& frame=*history->current;if(frame.motion_count<frame.motions.size())frame.motions[frame.motion_count++]=record;else ++frame.motion_dropped;
        if(!REXCVAR_GET(em2_behavior_motion_guid).empty()){
            if(!history->trajectory)history->trajectory=std::make_unique<MotionTrajectory>();
            history->trajectory->Push(frame.tick,record);
        }
    }catch(...){}};
    try{original(ctx,base);}catch(...){commit(false);throw;}commit(true);
}
__declspec(noinline) void TraceMotionSet(PPCContext& ctx,uint8_t* base){
    MotionSetRecord record;bool capture=false;
    {Preserve preserve(&ctx);try{em2::GuestReadBatch reads;auto& frame=*history->current;
        if(frame.set_count==frame.sets.size())++frame.set_dropped;
        else{capture=true;record.order=++frame.motion_order;record.caller=uint32_t(ctx.lr);record.motion=ctx.r3.u32;record.thread=GetCurrentThreadId();
            record.agreed=em2::InAgreedSimulationTick();record.next=ctx.r4.u32;record.previous=ctx.r5.u32;record.before=ReadMotionView(record.motion,base);}
    }catch(...){capture=false;}}
    const auto commit=[&](bool returned){if(!capture)return;Preserve preserve(&ctx);try{em2::GuestReadBatch reads;record.returned=returned;record.after=ReadMotionView(record.motion,base);
        auto& frame=*history->current;if(frame.set_count<frame.sets.size())frame.sets[frame.set_count++]=record;else ++frame.set_dropped;
    }catch(...){}};
    try{__imp__sub_82788C00(ctx,base);}catch(...){commit(false);throw;}commit(true);
}
void DumpMotionView(uint64_t tick,uint64_t order,const char* kind,unsigned phase,const MotionView& v){
    REXLOG_INFO("Native behavior motion view: tick {} order {} kind {} phase {} readable {} entity {:08X} GUID [{}] owner92/93 {}/{} positioned {} position [{}] physics {:08X} activation {} controller {:08X} native-state {} attribute {:08X} value {:08X} body-velocity-readable {} body-velocity [{}] words [{}] controller-words [{}] clocks [{}]",
        tick,order,kind,phase,v.readable,v.entity,Hex(v.guid),v.owner92,v.owner93,v.positioned,Hex(v.position),v.physics,v.activation,v.controller,v.native_state,v.attribute,v.attribute_value,v.body_velocity_readable,Hex(v.body_velocity),Hex(v.words),Hex(v.controller_words),Hex(v.clocks));
    REXLOG_INFO("Native behavior motion branch: tick {} order {} kind {} phase {} fields [{}] speed-value-readable {} speed-readable {} speed-words [{}]",tick,order,kind,phase,Hex(v.branch),v.speed_value_readable,v.speed_readable,Hex(v.speed_words));
}
}
void em2::net::RunNativeMotionTrace(PPCContext& ctx,uint8_t* base,PPCFunc* original){
    if(!original)throw std::invalid_argument("Native Motion trace requires its original callback");
    if(!MotionCaptureAllowed(ctx.r3.u32,base)){original(ctx,base);return;}TraceMotion(ctx,base,original,MotionStage::Physics);
}
namespace {
uint32_t FilteredVelocityMotion(uint32_t interface_address,uint8_t* base) noexcept{
    HostState preserve;
    try{
        if(!history || !history->filtered_motion || !MotionCaptureAllowed(history->filtered_motion,base))return 0;
        em2::GuestReadBatch reads;const auto motion=history->filtered_motion;
        const auto physics=Word(motion+80,base),controller=physics?Word(physics+176,base):0;
        const auto body=controller?Word(controller+16,base):0;
        // Read-only confirmed native proxy layout: virtual slot60 on its +224
        // interface writes the vector at interface+208 == body+432.
        return body && uint64_t(body)+224==interface_address && Word(body,base)==0x82030384 &&
            Word(interface_address,base)==0x8202E5CC?motion:0;
    }catch(...){return 0;}
}
uint32_t FilteredLaunchMotion(uint32_t action,uint32_t context,uint8_t* base) noexcept{
    HostState preserve;
    try{
        if(!history || !history->filtered_motion || !MotionCaptureAllowed(history->filtered_motion,base))return 0;
        em2::GuestReadBatch reads;const auto motion=history->filtered_motion;
        if(!Readable(action,176,base) || Word(Word(action,base)+72,base)!=0x82998D00)return 0;
        const auto controller=Word(context,base),owner=Word(motion+48,base);
        return controller && owner && Word(controller+60,base)==owner?motion:0;
    }catch(...){return 0;}
}
}
PPC_FUNC_IMPL(sub_82477A88){
    const auto motion=FilteredVelocityMotion(ctx.r3.u32,base);
    if(!motion){__imp__sub_82477A88(ctx,base);return;}
    TraceMotion(ctx,base,__imp__sub_82477A88,MotionStage::VelocityWrite,motion);
}
PPC_FUNC_IMPL(sub_8294D008){
    const auto motion=FilteredLaunchMotion(ctx.r3.u32,ctx.r4.u32,base);
    if(!motion){__imp__sub_8294D008(ctx,base);return;}
    TraceMotion(ctx,base,__imp__sub_8294D008,MotionStage::LaunchAction,motion);
}
PPC_FUNC_IMPL(sub_828D00F0){
    if(!CaptureAllowed()){__imp__sub_828D00F0(ctx,base);return;}
    TraceChooser(ctx,base);
}
PPC_FUNC_IMPL(sub_829886D8){if(!CaptureAllowed()){__imp__sub_829886D8(ctx,base);return;}TraceState(ctx,base);}
PPC_FUNC_IMPL(sub_828CB0E0){if(!CaptureAllowed()){__imp__sub_828CB0E0(ctx,base);return;}TraceRange(ctx,base);}
PPC_FUNC_IMPL(sub_829A5140){if(!CaptureAllowed()){__imp__sub_829A5140(ctx,base);return;}TraceChild(ctx,base);}
PPC_FUNC_IMPL(sub_82788C00){if(!MotionCaptureAllowed(ctx.r3.u32,base)){__imp__sub_82788C00(ctx,base);return;}TraceMotionSet(ctx,base);}
PPC_FUNC_IMPL(sub_8278D158){if(!MotionCaptureAllowed(ctx.r3.u32,base)){__imp__sub_8278D158(ctx,base);return;}TraceMotion(ctx,base,__imp__sub_8278D158,MotionStage::Ordinary);}
PPC_FUNC_IMPL(sub_8278E8E0){if(!MotionCaptureAllowed(ctx.r3.u32,base)){__imp__sub_8278E8E0(ctx,base);return;}TraceMotion(ctx,base,__imp__sub_8278E8E0,MotionStage::Dispatch);}
void em2::net::DumpNativeBehaviorTrace() noexcept{
    HostState preserve;
    try{if(!history)return;
        if(auto* trajectory=history->trajectory.get();trajectory && !trajectory->published){
            trajectory->published=true;
            std::array<const MotionTrajectory::Entry*,2048> entries{};
            for(size_t i=0;i<trajectory->count;++i)entries[i]=&trajectory->entries[i];
            std::sort(entries.begin(),entries.begin()+trajectory->count,[](const auto* a,const auto* b){
                return a->tick!=b->tick?a->tick<b->tick:a->record.order<b->record.order;
            });
            REXLOG_INFO("Native filtered motion trajectory: retained {}, capacity {}",trajectory->count,entries.size());
            for(size_t i=0;i<trajectory->count;++i){const auto& entry=*entries[i];const auto& r=entry.record;
                REXLOG_INFO("Native trajectory update: tick {} order {} stage {} caller {:08X} agreed {} f1 {:016X} args [{}] returned {}",
                    entry.tick,r.order,uint32_t(r.stage),r.caller,r.agreed,r.f1,Hex(r.args),r.returned);
                if(r.stage==MotionStage::VelocityWrite)REXLOG_INFO("Native trajectory velocity writer: tick {} order {} input [{}] parents [{}]",entry.tick,r.order,Hex(r.input_velocity),Hex(r.parents));
                if(r.stage==MotionStage::LaunchAction)REXLOG_INFO("Native trajectory launch action: tick {} order {} action {:08X} delta {:08X} before [{}] after [{}] child-before [{}] child-after [{}]",entry.tick,r.order,r.action,r.action_delta,Hex(r.action_before),Hex(r.action_after),Hex(r.child_before),Hex(r.child_after));
                DumpMotionView(entry.tick,r.order,"trajectory",0,r.before);DumpMotionView(entry.tick,r.order,"trajectory",1,r.after);
            }
        }
        std::array<Frame*,4> frames{};for(unsigned i=0;i<frames.size();++i)frames[i]=&history->frames[i];
        std::sort(frames.begin(),frames.end(),[](const auto* a,const auto* b){return a->tick<b->tick;});
        for(auto* frame:frames)if(frame->valid && !frame->published){frame->published=true;
            REXLOG_INFO("Native behavior failure history: tick {}, records {}, dropped {}",frame->tick,frame->count,frame->dropped);
            REXLOG_INFO("Native behavior state history: tick {}, states {}, dropped {}, ranges {}, dropped {}",frame->tick,frame->state_count,frame->state_dropped,frame->range_count,frame->range_dropped);
            REXLOG_INFO("Native behavior child history: tick {}, children {}, dropped {}",frame->tick,frame->child_count,frame->child_dropped);
            REXLOG_INFO("Native behavior motion history: tick {}, updates {}, dropped {}, setters {}, dropped {}",frame->tick,frame->motion_count,frame->motion_dropped,frame->set_count,frame->set_dropped);
            for(unsigned n=0;n<frame->motion_count;++n){const auto& r=frame->motions[n];
                REXLOG_INFO("Native behavior motion update: tick {} order {} stage {} caller {:08X} motion {:08X} thread {} agreed {} f1 {:016X} args [{}] returned {} result {:08X}",frame->tick,r.order,uint32_t(r.stage),r.caller,r.motion,r.thread,r.agreed,r.f1,Hex(r.args),r.returned,r.result);
                DumpMotionView(frame->tick,r.order,"update",0,r.before);DumpMotionView(frame->tick,r.order,"update",1,r.after);}
            for(unsigned n=0;n<frame->set_count;++n){const auto& r=frame->sets[n];
                REXLOG_INFO("Native behavior motion setter: tick {} order {} caller {:08X} motion {:08X} thread {} agreed {} next {} previous {} returned {}",frame->tick,r.order,r.caller,r.motion,r.thread,r.agreed,r.next,r.previous,r.returned);
                DumpMotionView(frame->tick,r.order,"setter",0,r.before);DumpMotionView(frame->tick,r.order,"setter",1,r.after);}
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
