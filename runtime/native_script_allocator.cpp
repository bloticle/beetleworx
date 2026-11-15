#include "ppc_context.h"
#include "guest_read_batch.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/kernel/kernel_state.h>
#include <rex/memory.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <cstring>
#include <mutex>
#include <windows.h>
#include <immintrin.h>
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(bool,em2_probe_native_script_persistence);
PPC_EXTERN_FUNC(__imp__sub_82730F78);
PPC_EXTERN_FUNC(__imp__sub_826D41A0);
namespace {
constexpr uint32_t kCaptureArenaBytes=8u*1024*1024;
struct Backing {uint8_t* base=nullptr;rex::memory::Memory* memory=nullptr;uint32_t address=0,size=0;};
std::array<Backing,4> backings{};
std::mutex backing_mutex;
std::atomic<unsigned> backing_count{0};
thread_local uint32_t pending_backing=0;
struct RestoreHost {DWORD error=GetLastError();unsigned csr=_mm_getcsr();~RestoreHost(){SetLastError(error);_mm_setcsr(csr);}};
uint32_t ReserveBacking(uint8_t* base,rex::memory::Memory* memory,uint32_t size){
    RestoreHost host;std::lock_guard lock(backing_mutex);
    for(auto& block:backings)if(!block.address){
        const auto address=memory->SystemHeapAlloc(size,32);
        if(address){block={base,memory,address,size};backing_count.fetch_add(1,std::memory_order_release);}
        return address;
    }
    return 0;
}
// These blocks belong exclusively to SystemHeap. Never pass them to the
// original context-7 allocator, including during native VM destruction.
bool ServiceBacking(PPCContext& ctx,uint8_t* base){
    if(!pending_backing && !backing_count.load(std::memory_order_acquire))return false;
    RestoreHost host;
    const auto old=ctx.r4.u32,size=ctx.r6.u32;
    if(!old && pending_backing && size==kCaptureArenaBytes){ctx.r3.u64=pending_backing;pending_backing=0;return true;}
    if(!old)return false;
    std::lock_guard lock(backing_mutex);
    for(auto& block:backings)if(block.base==base && block.address==old){
        if(!size){block.memory->SystemHeapFree(old);block={};backing_count.fetch_sub(1,std::memory_order_release);ctx.r3.u64=0;return true;}
        // Keep native realloc failure semantics: the old allocation survives.
        if(size>64u*1024*1024){ctx.r3.u64=0;return true;}
        const auto replacement=block.memory->SystemHeapAlloc(size,32);
        if(replacement){std::memcpy(base+replacement,base+old,std::min(size,block.size));block.memory->SystemHeapFree(old);block.address=replacement;block.size=size;}
        ctx.r3.u64=replacement;return true;
    }
    return false;
}
}
// Only the private main-VM capture probe uses expanded PC virtual backing.
// The native HKS arena, GC, serializer and ownership callbacks remain in use.
PPC_FUNC_IMPL(sub_826D41A0){
    const auto options=ctx.r3.u32;
    if(!REXCVAR_GET(em2_private_profile) || !REXCVAR_GET(em2_probe_native_script_persistence) ||
       ctx.lr!=0x827321D0 || !em2::GuestReadBatch::Readable(base+options,24) ||
       PPC_LOAD_U32(options)!=1572864 || PPC_LOAD_U32(options+4)!=0x82730F78 || pending_backing){
        __imp__sub_826D41A0(ctx,base);return;
    }
    auto* kernel=rex::kernel::kernel_state();
    if(!kernel || !kernel->memory()){__imp__sub_826D41A0(ctx,base);return;}
    const auto address=ReserveBacking(base,kernel->memory(),kCaptureArenaBytes);
    if(!address){__imp__sub_826D41A0(ctx,base);return;}
    struct ResetOptions {
        PPCContext& ctx;uint8_t* base;uint32_t options,address;
        ~ResetOptions(){
            PPC_STORE_U32(options,1572864);
            if(pending_backing==address){auto cleanup=ctx;cleanup.r4.u64=address;cleanup.r6.u64=0;ServiceBacking(cleanup,base);}
            pending_backing=0;
        }
    }reset{ctx,base,options,address};
    pending_backing=address;PPC_STORE_U32(options,kCaptureArenaBytes);
    {RestoreHost host;REXLOG_INFO("Private rollback HKS arena: 8 MiB PC virtual backing {:08X}; console physical pools unchanged",address);}
    __imp__sub_826D41A0(ctx,base);
}
namespace em2 {
bool TestPrivateNativeScriptBacking(PPCContext& ctx,uint8_t* base){
    RestoreHost host;
    auto* kernel=rex::kernel::kernel_state();if(!kernel || !kernel->memory())return false;
    const auto before=backing_count.load(std::memory_order_acquire);
    auto address=ReserveBacking(base,kernel->memory(),4096);if(!address)return false;
    struct Release {PPCContext& ctx;uint8_t* base;uint32_t& address;~Release(){if(address){auto c=ctx;c.r4.u64=address;c.r6.u64=0;ServiceBacking(c,base);}}}release{ctx,base,address};
    for(unsigned i=0;i<4096;++i)base[address+i]=uint8_t(i*31+7);
    auto c=ctx;c.r4.u64=address;c.r5.u64=1;c.r6.u64=8192;
    const auto error=GetLastError();const auto csr=_mm_getcsr();
    if(!ServiceBacking(c,base) || !c.r3.u32 || GetLastError()!=error || _mm_getcsr()!=csr)return false;
    address=c.r3.u32;
    for(unsigned i=0;i<4096;++i)if(base[address+i]!=uint8_t(i*31+7))return false;
    c.r4.u64=address;c.r6.u64=64u*1024*1024+1;
    if(!ServiceBacking(c,base) || c.r3.u32 || GetLastError()!=error || _mm_getcsr()!=csr)return false;
    for(unsigned i=0;i<4096;++i)if(base[address+i]!=uint8_t(i*31+7))return false;
    // Unowned addresses must fall through without changing native registers.
    c.r4.u64=address+32;const auto unowned=c;
    if(ServiceBacking(c,base) || std::memcmp(&c,&unowned,sizeof(c)))return false;
    c.r4.u64=address;c.r6.u64=0;
    if(!ServiceBacking(c,base) || c.r3.u32)return false;
    address=0;
    if(ServiceBacking(c,base) || backing_count.load(std::memory_order_acquire)!=before || GetLastError()!=error || _mm_getcsr()!=csr)return false;
    REXLOG_INFO("Private HKS PC backing lifecycle passed: actual SDK allocation/realloc/free, copied payload, bounded failure retains old block, unowned/freed addresses no longer claimed, host state retained");return true;
}
}
// Original HKS allocator callback: userdata, old pointer, old size, new size.
// Retain original ownership except the explicitly tracked private PC blocks.
PPC_FUNC_IMPL(sub_82730F78){
    if(ServiceBacking(ctx,base))return;
    if(!REXCVAR_GET(em2_private_profile) || !REXCVAR_GET(em2_probe_native_script_persistence)){
        __imp__sub_82730F78(ctx,base);return;
    }
    const auto userdata=ctx.r3.u32,old_pointer=ctx.r4.u32,old_size=ctx.r5.u32,new_size=ctx.r6.u32,caller=uint32_t(ctx.lr);
    const auto full_size=ctx.r6.u64;
    __imp__sub_82730F78(ctx,base);
    const auto error=GetLastError();const auto csr=_mm_getcsr();
    struct Restore {DWORD error;unsigned csr;~Restore(){SetLastError(error);_mm_setcsr(csr);}}restore{error,csr};
    static thread_local unsigned reported=0;
    if(new_size && (!ctx.r3.u32 || (new_size>=32768 && reported++<16)))
        REXLOG_INFO("Native HKS allocator: caller {:08X}, context {:08X}, old {:08X}/{} bytes, requested {} (full {:016X}), result {:08X}",caller,userdata,old_pointer,old_size,new_size,full_size,ctx.r3.u32);
}
