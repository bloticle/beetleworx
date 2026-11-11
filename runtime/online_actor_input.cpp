#include "online_actor_input.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/logging.h>
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
PPC_EXTERN_FUNC(__imp__sub_82732A18);
PPC_EXTERN_FUNC(__imp__sub_827B58D0);
PPC_EXTERN_FUNC(__imp__sub_82A69948);

namespace {
using Actor=em2::net::OwnedActorInput;
using Result=em2::net::ActorInputResult;
using Action=em2::net::NativeActorAction;
std::mutex mutex;
std::array<Actor,16> actors{};
DWORD owner_thread=0;
uint64_t generation=0;
struct Environment {unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(csr);SetLastError(error);}};
struct ContextScope {PPCContext& ctx;PPCContext saved;Environment environment;explicit ContextScope(PPCContext& c):ctx(c),saved(c){}~ContextScope(){ctx=saved;}};
bool Readable(uint32_t address,size_t size,uint8_t* base){return base && address>=4096 && uint64_t(address)+size<=0x100000000ull && em2::GuestReadBatch::Readable(base+address,size);}
std::optional<Actor> ByPeer(uint64_t peer){std::lock_guard lock(mutex);for(const auto& actor:actors)if(actor.peer==peer && peer)return actor;return {};}
std::optional<Actor> ByListener(uint32_t listener){std::lock_guard lock(mutex);for(const auto& actor:actors)if(actor.peer && actor.listener==listener)return actor;return {};}
bool Owner(){std::lock_guard lock(mutex);return owner_thread && owner_thread==GetCurrentThreadId();}
bool Resolve(uint32_t registry,uint32_t handle,PPCContext& ctx,uint8_t* base,uint32_t& entity){
    ContextScope restore(ctx);const auto top=ctx.r1.u32&~15u;
    if(!registry || !handle || top<8192 || !Readable(registry,24,base) || !Readable(top-4096,4096,base))return false;
    std::array<uint8_t,4096> saved;std::memcpy(saved.data(),base+top-4096,saved.size());
    struct Scratch {uint8_t* destination;const std::array<uint8_t,4096>& saved;~Scratch(){std::memcpy(destination,saved.data(),saved.size());}}scratch{base+top-4096,saved};
    const auto key=top-512;PPC_STORE_U32(key,handle);ctx.r1.u64=top-2048;ctx.r3.u64=registry;ctx.r4.u64=key;
    __imp__sub_82732A18(ctx,base);entity=ctx.r3.u32;return entity!=0;
}
bool Inspect(Actor& actor,PPCContext& ctx,uint8_t* base){
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
Result Admission(uint64_t peer,const Action& action,const em2::net::ActorLookBasis& basis,PPCContext& ctx,uint8_t* base,Actor& actor){
    auto found=ByPeer(peer);if(!found)return Result::Unregistered;actor=*found;
    if(!Owner())return Result::WrongThread;
    if(!Current(actor,ctx,base))return Result::Stale;
    if(!actor.enabled)return Result::Disabled;
    if(replay)return Result::Nested;
    if(!Readable(action.action,4,base) || !std::isfinite(action.x) || !std::isfinite(action.y) || std::abs(action.x)>1 || std::abs(action.y)>1)return Result::InvalidInput;
    if(!BasisValid(actor,basis))return Result::InvalidBasis;
    return Result::Applied;
}
// Test-only ingress to prove original native callback isolation before a
// production motion/basis route exists. It is not exposed in the public API.
Result NativeCallbackFixture(uint64_t peer,const Action& action,const em2::net::ActorLookBasis& basis,PPCContext& ctx,uint8_t* base,PPCFunc* callback){
    ContextScope preserve(ctx);Actor actor;const auto admitted=Admission(peer,action,basis,ctx,base,actor);
    if(admitted!=Result::Applied)return admitted;
    Replay state{actor};ReplayScope scoped(state);
    ctx.r3.u64=actor.listener;ctx.r4.u64=action.event;ctx.r5.u64=action.action;ctx.f1.f64=action.x;ctx.f2.f64=action.y;
    callback(ctx,base);return Result::Applied;
}
}
bool em2::net::RegisterOwnedActorInput(uint64_t peer,uint32_t handle,PPCContext& ctx,uint8_t* base,Actor& output){
    Environment preserve;output={};if(!peer || !base)return false;
    {std::lock_guard lock(mutex);if(owner_thread && owner_thread!=GetCurrentThreadId())return false;}
    Actor candidate;candidate.peer=peer;candidate.handle=handle;candidate.registry=PPC_LOAD_U32(0x833DA7E4);
    if(!Inspect(candidate,ctx,base))return false;
    std::lock_guard lock(mutex);
    for(const auto& actor:actors)if(actor.peer && (actor.peer==peer || actor.handle==handle || actor.entity==candidate.entity || actor.controller==candidate.controller || actor.listener==candidate.listener))return false;
    const auto free=std::find_if(actors.begin(),actors.end(),[](const Actor& value){return !value.peer;});if(free==actors.end())return false;
    owner_thread=GetCurrentThreadId();candidate.generation=++generation;candidate.enabled=true;*free=candidate;output=candidate;return true;
}
bool em2::net::SetOwnedActorInputEnabled(uint64_t peer,bool enabled){std::lock_guard lock(mutex);if(owner_thread!=GetCurrentThreadId())return false;for(auto& actor:actors)if(actor.peer==peer && peer){actor.enabled=enabled;return true;}return false;}
void em2::net::UnregisterOwnedActorInput(uint64_t peer){std::lock_guard lock(mutex);if(owner_thread!=GetCurrentThreadId())return;for(auto& actor:actors)if(actor.peer==peer && peer)actor={};}
void em2::net::ResetOwnedActorInputs(){std::lock_guard lock(mutex);if(owner_thread && owner_thread!=GetCurrentThreadId())return;actors={};owner_thread=0;}
bool em2::net::OwnedActorInputFor(uint64_t peer,PPCContext& ctx,uint8_t* base,Actor& output){Environment preserve;output={};auto found=ByPeer(peer);if(!found || !Owner() || !Current(*found,ctx,base))return false;output=*found;return true;}
em2::net::ActorInputResult em2::net::ReplayOwnedActorAction(uint64_t peer,const Action& action,const ActorLookBasis& basis,PPCContext& ctx,uint8_t* base){
    Environment preserve;Actor actor;const auto admitted=Admission(peer,action,basis,ctx,base,actor);
    if(admitted!=Result::Applied)return admitted;
    // Even an orthonormal peer basis is NOT permission to let the later native
    // NavigationInput task resolve its view through a retail two-slot camera.
    // No audited basis consumer exists yet: leave native action caches intact.
    return Result::BasisRouteUnverified;
}
bool em2::net::SuppressOwnedActorInput(PPCContext& ctx,uint8_t* base){if(!Quarantine(ctx,base))return false;ctx.r3.u64=0;return true;}
PPC_FUNC_IMPL(sub_8276B968){
    // Only this exact base human movement query, while replaying this actor,
    // can resolve to its owned entity. All normal native routes stay untouched.
    if(replay && ctx.lr==0x82783EB0 && ctx.r29.u32==replay->actor.listener && ctx.r3.u32==PPC_LOAD_U32(0x833DAB60) && ctx.r4.u32<2 && Current(replay->actor,ctx,base)){
        ctx.r3.u64=replay->actor.entity;return;
    }
    __imp__sub_8276B968(ctx,base);
}

namespace {
void FixtureBase(PPCContext& ctx,uint8_t* base){if(!em2::net::SuppressOwnedActorInput(ctx,base))__imp__sub_82783CE8(ctx,base);}
void FixtureMickey(PPCContext& ctx,uint8_t* base){if(!em2::net::SuppressOwnedActorInput(ctx,base))__imp__sub_82A3C930(ctx,base);}
void ThrowAfterBase(PPCContext& ctx,uint8_t* base){FixtureBase(ctx,base);throw std::runtime_error("owned actor fixture unwind");}
}
bool em2::net::TestOwnedActorInput(){
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
    struct Restore {uint8_t* base;decltype(actors) prior;DWORD thread;uint64_t serial;const Replay* scope=replay;Environment environment;
        explicit Restore(uint8_t* memory):base(memory){std::lock_guard lock(mutex);prior=actors;thread=owner_thread;serial=generation;actors={};owner_thread=0;}
        ~Restore(){std::lock_guard lock(mutex);actors=prior;owner_thread=thread;generation=serial;replay=scope;if(base)VirtualFree(base,0,MEM_RELEASE);}
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
    REXLOG_INFO("Owned actor input passed {} native registry/controller/quarantine and original callback checks: opposite extra actor inputs, unchanged retail sentinels, stale handle/world rejection, disabled input, typed basis rejection, nesting/exception and untouched retail path; production replay BLOCKED until per-peer NativeMotion basis and individual death routing are audited; not sixteen-player gameplay",checks);
    return true;
}
