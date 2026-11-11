#pragma once
#include <rex/ui/d3d12/d3d12_api.h>
#include <array>
#include <chrono>
#include <cstdint>

namespace rex::ui::d3d12 {class D3D12Provider;}
namespace rex::graphics::d3d12 {class D3D12CommandProcessor;}
namespace em2 {
class GpuDrawProfiler;
// GPU-worker owned. ResetAfterIdle must follow native submission completion.
class GpuExecutionProfiler {
public:
    static constexpr unsigned capacity=64,invalid_token=capacity;
    struct Stats {uint64_t samples=0,ticks=0,max_ticks=0,skipped=0,invalid=0,frequency=0;};
    ~GpuExecutionProfiler(){ResetAfterIdle();}
    GpuExecutionProfiler()=default;
    GpuExecutionProfiler(const GpuExecutionProfiler&)=delete;
    GpuExecutionProfiler& operator=(const GpuExecutionProfiler&)=delete;
    unsigned Begin(rex::ui::d3d12::D3D12Provider&,ID3D12GraphicsCommandList*,uint64_t submission,uint64_t completed);
    void End(ID3D12GraphicsCommandList*,unsigned token);
    void Cancel(unsigned token);
    void Poll(uint64_t completed);
    void FlushReport();
    void ResetAfterIdle();
    Stats stats()const{return stats_;}
private:
    struct Slot {uint64_t submission=0;bool pending=false,ended=false;};
    bool Initialize(rex::ui::d3d12::D3D12Provider&);
    ID3D12QueryHeap* heap_=nullptr;
    ID3D12Resource* readback_=nullptr;
    uint64_t* mapped_=nullptr;
    bool unavailable_=false;
    std::array<Slot,capacity> slots_{};
    Stats stats_{},window_{};
    std::chrono::steady_clock::time_point last_report_=std::chrono::steady_clock::now();
};
class GpuExecutionScope {
public:
    GpuExecutionScope(GpuExecutionProfiler&,rex::graphics::d3d12::D3D12CommandProcessor&,GpuDrawProfiler* draws=nullptr);
    ~GpuExecutionScope();
    GpuExecutionScope(const GpuExecutionScope&)=delete;
    GpuExecutionProfiler& profile;
    rex::graphics::d3d12::D3D12CommandProcessor& processor;
    GpuDrawProfiler* draws;
private:
    GpuExecutionScope* previous_;
};
bool TestGpuExecutionTiming(rex::graphics::d3d12::D3D12CommandProcessor&);
}
