#include "ppc_context.h"
#include <rex/logging.h>
#include <rex/runtime/thread_state.h>
#include <rex/kernel/xthread.h>
#include <string>
#include <unordered_map>
#include <mutex>

PPC_EXTERN_FUNC(__imp__sub_8261A8D8);
PPC_EXTERN_FUNC(__imp__sub_826C9930);
PPC_EXTERN_FUNC(__imp__sub_831A98A0);
PPC_EXTERN_FUNC(__imp__sub_826E5358);
PPC_EXTERN_FUNC(__imp__sub_826EABC8);
PPC_EXTERN_FUNC(__imp__sub_8271CA70);
PPC_EXTERN_FUNC(__imp__sub_826D8320);
PPC_EXTERN_FUNC(__imp__sub_826E53F0);
PPC_EXTERN_FUNC(__imp__sub_826CAE90);
namespace {
struct TableCreation { uint32_t caller, array, count; };
std::mutex table_mutex;
std::unordered_map<uint32_t, TableCreation> table_creations;
}

PPC_FUNC_IMPL(sub_826E53F0) {
    auto caller = static_cast<uint32_t>(ctx.lr);
    __imp__sub_826E53F0(ctx, base);
    auto table = ctx.r3.u32;
    TableCreation creation{caller, PPC_LOAD_U32(table+24), PPC_LOAD_U32(table+28)};
    std::lock_guard lock(table_mutex);
    table_creations[table] = creation;
}

PPC_FUNC_IMPL(sub_826CAE90) {
    auto address=ctx.r3.u32, bytes=ctx.r5.u32;
    auto value=ctx.r4.u8;
    auto caller=ctx.lr;
    __imp__sub_826CAE90(ctx, base);
    if (bytes == 28 || bytes == 264) {
        for (unsigned i=0; i<bytes; ++i) if (PPC_LOAD_U8(address+i) != value) {
            REXLOG_ERROR("Fast fill mismatch: address={:08X} bytes={} offset={} expected={} got={} caller={:08X}", address, bytes, i, value, PPC_LOAD_U8(address+i), caller);
            break;
        }
    }
}

PPC_FUNC_IMPL(sub_826D8320) {
    auto table = ctx.r3.u32;
    if (PPC_LOAD_U32(table+28) && !PPC_LOAD_U32(table+24)) {
        {
            std::lock_guard lock(table_mutex);
            auto found = table_creations.find(table);
            if (found != table_creations.end()) REXLOG_ERROR("VM table creation: caller={:08X} array={:08X} count={}", found->second.caller, found->second.array, found->second.count);
            else REXLOG_ERROR("VM invalid object was not created by the table constructor");
        }
        REXLOG_ERROR("VM invalid table at {:08X}: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}", table,
            PPC_LOAD_U32(table), PPC_LOAD_U32(table+4), PPC_LOAD_U32(table+8), PPC_LOAD_U32(table+12),
            PPC_LOAD_U32(table+16), PPC_LOAD_U32(table+20), PPC_LOAD_U32(table+24), PPC_LOAD_U32(table+28));
        std::string bytes;
        for (unsigned i=0; i<96; ++i) { auto c=PPC_LOAD_U8(table+32+i); bytes.push_back(c>=32&&c<127 ? c : '.'); }
        REXLOG_ERROR("VM table trailing bytes: {}", bytes);
    }
    __imp__sub_826D8320(ctx, base);
}

PPC_FUNC_IMPL(sub_8271CA70) {
    auto bytes = ctx.r4.u32;
    auto caller = ctx.lr;
    __imp__sub_8271CA70(ctx, base);
    if (bytes && !ctx.r3.u32) REXLOG_ERROR("VM allocation failed: bytes={} caller={:08X}", bytes, caller);
}

PPC_FUNC_IMPL(sub_826E5358) {
    auto table = ctx.r3.u32, requested = ctx.r5.u32;
    auto caller = ctx.lr;
    __imp__sub_826E5358(ctx, base);
    if (PPC_LOAD_U32(table+28) && !PPC_LOAD_U32(table+24))
        REXLOG_ERROR("VM array grow invalid: table={:08X} requested={} count={} caller={:08X}", table, requested, PPC_LOAD_U32(table+28), caller);
}

PPC_FUNC_IMPL(sub_826EABC8) {
    auto table = ctx.r3.u32, requested = ctx.r5.u32;
    auto caller = ctx.lr;
    __imp__sub_826EABC8(ctx, base);
    if (PPC_LOAD_U32(table+28) && !PPC_LOAD_U32(table+24))
        REXLOG_ERROR("VM resize invalid: table={:08X} requested={} count={} caller={:08X}", table, requested, PPC_LOAD_U32(table+28), caller);
}

PPC_FUNC_IMPL(sub_831A98A0) {
    auto object = ctx.r3.u32;
    auto table = PPC_LOAD_U32(object);
    REXLOG_INFO("Worker start: object={:08X} callback={:08X} stack={:08X}", object, PPC_LOAD_U32(table+12), ctx.r1.u32);
    __imp__sub_831A98A0(ctx, base);
}

PPC_FUNC_IMPL(sub_826C9930) {
    auto r30 = ctx.r30.u64, r31 = ctx.r31.u64, sp = ctx.r1.u64;
    auto caller = ctx.lr;
    if (auto* state = rex::runtime::ThreadState::Get(); state && state->context() != &ctx)
        REXLOG_ERROR("Wait uses different contexts: active={} thread={} activeSP={:X} threadSP={:X}", reinterpret_cast<uintptr_t>(&ctx), reinterpret_cast<uintptr_t>(state->context()), ctx.r1.u64, state->context()->r1.u64);
    __imp__sub_826C9930(ctx, base);
    if (r30 != ctx.r30.u64 || r31 != ctx.r31.u64 || sp != ctx.r1.u64)
        REXLOG_ERROR("Wait clobber: caller={:08X} r30={:X}->{:X} r31={:X}->{:X} sp={:X}->{:X}", caller, r30, ctx.r30.u64, r31, ctx.r31.u64, sp, ctx.r1.u64);
}

// Observe the metadata that sizes shader parameter copies without changing
// guest registers, memory, or the original function's behavior.
PPC_FUNC_IMPL(sub_8261A8D8) {
    auto object = ctx.r3.u32;
    auto index = ctx.r4.s32;
    auto count = PPC_LOAD_U16(object + 440);
    if (index >= 0 && index < count) {
        auto records = PPC_LOAD_U32(object + 432);
        auto indices = PPC_LOAD_U32(object + 436);
        auto slot = PPC_LOAD_U16(indices + index * 2);
        if (slot != 0xFFFF) {
            auto record = records + slot * 8;
            auto vectors = PPC_LOAD_U16(record);
            if (vectors > 4096 || slot >= PPC_LOAD_U16(object + 444)) {
                REXLOG_ERROR("Shader parameter copy: object={:08X} index={} slots={} records={:08X} indices={:08X} slot={} record={:08X} vectors={} offset={} values={:08X}",
                    object, index, count, records, indices, slot, record, vectors,
                    PPC_LOAD_U16(record + 2), ctx.r6.u32);
                REXLOG_ERROR("Shader record words: {:08X} {:08X}", PPC_LOAD_U32(record), PPC_LOAD_U32(record + 4));
            }
        }
    }
    __imp__sub_8261A8D8(ctx, base);
}
