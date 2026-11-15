#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
struct PPCContext;
namespace em2 {
struct NativeScheduledScript {
    uint32_t address=0,reference=0,coroutine=0;
    std::array<uint8_t,132> bytes{};
    bool operator==(const NativeScheduledScript&)const=default;
};
struct NativeScriptScheduler {
    uint32_t manager=0,global=0,registry=0,array=0,capacity=0,thread=0;
    std::vector<NativeScheduledScript> tasks;
    bool operator==(const NativeScriptScheduler&)const=default;
};
// Completed, exclusively owned scheduler boundary; rejects an in-progress
// coroutine. Checks every native registry reference against its task's thread.
bool ReadNativeScriptScheduler(PPCContext&,uint8_t*,uint32_t state,uint32_t manager,NativeScriptScheduler&,std::string&);
struct NativeScheduledScriptReplacement {
    unsigned task=0;uint32_t rooted_thread_slot=0;
    // Captured bytes at record +96: wait value, two native flags, padding and
    // pending argument count. Identity/name/compiler settings remain unchanged.
    std::array<uint8_t,12> scheduling{};
};
// Trusted local restore primitive, not complete scheduler/world restoration.
// `expected` describes the CURRENT native inventory, checked before writes.
// The full snapshot owner must map captured tasks to this inventory and restore
// all related roots, task creation/deletion, globals and native gameplay state.
// It also supplies transaction backup/recovery if a native mutation fails.
bool RebindNativeScheduledScripts(PPCContext&,uint8_t*,uint32_t state,const NativeScriptScheduler& expected,
    std::span<const NativeScheduledScriptReplacement>,std::string&);
bool TestNativeScriptScheduler(PPCContext&,uint8_t*,uint32_t state,uint32_t original_thread_slot,uint32_t restored_thread_slot,uint32_t gc_closure_slot);
}
