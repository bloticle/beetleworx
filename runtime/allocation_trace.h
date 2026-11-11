#pragma once
#include <cstdint>
#include <array>
#include <string_view>
namespace em2 {
struct AllocationTrace {
    uintptr_t guest_base=0;
    uintptr_t guest_context=0;
    uint32_t heap=0,size_pointer=0,alignment_pointer=0,requested=0,alignment=0;
    uint32_t flags=0,kind=0,bucket=0,caller=0,depth=0;
    bool active=false,has_size=false,has_alignment=false;
};
// POD thread-local snapshot: callable from the exception filter without locks,
// allocation, logging or guest-memory reads.
AllocationTrace CurrentNativeAllocation();
void RecordNativeLoadLevel(std::string_view);
std::array<char,64> CurrentNativeLoadLevel();
// Native owner-thread load incarnation, including repeated loads of the same
// destination. Saturates rather than reusing an old incarnation.
uint64_t CurrentNativeLoadGeneration();
bool TestAllocationTrace();
// Diagnostic mode only: deliberately faults inside a tracked allocation to
// verify the real exception filter. Never called by ordinary gameplay.
void RunAllocationCrashProbe(bool physical=false);
}
