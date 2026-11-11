#pragma once
#include <rex/ui/d3d12/d3d12_api.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <unordered_map>

namespace rex::ui::d3d12 {class D3D12Provider;}
namespace em2 {
// Diagnostic metadata belongs to the PSO, including asynchronously created PSOs.
// Call after successful creation and before publishing the native pipeline.
void TagGpuProfilePipeline(ID3D12PipelineState*,const D3D12_GRAPHICS_PIPELINE_STATE_DESC&);
void TagGpuProfileComputePipeline(ID3D12PipelineState*,const void* shader,size_t bytes);
void LabelGpuProfileTransferPipeline(ID3D12PipelineState*,uint32_t native_mode);
struct GpuProfilePipeline {
    uint64_t key=0,vs=0,ps=0;uint32_t samples=0,targets=0;
    uint32_t color_format=0,write_mask=0,depth_format=0,depth_enable=0,depth_write=0,depth_func=0,cull=0,depth_clip=0,front_ccw=0;
    int32_t depth_bias=0;float slope_bias=0,bias_clamp=0;
};
enum class GpuProfileWork:uint32_t {Draw,Dispatch,Copy,Clear};
bool TestGpuCopyClassification();
class GpuDrawProfiler {
public:
    static constexpr unsigned capacity=2048,invalid_token=capacity;
    struct Bucket {GpuProfilePipeline pipeline;GpuProfileWork kind{};uint64_t calls=0,samples=0,ticks=0,max_ticks=0,skipped=0,last_small_submission=0;};
    explicit GpuDrawProfiler(bool sample_every_call=false):every_call_(sample_every_call){}
    ~GpuDrawProfiler(){ResetAfterIdle();}
    GpuDrawProfiler(const GpuDrawProfiler&)=delete;
    GpuDrawProfiler& operator=(const GpuDrawProfiler&)=delete;
    bool Initialize(rex::ui::d3d12::D3D12Provider&);
    GpuProfilePipeline Identify(ID3D12PipelineState*) noexcept;
    unsigned Begin(ID3D12GraphicsCommandList*,uint64_t submission,GpuProfilePipeline,GpuProfileWork,uint64_t elements) noexcept;
    void End(ID3D12GraphicsCommandList*,unsigned) noexcept;
    void Poll(uint64_t completed);
    void FlushReport();
    void ResetAfterIdle(); // Caller has proved all using GPU lists have drained.
    uint64_t completed_samples()const{return completed_samples_;}
    uint64_t recorded_calls()const{return recorded_calls_;}
    uint64_t recorded_calls(GpuProfileWork kind)const{return work_calls_[unsigned(kind)];}
private:
    struct Slot {uint64_t submission=0,key=0;bool pending=false,ended=false;};
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
    uint64_t* mapped_=nullptr;
    uint64_t frequency_=0,random_=0x6d2b79f5d8371ab9,completed_samples_=0,recorded_calls_=0;
    bool unavailable_=false,every_call_=false;
    std::array<Slot,capacity> slots_{};
    std::unordered_map<uint64_t,Bucket> buckets_;
    std::array<uint64_t,4> work_calls_{};
    std::chrono::steady_clock::time_point last_report_=std::chrono::steady_clock::now();
};

// Borrowed, synchronous recording facade. Native SDK replay must not retain it.
// GetDevice/QueryInterface return the real interfaces. No GPU commands or state
// bindings are suppressed; only selected work receives timestamp queries.
class ProfiledGpuList final:public ID3D12GraphicsCommandList {
public:
    ProfiledGpuList(ID3D12GraphicsCommandList* real,GpuDrawProfiler& profile,uint64_t submission):real_(real),profile_(profile),submission_(submission){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{return real_->QueryInterface(iid,out);}
    ULONG STDMETHODCALLTYPE AddRef()override{return real_->AddRef();}
    ULONG STDMETHODCALLTYPE Release()override{return real_->Release();}
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid,UINT* size,void* data)override{return real_->GetPrivateData(guid,size,data);}
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid,UINT size,const void* data)override{return real_->SetPrivateData(guid,size,data);}
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid,const IUnknown* data)override{return real_->SetPrivateDataInterface(guid,data);}
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name)override{return real_->SetName(name);}
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid,void** out)override{return real_->GetDevice(iid,out);}
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType()override{return real_->GetType();}
    HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator* allocator,ID3D12PipelineState* initial)override{const auto result=real_->Reset(allocator,initial);if(SUCCEEDED(result)){pipeline_=profile_.Identify(initial);viewport_known_=scissor_known_=false;}return result;}
    void STDMETHODCALLTYPE ClearState(ID3D12PipelineState* initial)override{real_->ClearState(initial);pipeline_=profile_.Identify(initial);viewport_known_=scissor_known_=false;}
    void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState* state)override{real_->SetPipelineState(state);pipeline_=profile_.Identify(state);}
    void STDMETHODCALLTYPE DrawInstanced(UINT count,UINT instances,UINT start,UINT first_instance)override;
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT count,UINT instances,UINT start,INT base,UINT first_instance)override;
    void STDMETHODCALLTYPE Dispatch(UINT x,UINT y,UINT z)override;
#include "gpu_draw_profile_forwarders.inl"
private:
    ID3D12GraphicsCommandList* real_;
    GpuDrawProfiler& profile_;
    uint64_t submission_;
    GpuProfilePipeline pipeline_{};
    D3D12_VIEWPORT viewport_{};D3D12_RECT scissor_{};
    bool viewport_known_=false,scissor_known_=false;
    void ObserveShadowDraw(uint64_t elements) noexcept;
};
}
