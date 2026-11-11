#include "ppc_config.h"
#include "shadow_resize.h"
#include "shadow_resources.h"
#include "shadow_distance.h"
#include "shadow_cast_probe.h"
#include "native_texture_retirement.h"
#include "pc_settings.h"
#include "physical_arena.h"
#include "gpu_sync.h"
#include <rex/logging.h>
#include <rex/cvar.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <vector>
#include <mutex>
#include <memory>
#include <new>
#include <immintrin.h>
#include <cstring>
#include <exception>
#include <windows.h>

PPC_EXTERN_FUNC(__imp__sub_82546EA8);
PPC_EXTERN_FUNC(__imp__sub_82547E80);
PPC_EXTERN_FUNC(__imp__sub_829BC1B0);
PPC_EXTERN_FUNC(__imp__sub_829BA908);
PPC_EXTERN_FUNC(__imp__RtlEnterCriticalSection);
PPC_EXTERN_FUNC(__imp__RtlLeaveCriticalSection);
PPC_EXTERN_FUNC(EM2RefreshShadowViews);
PPC_EXTERN_FUNC(__imp__sub_8255F758);
PPC_EXTERN_FUNC(__imp__sub_82546A90);
PPC_EXTERN_FUNC(__imp__sub_82546AE0);
PPC_EXTERN_FUNC(__imp__sub_8256D488);
PPC_EXTERN_FUNC(__imp__sub_825DEB88);
PPC_EXTERN_FUNC(__imp__sub_8256E5C0);
PPC_EXTERN_FUNC(__imp__sub_8256E5E0);
PPC_EXTERN_FUNC(__imp__sub_8256E7E0);
PPC_EXTERN_FUNC(__imp__sub_8256D578);
PPC_EXTERN_FUNC(sub_82546A90);
REXCVAR_DECLARE(bool,em2_trace_shadow_preparation);
namespace {
struct UnboundCreation {
    unsigned slot_calls=0,map_calls=0;
    uint32_t target=0,map=0;
};
thread_local UnboundCreation* unbound_creation=nullptr;
struct UnboundScope {
    UnboundCreation* previous=unbound_creation;
    explicit UnboundScope(UnboundCreation& value){unbound_creation=&value;}
    ~UnboundScope(){unbound_creation=previous;}
};
bool PrepareUnboundMap(PPCContext& ctx) {
    if(!unbound_creation || ctx.lr!=0x8256D290)return false;
    ++unbound_creation->map_calls;unbound_creation->map=ctx.r3.u32;
    // Native base constructor stores r6 at map+36 and r7 at map+40. A null
    // source skips registration, so staging never overwrites reserved slot0.
    ctx.r6.u64=ctx.r7.u64=0;return true;
}
struct Target {uint32_t handle=0,size=0,slot=0;};
template<class Allocator=std::allocator<Target>>struct ShadowState {
    uint32_t owner=0,refresh=0;
    std::array<Target,3> current{};
    std::vector<Target,Allocator> retired;
    bool destroying=false;
    ShadowState()=default;
    explicit ShadowState(const Allocator& allocator):retired(allocator){}
};
using State=ShadowState<>;
State state;
std::recursive_mutex mutex;
// Native render clicks retain child views independently of the shadow owner.
// Keep their shared source alive while queued clicks can still execute. Only
// the child's actual destructor may detach a transferred source reference.
struct RetiredSource {uint32_t map,source,slot;};
std::vector<RetiredSource> retired_sources;
bool TransferredSource(uint8_t* base,uint32_t map,uint32_t source,uint32_t slot) {
    return std::any_of(retired_sources.begin(),retired_sources.end(),[&](const auto& value){
        return value.map==map && value.source==source && value.slot==slot &&
            PPC_LOAD_U32(map+36)==source && PPC_LOAD_U32(map+40)==slot;
    });
}
std::atomic<uint64_t> requested{0},consumed{0};
std::atomic<bool> work{false},refresh{false};
std::atomic<em2::ShadowResizeStatus> status{em2::ShadowResizeStatus::Idle};
struct Restore {PPCContext& ctx;PPCContext saved=ctx;unsigned csr=_mm_getcsr();~Restore(){ctx=saved;_mm_setcsr(csr);}};
uint32_t Width(uint8_t* base,uint32_t handle) {
    if(!handle || PPC_LOAD_U32(handle)!=0x8203D708)return 0;
    const auto surface=PPC_LOAD_U32(handle+16);if(!surface || PPC_LOAD_U32(surface)!=0x82043108)return 0;
    const auto dimensions=PPC_LOAD_U32(surface+52);if(!dimensions)return 0;
    const auto width=PPC_LOAD_U32(dimensions+12),height=PPC_LOAD_U32(dimensions+16);
    return width==height && width>=128 && width<=2048 && !(width&(width-1))?width:0;
}
bool CanonicalView(uint8_t* base,uint32_t view) {
    return view && PPC_LOAD_U32(view)==0x8203DEC0 && PPC_LOAD_U32(view+56)==0x8255F758 &&
        PPC_LOAD_U32(view+60)==0x8255F6A0 && PPC_LOAD_U32(view+64)==0x8255F6B8;
}
uint32_t BindingChild(uint8_t* base,uint32_t target) {
    if(!target || PPC_LOAD_U32(target)!=0x8203D708)return 0;
    const auto click=PPC_LOAD_U32(target+24);
    if(!click || PPC_LOAD_U32(click+100)!=1)return 0;
    const auto node=PPC_LOAD_U32(click+92);
    if(!node || node!=PPC_LOAD_U32(click+96))return 0;
    const auto map=PPC_LOAD_U32(node+8);
    return map && PPC_LOAD_U32(map)==0x8203EA6C ? map:0;
}
template<class Allocator>bool Referenced(const ShadowState<Allocator>& current,uint8_t* base,uint32_t handle) {
    for(unsigned i=0;i<3;++i) {
        const auto view=PPC_LOAD_U32(current.owner+164+i*4);
        if(!view)continue;
        if(!CanonicalView(base,view))return true; // Unknown getter: retain resources.
        if(PPC_LOAD_U32(view+84)==handle)return true;
    }
    return false;
}
template<class Allocator,class Backend>bool Collect(ShadowState<Allocator>& current,uint8_t* base,Backend& backend) {
    std::vector<Target> ready;
    for(const auto& target:current.retired)if(!Referenced(current,base,target.handle))ready.push_back(target);
    if(ready.empty())return true;
    if(!backend.Drain())return false;
    for(const auto& target:ready) {
        std::erase_if(current.retired,[&](const Target& entry){return entry.handle==target.handle;});
        backend.Release(target.handle);
    }
    return true;
}
// Allocate the complete set before publishing any owner pointer. Retired targets
// remain valid while native views retain their raw handles; reuse them when a
// later selection requests the same dimensions.
template<class Allocator,class Backend>em2::ShadowResizeStatus Resize(ShadowState<Allocator>& current,uint8_t* base,const std::array<uint32_t,3>& sizes,Backend& backend) {
    using Result=em2::ShadowResizeStatus;
    if constexpr(requires{backend.ValidateCurrent(current.current);})
        if(!backend.ValidateCurrent(current.current))return backend.Failure();
    std::array<Target,3> replacement=current.current;
    std::array<bool,3> create{};
    for(unsigned i=0;i<3;++i)if(current.current[i].size!=sizes[i]) {
        auto found=std::find_if(current.retired.begin(),current.retired.end(),[&](const Target& target){return target.slot==i && target.size==sizes[i];});
        if(found!=current.retired.end())replacement[i]=*found;else create[i]=true;
    }
    // Every newly created target can add at most one retired record: either its
    // failed staging allocation, or the old target after successful publication.
    // Reuse erases a retired record before retiring the old target, so it needs
    // no extra capacity. Allocate all bookkeeping before native GPU/binding work.
    const auto additional=std::count(create.begin(),create.end(),true);
    if(size_t(additional)>current.retired.max_size()-current.retired.size())return Result::MemoryLimit;
    try {current.retired.reserve(current.retired.size()+additional);}
    catch(const std::bad_alloc&){return Result::MemoryLimit;}
    if(!backend.Admit(sizes,create))return Result::MemoryLimit;
    if(!backend.Drain())return Result::GpuWaitFailed;
    std::array<Target,3> allocated{};size_t allocated_count=0;
    for(unsigned i=0;i<3;++i)if(create[i]) {
        const auto handle=backend.Create(i,sizes[i]);
        if(!handle) {
            current.retired.insert(current.retired.end(),allocated.begin(),allocated.begin()+allocated_count);
            if constexpr(requires{backend.Failure();})return backend.Failure();
            return Result::CreationFailed;
        }
        replacement[i]={handle,sizes[i],i};allocated[allocated_count++]=replacement[i];
    }
    if constexpr(requires{backend.Commit(current.current,replacement);}) {
        if(!backend.Commit(current.current,replacement)) {
            current.retired.insert(current.retired.end(),allocated.begin(),allocated.begin()+allocated_count);
            return backend.Failure();
        }
    }
    for(unsigned i=0;i<3;++i)if(replacement[i].handle!=current.current[i].handle) {
        std::erase_if(current.retired,[&](const Target& target){return target.handle==replacement[i].handle;});
        current.retired.push_back(current.current[i]);
        PPC_STORE_U32(current.owner+176+i*4,replacement[i].handle);
    }
    current.current=replacement;current.refresh=7;
    return Result::Applied;
}
struct NativeBackend {
    PPCContext& ctx;uint8_t* base;
    em2::ShadowResizeStatus failure=em2::ShadowResizeStatus::CreationFailed;
    em2::ShadowResizeStatus Failure() const{return failure;}
    bool Drain(){return em2::SynchronizeGraphicsForResourceChange();}
    void Release(uint32_t handle){
        const bool trace=REXCVAR_GET(em2_trace_shadow_preparation);
        if(trace){REXLOG_INFO("Live shadow retirement begins: target={:08X} child={:08X}",handle,BindingChild(base,handle));if(auto logger=rex::GetLogger())logger->flush();}
        ctx.r3.u64=handle;__imp__sub_82547E80(ctx,base);
        if(trace){REXLOG_INFO("Live shadow retirement completed: target={:08X}",handle);if(auto logger=rex::GetLogger())logger->flush();}
    }
    bool ValidateCurrent(const std::array<Target,3>& current) {
        const auto manager=PPC_LOAD_U32(0x833CDD90);
        if(!manager){failure=em2::ShadowResizeStatus::NativeSlotLimit;return false;}
        const auto source=PPC_LOAD_U32(manager+264);
        // This interface owns the 6000-entry native material cache cleared by
        // the child-view destructor. Do not interpret a foreign source layout.
        if(!source || PPC_LOAD_U32(source+8)!=0x8203EB5C) {
            failure=em2::ShadowResizeStatus::NativeSlotLimit;return false;
        }
        unsigned occupied=0;
        for(const auto& target:current) {
            const auto map=BindingChild(base,target.handle);
            const auto slot=target.handle?PPC_LOAD_U32(target.handle+32):0;
            if(!source || !map || slot<2 || slot>4 || (occupied&(1u<<slot)) ||
               PPC_LOAD_U32(map+36)!=source || PPC_LOAD_U32(map+40)!=slot || !PPC_LOAD_U32(map+44)) {
                REXLOG_WARN("Live shadow binding validation failed: target={:08X} map={:08X} native_slot={} shared_source={:08X}",target.handle,map,slot,source);
                failure=em2::ShadowResizeStatus::NativeSlotLimit;return false;
            }
            const auto binding=PPC_LOAD_U32(source+12+slot*4);
            const auto type=binding?PPC_LOAD_U32(binding):0;
            const auto setter=type?PPC_LOAD_U32(type+4):0;
            if(setter<0x82000000 || setter>=0x84000000){failure=em2::ShadowResizeStatus::NativeSlotLimit;return false;}
            occupied|=1u<<slot;
        }
        for(unsigned i=0;i<3;++i)if(PPC_LOAD_U32(manager+268+i*4)!=5){failure=em2::ShadowResizeStatus::NativeSlotLimit;return false;}
        return occupied==0x1C;
    }
    bool Commit(const std::array<Target,3>& old,const std::array<Target,3>& replacement) {
        if(!ValidateCurrent(old))return false;
        const auto manager=PPC_LOAD_U32(0x833CDD90),source=PPC_LOAD_U32(manager+264);
        std::array<uint32_t,3> old_maps{},new_maps{},slots{};
        // Validate the complete set before changing any reference or binding.
        for(unsigned i=0;i<3;++i)if(old[i].handle!=replacement[i].handle) {
            old_maps[i]=BindingChild(base,old[i].handle);new_maps[i]=BindingChild(base,replacement[i].handle);
            slots[i]=PPC_LOAD_U32(old[i].handle+32);
            const bool unbound=new_maps[i] && !PPC_LOAD_U32(new_maps[i]+36) && !PPC_LOAD_U32(new_maps[i]+40);
            const bool reusable=new_maps[i] && TransferredSource(base,new_maps[i],source,slots[i]);
            if(!new_maps[i] || PPC_LOAD_U32(replacement[i].handle+32)!=0 || (!unbound && !reusable) ||
               !PPC_LOAD_U32(new_maps[i]+44) || PPC_LOAD_U32(replacement[i].handle+28)!=source) {
                failure=em2::ShadowResizeStatus::NativeSlotLimit;return false;
            }
        }
        const auto count=std::count_if(new_maps.begin(),new_maps.end(),[](auto map){return map!=0;});
        if(size_t(count)>retired_sources.max_size()-retired_sources.size()){failure=em2::ShadowResizeStatus::MemoryLimit;return false;}
        try{retired_sources.reserve(retired_sources.size()+count);}
        catch(const std::bad_alloc&){failure=em2::ShadowResizeStatus::MemoryLimit;return false;}
        // This is a camera/target replacement within the SAME source registry.
        // 8256E7E0 is full source teardown: it unregisters every scene object,
        // and ordinary frames do not repopulate that registry. Retain geometry
        // and invalidate only the changed camera's visibility result below.
        for(unsigned i=0;i<3;++i)if(new_maps[i]) {
            // Attach the new map through the engine's actual reference setter,
            // then bind the new camera into the existing native view slot. No
            // slot is returned to the free pool during this ownership transfer.
            ctx.r3.u64=new_maps[i]+36;ctx.r4.u64=source;__imp__sub_825DEB88(ctx,base);
            PPC_STORE_U32(new_maps[i]+40,slots[i]);
            ctx.r3.u64=source;ctx.r4.u64=slots[i];ctx.r5.u64=PPC_LOAD_U32(new_maps[i]+44);
            __imp__sub_8256E5C0(ctx,base);
            PPC_STORE_U8(source+204324+slots[i],1);
            PPC_STORE_U32(replacement[i].handle+32,slots[i]);
            std::erase_if(retired_sources,[&](const auto& value){return value.map==new_maps[i];});
            retired_sources.push_back({old_maps[i],source,slots[i]});
            // The target no longer owns the slot, but its retained child may
            // still execute in the native click list before generator refresh.
            // Leave BOTH its source and view index valid until destruction.
            PPC_STORE_U32(old[i].handle+32,0);
        }
        REXLOG_INFO("Live shadow binding ownership transferred; native slots retained, retired child sources preserved until destruction");
        return true;
    }
    uint32_t Create(unsigned slot,uint32_t size) {
        UnboundCreation creation;UnboundScope scope{creation};
        em2::ShadowCreationAuditScope audit({true,state.owner,slot,state.current[slot].size,size});
        ctx.r3.u64=ctx.r4.u64=size;ctx.r5.u64=ctx.r1.u32+96;
        PPC_STORE_U32(ctx.r5.u32,9);PPC_STORE_U32(ctx.r5.u32+4,3);PPC_STORE_U32(ctx.r5.u32+8,0);
        ctx.r6.u64=ctx.r7.u64=0;ctx.r8.u64=3;
        __imp__sub_82546EA8(ctx,base);
        const auto trace=em2::CurrentShadowCreation();
        if(trace.failed_allocation_size)failure=trace.failed_allocation_address?
            em2::ShadowResizeStatus::CreationFailed:em2::ShadowResizeStatus::MemoryLimit;
        const auto target=ctx.r3.u32;
        const auto actual=Width(base,target);
        const auto map=target?BindingChild(base,target):0;
        if(target && (actual!=size || creation.slot_calls!=1 || creation.map_calls!=1 ||
           creation.target!=target || creation.map!=map || !map || PPC_LOAD_U32(target+32) ||
           PPC_LOAD_U32(map+36) || PPC_LOAD_U32(map+40))) {
            REXLOG_WARN("Live shadow staging rejected: target={:08X} width={} requested={} allocator_calls={} map_calls={} map={:08X}",target,actual,size,creation.slot_calls,creation.map_calls,map);
            state.retired.push_back({target,actual,slot});return 0;
        }
        if(target) {
            const auto trace=em2::CurrentShadowCreation();
            REXLOG_INFO("Live shadow staged unbound: target={:08X} map={:08X} size={} owner_slot={} physical_requests={} physical_completed={} physical_requested_bytes={} physical_granted_bytes={}",target,map,size,slot,trace.physical_requests,trace.physical_completed,trace.physical_requested_bytes,trace.physical_granted_bytes);
        }
        return target;
    }
    bool Admit(const std::array<uint32_t,3>& sizes,const std::array<bool,3>& create) {
        uint64_t logical=0,physical=0,largest=0;
        for(unsigned i=0;i<3;++i)if(create[i]) {
            const auto pixels=uint64_t(sizes[i])*sizes[i];logical+=pixels*4;
            const auto bound=pixels*8+1024*1024;physical+=bound;largest=std::max(largest,bound);
        }
        if(!logical)return true;
        const auto manager=PPC_LOAD_U32(0x833CDD90);if(!manager)return false;
        const auto used=PPC_LOAD_U32(manager+172),budget=PPC_LOAD_U32(manager+184);
        if(uint64_t(used)+logical>budget)return false; // Do not invoke native reclamation.
        // This reserves physical-arena capacity, not the separate CPU metadata
        // heaps. In particular pool+3020 is backing capacity, not bytes used.
        em2::PhysicalArenaStats stats{};
        return em2::ReadPhysicalArenaStats(base,0x833D2760+32128,4096,stats) &&
            stats.free>=physical && stats.largest_aligned>=largest;
    }
};
void UpdateWork(){work.store(!state.retired.empty(),std::memory_order_release);refresh.store(state.refresh!=0,std::memory_order_release);}
// Sparse-fixture virtual call used only by TestLiveShadowResize. The real
// native dispatcher invokes this exactly as it invokes the game's slot setter.
void FixtureSlotSetter(PPCContext& ctx,uint8_t* base) {
    PPC_STORE_U32(ctx.r3.u32+8,ctx.r4.u32);
    PPC_STORE_U32(ctx.r3.u32+12,PPC_LOAD_U32(ctx.r3.u32+12)+1);
}
void FixtureSourceActivity(PPCContext& ctx,uint8_t* base) {
    PPC_STORE_U32(ctx.r3.u32+16,ctx.r4.u32);
    PPC_STORE_U32(ctx.r3.u32+20,PPC_LOAD_U32(ctx.r3.u32+20)+1);
}
void FixtureCachedMaterialRemoval(PPCContext& ctx,uint8_t* base) {
    const auto object=ctx.r3.u32,map=PPC_LOAD_U32(object+16);
    PPC_STORE_U32(object+8,PPC_LOAD_U32(object+8)+1);
    PPC_STORE_U32(object+12,PPC_LOAD_U32(map+36));
    PPC_STORE_U32(object+20,ctx.r4.u32);
}
// A standard-conforming allocator used only by the failure-injection fixture.
// Production State uses std::allocator, with no test branch in its allocation.
struct AllocationBudget {int remaining=-1;unsigned calls=0;};
template<class T>struct BudgetAllocator {
    using value_type=T;AllocationBudget* budget=nullptr;
    BudgetAllocator()=default;
    explicit BudgetAllocator(AllocationBudget& value):budget(&value){}
    template<class U>BudgetAllocator(const BudgetAllocator<U>& other):budget(other.budget){}
    T* allocate(size_t count) {
        if(budget){++budget->calls;if(!budget->remaining)throw std::bad_alloc();if(budget->remaining>0)--budget->remaining;}
        return std::allocator<T>{}.allocate(count);
    }
    void deallocate(T* data,size_t count){std::allocator<T>{}.deallocate(data,count);}
    template<class U>bool operator==(const BudgetAllocator<U>& other)const{return budget==other.budget;}
};
}
PPC_FUNC_IMPL(sub_82546A90) {
    if(unbound_creation && ctx.lr==0x8256D124) {
        ++unbound_creation->slot_calls;unbound_creation->target=ctx.r31.u32;
        ctx.r3.u64=0;return;
    }
    __imp__sub_82546A90(ctx,base);
}
PPC_FUNC_IMPL(sub_8256D488) {
    PrepareUnboundMap(ctx);__imp__sub_8256D488(ctx,base);
}
PPC_FUNC_IMPL(sub_8256D578) {
    {
        std::lock_guard lock(mutex);
        const auto map=ctx.r3.u32;
        const auto found=std::find_if(retired_sources.begin(),retired_sources.end(),[&](const auto& value){return value.map==map;});
        if(found!=retired_sources.end()) {
            if(REXCVAR_GET(em2_trace_shadow_preparation)){REXLOG_INFO("Live shadow retired child destruction: child={:08X} source={:08X} slot={}",map,found->source,found->slot);if(auto logger=rex::GetLogger())logger->flush();}
            if(PPC_LOAD_U32(map)==0x8203EA6C && PPC_LOAD_U32(map+36)==found->source && PPC_LOAD_U32(map+40)==found->slot) {
                Restore restore{ctx};
                ctx.r3.u64=map+36;ctx.r4.u64=0;__imp__sub_825DEB88(ctx,base);
                PPC_STORE_U32(map+40,0);
            }
            retired_sources.erase(found);
        }
    }
    // A transferred view must not clear the replacement's material cache. Its
    // source reference was released above at refcount-zero destruction, never
    // while a queued native render click could still use it. Ordinary views
    // retain the complete original destructor and invalidation behavior.
    __imp__sub_8256D578(ctx,base);
}
void em2::QueueShadowResize(){requested.fetch_add(1,std::memory_order_release);status.store(ShadowResizeStatus::Queued,std::memory_order_release);}
em2::ShadowResizeStatus em2::LiveShadowResizeStatus(){return status.load(std::memory_order_acquire);}
void em2::ApplyLiveShadowResize(PPCContext& ctx,uint8_t* base,uint32_t renderer) {
    const auto serial=requested.load(std::memory_order_acquire);
    if(serial==consumed.load(std::memory_order_acquire) && !work.load(std::memory_order_acquire))return;
    Restore restore{ctx};
    if(ctx.r3.u32!=1 || !renderer || renderer!=PPC_LOAD_U32(0x833CE200) ||
       PPC_LOAD_U32(renderer)!=0x82053F28 || PPC_LOAD_U32(renderer+212)!=3 ||
       PPC_LOAD_U32(renderer+36) || PPC_LOAD_U8(renderer+208))return;
    const auto owner=PPC_LOAD_U32(0x833E12AC);if(!owner || PPC_LOAD_U32(owner)!=0x820FAB5C || !PPC_LOAD_U8(owner+224))return;
    std::lock_guard lock(mutex);if(state.destroying)return;
    if(state.owner!=owner) {
        if(!state.retired.empty())return;
        state={};state.owner=owner;UpdateWork();
        for(unsigned i=0;i<3;++i){const auto target=PPC_LOAD_U32(owner+176+i*4);const auto size=Width(base,target);if(!size){state={};return;}state.current[i]={target,size,i};}
    }
    for(unsigned i=0;i<3;++i) {
        const auto view=PPC_LOAD_U32(owner+164+i*4);
        if(view && !CanonicalView(base,view)){status=ShadowResizeStatus::Unsupported;consumed=serial;return;}
        if(!state.current[i].handle || PPC_LOAD_U32(owner+176+i*4)!=state.current[i].handle || Width(base,state.current[i].handle)!=state.current[i].size){status=ShadowResizeStatus::Unsupported;consumed=serial;return;}
    }
    // Outer frame is closed. Hold the native renderer's frame and resource
    // locks while creating/publishing, in the same order as its render path.
    ctx.r1.u64=(ctx.r1.u32-128)&~uint32_t(15);PPC_STORE_U32(ctx.r1.u32,restore.saved.r1.u32);
    ctx.r3.u64=renderer+100;__imp__RtlEnterCriticalSection(ctx,base);
    ctx.r3.u64=renderer+172;__imp__RtlEnterCriticalSection(ctx,base);
    struct Unlock {PPCContext& ctx;uint8_t* base;uint32_t renderer;~Unlock(){ctx.r3.u64=renderer+172;__imp__RtlLeaveCriticalSection(ctx,base);ctx.r3.u64=renderer+100;__imp__RtlLeaveCriticalSection(ctx,base);}} unlock{ctx,base,renderer};
    NativeBackend backend{ctx,base};
    if(!Collect(state,base,backend)){status=ShadowResizeStatus::GpuWaitFailed;UpdateWork();return;}
    if(serial!=consumed.load(std::memory_order_acquire)) {
        const uint32_t player[]={128,256,512,1024,2048},environment[]={512,1024,2048,2048};
        const auto sizes=ShadowDistanceTargetSizes(owner,{player[PCSetting(4)],environment[PCSetting(5)],environment[PCSetting(5)]});
        bool changed=false;for(unsigned i=0;i<3;++i)changed|=sizes[i]!=state.current[i].size;
        if(changed) {
            // No GPU wait while holding this allocation lock. Drain first;
            // Resize's admission/creation stage uses the completed drain.
            if(!backend.Drain()){status=ShadowResizeStatus::GpuWaitFailed;UpdateWork();return;}
            struct CreationBackend:NativeBackend {bool Drain(){return true;}} creation_backend{{ctx,base}};
            std::lock_guard physical_lock(NativePhysicalArenaMutex());
            status=Resize(state,base,sizes,creation_backend);
            if(status.load()==ShadowResizeStatus::Applied)status=ShadowResizeStatus::Queued;
            REXLOG_INFO("Live shadow resize: player={} environment={} status={} retired={}",sizes[0],sizes[1],int(status.load()),state.retired.size());
            if(REXCVAR_GET(em2_trace_shadow_preparation))if(auto logger=rex::GetLogger())logger->flush();
        }else status=ShadowResizeStatus::Applied;
        consumed.store(serial,std::memory_order_release);
    }
    UpdateWork();
}
int em2::TrackedShadowTargetSlot(uint32_t owner,uint32_t target) {
    std::lock_guard lock(mutex);
    if(!target || state.owner!=owner || state.destroying)return -1;
    for(const auto& value:state.current)if(value.handle==target && value.slot<3)return int(value.slot);
    for(const auto& value:state.retired)if(value.handle==target && value.slot<3)return int(value.slot);
    return -1;
}
bool em2::ShadowOwnerNeedsRefresh(uint8_t* base,uint32_t owner) {
    const auto csr=_mm_getcsr();bool needed=false;
    {
        std::lock_guard lock(mutex);
        if(base && state.owner==owner && !state.destroying)
            for(unsigned i=0;i<3;++i)needed|=(state.refresh&(1u<<i)) && PPC_LOAD_U32(owner+152+i*4);
    }
    _mm_setcsr(csr);return needed;
}
PPC_FUNC_IMPL(sub_829BA908) {
    em2::NativeShadowCastProbe cast_probe(uint32_t(ctx.lr),ctx.r3.u32,ctx.r4.u32,base);
    em2::ObserveNativeShadowPreparation(uint32_t(ctx.lr),ctx.r3.u32,ctx.r4.u32,ctx.r26.u32,ctx.r28.u32,base);
    struct DistanceUpdate {
        uint32_t caller,owner,selected,camera_count,cameras;uint8_t* base;
        int exceptions=std::uncaught_exceptions();
        ~DistanceUpdate(){if(std::uncaught_exceptions()==exceptions)em2::UpdateNativeShadowDistance(caller,owner,selected,camera_count,cameras,base);}
    } distance{uint32_t(ctx.lr),ctx.r3.u32,ctx.r4.u32,ctx.r26.u32,ctx.r28.u32,base};
    if(!refresh.load(std::memory_order_acquire)){__imp__sub_829BA908(ctx,base);return;}
    std::lock_guard lock(mutex);
    if(ctx.r3.u32!=state.owner || state.destroying){__imp__sub_829BA908(ctx,base);return;}
    EM2RefreshShadowViews(ctx,base);
    const auto csr=_mm_getcsr();
    for(unsigned i=0;i<3;++i){const auto view=PPC_LOAD_U32(state.owner+164+i*4);if(!CanonicalView(base,view))continue;
        const auto target=PPC_LOAD_U32(view+84);
        for(const auto& current:state.current)if(current.handle==target && PPC_LOAD_U16(view+104)==current.size)state.refresh&=~(1u<<i);
    }
    if(status.load()==em2::ShadowResizeStatus::Queued) {
        bool active_pending=false;
        for(unsigned i=0;i<3;++i)active_pending|=(state.refresh&(1u<<i)) && PPC_LOAD_U32(state.owner+152+i*4);
        if(!active_pending)status=em2::ShadowResizeStatus::Applied;
    }
    UpdateWork();_mm_setcsr(csr);
}
PPC_FUNC_IMPL(sub_82547E80) {
    {std::lock_guard lock(mutex);const auto handle=ctx.r3.u32;std::erase_if(state.retired,[&](const Target& target){return target.handle==handle;});
        if(!state.destroying)for(auto& target:state.current)if(target.handle==handle){target.handle=0;status=em2::ShadowResizeStatus::Unsupported;}
        UpdateWork();}
    __imp__sub_82547E80(ctx,base);
}
PPC_FUNC_IMPL(sub_829BC1B0) {
    std::lock_guard lock(mutex);const auto owner=ctx.r3.u32;
    if(owner!=state.owner){__imp__sub_829BC1B0(ctx,base);return;}
    state.destroying=true;__imp__sub_829BC1B0(ctx,base);
    Restore restore{ctx};auto remaining=std::move(state.retired);state={};UpdateWork();
    for(const auto& target:remaining){ctx.r3.u64=target.handle;__imp__sub_82547E80(ctx,base);}
}

bool em2::TestLiveShadowResize() {
    if(!TestNativeTextureRetirement())return false;
    if(!TestNativeShadowCastProbe())return false;
    if(!TestShadowDistancePolicy())return false;
    auto saved_retired_sources=std::move(retired_sources);retired_sources.clear();
    struct RestoreRetiredSources {std::vector<RetiredSource>& saved;~RestoreRetiredSources(){retired_sources=std::move(saved);}} restore_retired_sources{saved_retired_sources};
    const auto csr=_mm_getcsr();struct FP {unsigned value;~FP(){_mm_setcsr(value);}}fp{csr};
    auto check=[](bool ok,const char* text){if(!ok)REXLOG_ERROR("Live shadow resize test: {}",text);return ok;};
    std::vector<uint8_t> memory(0x20000);auto* base=memory.data();constexpr uint32_t owner=0x10000;
    auto reset=[&] {
        std::fill(memory.begin(),memory.end(),0);State result{};result.owner=owner;
        for(unsigned i=0;i<3;++i) {
            const uint32_t target=0x14000+i*256,view=0x11000+i*256;
            result.current[i]={target,i?1024u:128u,i};PPC_STORE_U32(owner+176+i*4,target);PPC_STORE_U32(owner+164+i*4,view);
            PPC_STORE_U32(view,0x8203DEC0);PPC_STORE_U32(view+56,0x8255F758);PPC_STORE_U32(view+60,0x8255F6A0);PPC_STORE_U32(view+64,0x8255F6B8);
            PPC_STORE_U32(view+84,target);PPC_STORE_U16(view+104,result.current[i].size);
        }
        return result;
    };
    struct Backend {
        bool admission=true,drain=true;int fail_create=-1;unsigned created=0,drains=0,admissions=0;uint32_t next=0x18000;
        std::vector<uint32_t> released;
        bool Admit(const std::array<uint32_t,3>&,const std::array<bool,3>&){++admissions;return admission;}
        bool Drain(){++drains;return drain;}
        uint32_t Create(unsigned,uint32_t){if(int(created++)==fail_create)return 0;next+=256;return next;}
        void Release(uint32_t target){released.push_back(target);}
    };
    const std::array<uint32_t,3> high={1024,2048,2048};
    unsigned allocation_checks=0;
    auto allocation_check=[&](bool valid,const char* message){++allocation_checks;return check(valid,message);};
    {
        auto original=reset();AllocationBudget budget{0};
        ShadowState<BudgetAllocator<Target>> model{BudgetAllocator<Target>{budget}};model.owner=original.owner;model.current=original.current;
        const auto before=memory;Backend backend;const auto fp_before=_mm_getcsr();
        if(!allocation_check(Resize(model,base,high,backend)==ShadowResizeStatus::MemoryLimit && budget.calls==1 &&
            !backend.admissions && !backend.drains && !backend.created && model.retired.empty() && !model.refresh &&
            memory==before && _mm_getcsr()==fp_before,
            "actual bookkeeping allocation failure returns MemoryLimit before admission, GPU work, construction or publication"))return false;
        for(unsigned i=0;i<3;++i)if(!allocation_check(model.current[i].handle==original.current[i].handle,
            "bookkeeping allocation failure preserves every active target"))return false;
    }
    struct CommitBackend:Backend {
        AllocationBudget& budget;bool commit=true;unsigned commits=0;
        explicit CommitBackend(AllocationBudget& value):budget(value){}
        bool Commit(const std::array<Target,3>&,const std::array<Target,3>&){++commits;budget.remaining=0;return commit;}
        ShadowResizeStatus Failure()const{return ShadowResizeStatus::CreationFailed;}
    };
    for(unsigned failure=0;failure<3;++failure) {
        auto original=reset();AllocationBudget budget{1};
        ShadowState<BudgetAllocator<Target>> model{BudgetAllocator<Target>{budget}};model.owner=original.owner;model.current=original.current;
        CommitBackend backend{budget};if(failure==1)backend.commit=false;if(failure==2)backend.fail_create=1;
        const auto result=Resize(model,base,high,backend);
        if(!allocation_check(result==(failure?ShadowResizeStatus::CreationFailed:ShadowResizeStatus::Applied) && budget.calls==1 &&
            model.retired.size()==(failure==2?1u:3u) && backend.commits==(failure==2?0u:1u),
            "one preallocation covers successful publication, failed commit quarantine and partial creation quarantine"))return false;
        for(unsigned i=0;i<3;++i)if(!allocation_check(PPC_LOAD_U32(owner+176+i*4)==(failure?original.current[i].handle:model.current[i].handle),
            "preallocated publication or rollback retains the correct owner pointers"))return false;
    }
    {
        auto original=reset();AllocationBudget budget{1};
        using FixtureState=ShadowState<BudgetAllocator<Target>>;
        FixtureState model{BudgetAllocator<Target>{budget}};model.owner=original.owner;model.current=original.current;
        struct RejectedStagingBackend:Backend {
            FixtureState& model;AllocationBudget& budget;
            RejectedStagingBackend(FixtureState& value,AllocationBudget& allocation):model(value),budget(allocation){fail_create=2;}
            uint32_t Create(unsigned slot,uint32_t size) {
                const auto handle=Backend::Create(slot,size);budget.remaining=0;
                // Production NativeBackend::Create retains a target whose
                // post-constructor validation failed, before returning zero.
                if(!handle)model.retired.push_back({0x19F00,0,slot});return handle;
            }
        } backend{model,budget};
        const auto before=memory;
        if(!allocation_check(Resize(model,base,high,backend)==ShadowResizeStatus::CreationFailed && budget.calls==1 &&
            model.retired.size()==3 && memory==before && !model.refresh,
            "rejected native staging plus earlier successful allocations fit preallocated quarantine without publishing"))return false;
        if(!allocation_check(Collect(model,base,backend) && model.retired.empty() && backend.released.size()==3,
            "all rejected/partial staging handles remain tracked through fenced cleanup"))return false;
    }
    // All eight mixed reuse/new-create patterns. The allocator refuses further
    // allocations as soon as Commit runs, including publication's erase/push
    // order when a newly created slot precedes reused slots.
    for(unsigned mask=0;mask<8;++mask) {
        auto original=reset();AllocationBudget budget{1};
        ShadowState<BudgetAllocator<Target>> model{BudgetAllocator<Target>{budget}};model.owner=original.owner;model.current=original.current;
        CommitBackend backend{budget};if(!allocation_check(Resize(model,base,high,backend)==ShadowResizeStatus::Applied,"mixed reuse fixture stages initial high targets"))return false;
        std::array<uint32_t,3> sizes{};unsigned created=0;
        for(unsigned i=0;i<3;++i){const bool reuse=(mask&(1u<<i))!=0;sizes[i]=reuse?original.current[i].size:512u;created+=!reuse;}
        budget.remaining=created?1:0;
        if(!allocation_check(Resize(model,base,sizes,backend)==ShadowResizeStatus::Applied && budget.calls==(created?2u:1u) &&
            backend.created==3+created && model.retired.size()==3+created && backend.commits==2,
            "mixed reuse publication cannot allocate after native commit; pure reuse allocates nothing"))return false;
        for(unsigned i=0;i<3;++i)if(!allocation_check(model.current[i].size==sizes[i] && PPC_LOAD_U32(owner+176+i*4)==model.current[i].handle &&
            (!(mask&(1u<<i)) || model.current[i].handle==original.current[i].handle),"mixed reuse publishes exact requested target handles"))return false;
    }
    {
        auto model=reset();Backend backend;const auto old=model.current;
        if(!check(Resize(model,base,high,backend)==ShadowResizeStatus::Applied && backend.created==3 && backend.drains==1 &&
            model.refresh==7 && model.retired.size()==3,"complete replacement publishes three targets and schedules native view refresh"))return false;
        for(unsigned i=0;i<3;++i)if(!check(PPC_LOAD_U32(owner+176+i*4)==model.current[i].handle && PPC_LOAD_U32(0x11000+i*256+84)==old[i].handle,"owner publication retains old view bindings until native preparation"))return false;
        if(!check(Collect(model,base,backend) && backend.released.empty() && backend.drains==1,"referenced retired targets cause no GPU wait or early free"))return false;
        // Run the actual native raw setter, not a model of its assignment.
        PPCContext setter{};setter.r3.u64=0x11100;setter.r5.u64=model.current[1].handle;setter.r6.u64=0;setter.fpscr.InitHost();
        __imp__sub_8255F758(setter,base);
        if(!check(Collect(model,base,backend) && backend.released.size()==1 && backend.released[0]==old[1].handle && backend.drains==2 && model.retired.size()==2,
            "actual native rebind makes only that old target eligible; native GPU drain precedes release"))return false;
        const auto created=backend.created;
        const std::array<uint32_t,3> revert={old[0].size,high[1],old[2].size};
        if(!check(Resize(model,base,revert,backend)==ShadowResizeStatus::Applied && backend.created==created && model.current[0].handle==old[0].handle && model.current[2].handle==old[2].handle,
            "rapid changes reuse still-live retired dimensions without allocating duplicates"))return false;
    }
    for(unsigned failure=0;failure<3;++failure) {
        auto model=reset();const auto old=model.current;Backend backend;
        if(failure==0)backend.admission=false;
        if(failure==1)backend.drain=false;
        if(failure==2)backend.fail_create=1;
        const auto result=Resize(model,base,high,backend);
        if(!check(result==(failure==0?ShadowResizeStatus::MemoryLimit:failure==1?ShadowResizeStatus::GpuWaitFailed:ShadowResizeStatus::CreationFailed),"admission, GPU and partial-creation failures are distinguished"))return false;
        for(unsigned i=0;i<3;++i)if(!check(model.current[i].handle==old[i].handle && PPC_LOAD_U32(owner+176+i*4)==old[i].handle,"failed transaction leaves every active owner target unchanged"))return false;
        if(failure==2) {
            if(!check(model.retired.size()==1 && backend.released.empty(),"partial creation is quarantined, not freed without a fence"))return false;
            backend.drain=false;
            if(!check(!Collect(model,base,backend) && backend.released.empty() && model.retired.size()==1,"failed cleanup fence preserves quarantined resources"))return false;
            backend.drain=true;
            if(!check(Collect(model,base,backend) && backend.released.size()==1 && model.retired.empty(),"successful cleanup fence releases quarantined allocation"))return false;
        }
    }
    {
        auto model=reset();Backend backend;Resize(model,base,high,backend);
        PPC_STORE_U32(0x11000+60,0x8255F6A4);
        for(unsigned i=0;i<3;++i)PPC_STORE_U32(0x11000+i*256+84,0);
        if(!check(Collect(model,base,backend) && backend.released.empty() && model.retired.size()==3,"unknown native getter conservatively retains old targets"))return false;
    }
    // Untouched preparation must preserve native context/memory/FP behavior.
    auto* sparse=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));
    if(!sparse)return false;
    struct Free {uint8_t* p;~Free(){VirtualFree(p,0,MEM_RELEASE);}}free_sparse{sparse};
    if(!VirtualAlloc(sparse,0x20000,MEM_COMMIT,PAGE_READWRITE))return false;
    if(!VirtualAlloc(sparse+0x833C0000,0x20000,MEM_COMMIT,PAGE_READWRITE))return false;
    {
        auto* base=sparse;constexpr uint32_t arena=0x833D2760+32128,manager=0x18000;
        constexpr uint32_t mib=1024*1024;
        PPC_STORE_U32(0x833CDD90,manager);PPC_STORE_U32(manager+172,36*mib);PPC_STORE_U32(manager+184,64*mib);
        auto block=[&](uint32_t address,uint32_t offset,uint32_t size,bool is_free,uint32_t previous,uint32_t next) {
            PPC_STORE_U32(address,offset);PPC_STORE_U32(address+4,size);PPC_STORE_U8(address+11,is_free);PPC_STORE_U32(address+16,previous);PPC_STORE_U32(address+20,next);
        };
        PPC_STORE_U32(arena,0x821E3A70);PPC_STORE_U32(arena+8,0x1000);PPC_STORE_U32(arena+12,0);PPC_STORE_U32(arena+20,64*mib);
        block(0x1000,0,64*mib,true,0x1000,0x1000);
        PPCContext context{};NativeBackend backend{context,base};
        const std::array<uint32_t,3> sizes={2048,2048,2048};const std::array<bool,3> create={true,false,false};
        if(!check(backend.Admit(sizes,create),"production admission accepts a 16 MiB logical target with conservative 33 MiB physical headroom"))return false;
        for(unsigned i=0;i<3;++i)PPC_STORE_U32(manager+268+i*4,i+2);
        for(unsigned i=0;i<3;++i){__imp__sub_82546A90(context,base);if(!check(context.r3.u32==i+2,"actual native allocator consumes the constructor's binding slots 2, 3 and 4"))return false;}
        __imp__sub_82546A90(context,base);
        if(!check(context.r3.u32==5,"actual native allocator returns the invalid binding sentinel when all slots are occupied"))return false;
        {
            UnboundCreation creation;
            {
                UnboundScope scope{creation};context.lr=0x8256D124;context.r31.u64=0xA100;
                sub_82546A90(context,base);
                if(!check(context.r3.u32==0 && creation.slot_calls==1 && creation.target==0xA100,
                    "production staging uses the unowned sentinel without consuming an exhausted native slot"))return false;
                context.r3.u64=0xB100;context.r6.u64=0x12345678;context.r7.u64=5;context.lr=0x8256D290;
                if(!check(PrepareUnboundMap(context) && !context.r6.u64 && !context.r7.u64 && creation.map==0xB100 && creation.map_calls==1,
                    "staged map constructor receives a null source and unowned slot, so it cannot overwrite slot0"))return false;
                context.lr=0x11111111;context.r6.u64=19;context.r7.u64=23;
                if(!check(!PrepareUnboundMap(context) && context.r6.u64==19 && context.r7.u64==23,
                    "unrelated map call sites retain their original arguments"))return false;
            }
            context.lr=0x8256D124;sub_82546A90(context,base);
            if(!check(context.r3.u32==5 && !unbound_creation,"scope exit restores the actual native allocator"))return false;
        }
        context.r3.u64=0;__imp__sub_82546AE0(context,base);
        context.r3.u64=5;__imp__sub_82546AE0(context,base);
        for(unsigned i=0;i<3;++i)if(!check(PPC_LOAD_U32(manager+268+i*4)==5,"native release ignores the unowned and exhausted sentinels"))return false;
        PPC_STORE_U32(manager+172,60*mib);
        if(!check(!backend.Admit(sizes,create),"production admission enforces the independent native target-pool budget"))return false;
        PPC_STORE_U32(manager+172,36*mib);PPC_STORE_U32(arena+12,16*mib);
        block(0x1000,0,24*mib,true,0x1080,0x1040);block(0x1040,24*mib,16*mib,false,0x1000,0x1080);block(0x1080,40*mib,24*mib,true,0x1040,0x1000);
        if(!check(!backend.Admit(sizes,create),"production admission rejects fragmented physical backing despite sufficient aggregate free bytes"))return false;
        PPC_STORE_U32(arena+12,40*mib);block(0x1000,0,24*mib,true,0x1040,0x1040);block(0x1040,24*mib,40*mib,false,0x1000,0x1000);
        if(!check(!backend.Admit(sizes,create),"production admission rejects insufficient physical capacity"))return false;
        if(!check(backend.Admit(sizes,{false,false,false}),"retired-target reuse requires no additional arena capacity"))return false;
    }
    {
        auto* base=sparse;constexpr uint32_t manager=0x18000,source=0x20000,type=0x17F00;
        if(!VirtualAlloc(base,0x60000,MEM_COMMIT,PAGE_READWRITE))return false;
        if(!VirtualAlloc(base+PPC_IMAGE_BASE+PPC_IMAGE_SIZE,0x10000,MEM_COMMIT,PAGE_READWRITE))return false;
        PPC_LOOKUP_FUNC(base,0x822F0100)=FixtureSlotSetter;
        PPC_LOOKUP_FUNC(base,0x822F0110)=FixtureCachedMaterialRemoval;
        PPC_LOOKUP_FUNC(base,0x822F0120)=FixtureSourceActivity;
        std::memset(base,0,0x60000);PPC_STORE_U32(0x833CDD90,manager);
        PPC_STORE_U32(source+8,0x8203EB5C);
        PPC_STORE_U32(manager+264,source);PPC_STORE_U32(source+4,10);PPC_STORE_U32(type+4,0x822F0100);
        PPC_STORE_U32(type+24,0x822F0120);
        for(unsigned i=0;i<3;++i) {
            const uint32_t binding=0x17000+i*0x100;
            PPC_STORE_U32(source+12+(i+2)*4,binding);PPC_STORE_U32(binding,type);
            PPC_STORE_U32(manager+268+i*4,5);
        }
        std::array<Target,3> old{},replacement{};
        auto initialize=[&](uint32_t target,uint32_t slot,uint32_t source_pointer,uint32_t camera) {
            const uint32_t material=target+128,node=target+256,map=target+320;
            PPC_STORE_U32(target,0x8203D708);PPC_STORE_U32(target+24,material);
            PPC_STORE_U32(target+28,source);PPC_STORE_U32(target+32,slot);
            PPC_STORE_U32(material+100,1);PPC_STORE_U32(material+92,node);PPC_STORE_U32(material+96,node);
            PPC_STORE_U32(node+8,map);PPC_STORE_U32(map,0x8203EA6C);
            PPC_STORE_U32(map+36,source_pointer);PPC_STORE_U32(map+40,slot);PPC_STORE_U32(map+44,camera);
        };
        for(unsigned i=0;i<3;++i) {
            old[i]={0x8000+i*0x500,512,i};replacement[i]={0xA000+i*0x500,1024,i};
            initialize(old[i].handle,i+2,source,0x2000+i*16);
            initialize(replacement[i].handle,0,0,0x3000+i*16);
        }
        PPCContext context{};context.r1.u64=0x7000;context.fpscr.InitHost();NativeBackend backend{context,base};
        if(!check(backend.ValidateCurrent(old),"production validation recognizes three uniquely owned native view slots"))return false;
        constexpr uint32_t cached=0x1C000,cache_type=0x17E00,destructed_map=0x1D000;
        PPC_STORE_U32(cache_type+156,0x822F0110);PPC_STORE_U32(cached,cache_type);
        PPC_STORE_U32(cached+16,BindingChild(base,old[0].handle));
        PPC_STORE_U32(source+36,cached);PPC_STORE_U32(source+204292,1);
        const auto bad_map=BindingChild(base,replacement[2].handle);
        PPC_STORE_U32(bad_map+36,source);std::vector<uint8_t> before(base,base+0x60000);
        if(!check(!backend.Commit(old,replacement) && !std::memcmp(before.data(),base,before.size()),
            "ineligible staged map rejects the whole commit without changing bindings, references or ownership"))return false;
        PPC_STORE_U32(bad_map+36,0);
        // Exercise the original non-deleting texture-map destructor as an
        // independent oracle for the source invalidation callback and ordering.
        // Allocation, texture release and the 6000 cached materials themselves
        // remain fixture data; the original native invalidation loop runs.
        PPC_STORE_U32(cached+16,destructed_map);PPC_STORE_U32(destructed_map+36,source);
        PPC_STORE_U32(source+36,cached);PPC_STORE_U32(source+204292,1);
        if(!VirtualAlloc(base+0x82030000,0x10000,MEM_COMMIT,PAGE_READWRITE))return false;
        PPC_STORE_U32(0x8203EB5C+12,0x8256E7E0);
        const auto lookup_address=reinterpret_cast<uintptr_t>(&PPC_LOOKUP_FUNC(base,0x8256E7E0));
        if(!VirtualAlloc(reinterpret_cast<void*>(lookup_address&~uintptr_t(0xFFFF)),0x10000,MEM_COMMIT,PAGE_READWRITE))return false;
        PPC_LOOKUP_FUNC(base,0x8256E7E0)=__imp__sub_8256E7E0;
        context.r3.u64=destructed_map;__imp__sub_8256D578(context,base);
        if(!check(PPC_LOAD_U32(cached+8)==1 && PPC_LOAD_U32(cached+12)==source &&
            PPC_LOAD_U32(cached+20)==source && !PPC_LOAD_U32(source+36) &&
            PPC_LOAD_U32(source+4)==9 && PPC_LOAD_U32(destructed_map)==0x82039314,
            "original native map destructor invalidates cached materials before dropping its source reference"))return false;
        PPC_STORE_U32(source+4,10);PPC_STORE_U32(cached+8,0);
        for(unsigned round=0;round<4;++round) {
            PPC_STORE_U32(cached+16,BindingChild(base,old[0].handle));
            // Registrations at both ends and all counters survive a camera swap.
            PPC_STORE_U32(source+36,cached);PPC_STORE_U32(source+36+5999*4,cached);
            for(unsigned offset:{204292u,204296u,204320u})PPC_STORE_U32(source+offset,99);
            if(!check(backend.Commit(old,replacement),"production commit transfers all three bindings through native ref/slot setters"))return false;
            if(!check(!PPC_LOAD_U32(cached+8) && PPC_LOAD_U32(source+36)==cached && PPC_LOAD_U32(source+36+5999*4)==cached &&
                PPC_LOAD_U32(source+204292)==99 && PPC_LOAD_U32(source+204296)==99 && PPC_LOAD_U32(source+204320)==99 &&
                !PPC_LOAD_U8(source+204324) && !PPC_LOAD_U8(source+204325) &&
                PPC_LOAD_U8(source+204326)==1 && PPC_LOAD_U8(source+204327)==1 && PPC_LOAD_U8(source+204328)==1,
                "camera replacement preserves registered geometry and counters, invalidates only shadow-camera slots, and never invokes source teardown"))return false;
            for(unsigned i=0;i<3;++i) {
                const auto retired_map=BindingChild(base,old[i].handle),active_map=BindingChild(base,replacement[i].handle);
                if(!check(!PPC_LOAD_U32(old[i].handle+32) && TransferredSource(base,retired_map,source,i+2) &&
                    PPC_LOAD_U32(replacement[i].handle+32)==i+2 && PPC_LOAD_U32(active_map+36)==source && PPC_LOAD_U32(active_map+40)==i+2 &&
                    PPC_LOAD_U32(0x17000+i*0x100+8)==PPC_LOAD_U32(active_map+44) &&
                    PPC_LOAD_U32(0x17000+i*0x100+12)==round+1 && PPC_LOAD_U32(manager+268+i*4)==5,
                    "active maps inherit their exact slot, queued retired views retain source/index, native dispatch binds the camera and the slot pool stays occupied"))return false;
            }
            if(!check(PPC_LOAD_U32(source+4)==13 && retired_sources.size()==3,"source references remain held for queued retired views and do not grow on reuse"))return false;
            // Replay the precise native operations that crashed when commit
            // detached a view that was still queued for execution.
            const auto queued_child=BindingChild(base,old[0].handle);
            for(unsigned enabled:{0u,1u}){
                context.r3.u64=PPC_LOAD_U32(queued_child+36);context.r4.u64=5;context.r5.u64=enabled;
                __imp__sub_8256E5E0(context,base);
            }
            for(unsigned i=0;i<3;++i)if(!check(PPC_LOAD_U32(0x17000+i*0x100+16)==1 &&
                PPC_LOAD_U32(0x17000+i*0x100+20)==2*(round+1),"queued retired child executes both original native source operations after replacement publication"))return false;
            std::swap(old,replacement);
        }
        auto partial=old;partial[0]=replacement[0];
        if(!check(backend.Commit(old,partial) && PPC_LOAD_U32(0x17000+12)==5 &&
            PPC_LOAD_U32(0x17100+12)==4 && PPC_LOAD_U32(0x17200+12)==4 &&
            PPC_LOAD_U32(source+4)==13 && retired_sources.size()==3,"one-slot change preserves both unchanged native bindings and shared reference count"))return false;
        auto invalid=partial;invalid[1].handle=0;
        before.assign(base,base+0x60000);
        if(!check(!backend.ValidateCurrent(invalid) && !std::memcmp(before.data(),base,before.size()),
            "missing active target rejects before reading its slot or mutating guest memory"))return false;
        PPC_STORE_U32(cached+16,BindingChild(base,partial[0].handle));
        PPC_STORE_U32(source+36,cached);PPC_STORE_U32(source+204292,1);
        before.assign(base,base+0x60000);
        if(!check(backend.Commit(partial,partial) && !std::memcmp(before.data(),base,before.size()),
            "unchanged targets preserve registered native cache entries and all bindings"))return false;
        PPC_STORE_U32(source+8,0x11111111);before.assign(base,base+0x60000);
        if(!check(!backend.Commit(partial,old) && !std::memcmp(before.data(),base,before.size()),
            "foreign source interface rejects before any native cache or binding mutation"))return false;
        PPC_STORE_U32(source+8,0x8203EB5C);
        const auto retired=retired_sources.front();
        // Exercise the actual destructor hook after native references have
        // reached zero; ordinary source invalidation was independently checked
        // above. No queued view is allowed to lose this source at commit time.
        PPC_STORE_U32(retired.map+44,0);PPC_STORE_U32(retired.map+48,0);
        const auto references=PPC_LOAD_U32(source+4);
        const auto cache_removals=PPC_LOAD_U32(cached+8);
        context.r3.u64=retired.map;sub_8256D578(context,base);
        if(!check(!PPC_LOAD_U32(retired.map+36) && !PPC_LOAD_U32(retired.map+40) &&
            PPC_LOAD_U32(source+4)==references-1 && PPC_LOAD_U32(cached+8)==cache_removals &&
            retired_sources.size()==2,"actual retired-view destruction releases its held source exactly once without invalidating replacement materials"))return false;
    }
    std::memset(sparse,0,0x20000);
    std::vector<uint8_t> before(0x20000),expected_memory(0x20000);
    PPCContext input{};input.r1.u64=0xF000;input.r3.u64=owner;input.lr=0x12345678;input.fpscr.InitHost();
    const auto initial_csr=_mm_getcsr();auto expected=input;__imp__sub_829BA908(expected,sparse);const auto expected_csr=_mm_getcsr();
    std::memcpy(expected_memory.data(),sparse,expected_memory.size());std::memcpy(sparse,before.data(),before.size());_mm_setcsr(initial_csr);
    auto actual=input;EM2RefreshShadowViews(actual,sparse);
    if(!check(!std::memcmp(&expected,&actual,sizeof(actual)) && !std::memcmp(expected_memory.data(),sparse,expected_memory.size()) && _mm_getcsr()==expected_csr,
        "inactive-owner generated preparation preserves the real native context, whole memory and FP state"))return false;
    {
        std::lock_guard lock(mutex);const auto saved_state=state;const auto saved_refresh=refresh.load();
        state={};state.owner=owner;state.refresh=5;
        const auto before_csr=_mm_getcsr();
        state.current[1]={0x15000,256,1};state.retired.push_back({0x16000,1024,1});
        const bool retained_slot=TrackedShadowTargetSlot(owner,0x15000)==1 && TrackedShadowTargetSlot(owner,0x16000)==1 &&
            TrackedShadowTargetSlot(owner+4,0x16000)==-1 && TrackedShadowTargetSlot(owner,0x17000)==-1 && TrackedShadowTargetSlot(owner,0)==-1;
        PPC_STORE_U32(owner+152,0);PPC_STORE_U32(owner+156,1);PPC_STORE_U32(owner+160,0);
        const bool inactive_refresh=!ShadowOwnerNeedsRefresh(base,owner);
        PPC_STORE_U32(owner+152,1);
        const bool active_refresh=ShadowOwnerNeedsRefresh(base,owner) && !ShadowOwnerNeedsRefresh(base,owner+4);
        state.destroying=true;
        const bool destroyed_slot=TrackedShadowTargetSlot(owner,0x16000)==-1;
        const bool destroyed_refresh=!ShadowOwnerNeedsRefresh(base,owner);
        state=saved_state;refresh=saved_refresh;
        if(!check(retained_slot && destroyed_slot && inactive_refresh && active_refresh && destroyed_refresh && _mm_getcsr()==before_csr,"refresh gates and target tracking retain retired logical slots, bypass unchanged selection only for pending active sources, reject foreign/destroyed owners and preserve FP control"))return false;
    }
    REXLOG_INFO("Live shadow resize bookkeeping passed {} allocation-injection checks: real allocator refusal before native work, preallocated commit/publication/quarantine and all eight new/reused target patterns",allocation_checks);
    REXLOG_INFO("Live shadow resize lifecycle passed: transactional publication, actual native raw-view rebinding, scoped unbound staging arguments, real native reference/slot-dispatch ownership transfers and reversal, original map destructor/source cache invalidation ordering, geometry registry preservation and camera-only invalidation on repeated transfers, unchanged/rejected/foreign-source cache preservation, one-slot changes, GPU-fenced retirement, reuse, failure quarantine and native preparation parity; actual native creation/gameplay remain live checks");
    return true;
}
