#pragma once
#include <cstdint>
#include <atomic>
#include <algorithm>
#include "ppc_context.h"

namespace em2 {
struct SimulationBatch {unsigned steps=0;bool finish_pending=false;};
class SimulationDebt {
public:
    void Reset(){pending_=0;}
    unsigned Add(unsigned due){pending_+=due>8-pending_?8-pending_:due;return pending_;}
    bool Consume(unsigned completed){if(completed>pending_)return false;pending_-=completed;return true;}
private:
    unsigned pending_=0;
};
// Defer finalization until the next tick is admitted. If admission stops, the
// native outer finish call owns the last pending tick, exactly once.
template<class Prepare,class Finish,class Run,class Boundary>
SimulationBatch RunSimulationBatch(unsigned planned,Prepare prepare,Finish finish,Run run,Boundary boundary) {
    SimulationBatch result;
    for(unsigned i=0;i<planned;++i) {
        if(!prepare()) break;
        if(result.finish_pending) {finish();result.finish_pending=false;}
        if(!boundary())break;
        if(!run()) break;
        ++result.steps;result.finish_pending=true;
    }
    return result;
}
template<class Prepare,class Finish,class Run>
SimulationBatch RunSimulationBatch(unsigned planned,Prepare prepare,Finish finish,Run run) {
    return RunSimulationBatch(planned,prepare,finish,run,[]{return true;});
}
// Integer accumulator: a step is exactly one thirtieth of a second, without
// rounding 33.333333 ms on every frame. Presentation frequency is independent.
class SimulationScheduler {
public:
    void Reset() { remainder_ = 0; started_ = false; }
    unsigned Advance(int64_t elapsed_ns,unsigned speed_percent=100) {
        if (!started_) { started_ = true; return speed_percent?1:0; }
        if (elapsed_ns < 0) elapsed_ns = 0;
        // Loading, suspension and debugging must not trigger minutes of catchup.
        if (elapsed_ns > 250000000) elapsed_ns = 250000000;
        remainder_ += uint64_t(elapsed_ns) * 30 * std::min(speed_percent,800u) / 100;
        const auto steps = unsigned(remainder_ / 1000000000);
        remainder_ %= 1000000000;
        return steps;
    }
    double Alpha() const { return double(remainder_) / 1000000000.0; }
private:
    uint64_t remainder_ = 0;
    bool started_ = false;
};
inline unsigned ScheduleSimulationFrame(SimulationScheduler& clock,SimulationDebt& debt,int64_t elapsed_ns,bool network,bool halted,unsigned speed_percent=100) {
    if(halted){debt.Reset();return 0;}
    const auto due=clock.Advance(elapsed_ns,network?100:speed_percent);
    return network?debt.Add(due):due;
}
bool TestSimulation();
int SimulationSpeedPercent();
int EffectiveSimulationSpeedPercent();
bool SetSimulationSpeedPercent(int percent);
bool RequestSimulationSteps(unsigned count);
void ResetPlayerMotionHistory(PPCContext& ctx,uint8_t* base);
void RecordCameraInput(unsigned user,int16_t x,int16_t y,uint16_t buttons);
bool InAgreedSimulationTick();
// Current native update only; no timeline mutation or network-owner lookup.
bool CurrentAgreedSimulationTick(uint64_t& tick);
void ResetOnlineRandomTraceHistory();
void DumpOnlineRandomTraceHistory();
void RecordNativeRandom(bool reseed,uint32_t caller,uint32_t state,uint32_t result,const PPCContext* context=nullptr,uint8_t* base=nullptr);
extern std::atomic<uint64_t> simulation_steps;
extern std::atomic<uint64_t> interpolated_nodes;
extern std::atomic<uint64_t> maximum_simulation_us;
extern std::atomic<uint64_t> maximum_render_us;
}
