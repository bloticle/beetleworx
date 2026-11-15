#include "native_entity_lifetime.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <atomic>
#include <thread>
#include <limits>

PPC_EXTERN_FUNC(__imp__sub_82747E08); // Registry inserts handle/entity maps.
PPC_EXTERN_FUNC(__imp__sub_827448B0); // Registry removes both maps.
PPC_EXTERN_FUNC(__imp__sub_82744E90); // Calls teardown before queuing NiPointer.
PPC_EXTERN_FUNC(__imp__sub_827437B0); // Processes deferred teardown, including callbacks.
PPC_EXTERN_FUNC(sub_82747E08);
PPC_EXTERN_FUNC(sub_827448B0);
PPC_EXTERN_FUNC(sub_82744E90);
PPC_EXTERN_FUNC(sub_827437B0);

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
};
// Only the same-executable fixture changes its own thread's call table.
thread_local const Calls* test_calls=nullptr;
}

PPC_FUNC_IMPL(sub_82747E08){Mutation mutation;(test_calls?test_calls->insert:__imp__sub_82747E08)(ctx,base);}
PPC_FUNC_IMPL(sub_827448B0){Mutation mutation;(test_calls?test_calls->remove:__imp__sub_827448B0)(ctx,base);}
PPC_FUNC_IMPL(sub_82744E90){Mutation mutation;(test_calls?test_calls->teardown:__imp__sub_82744E90)(ctx,base);}
PPC_FUNC_IMPL(sub_827437B0){
    // Empty updates must not invalidate every older world snapshot. The native
    // method itself reads this count before visiting pending entity callbacks.
    if(base && !PPC_LOAD_U32(ctx.r3.u32+196)){
        (test_calls?test_calls->flush:__imp__sub_827437B0)(ctx,base);return;
    }
    Mutation mutation;(test_calls?test_calls->flush:__imp__sub_827437B0)(ctx,base);
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
    Calls calls{Forwarded,Forwarded,Forwarded,Forwarded};
    struct Reset {const Calls* previous=test_calls;~Reset(){test_calls=previous;}}reset;
    test_calls=&calls;
    PPCContext ctx{};
    for(auto* hook:{sub_82747E08,sub_827448B0,sub_82744E90,sub_827437B0}){
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
    REXLOG_INFO("Native entity lifetime tests passed: four original-call hooks, nested mutation, exception cleanup and cross-thread fencing; no entity restore claim");
    return true;
}
