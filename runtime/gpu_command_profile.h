#pragma once
#include <array>
#include <cstdint>

namespace em2 {
enum class GpuCommandKind:unsigned {Draw,Copy,Swap};
struct GpuCommandSample {
    uint64_t calls=0,false_results=0,aborted=0,inclusive_ns=0,self_ns=0,max_ns=0;
    bool operator==(const GpuCommandSample&)const=default;
};
struct GpuCommandWindow {
    bool open=false,counter_reset=false;uint64_t started=0,elapsed_ns=0,root_ns=0,batch_start=0,bulk_packets=0;
    std::array<GpuCommandSample,3> commands{};
    bool operator==(const GpuCommandWindow&)const=default;
};
// Owned by a profiled processor and accessed only on its GPU worker thread.
class GpuCommandProfiler {
public:
    using Clock=uint64_t(*)(void*) noexcept;
    using Counter=uint64_t(*)(void*) noexcept;
    using Report=void(*)(const GpuCommandWindow&,void*);
    GpuCommandProfiler();
    GpuCommandProfiler(Clock clock,Counter counter,Report report,void* state):clock_(clock),counter_(counter),report_(report),state_(state){}
    class Call {
    public:
        Call(GpuCommandProfiler&,GpuCommandKind);
        ~Call();
        void End(bool result=true) noexcept;
        Call(const Call&)=delete;
        Call& operator=(const Call&)=delete;
    private:
        uint64_t Finish(bool result,bool aborted);
        GpuCommandProfiler& owner_;GpuCommandKind kind_;Call* parent_;
        uint64_t started_,children_=0;bool finished_=false;
    };
    const GpuCommandWindow& window()const{return window_;}
    bool active()const{return active_!=nullptr;}
private:
    uint64_t Now()const noexcept;
    uint64_t Batches()const noexcept;
    void MaybeReport(uint64_t now) noexcept;
    Clock clock_;Counter counter_;Report report_;void* state_=nullptr;
    GpuCommandWindow window_;Call* active_=nullptr;
};
bool TestGpuCommandProfiling();
}
