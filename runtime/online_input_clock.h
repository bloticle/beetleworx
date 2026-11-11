#pragma once
#include <cstdint>
#include <memory>
struct PPCContext;
namespace em2::net {
class NativeInputClockState {
public:
    NativeInputClockState();
    ~NativeInputClockState();
    NativeInputClockState(const NativeInputClockState&)=delete;
    NativeInputClockState& operator=(const NativeInputClockState&)=delete;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
    friend struct NativeInputClockAccess;
};
class NativeInputClockScratch {
public:
    NativeInputClockScratch(const NativeInputClockScratch&)=delete;
    NativeInputClockScratch& operator=(const NativeInputClockScratch&)=delete;
    NativeInputClockScratch(NativeInputClockScratch&&)=delete;
    NativeInputClockScratch& operator=(NativeInputClockScratch&&)=delete;
    ~NativeInputClockScratch()=default;
    // Callback-lifetime borrowed view; cannot be copied, moved or retained.
    uint32_t data()const;
    uint32_t capacity()const;
    // Only copied records INSIDE this invocation's sealed private data area.
    // Registration does not copy records or authorize any live object writes.
    bool OwnButtonHistory(uint32_t address,unsigned count);
    // Native kind2 repeat records only. Timer state is zero or already belongs
    // to this canonical source-counter interval; live wall-time stamps reject.
    bool OwnActionRecord(uint32_t address);
private:
    explicit NativeInputClockScratch(void* scope):scope_(scope){}
    void* scope_;
    friend struct NativeInputClockAccess;
};
using NativeInputClockCallback=bool(*)(PPCContext&,uint8_t*,NativeInputClockScratch&,void*);
bool InitializeNativeInputClock(NativeInputClockState&,uint64_t source_epoch,uint64_t first_source_tick,PPCContext&,uint8_t* base);
bool ResetNativeInputClock(NativeInputClockState&);
// Preserves full context, 8KiB scratch, MXCSR and LastError. Callback operates
// ONLY private copies. False/exception leaves clock progression unchanged;
// caller commits its own history/output transaction only after success.
// Repeating the same source tick repeats its counter, not an action-output cache.
bool RunNativeInputClock(NativeInputClockState&,uint64_t source_epoch,uint64_t source_tick,
    PPCContext&,uint8_t*,NativeInputClockCallback,void* user=nullptr);
// FIRST branch of the existing sub826C9F38 owner. False: ordinary QPC route.
// True: an exact owned native input callsite was handled (including fail-closed
// private-scope errors). Never install this as a global elapsed-time override.
bool TryNativeInputClockQuery(PPCContext&,uint8_t*);
bool TestNativeInputClock();
}
