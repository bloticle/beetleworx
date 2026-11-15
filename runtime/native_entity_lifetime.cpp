#include "native_entity_lifetime.h"
#include "guest_read_batch.h"
#include "simulation.h"
#include "native_projectile_impact.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <atomic>
#include <array>
#include <cstring>
#include <thread>
#include <limits>
#include <string>
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(bool,em2_probe_room_world_damage_events);

PPC_EXTERN_FUNC(__imp__sub_82747E08); // Registry inserts handle/entity maps.
PPC_EXTERN_FUNC(__imp__sub_827448B0); // Registry removes both maps.
PPC_EXTERN_FUNC(__imp__sub_82744E90); // Calls teardown before queuing NiPointer.
PPC_EXTERN_FUNC(__imp__sub_827437B0); // Processes deferred teardown, including callbacks.
PPC_EXTERN_FUNC(__imp__sub_827B9960); // Instantiates an entity template and its native components.
PPC_EXTERN_FUNC(__imp__sub_82517E50); // Assigns the four native BEntity GUID words.
PPC_EXTERN_FUNC(sub_82747E08);
PPC_EXTERN_FUNC(sub_827448B0);
PPC_EXTERN_FUNC(sub_82744E90);
PPC_EXTERN_FUNC(sub_827437B0);
PPC_EXTERN_FUNC(sub_82517E50);

namespace {
// All registries deliberately share this conservative fence. A registry may
// itself be freed/reallocated at the same address; a per-address counter must
// not silently restart and admit an older checkpoint.
std::atomic<uint64_t> revision{1},mutations{0};
std::atomic<bool> exhausted{false};
void Advance() noexcept {
    auto value=revision.load();
    for(;;){
        if(value==std::numeric_limits<uint64_t>::max()){exhausted.store(true);return;}
        if(revision.compare_exchange_weak(value,value+1))return;
    }
}
struct Mutation {
    Mutation() noexcept {mutations.fetch_add(1);Advance();}
    ~Mutation(){Advance();mutations.fetch_sub(1);}
};
struct Calls {
    PPCFunc* insert=__imp__sub_82747E08;
    PPCFunc* remove=__imp__sub_827448B0;
    PPCFunc* teardown=__imp__sub_82744E90;
    PPCFunc* flush=__imp__sub_827437B0;
    PPCFunc* identity=__imp__sub_82517E50;
};
// Only the same-executable fixture changes its own thread's call table.
thread_local const Calls* test_calls=nullptr;
void TraceLifecycle(const char* action,const PPCContext& ctx,uint8_t* base){
    if(test_calls || !base || !REXCVAR_GET(em2_private_profile) || !REXCVAR_GET(em2_probe_room_world_damage_events))return;
    const auto fp=_mm_getcsr();const auto host_error=GetLastError();
    struct Restore {unsigned fp;DWORD error;~Restore(){_mm_setcsr(fp);SetLastError(error);}} restore{fp,host_error};
    em2::GuestReadBatch reads;const auto entity=ctx.r4.u32;
    if(entity<4096 || uint64_t(entity)+24>0x100000000ull || !em2::GuestReadBatch::Readable(base+entity,24))return;
    uint64_t tick=0;const bool agreed=em2::CurrentAgreedSimulationTick(tick);
    REXLOG_INFO("Private native entity lifecycle: {} tick {} agreed {} entity {:08X} GUID {:08X}/{:08X}/{:08X}/{:08X} vtable {:08X} caller {:08X}",
        action,tick,agreed,entity,PPC_LOAD_U32(entity+4),PPC_LOAD_U32(entity+8),PPC_LOAD_U32(entity+12),PPC_LOAD_U32(entity+16),PPC_LOAD_U32(entity),uint32_t(ctx.lr));
    if(agreed && action[0]=='i'){
        // PPC prologues save LR at caller-SP - 8 before allocating the frame.
        // This is diagnostic context only, never portable spawn arguments.
        auto stack=ctx.r1.u32;std::string callers=fmt::format("{:08X}",uint32_t(ctx.lr));
        for(unsigned depth=0;depth<8;++depth){
            if(stack<4096 || uint64_t(stack)+4>0x100000000ull || !em2::GuestReadBatch::Readable(base+stack,4))break;
            const auto parent=PPC_LOAD_U32(stack);
            if(parent<=stack || parent-stack>1024*1024 || (parent&15) || !em2::GuestReadBatch::Readable(base+parent-8,4))break;
            const auto pc=PPC_LOAD_U32(parent-8);
            if((pc&3) || pc<0x822F0000 || pc>=0x83346908)break;
            callers+=fmt::format("/{:08X}",pc);stack=parent;
        }
        REXLOG_INFO("Private native entity factory stack: tick {} entity {:08X} callers {}; read-only bounded PPC frame walk",tick,entity,callers);
    }
}
}

PPC_FUNC_IMPL(sub_82747E08){TraceLifecycle("insert",ctx,base);Mutation mutation;(test_calls?test_calls->insert:__imp__sub_82747E08)(ctx,base);}
PPC_FUNC_IMPL(sub_827448B0){TraceLifecycle("remove",ctx,base);Mutation mutation;(test_calls?test_calls->remove:__imp__sub_827448B0)(ctx,base);}
PPC_FUNC_IMPL(sub_82517E50){Mutation mutation;(test_calls?test_calls->identity:__imp__sub_82517E50)(ctx,base);}
PPC_FUNC_IMPL(sub_82744E90){Mutation mutation;(test_calls?test_calls->teardown:__imp__sub_82744E90)(ctx,base);}
PPC_FUNC_IMPL(sub_827437B0){
    // Empty updates must not invalidate every older world snapshot. The native
    // method itself reads this count before visiting pending entity callbacks.
    if(base && !PPC_LOAD_U32(ctx.r3.u32+196)){
        (test_calls?test_calls->flush:__imp__sub_827437B0)(ctx,base);return;
    }
    Mutation mutation;(test_calls?test_calls->flush:__imp__sub_827437B0)(ctx,base);
}
PPC_FUNC_IMPL(sub_827B9960){
    const auto impact=em2::BeginNativeImpactClone(ctx,base);
    uint64_t tick=0;
    const bool trace=base && REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_probe_room_world_damage_events) && em2::CurrentAgreedSimulationTick(tick);
    if(!trace){__imp__sub_827B9960(ctx,base);if(impact)em2::EndNativeImpactClone(*impact,ctx,base);return;}
    const auto prototype=ctx.r3.u32,initialize=ctx.r4.u32,new_guid=ctx.r5.u32,caller=uint32_t(ctx.lr);
    std::array<uint32_t,4> source{};std::string name;bool source_readable=false;
    struct Preserve {unsigned fp=_mm_getcsr();DWORD error=GetLastError();~Preserve(){_mm_setcsr(fp);SetLastError(error);}};
    {
        Preserve preserve;em2::GuestReadBatch reads;
        if(prototype>=4096 && uint64_t(prototype)+20<=0x100000000ull && em2::GuestReadBatch::Readable(base+prototype,20)){
            source_readable=true;for(unsigned i=0;i<4;++i)source[i]=PPC_LOAD_U32(prototype+4+i*4);
            const auto text=PPC_LOAD_U32(prototype);
            for(unsigned i=0;i<255;++i){
                const auto address=uint64_t(text)+i;
                if(!text || address>UINT32_MAX || !em2::GuestReadBatch::Readable(base+address,1))break;
                const auto c=PPC_LOAD_U8(uint32_t(address));if(!c)break;
                if(c<32 || c>126){name="<non-printable>";break;}name.push_back(char(c));
            }
        }
    }
    __imp__sub_827B9960(ctx,base);
    if(impact)em2::EndNativeImpactClone(*impact,ctx,base);
    {
        Preserve preserve;em2::GuestReadBatch reads;std::array<uint32_t,4> id{};const auto result=ctx.r3.u32;
        const bool valid=result>=4096 && uint64_t(result)+28<=0x100000000ull && em2::GuestReadBatch::Readable(base+result+8,20) && PPC_LOAD_U32(result+8)==0x8208D908;
        if(valid)for(unsigned i=0;i<4;++i)id[i]=PPC_LOAD_U32(result+12+i*4);
        REXLOG_INFO("Private native entity clone: tick {} caller {:08X} template {:08X} readable {} name '{}' ID {:08X}/{:08X}/{:08X}/{:08X}, flags {}/{}, result-valid {} entity {:08X} GUID {:08X}/{:08X}/{:08X}/{:08X}; observation only",
            tick,caller,prototype,source_readable,name,source[0],source[1],source[2],source[3],initialize,new_guid,valid,valid?result+8:0,id[0],id[1],id[2],id[3]);
    }
}

bool em2::SampleNativeEntityLifetimeRevision(uint64_t& out) noexcept {
    const auto before=revision.load();
    if(exhausted.load() || mutations.load() || revision.load()!=before)return false;
    out=before;return true;
}

namespace {
thread_local bool arguments_ok=false,nested_ok=false;
void Forwarded(PPCContext& ctx,uint8_t* base){
    uint64_t stamp=42;
    arguments_ok=ctx.r3.u32==0x1234 && ctx.r4.u32==0x5678 && base==nullptr &&
        !em2::SampleNativeEntityLifetimeRevision(stamp) && stamp==42;
    ctx.r3.u64=0x9abc;
}
void Nested(PPCContext& ctx,uint8_t* base){
    sub_827448B0(ctx,base);
    uint64_t stamp=42;
    nested_ok=!em2::SampleNativeEntityLifetimeRevision(stamp) && stamp==42;
}
void Throws(PPCContext&,uint8_t*){throw 7;}
}
bool em2::TestNativeEntityLifetime(){
    auto check=[](bool ok,const char* what){if(!ok)REXLOG_ERROR("Native entity lifetime: {}",what);return ok;};
    Calls calls{Forwarded,Forwarded,Forwarded,Forwarded,Forwarded};
    struct Reset {const Calls* previous=test_calls;~Reset(){test_calls=previous;}}reset;
    test_calls=&calls;
    PPCContext ctx{};
    for(auto* hook:{sub_82747E08,sub_827448B0,sub_82744E90,sub_827437B0,sub_82517E50}){
        uint64_t before=0,after=0;ctx.r3.u64=0x1234;ctx.r4.u64=0x5678;arguments_ok=false;
        if(!SampleNativeEntityLifetimeRevision(before))return false;
        hook(ctx,nullptr);
        if(!check(arguments_ok && ctx.r3.u32==0x9abc && SampleNativeEntityLifetimeRevision(after) && after>before,
            "native arguments/results preserved; entry and completion invalidate prior stamp"))return false;
    }
    calls.insert=Nested;ctx.r3.u64=0x1234;ctx.r4.u64=0x5678;
    sub_82747E08(ctx,nullptr);
    uint64_t stamp=0;
    if(!check(arguments_ok && nested_ok && SampleNativeEntityLifetimeRevision(stamp),"nested calls stay fenced until outer completion"))return false;
    calls.insert=Throws;bool caught=false;
    try{sub_82747E08(ctx,nullptr);}catch(int value){caught=value==7;}
    if(!check(caught && SampleNativeEntityLifetimeRevision(stamp),"native exception propagates and mutation scope closes"))return false;
    std::atomic<bool> entered=false,finish=false;bool rejected=false;
    std::thread worker([&]{Mutation mutation;entered.store(true);while(!finish.load())std::this_thread::yield();});
    while(!entered.load())std::this_thread::yield();
    auto unchanged=stamp;rejected=!SampleNativeEntityLifetimeRevision(unchanged) && unchanged==stamp;
    finish.store(true);worker.join();
    if(!check(rejected && SampleNativeEntityLifetimeRevision(unchanged) && unchanged>stamp,"cross-thread mutations invalidate stamps without touching guest memory"))return false;
    // Exercise the real GUID setter, not only the forwarding substitute. Its
    // four writes and complete PPC register effects must remain byte-exact.
    calls.identity=__imp__sub_82517E50;
    std::array<uint8_t,16384> memory{};
    for(unsigned i=0;i<4;++i){const auto value=uint32_t(0xA5310000+i);auto* base=memory.data();PPC_STORE_U32(0x2000+i*4,value);}
    auto expected_memory=memory;PPCContext input{};input.r3.u64=0x1000;input.r4.u64=0x2000;input.lr=0x827B9A30;
    auto expected=input;__imp__sub_82517E50(expected,expected_memory.data());
    uint64_t before_guid=0,after_guid=0;auto actual=input;
    if(!SampleNativeEntityLifetimeRevision(before_guid))return false;
    sub_82517E50(actual,memory.data());
    if(!check(memory==expected_memory && !std::memcmp(&actual,&expected,sizeof actual) &&
        SampleNativeEntityLifetimeRevision(after_guid) && after_guid>before_guid,
        "real native GUID assignment retains exact memory/context effects and invalidates prior identity observations"))return false;
    REXLOG_INFO("Native entity lifetime tests passed: five original-call hooks including real GUID assignment, nested mutation, exception cleanup and cross-thread fencing; no entity restore claim");
    return true;
}
