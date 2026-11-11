#include "gpu_execution_profile.h"
#include "gpu_draw_profile.h"
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/logging.h>
#include <immintrin.h>
#include <algorithm>
#include <cstring>

extern "C" void EM2OriginalDeferredExecute(void*,ID3D12GraphicsCommandList*,ID3D12GraphicsCommandList1*);
namespace {
struct FpGuard {unsigned csr=_mm_getcsr();~FpGuard(){_mm_setcsr(csr);}};
constinit thread_local em2::GpuExecutionScope* active_execution=nullptr;
}
bool em2::GpuExecutionProfiler::Initialize(rex::ui::d3d12::D3D12Provider& provider) {
    if(heap_)return true;
    if(unavailable_)return false;
    auto* device=provider.GetDevice();
    D3D12_QUERY_HEAP_DESC query{};query.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;query.Count=capacity*2;
    D3D12_HEAP_PROPERTIES memory{};memory.Type=D3D12_HEAP_TYPE_READBACK;memory.CreationNodeMask=memory.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC resource{};resource.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;resource.Width=capacity*16;
    resource.Height=resource.DepthOrArraySize=resource.MipLevels=resource.SampleDesc.Count=1;resource.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_RANGE range{0,capacity*16};
    if(FAILED(provider.GetDirectQueue()->GetTimestampFrequency(&stats_.frequency)) || !stats_.frequency ||
       FAILED(device->CreateQueryHeap(&query,IID_PPV_ARGS(&heap_))) ||
       FAILED(device->CreateCommittedResource(&memory,D3D12_HEAP_FLAG_NONE,&resource,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback_))) ||
       FAILED(readback_->Map(0,&range,reinterpret_cast<void**>(&mapped_)))) {
        ResetAfterIdle();unavailable_=true;
        REXLOG_WARN("GPU execution timestamps unavailable; native rendering continues");return false;
    }
    return true;
}
unsigned em2::GpuExecutionProfiler::Begin(rex::ui::d3d12::D3D12Provider& provider,ID3D12GraphicsCommandList* list,uint64_t submission,uint64_t completed) {
    FpGuard fp;
    if(!Initialize(provider))return invalid_token;
    Poll(completed);
    if(!submission || submission<=completed){++stats_.invalid;++window_.invalid;return invalid_token;}
    for(unsigned i=0;i<capacity;++i)if(!slots_[i].pending) {
        slots_[i]={submission,true,false};list->EndQuery(heap_,D3D12_QUERY_TYPE_TIMESTAMP,i*2);return i;
    }
    ++stats_.skipped;++window_.skipped;return invalid_token; // Never stall the GPU worker for profiling.
}
void em2::GpuExecutionProfiler::End(ID3D12GraphicsCommandList* list,unsigned token) {
    if(token>=capacity || !slots_[token].pending || slots_[token].ended)return;
    list->EndQuery(heap_,D3D12_QUERY_TYPE_TIMESTAMP,token*2+1);
    list->ResolveQueryData(heap_,D3D12_QUERY_TYPE_TIMESTAMP,token*2,2,readback_,token*16);
    slots_[token].ended=true;
}
void em2::GpuExecutionProfiler::Cancel(unsigned token){if(token<capacity && !slots_[token].ended)slots_[token]={};}
void em2::GpuExecutionProfiler::Poll(uint64_t completed) {
    FpGuard fp;
    for(unsigned i=0;i<capacity;++i) {
        auto& slot=slots_[i];if(!slot.pending || !slot.ended || slot.submission>completed)continue;
        uint64_t times[2];std::memcpy(times,mapped_+i*2,sizeof(times));slot={};
        if(times[1]<times[0]){++stats_.invalid;++window_.invalid;continue;}
        const auto ticks=times[1]-times[0];++stats_.samples;stats_.ticks+=ticks;stats_.max_ticks=std::max(stats_.max_ticks,ticks);
        ++window_.samples;window_.ticks+=ticks;window_.max_ticks=std::max(window_.max_ticks,ticks);
    }
    if(std::chrono::steady_clock::now()-last_report_>=std::chrono::seconds(5))FlushReport();
}
void em2::GpuExecutionProfiler::FlushReport() {
    FpGuard fp;
    const auto now=std::chrono::steady_clock::now();
    const double elapsed=std::chrono::duration<double>(now-last_report_).count();last_report_=now;
    const auto sample=window_;window_={};
    if(!stats_.frequency || !(sample.samples || sample.skipped || sample.invalid))return;
    REXLOG_INFO("GPU deferred execution timestamps: {:.3f}s observation window, {} samples, {:.3f} ms GPU intervals total, {:.3f}/{:.3f} ms mean/max, {} busy skips, {} invalid (within-submission GPU intervals; excludes work outside deferred replay; not CPU wall time, total frame time or whole-device utilization)",
        elapsed,sample.samples,sample.ticks*1000.0/stats_.frequency,
        sample.samples?sample.ticks*1000.0/stats_.frequency/sample.samples:0.0,
        sample.max_ticks*1000.0/stats_.frequency,sample.skipped,sample.invalid);
}

void em2::GpuExecutionProfiler::ResetAfterIdle() {
    if(mapped_){D3D12_RANGE written{0,0};readback_->Unmap(0,&written);mapped_=nullptr;}
    if(readback_){readback_->Release();readback_=nullptr;}
    if(heap_){heap_->Release();heap_=nullptr;}
    slots_={};stats_={};window_={};unavailable_=false;last_report_=std::chrono::steady_clock::now();
}
em2::GpuExecutionScope::GpuExecutionScope(GpuExecutionProfiler& p,rex::graphics::d3d12::D3D12CommandProcessor& cp,GpuDrawProfiler* d):profile(p),processor(cp),draws(d),previous_(active_execution){active_execution=this;}
em2::GpuExecutionScope::~GpuExecutionScope(){active_execution=previous_;}
namespace {
__declspec(noinline) void ProfiledDeferredExecute(void* owner,ID3D12GraphicsCommandList* list,ID3D12GraphicsCommandList1* list1,em2::GpuExecutionScope* scope) {
    unsigned token=em2::GpuExecutionProfiler::invalid_token;
    try{token=scope->profile.Begin(scope->processor.GetD3D12Provider(),list,scope->processor.GetCurrentSubmission(),scope->processor.GetCompletedSubmission());}catch(...){}
    bool draw_profile=false;
    try{if(scope->draws){draw_profile=scope->draws->Initialize(scope->processor.GetD3D12Provider());scope->draws->Poll(scope->processor.GetCompletedSubmission());}}catch(...){}
    try{
        if(draw_profile){em2::ProfiledGpuList facade(list,*scope->draws,scope->processor.GetCurrentSubmission());EM2OriginalDeferredExecute(owner,&facade,list1);}
        else EM2OriginalDeferredExecute(owner,list,list1);
    }catch(...){scope->profile.Cancel(token);throw;}
    const auto csr=_mm_getcsr();scope->profile.End(list,token);_mm_setcsr(csr);
}
}
void rex::graphics::d3d12::DeferredCommandList::Execute(ID3D12GraphicsCommandList* list,ID3D12GraphicsCommandList1* list1) {
    if(auto* scope=active_execution)return ProfiledDeferredExecute(this,list,list1,scope);
    EM2OriginalDeferredExecute(this,list,list1);
}
