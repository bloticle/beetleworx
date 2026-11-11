#include "crash_handler.h"
#include "physical_arena.h"
#include <Windows.h>
#include <cstdio>
#include <string>
#include <exception>
#include <cstdlib>
#include <cstring>
#include "ppc_context.h"
#include "allocation_trace.h"
#include "shadow_resources.h"
#include <rex/runtime/thread_state.h>

namespace {
std::wstring crash_path;
LONG WINAPI RecordCrash(EXCEPTION_POINTERS* exception) {
    char buffer[8192];
    int length = std::snprintf(buffer, sizeof(buffer),
        "Beetleworx unhandled exception\ncode=0x%08lX\nmodule=%p\npc=%p\naccess=%llu\naddress=0x%llX\n",
        exception->ExceptionRecord->ExceptionCode, GetModuleHandleW(nullptr),
        exception->ExceptionRecord->ExceptionAddress,
        static_cast<unsigned long long>(exception->ExceptionRecord->NumberParameters > 0 ? exception->ExceptionRecord->ExceptionInformation[0] : 0),
        static_cast<unsigned long long>(exception->ExceptionRecord->NumberParameters > 1 ? exception->ExceptionRecord->ExceptionInformation[1] : 0));
    HANDLE file = CreateFileW(crash_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) { DWORD written; WriteFile(file, buffer, length, &written, nullptr); }
    const auto level=em2::CurrentNativeLoadLevel();
    if(level[0])length+=std::snprintf(buffer+length,sizeof(buffer)-length,"requested_level=%s\n",level.data());
    CONTEXT context = *exception->ContextRecord;
    length += std::snprintf(buffer+length, sizeof(buffer)-length, "rax=%llX rcx=%llX rdx=%llX r8=%llX r9=%llX rsp=%llX\n", context.Rax, context.Rcx, context.Rdx, context.R8, context.R9, context.Rsp);
    MEMORY_BASIC_INFORMATION candidate{};
    __try {
    const auto allocation=em2::CurrentNativeAllocation();
    const auto shadow=em2::CurrentShadowCreation();
    if(shadow.active)length+=std::snprintf(buffer+length,sizeof(buffer)-length,
        "shadow_creation owner=%08X slot=%u cached=%u requested=%u root_requests=%u root_requested_bytes=%llu physical_requests=%u physical_completed=%u physical_requested_bytes=%llu physical_granted_bytes=%llu last_caller=%08X rejected_size=%u rejected_address=%08X\n",
        shadow.owner,shadow.slot,shadow.cached_size,shadow.requested_size,shadow.requests,
        static_cast<unsigned long long>(shadow.requested_bytes),shadow.physical_requests,shadow.physical_completed,
        static_cast<unsigned long long>(shadow.physical_requested_bytes),static_cast<unsigned long long>(shadow.physical_granted_bytes),shadow.last_caller,shadow.failed_allocation_size,shadow.failed_allocation_address);
    auto* state = rex::runtime::ThreadState::Get();
    auto cpu_address = state ? reinterpret_cast<uintptr_t>(state->context()) : 0;
    if(!cpu_address && allocation.active)cpu_address=allocation.guest_context;
    if(!cpu_address)cpu_address=context.Rcx;
    if (VirtualQuery(reinterpret_cast<void*>(cpu_address), &candidate, sizeof(candidate)) && candidate.State == MEM_COMMIT && !(candidate.Protect & (PAGE_NOACCESS|PAGE_GUARD)) && cpu_address + sizeof(PPCContext) <= reinterpret_cast<uintptr_t>(candidate.BaseAddress) + candidate.RegionSize) {
        auto* cpu = reinterpret_cast<const PPCContext*>(cpu_address);
        length += std::snprintf(buffer+length, sizeof(buffer)-length, "guest ctr=%llX lr=%llX r3=%llX r4=%llX r5=%llX r29=%llX\n", cpu->ctr.u64, cpu->lr, cpu->r3.u64, cpu->r4.u64, cpu->r5.u64, cpu->r29.u64);
        length += std::snprintf(buffer+length, sizeof(buffer)-length, "guest r10=%llX r11=%llX r28=%llX r30=%llX r31=%llX\n", cpu->r10.u64, cpu->r11.u64, cpu->r28.u64, cpu->r30.u64, cpu->r31.u64);
        length += std::snprintf(buffer+length, sizeof(buffer)-length, "guest r6=%llX r7=%llX r8=%llX r9=%llX sp=%llX\n", cpu->r6.u64, cpu->r7.u64, cpu->r8.u64, cpu->r9.u64, cpu->r1.u64);
        if(allocation.active) {
            length+=std::snprintf(buffer+length,sizeof(buffer)-length,
                "allocation heap=%08X requested=%u alignment=%u flags=%08X kind=%u bucket=%u caller=%08X depth=%u size_ptr=%08X align_ptr=%08X size_valid=%u align_valid=%u\n",
                allocation.heap,allocation.requested,allocation.alignment,allocation.flags,allocation.kind,allocation.bucket,
                allocation.caller,allocation.depth,allocation.size_pointer,allocation.alignment_pointer,unsigned(allocation.has_size),unsigned(allocation.has_alignment));
            if(allocation.heap==0x833D2760 && (cpu->r10.u32==0x8206C514 || cpu->r10.u32==0x8206C530)) {
                auto* base=reinterpret_cast<uint8_t*>(allocation.guest_base);
                const auto heap=allocation.heap;
                length+=std::snprintf(buffer+length,sizeof(buffer)-length,
                    "physical_arena total_budget=%u main_bytes=%u main_base=%08X reserve_bytes=%u reserve_base=%08X effective_alignment=%u\n",
                    PPC_LOAD_U32(0x8339A138),PPC_LOAD_U32(heap+32156),PPC_LOAD_U32(heap+32160),
                    PPC_LOAD_U32(heap+32200),PPC_LOAD_U32(heap+32204),cpu->r31.u32);
                  em2::PhysicalArenaStats stats{};
                  const bool snapshot=em2::ReadPhysicalArenaStats(base,heap+32128,cpu->r31.u32,stats);
                  length+=std::snprintf(buffer+length,sizeof(buffer)-length,
                      "physical_main_snapshot valid=%u used=%u free=%u largest_aligned=%u blocks=%u\n",
                      unsigned(snapshot),stats.used,stats.free,stats.largest_aligned,stats.blocks);
            }
            // The allocator's live r31 identifies the selected native pool;
            // these accesses remain inside the exception filter's SEH guard.
            auto* base=reinterpret_cast<uint8_t*>(allocation.guest_base);const auto pool=cpu->r31.u32;
            const bool native_oom=exception->ExceptionRecord->NumberParameters>1 &&
                exception->ExceptionRecord->ExceptionInformation[0]==1 &&
                exception->ExceptionRecord->ExceptionInformation[1]==allocation.guest_base && cpu->r10.u32==0x8206C56C;
            MEMORY_BASIC_INFORMATION pool_region{};const auto pool_address=allocation.guest_base+uint64_t(pool)+PPC_PHYS_HOST_OFFSET(pool);
            const bool readable=pool && VirtualQuery(reinterpret_cast<void*>(pool_address),&pool_region,sizeof(pool_region)) &&
                pool_region.State==MEM_COMMIT && !(pool_region.Protect&(PAGE_NOACCESS|PAGE_GUARD)) &&
                pool_address+3046<=reinterpret_cast<uintptr_t>(pool_region.BaseAddress)+pool_region.RegionSize;
            if(base && native_oom && readable) {
                const auto label=PPC_LOAD_U32(pool+2996);
                length+=std::snprintf(buffer+length,sizeof(buffer)-length,
                    "allocation selected_pool=%08X counter_0=%u counter_3028=%u fatal=%u label_ptr=%08X backend=%08X backing_base=%08X aligned_base=%08X configured_bytes=%u managed_bytes=%u\n",pool,
                    PPC_LOAD_U32(pool),PPC_LOAD_U32(pool+3028),unsigned(PPC_LOAD_U8(pool+3045)),label,
                    PPC_LOAD_U32(pool+3004),PPC_LOAD_U32(pool+3008),PPC_LOAD_U32(pool+3012),PPC_LOAD_U32(pool+3016),PPC_LOAD_U32(pool+3020));
                MEMORY_BASIC_INFORMATION label_region{};const auto label_address=allocation.guest_base+uint64_t(label)+PPC_PHYS_HOST_OFFSET(label);
                if(label && VirtualQuery(reinterpret_cast<void*>(label_address),&label_region,sizeof(label_region)) &&
                   label_region.State==MEM_COMMIT && !(label_region.Protect&(PAGE_NOACCESS|PAGE_GUARD)) &&
                   label_address+64<=reinterpret_cast<uintptr_t>(label_region.BaseAddress)+label_region.RegionSize) {
                    char preview[64]{};bool printable=true;
                    for(unsigned i=0;i<63;++i){const auto c=PPC_LOAD_U8(label+i);if(!c)break;if(c<32 || c>=127){printable=false;break;}preview[i]=char(c);}
                    if(printable && preview[0])length+=std::snprintf(buffer+length,sizeof(buffer)-length,"allocation pool_label=%s\n",preview);
                }
            }
        }
        for (auto* mapping = PPCFuncMappings; mapping->guest; ++mapping)
            if (mapping->guest == cpu->ctr.u32)
                length += std::snprintf(buffer+length, sizeof(buffer)-length, "guest mapping expected=%p\n", reinterpret_cast<void*>(mapping->host));
    }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        length += std::snprintf(buffer+length, sizeof(buffer)-length, "Guest context unavailable\n");
    }
    __try {
    for (unsigned i = 0; i < 48 && length > 0 && length < sizeof(buffer)-128; ++i) {
        length += std::snprintf(buffer+length, sizeof(buffer)-length, "frame[%u]=%016llX rsp=%016llX\n", i, context.Rip, context.Rsp);
        DWORD64 module_base = 0;
        auto function = RtlLookupFunctionEntry(context.Rip, &module_base, nullptr);
        if (function) {
            PVOID handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, module_base, context.Rip, function, &context, &handler_data, &establisher, nullptr);
        } else {
            MEMORY_BASIC_INFORMATION region{};
            if (!VirtualQuery(reinterpret_cast<void*>(context.Rsp), &region, sizeof(region)) || region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD))) break;
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
        }
        if (!context.Rip) break;
    }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        length += std::snprintf(buffer+length, sizeof(buffer)-length, "Unwind stopped at unreadable frame\n");
    }
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written;
        SetFilePointer(file, 0, nullptr, FILE_BEGIN);
        if (length > 0) WriteFile(file, buffer, static_cast<DWORD>(length), &written, nullptr);
        CloseHandle(file);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
[[noreturn]] void RecordTermination(){
    CONTEXT context{};RtlCaptureContext(&context);
    EXCEPTION_RECORD record{};record.ExceptionCode=0xE00000F1;record.ExceptionAddress=reinterpret_cast<void*>(context.Rip);
    EXCEPTION_POINTERS pointers{&record,&context};RecordCrash(&pointers);
    char reason[1024]="std::terminate without an active C++ exception\n";
    if(const auto exception=std::current_exception()){
        try{std::rethrow_exception(exception);}catch(const std::exception& error){std::snprintf(reason,sizeof(reason),"std::terminate: %s\n",error.what());}
        catch(...){std::snprintf(reason,sizeof(reason),"std::terminate with a non-std C++ exception\n");}
    }
    const auto file=CreateFileW(crash_path.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file!=INVALID_HANDLE_VALUE){DWORD written;WriteFile(file,reason,static_cast<DWORD>(std::strlen(reason)),&written,nullptr);CloseHandle(file);}
    std::abort();
}
}
void InstallCrashHandler(const std::filesystem::path& log_path) {
    crash_path = log_path.wstring();
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(RecordCrash);
    std::set_terminate(RecordTermination);
}
