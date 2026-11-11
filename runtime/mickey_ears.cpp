#include "ppc_context.h"
#include "guest_read_batch.h"
#include "mickey_ears.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <array>
#include <cstring>
#include <immintrin.h>
#include <windows.h>
#include <atomic>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <algorithm>

PPC_EXTERN_FUNC(__imp__sub_82B289C8);
PPC_EXTERN_FUNC(__imp__sub_825B8BC8);
PPC_EXTERN_FUNC(__imp__sub_8266CA60);
PPC_EXTERN_FUNC(__imp__sub_825172F0);
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DEFINE_BOOL(em2_trace_mickey_ears,false,"Debug","Private bounded capture of native per-model ear evaluation and shared uniform writes; no rendering changes");
REXCVAR_DEFINE_BOOL(em2_instance_mickey_ears,true,"GPU","Bind native dynamic Mickey ear transforms per character instance");

namespace {
constexpr uint32_t names=0x833E36A4;
using Matrix=std::array<uint8_t,64>;
struct EarEvaluation {uint32_t state,animation,reference,player,palette,bones,entity=0,root=0;
    unsigned valid=0;std::array<Matrix,4> matrices{};uint64_t epoch=0;};
thread_local EarEvaluation* evaluation=nullptr;
struct TraceContext {
    DWORD error=GetLastError();unsigned csr=_mm_getcsr();
    ~TraceContext(){_mm_setcsr(csr);SetLastError(error);}
};
struct UniformSample {
    EarEvaluation owner{};uint32_t bank=0;Matrix bytes{};
};
thread_local std::array<UniformSample,96> samples;
thread_local unsigned sample_count=0;
bool Readable(uint32_t address,size_t count,uint8_t* base){
    return address>=4096 && uint64_t(address)+count<=0x100000000ull &&
        em2::GuestReadBatch::Readable(PPC_RAW_ADDR(address),count);
}
void ResolveOwner(EarEvaluation& value,uint8_t* base){
    em2::GuestReadBatch reads;
    if(!Readable(value.animation,52,base))return;
    const auto raw=PPC_LOAD_U32(value.animation+48);
    if(raw<4096 || raw>UINT32_MAX-104 || !Readable(raw,104,base))return;
    const auto entity=raw+8,components=PPC_LOAD_U32(entity+36),count=PPC_LOAD_U32(entity+40),types=PPC_LOAD_U32(entity+88);
    if(!count || count>256 || !Readable(components,size_t(count)*4,base))return;
    bool owns=false;
    for(unsigned i=0;i<count;++i)owns|=PPC_LOAD_U32(components+i*4)==value.animation+8;
    if(!owns || !Readable(0x833DB0EC,1,base))return;
    const auto type=PPC_LOAD_U8(0x833DB0EC);
    if(!Readable(types,unsigned(type)+1,base))return;
    const auto slot=PPC_LOAD_U8(types+type);
    if(slot>=count)return;
    const auto graph=PPC_LOAD_U32(components+unsigned(slot)*4);
    if(graph<8 || !Readable(graph-8,92,base))return;
    const auto root=PPC_LOAD_U32(graph-8+88);
    if(!Readable(root,60,base))return;
    value.entity=entity;value.root=root;
}
std::optional<UniformSample> CaptureUniform(PPCContext& ctx,uint8_t* base){
    if(!evaluation || ctx.r4.u32!=64)return {};
    // Exact address arguments used by 82B289C8, not string-name inference.
    const auto name=ctx.r3.u32,address=ctx.r5.u32;
    if(name<names || name>=names+16 || ((name-names)&3) || !Readable(address,64,base))return {};
    UniformSample sample{*evaluation,(name-names)/4,{}};
    std::memcpy(sample.bytes.data(),PPC_RAW_ADDR(address),sample.bytes.size());
    return sample;
}
void TraceUniform(const UniformSample& sample){
    if(sample_count==samples.size())return;
    unsigned owner_samples=0;
    for(unsigned i=0;i<sample_count;++i){
        const auto& old=samples[i];
        owner_samples+=old.owner.animation==sample.owner.animation && old.owner.reference==sample.owner.reference;
        if(old.owner.state==sample.owner.state && old.owner.animation==sample.owner.animation &&
           old.owner.reference==sample.owner.reference && old.owner.player==sample.owner.player &&
           old.owner.palette==sample.owner.palette && old.bank==sample.bank && old.bytes==sample.bytes)return;
    }
    // An idle original Mickey updates every frame. Reserve room for models
    // admitted later rather than exhausting the capture on the first actor.
    if(owner_samples>=8)return;
    samples[sample_count++]=sample;
    uint64_t hash=14695981039346656037ull;
    for(const auto byte:sample.bytes){hash^=byte;hash*=1099511628211ull;}
    REXLOG_INFO("Native Mickey ear uniform: state {:08X} animation {:08X} reference {:08X} player {} palette {:08X} bones {} bank {} matrix {:016X} entity {:08X} root {:08X}",
        sample.owner.state,sample.owner.animation,sample.owner.reference,sample.owner.player,
        sample.owner.palette,sample.owner.bones,sample.bank,hash,sample.owner.entity,sample.owner.root);
}
struct Pair {uint32_t root=0,entity=0,animation=0,state=0,bank=0;std::array<Matrix,2> matrices{};};
struct Match {uint32_t parent=0,root=0;};
std::mutex cache_mutex;
std::unordered_map<uint32_t,Pair> pairs;
std::unordered_map<uint32_t,Match> meshes;
std::atomic<bool> multiple_owners=false;
std::atomic<uint64_t> world_epoch{1};
void EraseEntity(uint32_t entity){
    TraceContext preserve;
    std::lock_guard lock(cache_mutex);bool changed=false;
    for(auto it=pairs.begin();it!=pairs.end();)if(it->second.entity==entity){it=pairs.erase(it);changed=true;}else ++it;
    if(changed){meshes.clear();world_epoch.fetch_add(1,std::memory_order_release);}multiple_owners.store(pairs.size()>1,std::memory_order_release);
}
void Publish(const EarEvaluation& value){
    if(!value.root)return;
    const unsigned bank=(value.valid&3)==3?0:(value.valid&12)==12?2:4;
    if(bank==4)return;
    std::lock_guard lock(cache_mutex);
    if(value.epoch!=world_epoch.load(std::memory_order_relaxed))return;
    for(auto it=pairs.begin();it!=pairs.end();)if(it->second.entity==value.entity && it->first!=value.root){it=pairs.erase(it);meshes.clear();}else ++it;
    if(!pairs.contains(value.root)){
        // Bound untrusted/stale model counts; retail world teardown also
        // invalidates everything before any guest root addresses are reused.
        if(pairs.size()>=256){pairs.clear();meshes.clear();}
        meshes.clear();
    }
    pairs[value.root]={value.root,value.entity,value.animation,value.state,bank,{value.matrices[bank],value.matrices[bank+1]}};
    multiple_owners.store(pairs.size()>1,std::memory_order_release);
}
std::optional<Pair> FindPair(uint32_t mesh,uint8_t* base){
    // The renderer's argument is a live NiMesh (its original body immediately
    // reads +60 and beyond). Cached draws need no page-query/tree traversal.
    if(mesh<4096 || mesh>UINT32_MAX-60)return {};
    const auto parent=PPC_LOAD_U32(mesh+56);
    std::lock_guard lock(cache_mutex);
    auto found=meshes.find(mesh);
    if(found==meshes.end() || found->second.parent!=parent){
        em2::GuestReadBatch reads;
        uint32_t object=mesh,root=0;
        for(unsigned depth=0;depth<64 && object>=4096;++depth){
            if(pairs.contains(object)){root=object;break;}
            if(!Readable(object,60,base))break;
            const auto next=PPC_LOAD_U32(object+56);
            if(next==object)break;object=next;
        }
        if(meshes.size()>=32768)meshes.clear();
        found=meshes.insert_or_assign(mesh,Match{parent,root}).first;
    }
    const auto owner=pairs.find(found->second.root);
    if(owner==pairs.end())return {};
    return owner->second;
}
bool ApplyPair(PPCContext& ctx,uint8_t* base,const Pair& pair,PPCFunc* setter){
    if(ctx.r1.u32<4096 || !Readable(ctx.r1.u32-2048,2048,base))return false;
    const auto saved=ctx;TraceContext preserve;
    struct Restore {PPCContext& context;PPCContext saved;~Restore(){context=saved;}} restore{ctx,saved};
    const auto stack=ctx.r1.u32-2048;ctx.r1.u64=stack;PPC_STORE_U32(stack,saved.r1.u32);
    bool ok=true;
    for(unsigned side=0;side<2;++side){
        const auto data=stack+512+side*64;
        std::memcpy(PPC_RAW_ADDR(data),pair.matrices[side].data(),64);
        ctx.r3.u64=names+(pair.bank+side)*4;ctx.r4.u64=64;ctx.r5.u64=data;
        setter(ctx,base);ok&=ctx.r3.u32!=0;
    }
    return ok;
}
void RenderMesh(PPCContext& ctx,uint8_t* base,PPCFunc* setter,PPCFunc* draw){
    if(multiple_owners.load(std::memory_order_acquire) && (REXCVAR_GET(em2_instance_mickey_ears) || (REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_trace_mickey_ears)))){
        TraceContext preserve_inputs;
        const auto pair=FindPair(ctx.r4.u32,base);
        if(pair){
            const auto mesh=ctx.r4.u32;
            // Static ears, disabled processors and native controller changes
            // must not keep replaying an old dynamic-pose snapshot.
            const bool active=Readable(pair->animation,444,base) && PPC_LOAD_U32(pair->animation+440)==pair->state;
            const bool applied=active && REXCVAR_GET(em2_instance_mickey_ears) && ApplyPair(ctx,base,*pair,setter);
            if(REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_trace_mickey_ears)){
                TraceContext preserve;static thread_local std::array<uint32_t,64> seen{};static thread_local unsigned count=0;
                if(count<seen.size() && std::find(seen.begin(),seen.begin()+count,mesh)==seen.begin()+count){
                    seen[count++]=mesh;
                    REXLOG_INFO("Native Mickey ear draw: mesh {:08X} root {:08X} entity {:08X} animation {:08X} bank {} applied {}",mesh,pair->root,pair->entity,pair->animation,pair->bank,applied);
                }
            }
        }
    }
    draw(ctx,base);
}
}

void em2::InvalidateMickeyEarInstances(){
    TraceContext preserve;
    {std::lock_guard lock(cache_mutex);world_epoch.fetch_add(1,std::memory_order_release);pairs.clear();meshes.clear();multiple_owners.store(false,std::memory_order_release);}
}

PPC_FUNC_IMPL(sub_82B289C8){
    if(!REXCVAR_GET(em2_instance_mickey_ears) && (!REXCVAR_GET(em2_private_profile) || !REXCVAR_GET(em2_trace_mickey_ears))){
        __imp__sub_82B289C8(ctx,base);return;
    }
    EarEvaluation current{ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32,ctx.r8.u32};
    current.epoch=world_epoch.load(std::memory_order_acquire);
    {TraceContext preserve;ResolveOwner(current,base);}
    struct Scope {EarEvaluation* previous;~Scope(){evaluation=previous;}} scope{evaluation};
    evaluation=&current;
    __imp__sub_82B289C8(ctx,base);
    if(ctx.r3.u32){TraceContext preserve;Publish(current);}
}

PPC_FUNC_IMPL(sub_825B8BC8){
    std::optional<UniformSample> sample;
    if(evaluation){TraceContext preserve;sample=CaptureUniform(ctx,base);}
    __imp__sub_825B8BC8(ctx,base);
    if(sample && ctx.r3.u32){
        TraceContext preserve;
        evaluation->matrices[sample->bank]=sample->bytes;evaluation->valid|=1u<<sample->bank;
        if(REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_trace_mickey_ears))TraceUniform(*sample);
    }
}

PPC_FUNC_IMPL(sub_8266CA60){
    RenderMesh(ctx,base,__imp__sub_825B8BC8,__imp__sub_8266CA60);
}

PPC_FUNC_IMPL(sub_825172F0){
    EraseEntity(ctx.r3.u32);
    __imp__sub_825172F0(ctx,base);
}

namespace {
struct DrawFixture {std::array<Matrix,4> globals{};std::array<Matrix,2> expected{};
    PPCContext input{};std::array<uint8_t,sizeof(PPCContext)> input_bytes{};bool check=false,ok=true,is_mickey=true;
    unsigned setters=0,draws=0;};
thread_local DrawFixture* fixture=nullptr;
void FixtureSetter(PPCContext& ctx,uint8_t* base){
    const auto bank=(ctx.r3.u32-names)/4;
    fixture->ok&=ctx.r4.u32==64 && bank<4;
    if(bank<4)std::memcpy(fixture->globals[bank].data(),PPC_RAW_ADDR(ctx.r5.u32),64);
    ++fixture->setters;ctx.r3.u64=1;ctx.r6.u64=0xBAD;ctx.f1.f64=-999;SetLastError(141);
}
void FixtureDraw(PPCContext& ctx,uint8_t*){
    const bool arguments=std::memcmp(&ctx,fixture->input_bytes.data(),sizeof(ctx))==0;
    if(!arguments)REXLOG_ERROR("Mickey ear fixture draw arguments changed: mesh {:08X}, stack {:08X}, r6 {:X}, f1 {}, expected r6 {:X}, f1 {}",ctx.r4.u32,ctx.r1.u32,ctx.r6.u64,ctx.f1.f64,fixture->input.r6.u64,fixture->input.f1.f64);
    fixture->ok&=arguments;
    if(fixture->check)fixture->ok&=fixture->globals[0]==fixture->expected[0] && fixture->globals[1]==fixture->expected[1];
    ++fixture->draws;ctx.r3.u64=0xABCDEF;ctx.r5.u64=0x123456;SetLastError(142);
}

}
bool em2::TestMickeyEarInstances(){
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
    struct Restore {
        uint8_t* base;bool enabled=REXCVAR_GET(em2_instance_mickey_ears);DWORD error=GetLastError();unsigned csr=_mm_getcsr();DrawFixture* prior=fixture;
        decltype(pairs) old_pairs;decltype(meshes) old_meshes;uint64_t old_epoch=world_epoch.load();
        explicit Restore(uint8_t* p):base(p){std::lock_guard lock(cache_mutex);old_pairs=std::move(pairs);old_meshes=std::move(meshes);multiple_owners=false;}
        ~Restore(){REXCVAR_SET(em2_instance_mickey_ears,enabled);fixture=prior;{std::lock_guard lock(cache_mutex);pairs=std::move(old_pairs);meshes=std::move(old_meshes);world_epoch=old_epoch;multiple_owners=pairs.size()>1;}if(base)VirtualFree(base,0,MEM_RELEASE);_mm_setcsr(csr);SetLastError(error);}
    } restore(base);
    if(!base || !VirtualAlloc(base,0x10000,MEM_COMMIT,PAGE_READWRITE) || !VirtualAlloc(base+0x833D0000,0x10000,MEM_COMMIT,PAGE_READWRITE))return false;
    constexpr uint32_t entity_raw=0x1000,animation=0x2000,components=0x3000,types=0x4000,graph=0x5000,root_a=0x6000,root_b=0x7000,mesh_a=0x8000,mesh_b=0x9000,other=0xA000;
    PPC_STORE_U32(animation+48,entity_raw);PPC_STORE_U32(entity_raw+8+36,components);PPC_STORE_U32(entity_raw+8+40,2);PPC_STORE_U32(entity_raw+8+88,types);
    PPC_STORE_U32(components,animation+8);PPC_STORE_U32(components+4,graph+8);PPC_STORE_U8(0x833DB0EC,9);PPC_STORE_U8(types+9,1);PPC_STORE_U32(graph+88,root_a);
    EarEvaluation a{1,animation,2,0,3,128};ResolveOwner(a,base);
    bool ok=a.entity==entity_raw+8 && a.root==root_a;
    if(!ok)REXLOG_ERROR("Mickey ear fixture owner failed: entity {:08X}, root {:08X}",a.entity,a.root);
    PPC_STORE_U32(components,animation+16);EarEvaluation invalid{1,animation,2,0,3,128};ResolveOwner(invalid,base);ok&=!invalid.root;PPC_STORE_U32(components,animation+8);
    a.valid=3;a.epoch=world_epoch.load();a.matrices[0].fill(0x11);a.matrices[1].fill(0x12);
    auto b=a;b.root=root_b;b.entity=0xB000;b.animation=0xC000;b.matrices[0].fill(0x21);b.matrices[1].fill(0x22);
    PPC_STORE_U32(a.animation+440,a.state);PPC_STORE_U32(b.animation+440,b.state);
    Publish(a);Publish(b);PPC_STORE_U32(mesh_a+56,root_a);PPC_STORE_U32(mesh_b+56,root_b);
    DrawFixture state;fixture=&state;REXCVAR_SET(em2_instance_mickey_ears,true);
    auto draw=[&](uint32_t mesh,const EarEvaluation* expected){
        PPCContext ctx{};ctx.r1.u64=0xF000;ctx.r3.u64=0xD000;ctx.r4.u64=mesh;ctx.r6.u64=0xA55A;ctx.f1.f64=17.5;ctx.lr=0x829ECFFC;
        state.input=ctx;std::memcpy(state.input_bytes.data(),&ctx,sizeof(ctx));state.check=expected!=nullptr;if(expected)state.expected={expected->matrices[0],expected->matrices[1]};
        RenderMesh(ctx,base,FixtureSetter,FixtureDraw);
        return ctx.r3.u32==0xABCDEF && ctx.r5.u32==0x123456 && GetLastError()==142;
    };
    ok&=draw(mesh_a,&a) && draw(mesh_b,&b) && draw(mesh_a,&a);
    ok&=state.setters==6 && state.draws==3;
    ok&=draw(other,nullptr) && state.setters==6;
    // Reparenting a reused mesh must change ownership even on a cache hit.
    PPC_STORE_U32(mesh_a+56,root_b);ok&=draw(mesh_a,&b) && state.setters==8;
    PPC_STORE_U32(b.animation+440,0);ok&=draw(mesh_a,nullptr) && state.setters==8;PPC_STORE_U32(b.animation+440,b.state);
    EraseEntity(b.entity);ok&=draw(mesh_a,nullptr) && state.setters==8;
    const auto stale=a;
    a.epoch=b.epoch=world_epoch.load();auto replacement=a;replacement.root=0xD000;replacement.matrices[0].fill(0x31);replacement.matrices[1].fill(0x32);
    Publish(b);Publish(replacement);PPC_STORE_U32(mesh_a+56,replacement.root);ok&=draw(mesh_a,&replacement) && state.setters==10;
    InvalidateMickeyEarInstances();Publish(stale);{std::lock_guard lock(cache_mutex);ok&=pairs.empty();}ok&=draw(mesh_a,nullptr) && state.setters==10;
    auto partial=a;partial.valid=1;Publish(partial);ok&=!multiple_owners.load();
    a.epoch=b.epoch=world_epoch.load();Publish(a);Publish(b);ok&=multiple_owners.load();REXCVAR_SET(em2_instance_mickey_ears,false);ok&=draw(mesh_b,nullptr) && state.setters==10;
    ok&=state.ok;
    if(ok)REXLOG_INFO("Mickey ear instance tests passed: validated rig ownership, A/B/A draw matrices, unrelated draws/reparenting, level invalidation, partial-pair/disabled fallback, entity retirement, active-processor validation and preserved native contexts");
    else REXLOG_ERROR("Mickey ear instance tests failed: fixture {} setters {} draws {}",state.ok,state.setters,state.draws);
    return ok;
}
