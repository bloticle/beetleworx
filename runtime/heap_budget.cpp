#include "heap_budget.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <rex/kernel/xmemory.h>
#include <array>
#include <cstring>
#include <immintrin.h>
#include <windows.h>

PPC_EXTERN_FUNC(__imp__sub_8272C738);
PPC_EXTERN_FUNC(__imp__sub_8272CC88);
namespace {
constexpr uint32_t kRootHeap=0x833D2760,kPhysicalBudget=0x8339A138;
// Native CPU pools share the SDK's 512 MiB physical backing with these arenas.
// 384 MiB graphics + the expanded CPU defaults cannot fit that address space.
constexpr uint32_t kNativePhysicalBytes=256u*1024*1024,kPCPhysicalBytes=320u*1024*1024;
constexpr uint32_t kNativeDefaultBytes=52u*1024*1024;
constexpr uint32_t kPCDefaultBytes=80u*1024*1024;
// Only the compiled-in DEFAULT configuration call from sub_8272CC88.
// Other pools, explicit overrides and already initialized descriptors retain
// their native behavior. This adds arena headroom, not guest address space.
bool ExpandDefaultHeap(const PPCContext& ctx,uint8_t* base) {
    return ctx.lr==0x8272CCBC && ctx.r4.u32==kNativeDefaultBytes &&
        PPC_LOAD_U32(ctx.r3.u32+2996)==0x8206C300 &&
        !PPC_LOAD_U32(ctx.r3.u32+3008) && !PPC_LOAD_U32(ctx.r3.u32+3016);
}
void Configure(PPCContext& ctx,uint8_t* base,bool report) {
    if(ExpandDefaultHeap(ctx,base)) {
        ctx.r4.u64=kPCDefaultBytes;
        if(report) {
            const auto csr=_mm_getcsr();
            REXLOG_INFO("Native DEFAULT heap: 52 -> 80 MiB; other pool budgets unchanged");
            _mm_setcsr(csr);
        }
    }
    __imp__sub_8272C738(ctx,base);
}
void ConfigurePhysical(PPCContext& ctx,uint8_t* base,PPCFunc* initialize,bool report) {
    const auto root=ctx.r3.u32;
    initialize(ctx,base);
    // The original initializer splits this total into main and reserved arenas,
    // stores their actual sizes, then obtains backing. Change the source budget
    // before that split, never an already-live allocator or a custom budget.
    // Reserve headroom even when starting at low quality: an in-game quality
    // change must not depend on having selected high shadows before startup.
    if(root==kRootHeap && PPC_LOAD_U32(kPhysicalBudget)==kNativePhysicalBytes &&
       !PPC_LOAD_U32(kRootHeap+32160) && !PPC_LOAD_U32(kRootHeap+32204)) {
        PPC_STORE_U32(kPhysicalBudget,kPCPhysicalBytes);
        if(report){const auto csr=_mm_getcsr();REXLOG_INFO("Native graphics physical-memory budget: 256 -> 320 MiB, including low-quality startup for later quality changes; shared CPU physical backing retained, reserved arena unchanged");_mm_setcsr(csr);}
    }
}
}
PPC_FUNC_IMPL(sub_8272C738){Configure(ctx,base,true);}
PPC_FUNC_IMPL(sub_8272CC88){ConfigurePhysical(ctx,base,__imp__sub_8272CC88,true);}

bool em2::TestNativeHeapBudget(rex::memory::Memory* memory) {
    const auto entry_csr=_mm_getcsr();struct RestoreFP {unsigned csr;~RestoreFP(){_mm_setcsr(csr);}} restore_fp{entry_csr};
    auto check=[](bool ok,const char* text){if(!ok)REXLOG_ERROR("Native heap budget test: {}",text);return ok;};
    for(unsigned variant=0;variant<6;++variant) {
        alignas(16) std::array<uint8_t,8192> expected_memory{},actual_memory{};
        auto* base=expected_memory.data();constexpr uint32_t pool=1024;
        PPC_STORE_U32(pool+2996,variant==2?0x8206C308:0x8206C300);
        PPC_STORE_U32(pool+3008,variant==3?0x100000:0);
        PPC_STORE_U32(pool+3016,variant==4?4096:0);
        actual_memory=expected_memory;
        PPCContext input{};input.r1.u64=0x1F00;input.r3.u64=pool;
        input.r4.u64=variant==5?60u*1024*1024:kNativeDefaultBytes;
        input.r5.u64=65536;input.r6.u64=131072;input.r7.u64=65536;input.r8.u64=65536;
        input.r9.u64=0;input.r31.u64=0x1122334455667788;input.lr=variant==1?0x8272DDC4:0x8272CCBC;
        input.fpscr.InitHost();
        auto expected=input;if(variant==0)expected.r4.u64=kPCDefaultBytes;
        const auto initial_csr=_mm_getcsr();
        __imp__sub_8272C738(expected,base);const auto expected_csr=_mm_getcsr();
        _mm_setcsr(initial_csr);auto actual=input;Configure(actual,actual_memory.data(),variant==0);
        if(!check(!std::memcmp(&expected,&actual,sizeof(actual)) && expected_memory==actual_memory && _mm_getcsr()==expected_csr,
            "configuration matches original translated setter with only the intended DEFAULT budget change"))return false;
        if(variant==0) {
            base=actual_memory.data();
            if(!check(PPC_LOAD_U32(pool+3016)==kPCDefaultBytes &&
                PPC_LOAD_U32(pool+3052)==65536 && PPC_LOAD_U32(pool+3112)==131072 &&
                PPC_LOAD_U32(pool+3172)==65536 && PPC_LOAD_U32(pool+3232)==65536,
                "expanded DEFAULT arena retains all native small-allocation bin counts"))return false;
        }
    }
    auto* physical=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));
    if(!physical)return false;
    struct Release {uint8_t* base;~Release(){VirtualFree(base,0,MEM_RELEASE);}}release{physical};
    if(!VirtualAlloc(physical,0x10000,MEM_COMMIT,PAGE_READWRITE) || !VirtualAlloc(physical+0x83390000,0x10000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(physical+0x833D0000,0x10000,MEM_COMMIT,PAGE_READWRITE))return false;
    for(unsigned variant=0;variant<7;++variant) {
        auto* base=physical;
        const auto initial_budget=variant==1?192u*1024*1024:variant==2?320u*1024*1024:kNativePhysicalBytes;
        PPC_STORE_U32(kPhysicalBudget,initial_budget);
        PPC_STORE_U32(kRootHeap+32160,variant==3?0xA0000000:0);
        PPC_STORE_U32(kRootHeap+32204,variant==4?0xB0000000:0);
        PPCContext input{};input.r3.u64=variant==5?kRootHeap+4:kRootHeap;input.r31.u64=0x12345678;input.fpscr.InitHost();
        const auto initial_csr=_mm_getcsr();
        const auto expected_budget=variant==0 || variant==6?kPCPhysicalBytes:PPC_LOAD_U32(kPhysicalBudget);
        auto original=+[](PPCContext& native,uint8_t* base){native.r3.u64=PPC_LOAD_U32(kPhysicalBudget);native.r31.u64=0xDEADBEEF;_mm_setcsr(_mm_getcsr()^0x8000);};
        auto expected=input;original(expected,base);const auto expected_csr=_mm_getcsr();
        PPC_STORE_U32(kPhysicalBudget,initial_budget);_mm_setcsr(initial_csr);
        ConfigurePhysical(input,base,original,variant==6);
        if(!check(!std::memcmp(&input,&expected,sizeof(input)) && _mm_getcsr()==expected_csr && PPC_LOAD_U32(kPhysicalBudget)==expected_budget,
            "physical startup headroom preserves native callback results, smaller/larger custom budgets and live backing"))return false;
    }
    // Exercise the real default initializer from zeroed globals and the
    // constructor's DEFAULT label (required by the guarded 52 -> 80 MiB hook).
    // Its output
    // context and every byte must match except the reviewed physical budget.
    std::array<uint8_t,0x10000> expected_root{},expected_globals{},expected_stack{};
    auto* base=physical;std::memset(base,0,0x10000);std::memset(base+0x83390000,0,0x10000);std::memset(base+0x833D0000,0,0x10000);
    PPC_STORE_U32(kRootHeap+5760+2996,0x8206C300);
    PPCContext seed{};seed.r1.u64=0xF000;seed.r3.u64=kRootHeap;seed.fpscr.InitHost();const auto start_csr=_mm_getcsr();
    auto expected=seed;__imp__sub_8272CC88(expected,base);const auto expected_csr=_mm_getcsr();
    if(!check(PPC_LOAD_U32(kPhysicalBudget)==kNativePhysicalBytes && PPC_LOAD_U32(0x8339A13C)==15u*1024*1024,"real native physical defaults and reserved budget"))return false;
    PPC_STORE_U32(kPhysicalBudget,kPCPhysicalBytes);
    std::memcpy(expected_root.data(),base+0x833D0000,0x10000);std::memcpy(expected_globals.data(),base+0x83390000,0x10000);std::memcpy(expected_stack.data(),base,0x10000);
    std::memset(base,0,0x10000);std::memset(base+0x83390000,0,0x10000);std::memset(base+0x833D0000,0,0x10000);_mm_setcsr(start_csr);
    PPC_STORE_U32(kRootHeap+5760+2996,0x8206C300);
    auto actual=seed;ConfigurePhysical(actual,base,__imp__sub_8272CC88,false);
    if(!check(!std::memcmp(&actual,&expected,sizeof(actual)) && _mm_getcsr()==expected_csr &&
        !std::memcmp(expected_root.data(),base+0x833D0000,0x10000) && !std::memcmp(expected_globals.data(),base+0x83390000,0x10000) && !std::memcmp(expected_stack.data(),base,0x10000),
        "real native default initializer changes only physical headroom, from zero globals with constructor DEFAULT label"))return false;
    // Mirror the nine 512 KiB-rounded terms at native 8272F1E0..8272F284.
    // The separate pool initialized before those terms uses global 8339A134.
    uint64_t cpu_bytes=0;
    for(const uint32_t offset: {32292u,25256u,18664u,12072u,32348u,28552u,21960u,15368u,8776u})
        cpu_bytes+=(uint64_t(PPC_LOAD_U32(kRootHeap+offset))+0x7FFFF)&~uint64_t(0x7FFFF);
    const auto metadata_bytes=PPC_LOAD_U32(0x8339A134);
    const auto combined=uint64_t(kPCPhysicalBytes)+cpu_bytes+metadata_bytes;
    if(!check(combined+32ull*1024*1024<=512ull*1024*1024,
        "combined native graphics/CPU/metadata backing leaves at least 32 MiB for platform allocations"))return false;
    if(!check(384ull*1024*1024+cpu_bytes>512ull*1024*1024,
        "regression: prior 384 MiB graphics budget exceeds shared physical capacity with native CPU defaults"))return false;
    if(memory) {
        auto* heap=memory->LookupHeapByType(true,65536);
        if(!heap || !check(heap->heap_size()==512u*1024*1024,"actual SDK physical heap capacity"))return false;
        std::array<uint32_t,4> allocations{};
        struct FreeBacking {rex::memory::BaseHeap* heap;std::array<uint32_t,4>& allocations;
            ~FreeBacking(){for(auto address:allocations)if(address)heap->Release(address);}} free_backing{heap,allocations};
        const std::array<uint32_t,4> sizes={kPCPhysicalBytes-15u*1024*1024,15u*1024*1024,metadata_bytes,static_cast<uint32_t>(cpu_bytes)};
        for(size_t i=0;i<sizes.size();++i) {
            if(!check(heap->Alloc(sizes[i],65536,rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,
                rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite|
                (i<2?rex::memory::kMemoryProtectWriteCombine:0),true,&allocations[i]),
                "actual SDK allocates all native startup backing concurrently"))return false;
            uint32_t allocated_size=0;
            if(!check(!(allocations[i]&65535) && heap->QuerySize(allocations[i],&allocated_size) && allocated_size==sizes[i],
                "actual SDK backing has the native 64 KiB alignment and complete requested size"))return false;
        }
        REXLOG_INFO("Native startup backing passed actual SDK physical allocations: main={} reserve={} metadata={} CPU={} bytes; no title",sizes[0],sizes[1],sizes[2],sizes[3]);
        for(auto& address:allocations) {
            if(!check(heap->Release(address),"actual SDK releases diagnostic startup backing"))return false;
            address=0;
        }
    }
    REXLOG_INFO("Native heap budget tests passed: DEFAULT headroom, seven physical startup guards, native initializer context/memory/FP parity; combined startup={} bytes, remaining={} bytes. Full level loading remains a live check",combined,512ull*1024*1024-combined);
    return true;
}
