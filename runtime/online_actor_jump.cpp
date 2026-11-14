#include "online_actor_jump.h"
#include "online_owned_probe.h"
#include "online_retail_jump.h"
#include "online_retail_input.h"
#include "online_actor_frames.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <rex/cvar.h>
REXCVAR_DECLARE(bool,em2_private_profile);
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <windows.h>
#include <immintrin.h>
PPC_EXTERN_FUNC(__imp__sub_82884018);
PPC_EXTERN_FUNC(__imp__sub_8287EA38);
PPC_EXTERN_FUNC(__imp__sub_8287ECF8);
PPC_EXTERN_FUNC(__imp__sub_8287D048);
PPC_EXTERN_FUNC(__imp__sub_8287ED00);
PPC_EXTERN_FUNC(__imp__sub_82A3C930);
namespace {
using Actor=em2::net::OwnedActorInput;
using Command=em2::net::NativeActorJump;
using Result=em2::net::ActorInputResult;
using Edge=em2::net::OwnedJumpEdge;
struct Environment {unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(csr);SetLastError(error);}};
struct Context {PPCContext& ctx;std::array<uint8_t,sizeof(PPCContext)> saved;Environment environment;
    explicit Context(PPCContext& value):ctx(value){std::memcpy(saved.data(),&ctx,sizeof(ctx));}
    ~Context(){std::memcpy(&ctx,saved.data(),sizeof(ctx));}};
struct Targets {Actor actor;uint32_t task=0,motion=0,physics=0,gravity=0,animation=0;};
struct EdgeState {Targets targets;uint64_t sequence=0,tick=0;bool held=false,failed=false;};
std::array<EdgeState,16> states{};
std::mutex state_mutex;
DWORD owner_thread=0;
struct Permit {uint64_t peer;const Command* command;};
thread_local const Permit* permit=nullptr;
// Caller holds state_mutex. Exact peer first, then a free row: a hole before
// an existing peer must never create duplicate edge streams after an exception.
auto StateSlot(uint64_t peer){
    auto slot=std::find_if(states.begin(),states.end(),[&](const EdgeState& value){return value.targets.actor.peer==peer;});
    return slot!=states.end()?slot:std::find_if(states.begin(),states.end(),[](const EdgeState& value){return !value.targets.actor.peer;});
}
bool Readable(uint32_t address,size_t size,uint8_t* base){return base && address>=4096 && uint64_t(address)+size<=0x100000000ull && em2::GuestReadBatch::Readable(base+address,size);}
bool Identity(const Actor& a,const Actor& b){return a.peer==b.peer && a.generation==b.generation && a.registry==b.registry && a.handle==b.handle && a.entity==b.entity && a.controller==b.controller && a.listener==b.listener && a.action_user==b.action_user;}
bool Identity(const Targets& a,const Targets& b){return Identity(a.actor,b.actor) && a.task==b.task && a.motion==b.motion && a.physics==b.physics && a.gravity==b.gravity && a.animation==b.animation;}
uint32_t Typed(const Actor& actor,uint32_t type,uint32_t adjustment,size_t bytes,uint8_t* base){
    const auto kinds=PPC_LOAD_U32(actor.entity+88),list=PPC_LOAD_U32(actor.entity+36);
    const auto kind=PPC_LOAD_U8(type);if(!Readable(kinds,unsigned(kind)+1,base))return 0;
    const auto slot=PPC_LOAD_U8(kinds+kind);if(slot==255 || !Readable(list,unsigned(slot)*4+4,base))return 0;
    const auto face=PPC_LOAD_U32(list+unsigned(slot)*4);
    return face>=adjustment && Readable(face-adjustment,bytes,base)?face-adjustment:0;
}
uint32_t JumpTask(const Actor& actor,uint8_t* base){
    const auto collection=actor.controller+156,buckets=PPC_LOAD_U32(collection+16),count=PPC_LOAD_U32(collection+20);
    if(!count || count>4096 || !Readable(buckets,size_t(count)*4,base))return 0;
    std::array<uint32_t,256> visited{};unsigned seen=0;uint32_t result=0;
    for(uint32_t i=0;i<count;++i){auto node=PPC_LOAD_U32(buckets+i*4);
        while(node){
            if(seen==visited.size() || !Readable(node,12,base) || std::find(visited.begin(),visited.begin()+seen,node)!=visited.begin()+seen)return 0;
            visited[seen++]=node;const auto name=PPC_LOAD_U32(node);
            if(Readable(name,5,base) && !std::memcmp(base+name,"Jump",5)){
                if(result)return 0;result=PPC_LOAD_U32(node+4);
            }
            node=PPC_LOAD_U32(node+8);
        }
    }
    const auto list=PPC_LOAD_U32(collection+36),listeners=PPC_LOAD_U32(collection+40);
    if(!result || listeners>256 || !Readable(list,size_t(listeners)*4,base))return 0;
    unsigned matches=0;for(unsigned i=0;i<listeners;++i)matches+=PPC_LOAD_U32(list+i*4)==result;
    return matches==1?result:0;
}
Result TargetsFor(uint64_t peer,const Command& command,PPCContext& ctx,uint8_t* base,Targets& out){
    if(permit)return Result::Nested;
    {std::lock_guard lock(state_mutex);if(owner_thread && owner_thread!=GetCurrentThreadId())return Result::WrongThread;}
    Actor actor;if(!em2::net::OwnedActorInputFor(peer,ctx,base,actor))return Result::Stale;
    if(!command.generation || command.generation!=actor.generation)return Result::Stale;
    if(!actor.enabled)return Result::Disabled;
    if(command.edge!=Edge::Pressed && command.edge!=Edge::Released)return Result::InvalidInput;
    if(!Readable(command.action.action,4,base) || !std::isfinite(command.action.x) || !std::isfinite(command.action.y) ||
       std::abs(command.action.x)>1 || std::abs(command.action.y)>1)return Result::InvalidInput;
    const auto uid=PPC_LOAD_U32(command.action.action),expected=PPC_LOAD_U32((command.edge==Edge::Pressed?0x833DAEBCu:0x833DAEC0u)+actor.action_user*12);
    if(!expected || uid!=expected)return Result::UnsupportedAction;
    for(unsigned offset:{280u,284u,288u,292u,296u,300u})if(PPC_LOAD_U32(actor.listener+offset)==uid)return Result::UnsupportedAction;
    const auto manager=PPC_LOAD_U32(0x833DAB60),profile=PPC_LOAD_U32(0x833DAD98);
    if(!Readable(manager,640,base) || !PPC_LOAD_U8(manager+357) || !Readable(profile,103,base) || PPC_LOAD_U8(profile+102) ||
       (PPC_LOAD_U8(actor.controller+68)&0x80))return Result::InvalidContext;
    const auto basic=Typed(actor,0x833DAE58,8,96,base),motion=Typed(actor,0x833DAF54,8,896,base),physics=Typed(actor,0x833DDF94,8,224,base);
    if(basic!=actor.controller+16 || !motion || !physics || PPC_LOAD_U32(motion+48)!=actor.entity-8 ||
       PPC_LOAD_U32(physics+48)!=actor.entity-8)return Result::InvalidContext;
    // These are the other per-entity components read/called by the original
    // Jump starter. Optional animation may be absent, but cannot be foreign.
    const auto gravity=Typed(actor,0x833DDF1C,8,96,base),animation=Typed(actor,0x833DCCF8,8,64,base);
    const auto animation_kind=PPC_LOAD_U8(0x833DCCF8);
    const auto type_list=PPC_LOAD_U32(actor.entity+88);
    if(!Readable(type_list,unsigned(animation_kind)+1,base))return Result::InvalidContext;
    const bool has_animation=PPC_LOAD_U8(type_list+animation_kind)!=255;
    if(!gravity || PPC_LOAD_U32(gravity+48)!=actor.entity-8 ||
       (has_animation && (!animation || PPC_LOAD_U32(animation+48)!=actor.entity-8)))return Result::InvalidContext;
    const auto task=JumpTask(actor,base);
    if(!Readable(task,44,base) || PPC_LOAD_U32(task)!=0x820BC838 || PPC_LOAD_U32(task+4)!=actor.entity ||
       PPC_LOAD_U32(0x820BC838+40)!=0x8287EA38 || PPC_LOAD_U32(0x820BC838+4)!=0x8287ECF8 || PPC_LOAD_U32(0x820BC838+8)!=0x8287D048)return Result::InvalidContext;
    out={actor,task,motion,physics,gravity,animation};return Result::Applied;
}
Result EdgeAgainst(const EdgeState* prior,const Targets& targets,const Command& command){
    const bool same=prior && prior->targets.actor.generation==targets.actor.generation;
    if(!command.sequence)return Result::InvalidTick;
    if(same){
        if(!Identity(prior->targets,targets))return Result::Stale;
        if(prior->failed)return Result::InvalidContext;
        if(prior->sequence==UINT64_MAX || command.sequence!=prior->sequence+1 || command.tick<prior->tick)return Result::InvalidTick;
        if((command.edge==Edge::Pressed)==prior->held)return Result::InvalidInput;
    }else{
        if(command.sequence!=1)return Result::InvalidTick;
        if(command.edge!=Edge::Pressed)return Result::InvalidInput;
    }
    return Result::Applied;
}
Result EdgeAdmission(const Targets& targets,const Command& command){
    std::lock_guard lock(state_mutex);
    const auto prior=std::find_if(states.begin(),states.end(),[&](const EdgeState& value){return value.targets.actor.peer==targets.actor.peer;});
    if(prior==states.end() && std::none_of(states.begin(),states.end(),[](const EdgeState& value){return !value.targets.actor.peer;}))return Result::InvalidContext;
    return EdgeAgainst(prior==states.end()?nullptr:&*prior,targets,command);
}
// Only a same-executable native fixture can replace the physics boundary. It
// observes a real original Jump-task stimulus; it is not a production body API.
struct Stimulus {uint32_t first=0,second=0;unsigned first_count=0,second_count=0;float height=0;bool throw_now=false;};
thread_local Stimulus* stimulus=nullptr;
}
bool em2::net::detail::OwnedJumpDispatchAuthorized(uint64_t peer,const Command& command) noexcept{return permit && permit->peer==peer && permit->command==&command;}
Result em2::net::detail::ValidateOwnedJumpTargets(uint64_t peer,const Command& command,PPCContext& ctx,uint8_t* base){
    // The input backend revalidates targets inside its private dispatch permit.
    const auto* previous=permit;permit=nullptr;
    struct Restore {const Permit* previous;~Restore(){permit=previous;}}restore{previous};
    Context preserve(ctx);Targets targets;return TargetsFor(peer,command,ctx,base,targets);
}
Result em2::net::ValidateOwnedActorJump(uint64_t peer,const Command& command,PPCContext& ctx,uint8_t* base){
    Context preserve(ctx);Targets targets;const auto result=TargetsFor(peer,command,ctx,base,targets);
    return result==Result::Applied?EdgeAdmission(targets,command):result;
}
Result em2::net::ValidateOwnedActorJumpBatch(uint64_t peer,std::span<const Command> commands,PPCContext& ctx,uint8_t* base){
    Context preserve(ctx);
    if(commands.size()>2)return Result::InvalidInput;
    if(commands.empty())return Result::Applied;
    std::optional<EdgeState> prior;
    {std::lock_guard lock(state_mutex);
        const auto slot=std::find_if(states.begin(),states.end(),[&](const EdgeState& value){return value.targets.actor.peer==peer;});
        if(slot!=states.end())prior=*slot;
        else if(std::none_of(states.begin(),states.end(),[](const EdgeState& value){return !value.targets.actor.peer;}))return Result::InvalidContext;
    }
    for(const auto& command:commands){
        if(command.tick!=commands.front().tick)return Result::InvalidTick;
        Targets targets;auto result=TargetsFor(peer,command,ctx,base,targets);
        if(result!=Result::Applied)return result;
        result=EdgeAgainst(prior?&*prior:nullptr,targets,command);if(result!=Result::Applied)return result;
        prior=EdgeState{targets,command.sequence,command.tick,command.edge==Edge::Pressed,false};
    }
    return Result::Applied;
}
Result em2::net::ReplayOwnedActorJump(uint64_t peer,const Command& command,PPCContext& ctx,uint8_t* base){
    Context preserve(ctx);Targets targets;auto result=TargetsFor(peer,command,ctx,base,targets);if(result!=Result::Applied)return result;
    result=EdgeAdmission(targets,command);if(result!=Result::Applied)return result;
    ObserveOwnedActorProbeJumpTargets(peer,targets.task,targets.motion,targets.physics,base);
    if(REXCVAR_GET(em2_private_profile)){
        Environment log_environment;
        REXLOG_INFO("Native Room owned Jump preflight: peer {} tick {} edge {} entity {:08X} task {:08X} flags {:08X} task bytes {}/{}/{} controller disabled {:02X} captured {} native state {} mode {}",
            peer,command.tick,unsigned(command.edge),targets.actor.entity,targets.task,PPC_LOAD_U32(targets.task+8),
            PPC_LOAD_U8(targets.task+16),PPC_LOAD_U8(targets.task+17),PPC_LOAD_U8(targets.task+18),
            PPC_LOAD_U8(targets.actor.controller+68),PPC_LOAD_U8(targets.actor.listener+620),PPC_LOAD_U32(targets.motion+384),PPC_LOAD_U32(targets.motion+208));
        if(command.edge==OwnedJumpEdge::Pressed)LogNativeActorTaskInventory(targets.actor.entity,base);
        for(unsigned slot=0;slot<2;++slot){
            RetailActorInput retail;uint32_t flags=0;
            if(RetailActorInputFor(slot,ctx,base,retail) && ReadNativeJumpTaskFlags(retail.entity,base,flags))
                REXLOG_INFO("Native Room retail Jump comparison: tick {} slot {} entity {:08X} flags {:08X}",command.tick,slot,retail.entity,flags);
        }
    }
    // Native backend checks staged tick readiness before invoking the handler.
    // Claim edge state only for a callback that actually entered native input.
    const Permit next{peer,&command};const auto* previous=permit;permit=&next;
    struct Scope {const Permit* saved;~Scope(){permit=saved;}}scope{previous};
    try{result=detail::DispatchOwnedActorJump(peer,command,ctx,base);}
    catch(...){
        std::lock_guard lock(state_mutex);auto slot=StateSlot(peer);
        if(slot!=states.end())*slot={targets,command.sequence,command.tick,command.edge==Edge::Pressed,true};
        owner_thread=GetCurrentThreadId();throw;
    }
    if(result!=Result::Applied)return result;
    if(REXCVAR_GET(em2_private_profile)){
        Environment log_environment;
        REXLOG_INFO("Native Room owned Jump returned: peer {} tick {} edge {} flags {:08X} native state {} mode {}",
            peer,command.tick,unsigned(command.edge),PPC_LOAD_U32(targets.task+8),PPC_LOAD_U32(targets.motion+384),PPC_LOAD_U32(targets.motion+208));
    }
    std::lock_guard lock(state_mutex);auto slot=StateSlot(peer);
    if(slot==states.end())return Result::InvalidContext;
    *slot={targets,command.sequence,command.tick,command.edge==Edge::Pressed,false};owner_thread=GetCurrentThreadId();return Result::Applied;
}
bool em2::net::ReadNativeJumpTaskFlags(uint32_t entity,uint8_t* base,uint32_t& flags){
    Environment preserve;em2::GuestReadBatch reads;
    if(!Readable(entity,96,base) || PPC_LOAD_U32(entity)!=0x8208D908)return false;
    Actor actor;actor.entity=entity;actor.controller=Typed(actor,0x833E2188,24,800,base);actor.listener=actor.controller+80;
    if(!actor.controller || PPC_LOAD_U32(actor.controller+24)!=0x8211DCE0 || PPC_LOAD_U32(actor.controller+64)!=entity-8)return false;
    const auto task=JumpTask(actor,base);
    if(!Readable(task,104,base) || PPC_LOAD_U32(task)!=0x820BC838 || PPC_LOAD_U32(task+4)!=entity)return false;
    flags=PPC_LOAD_U32(task+8);return true;
}
void em2::net::LogNativeActorTaskInventory(uint32_t entity,uint8_t* base){
    Environment preserve;em2::GuestReadBatch reads;
    if(!Readable(entity,96,base) || PPC_LOAD_U32(entity)!=0x8208D908)return;
    Actor actor;actor.entity=entity;actor.controller=Typed(actor,0x833E2188,24,800,base);
    if(!actor.controller || PPC_LOAD_U32(actor.controller+24)!=0x8211DCE0 || PPC_LOAD_U32(actor.controller+64)!=entity-8)return;
    const auto fsm=PPC_LOAD_U32(actor.controller+104);
    if(Readable(fsm,32,base)){
        REXLOG_INFO("Owned actor FSM inventory: entity {:08X} controller {:08X} vtable {:08X} query {:08X} lookup {:08X} words {:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}",
            entity,actor.controller,fsm,PPC_LOAD_U32(fsm+20),PPC_LOAD_U32(fsm+28),
            PPC_LOAD_U32(actor.controller+108),PPC_LOAD_U32(actor.controller+112),PPC_LOAD_U32(actor.controller+116),PPC_LOAD_U32(actor.controller+120),
            PPC_LOAD_U32(actor.controller+124),PPC_LOAD_U32(actor.controller+128),PPC_LOAD_U32(actor.controller+132),PPC_LOAD_U32(actor.controller+136),
            PPC_LOAD_U32(actor.controller+140),PPC_LOAD_U32(actor.controller+144),PPC_LOAD_U32(actor.controller+148),PPC_LOAD_U32(actor.controller+152));
        // 831C1F28 resolves an integer state key through the inline map+12.
        // Walk its bounded nodes without invoking any FSM query or transition.
        const auto slots=PPC_LOAD_U32(actor.controller+132),slot_count=PPC_LOAD_U32(actor.controller+136);
        if(slot_count && slot_count<=256 && Readable(slots,size_t(slot_count)*4,base)){
            std::array<uint32_t,256> state_nodes{};unsigned state_count=0;
            for(unsigned bucket=0;bucket<slot_count;++bucket){auto node=PPC_LOAD_U32(slots+bucket*4);
                while(node){
                    if(state_count==state_nodes.size() || !Readable(node,12,base) || std::find(state_nodes.begin(),state_nodes.begin()+state_count,node)!=state_nodes.begin()+state_count)return;
                    state_nodes[state_count++]=node;
                    const auto key=PPC_LOAD_U32(node),object=PPC_LOAD_U32(node+4);
                    if((key==PPC_LOAD_U32(actor.controller+108) || key==PPC_LOAD_U32(actor.controller+112)) && Readable(object,8,base)){
                        const auto table=PPC_LOAD_U32(object);
                        if(Readable(table,44,base))REXLOG_INFO("Owned actor FSM state: entity {:08X} index {} object {:08X} vtable {:08X} input {:08X} data {:08X}",entity,key,object,table,PPC_LOAD_U32(table+40),PPC_LOAD_U32(object+4));
                    }
                    node=PPC_LOAD_U32(node+8);
                }
            }
        }
    }
    const auto collection=actor.controller+156;
    const auto buckets=PPC_LOAD_U32(collection+16),count=PPC_LOAD_U32(collection+20);
    if(!count || count>4096 || !Readable(buckets,size_t(count)*4,base))return;
    std::array<uint32_t,256> visited{};unsigned seen=0;
    for(uint32_t i=0;i<count;++i){auto node=PPC_LOAD_U32(buckets+i*4);
        while(node){
            if(seen==visited.size() || !Readable(node,12,base) || std::find(visited.begin(),visited.begin()+seen,node)!=visited.begin()+seen)return;
            visited[seen++]=node;
            const auto name=PPC_LOAD_U32(node),task=PPC_LOAD_U32(node+4);
            std::array<char,97> label{};unsigned length=0;
            if(name>=4096)for(;length<96;++length){
                if(uint64_t(name)+length>=0x100000000ull || !Readable(name+length,1,base))break;
                const auto ch=PPC_LOAD_U8(name+length);if(!ch)break;
                label[length]=ch>=32 && ch<127?char(ch):'?';
            }
            if(Readable(task,12,base) && PPC_LOAD_U32(task+4)==entity){
                const auto vtable=PPC_LOAD_U32(task);
                const auto handler=Readable(vtable,44,base)?PPC_LOAD_U32(vtable+40):0;
                REXLOG_INFO("Owned actor task inventory: entity {:08X} name '{}' task {:08X} flags {:08X} vtable {:08X} input {:08X}",entity,label.data(),task,PPC_LOAD_U32(task+8),vtable,handler);
            }
            node=PPC_LOAD_U32(node+8);
        }
    }
}
bool em2::net::ForgetOwnedActorJump(uint64_t peer,uint64_t generation,PPCContext& ctx,uint8_t* base){
    Context preserve(ctx);{std::lock_guard lock(state_mutex);if(owner_thread && owner_thread!=GetCurrentThreadId())return false;}
    if(!peer || !generation || permit)return false;Actor live;if(OwnedActorInputFor(peer,ctx,base,live) && live.generation==generation)return false;
    std::lock_guard lock(state_mutex);for(auto& state:states)if(state.targets.actor.peer==peer && state.targets.actor.generation==generation){state={};return true;}return true;
}
PPC_FUNC_IMPL(sub_82884018){
    if(em2::net::RetailJumpFixtureStimulus(ctx,base))return;
    if(stimulus && (ctx.lr==0x8287E7C0 || ctx.lr==0x8287E814)){
        if(ctx.r3.u32==stimulus->first)++stimulus->first_count;else if(ctx.r3.u32==stimulus->second)++stimulus->second_count;
        else throw std::logic_error("native owned jump targeted a foreign physics component");
        stimulus->height=float(ctx.f1.f64);if(stimulus->throw_now)throw std::runtime_error("native jump stimulus unwind");return;
    }
    em2::net::ObserveOwnedActorProbeJumpStimulus(ctx,base);
    __imp__sub_82884018(ctx,base);
}
namespace {
void JumpFixtureZero(PPCContext& ctx,uint8_t*){ctx.r3.u64=0;}
void JumpFixtureProperties(PPCContext& ctx,uint8_t* base){
    const float result=ctx.lr==0x827868D4?12.f:1.f;
    PPC_STORE_U32(ctx.r5.u32,std::bit_cast<uint32_t>(result));ctx.r3.u64=1;
}
}
bool em2::net::TestOwnedActorJumps(PPCContext& ctx,uint8_t* base,uint64_t first_peer,uint64_t second_peer,void(*native_update)(PPCContext&,uint8_t*)){
    Context preserve_test(ctx);
    struct RestoreState {decltype(states) saved=states;DWORD thread=owner_thread;const Permit* previous=permit;Stimulus* capture=stimulus;
        ~RestoreState(){states=saved;owner_thread=thread;permit=previous;stimulus=capture;}}restore_state;
    states={};owner_thread=0;permit=nullptr;stimulus=nullptr;
    Actor a,b;if(!OwnedActorInputFor(first_peer,ctx,base,a) || !OwnedActorInputFor(second_peer,ctx,base,b))return false;
    if(!VirtualAlloc(base+0x50000,0x20000,MEM_COMMIT,PAGE_READWRITE))return false;
    struct Decommit {uint8_t* base;~Decommit(){VirtualFree(base+0x50000,0x20000,MEM_DECOMMIT);}}decommit{base};
    std::array<Actor,2> actors{a,b};
    std::array<uint32_t,2> types{},lists{},motions{};
    struct GuestRestore {
        struct Bytes {uint32_t address=0;std::array<uint8_t,1024> bytes{};size_t size=0;};
        uint8_t* base;std::array<Bytes,8> saved{};unsigned count=0;
        void Save(uint32_t address,size_t size){auto& entry=saved[count++];entry.address=address;entry.size=size;std::memcpy(entry.bytes.data(),base+address,size);}
        ~GuestRestore(){for(unsigned i=0;i<count;++i)std::memcpy(base+saved[i].address,saved[i].bytes.data(),saved[i].size);}
    }restore_guest{base};
    std::array<uint32_t,12> globals{0x833DAEBC,0x833DAEC0,0x833DDF1C,0x833DDF94,0x833DCCF8,0x833DA6F4,0x833DAE58,0x820BC838+68,0x8203D278,0x833DAEC4,0x833DAEC8,0x833DAECC};
    std::array<uint32_t,12> previous_globals{};for(unsigned i=0;i<globals.size();++i)previous_globals[i]=PPC_LOAD_U32(globals[i]);
    struct GlobalRestore {uint8_t* base;const decltype(globals)& addresses;const decltype(previous_globals)& values;
        ~GlobalRestore(){for(unsigned i=0;i<addresses.size();++i)PPC_STORE_U32(addresses[i],values[i]);}}restore_globals{base,globals,previous_globals};
    struct DispatchRestore {struct Entry{uint32_t address=0;PPCFunc* original=nullptr;};uint8_t* base;std::array<Entry,8> saved{};unsigned count=0;
        bool Map(uint32_t address,PPCFunc* function){auto* pointer=&PPC_LOOKUP_FUNC(base,address);const auto page=reinterpret_cast<uintptr_t>(pointer)&~uintptr_t(4095);
            if(!VirtualAlloc(reinterpret_cast<void*>(page),4096,MEM_COMMIT,PAGE_READWRITE))return false;saved[count++]={address,*pointer};*pointer=function;return true;}
        ~DispatchRestore(){for(unsigned i=0;i<count;++i)PPC_LOOKUP_FUNC(base,saved[i].address)=saved[i].original;}
    }restore_dispatch{base};
    if(!restore_dispatch.Map(0x8287EA38,__imp__sub_8287EA38) || !restore_dispatch.Map(0x8287ECF8,__imp__sub_8287ECF8) ||
       !restore_dispatch.Map(0x8287D048,__imp__sub_8287D048) || !restore_dispatch.Map(0x8287ED00,__imp__sub_8287ED00) ||
       !restore_dispatch.Map(uint32_t(PPC_CODE_BASE)+64,JumpFixtureProperties) || !restore_dispatch.Map(uint32_t(PPC_CODE_BASE)+68,JumpFixtureZero))return false;
    constexpr uint32_t properties_vt=0x66000,fsm_vt=0x66100,event_manager=0x65000,press=0x66200,release=0x66204;
    PPC_STORE_U32(properties_vt+212,uint32_t(PPC_CODE_BASE)+64);PPC_STORE_U32(fsm_vt+20,uint32_t(PPC_CODE_BASE)+68);PPC_STORE_U32(fsm_vt+28,uint32_t(PPC_CODE_BASE)+68);
    // Controller completion is now exercised too. Preserve the outer fixture's
    // native-frame continuation sink rather than leaving FSM update+12 null.
    const auto controller_fsm=PPC_LOAD_U32(a.controller+104);
    if(!Readable(controller_fsm,16,base) || !PPC_LOAD_U32(controller_fsm+12))return false;
    PPC_STORE_U32(fsm_vt+12,PPC_LOAD_U32(controller_fsm+12));
    // Keep ORIGINAL task flag notification construction/destruction. Its optional
    // display name is null only in this private image, matching the native empty
    // string singleton so notification copies never enter the uninitialized
    // headless kernel string pool. Restore both image values on every exit.
    PPC_STORE_U32(0x820BC838+68,uint32_t(PPC_CODE_BASE)+68);
    PPC_STORE_U32(0x8203D278,0);
    // Original native event manager's bounded guard stops external notifications
    // after native task flag changes, notification construction and jump stimulus.
    PPC_STORE_U32(event_manager+1008,10);PPC_STORE_U32(0x833DA6F4,event_manager);
    PPC_STORE_U32(0x833DAEBC,0x8207F5B0);PPC_STORE_U32(0x833DAEC0,0x8207F5B8);
    PPC_STORE_U32(0x833DAEC4,0x8207F5C4);PPC_STORE_U32(0x833DAEC8,0x820BC794);PPC_STORE_U32(0x833DAECC,0x820BC7A8);
    PPC_STORE_U8(0x833DDF1C,12);PPC_STORE_U8(0x833DDF94,13);PPC_STORE_U8(0x833DCCF8,14);PPC_STORE_U8(0x833DAE58,6);
    PPC_STORE_U32(press,0x8207F5B0);PPC_STORE_U32(release,0x8207F5B8);
    PPC_STORE_U32(press+8,0x820BC794);PPC_STORE_U32(release+8,0x820BC7A8);
    for(unsigned i=0;i<2;++i){
        const auto actor=actors[i];types[i]=PPC_LOAD_U32(actor.entity+88);lists[i]=PPC_LOAD_U32(actor.entity+36);motions[i]=Typed(actor,0x833DAF54,8,896,base);
        if(!motions[i])return false;
        restore_guest.Save(actor.controller,800);restore_guest.Save(types[i],256);restore_guest.Save(lists[i],64);restore_guest.Save(motions[i],1024);
        const uint32_t task=0x50000u+i*0x1000,gravity=0x52000u+i*0x1000,physics=0x54000u+i*0x1000,config=0x56000u+i*0x1000;
        const auto node=task+256,buckets=task+272,array=task+288;
        PPC_STORE_U32(task,0x820BC838);PPC_STORE_U32(task+4,actor.entity);PPC_STORE_U32(task+8,0);PPC_STORE_U8(task+17,1);PPC_STORE_U8(task+18,1);
        PPC_STORE_U32(node,0x8207F5B0);PPC_STORE_U32(node+4,task);PPC_STORE_U32(buckets,node);PPC_STORE_U32(array,task);
        PPC_STORE_U32(actor.controller+156+16,buckets);PPC_STORE_U32(actor.controller+156+20,1);
        PPC_STORE_U32(actor.controller+156+36,array);PPC_STORE_U32(actor.controller+156+40,1);PPC_STORE_U32(actor.controller+104,fsm_vt);
        PPC_STORE_U8(types[i]+6,0);PPC_STORE_U8(types[i]+12,2);PPC_STORE_U8(types[i]+13,3);PPC_STORE_U8(types[i]+14,255);
        PPC_STORE_U32(lists[i]+8,gravity+8);PPC_STORE_U32(lists[i]+12,physics+8);
        PPC_STORE_U32(gravity+8,properties_vt);PPC_STORE_U32(gravity+48,actor.entity-8);PPC_STORE_U32(gravity+72,std::bit_cast<uint32_t>(9.8f));
        PPC_STORE_U32(physics+48,actor.entity-8);PPC_STORE_U32(config+8,properties_vt);PPC_STORE_U32(motions[i]+120,config);
        PPC_STORE_U32(motions[i]+384,0);PPC_STORE_U32(motions[i]+208,0);PPC_STORE_U32(motions[i]+876,std::bit_cast<uint32_t>(1.f));
    }
    const auto manager=PPC_LOAD_U32(0x833DAB60);std::array<uint8_t,640> retail;std::memcpy(retail.data(),base+manager,640);
    unsigned checks=0;auto check=[&](bool value,const char* why){++checks;if(!value)REXLOG_ERROR("Owned actor jump: {}",why);return value;};
    uint64_t tick=2000000;
    auto prepare=[&](const Actor& actor){ActorLookBasis basis{actor.peer,actor.generation,tick};
        if(PrepareOwnedActorBasis(actor.peer,basis,ctx,base)!=Result::Applied)return false;
        NativeActorAction neutral{0,0x833DAEB8+actor.action_user*12,0,0};return ReplayOwnedActorMovement(actor.peer,neutral,tick,ctx,base)==Result::Applied;};
    auto command=[&](const Actor& actor,Edge edge,uint64_t sequence){return Command{actor.generation,tick,sequence,edge,{0,(edge==Edge::Pressed?press:release)+actor.action_user*8,1,0}};};
    Stimulus observed{0x54000,0x55000};stimulus=&observed;
    if(!BeginOwnedActorBasisTick(a.registry,tick,ctx,base))return false;
    auto first=command(a,Edge::Pressed,1),second=command(b,Edge::Pressed,1);
    if(b.action_user==1){auto wrong=second;wrong.action.action=press;
        if(!check(ValidateOwnedActorJump(b.peer,wrong,ctx,base)==Result::UnsupportedAction &&
            ReplayOwnedActorJump(b.peer,wrong,ctx,base)==Result::UnsupportedAction && !observed.second_count,
            "secondary owned action profile rejects primary Jump before consuming an edge or delivering a native stimulus"))return false;}
    if(!check(ReplayOwnedActorJump(a.peer,first,ctx,base)==Result::InvalidTick && observed.first_count==0,
        "Jump requires an admitted current Movement/basis stage before native input delivery"))return false;
    std::array<Command,2> batch{first,command(a,Edge::Released,2)};
    auto invalid_batch=batch;invalid_batch[1].sequence=3;
    if(!check(ValidateOwnedActorJumpBatch(a.peer,batch,ctx,base)==Result::Applied &&
        ValidateOwnedActorJumpBatch(a.peer,invalid_batch,ctx,base)==Result::InvalidTick &&
        ValidateOwnedActorJump(a.peer,first,ctx,base)==Result::Applied && observed.first_count==0 &&
        !(PPC_LOAD_U32(0x50000+8)&16),
        "whole ordered edge batch preflight rejects a later sequence gap without claiming its valid first edge"))return false;
    invalid_batch=batch;invalid_batch[1].edge=Edge::Pressed;invalid_batch[1].action.action=press;
    if(!check(ValidateOwnedActorJumpBatch(a.peer,invalid_batch,ctx,base)==Result::InvalidInput &&
        ValidateOwnedActorJumpBatch(a.peer,batch,ctx,base)==Result::Applied,
        "batch preflight enforces alternating edges using private history without mutating real history"))return false;
    if(!prepare(a) || !prepare(b))return false;
    constexpr auto padding_begin=offsetof(PPCContext,vscr_sat)+sizeof(ctx.vscr_sat),padding_end=offsetof(PPCContext,f0);
    static_assert(padding_end>padding_begin);
    std::memset(reinterpret_cast<uint8_t*>(&ctx)+padding_begin,0xA7,padding_end-padding_begin);
    std::array<uint8_t,sizeof(PPCContext)> incoming;std::memcpy(incoming.data(),&ctx,sizeof(ctx));
    const auto csr=_mm_getcsr();const auto error=GetLastError();
    if(!check(ValidateOwnedActorJump(a.peer,first,ctx,base)==Result::Applied && ValidateOwnedActorJump(a.peer,first,ctx,base)==Result::Applied &&
        ReplayOwnedActorJump(a.peer,first,ctx,base)==Result::Applied && observed.first_count==1 && observed.second_count==0 && std::abs(observed.height-12.f)<1e-6f &&
        (PPC_LOAD_U32(0x50000+8)&16) && !(PPC_LOAD_U32(0x51000+8)&16) && !std::memcmp(&ctx,incoming.data(),sizeof(ctx)) && _mm_getcsr()==csr && GetLastError()==error,
        "ORIGINAL derived input, task dispatch, Jump start and native physics stimulus affect only the intended owned actor"))return false;
    if(!check(ReplayOwnedActorJump(b.peer,second,ctx,base)==Result::Applied && observed.second_count==1 &&
        ReplayOwnedActorJump(a.peer,first,ctx,base)==Result::InvalidTick,"two extras target independent native physics components and duplicate edges reject"))return false;
    auto held=command(a,Edge::Pressed,2);
    if(!check(ReplayOwnedActorJump(a.peer,held,ctx,base)==Result::InvalidInput && observed.first_count==1,
        "held buttons cannot become repeated native Jump presses"))return false;
    auto up=command(a,Edge::Released,2);
    if(!check(ReplayOwnedActorJump(a.peer,up,ctx,base)==Result::Applied && !(PPC_LOAD_U32(0x50000+8)&16) && observed.first_count==1,
        "original JumpRelease dispatch clears the native Jump task active bit without synthetic velocity changes"))return false;
    auto gap=command(a,Edge::Pressed,4);
    if(!check(ReplayOwnedActorJump(a.peer,gap,ctx,base)==Result::InvalidTick,"missing or reordered native input edges cannot be silently skipped"))return false;
    auto stale=command(a,Edge::Pressed,3);++stale.generation;
    if(!check(ReplayOwnedActorJump(a.peer,stale,ctx,base)==Result::Stale,"queued input from another native actor generation cannot retarget a live extra"))return false;
    auto mismatched=command(a,Edge::Released,3);mismatched.action.action=press;
    if(!check(ReplayOwnedActorJump(a.peer,mismatched,ctx,base)==Result::UnsupportedAction,"typed edge must carry the matching exact native action UID"))return false;
    auto next=command(a,Edge::Pressed,3);
    const auto task_owner=PPC_LOAD_U32(0x50000+4);PPC_STORE_U32(0x50000+4,b.entity);
    const auto foreign_task=ReplayOwnedActorJump(a.peer,next,ctx,base);PPC_STORE_U32(0x50000+4,task_owner);
    const auto body_owner=PPC_LOAD_U32(0x54000+48);PPC_STORE_U32(0x54000+48,b.entity-8);
    const auto foreign_body=ReplayOwnedActorJump(a.peer,next,ctx,base);PPC_STORE_U32(0x54000+48,body_owner);
    const auto animation_face=PPC_LOAD_U32(lists[0]+16);PPC_STORE_U8(types[0]+14,4);PPC_STORE_U32(lists[0]+16,4);
    const auto invalid_animation=ReplayOwnedActorJump(a.peer,next,ctx,base);PPC_STORE_U8(types[0]+14,255);PPC_STORE_U32(lists[0]+16,animation_face);
    if(!check(foreign_task==Result::InvalidContext && foreign_body==Result::InvalidContext && invalid_animation==Result::InvalidContext && observed.first_count==1,
        "stale/shared task or physics ownership and malformed present animation cannot route native Jump effects"))return false;
    PPC_STORE_U32(0x50000+8,1);
    if(!check(ReplayOwnedActorJump(a.peer,next,ctx,base)==Result::Applied && observed.first_count==1 && PPC_LOAD_U32(0x50000+8)==1,
        "native task disable count remains authoritative; no ability is force-enabled"))return false;
    auto disabled_release=command(a,Edge::Released,4);if(ReplayOwnedActorJump(a.peer,disabled_release,ctx,base)!=Result::Applied)return false;
    PPC_STORE_U32(0x50000+8,0);
    ++tick;if(!BeginOwnedActorBasisTick(a.registry,tick,ctx,base) || !prepare(a))return false;
    auto failed=command(a,Edge::Pressed,5);observed.throw_now=true;bool thrown=false;
    std::array<uint8_t,sizeof(PPCContext)> exception_before;std::memcpy(exception_before.data(),&ctx,sizeof(ctx));
    try{ReplayOwnedActorJump(a.peer,failed,ctx,base);}catch(const std::runtime_error&){thrown=true;}observed.throw_now=false;
    if(!check(thrown && !permit && !std::memcmp(&ctx,exception_before.data(),sizeof(ctx)) && ReplayOwnedActorJump(a.peer,failed,ctx,base)==Result::InvalidContext,
        "native callback exception restores the permit/caller and faults that actor edge stream instead of replaying partial effects"))return false;
    if(!check(!ForgetOwnedActorJump(0,0,ctx,base) && !ForgetOwnedActorJump(a.peer,0,ctx,base) &&
        !ForgetOwnedActorJump(a.peer,a.generation,ctx,base) && !std::memcmp(retail.data(),base+manager,640),
        "unassigned/live ownership cannot discard held-edge history and original retail records remain unchanged"))return false;
    // Reuse the exact original native task fixture for actual TCP delivery.
    // The outer input suite owns/restores the basis clock; advance monotonically.
    states={};PPC_STORE_U32(0x50000+8,0);PPC_STORE_U32(0x51000+8,0);
    const auto before_first=observed.first_count,before_second=observed.second_count;
    if(!check(TestOwnedJumpMovementFrames(ctx,base,a.peer,b.peer,native_update) && observed.first_count==before_first+2 &&
        observed.second_count==before_second+1 && !(PPC_LOAD_U32(0x50000+8)&16) && !(PPC_LOAD_U32(0x51000+8)&16),
        "actual TCP frames trigger only two/one original per-peer Jump physics stimuli, held input does not repeat, releases clear native task flags"))return false;
    REXLOG_INFO("Owned actor Jump passed {} original input/task/physics-stimulus and edge lifecycle checks; no body trajectory/animation/live-control assertion",checks);
    return true;
}
