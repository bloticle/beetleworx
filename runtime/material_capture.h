#pragma once
#include <memory>
#include <cstdint>
#include <vector>
#include <semaphore>
#include <d3d12.h>
namespace rex::graphics {class Shader;}
namespace rex::graphics::d3d12 {class D3D12RenderTargetCache;class DeferredCommandList;class D3D12CommandProcessor;}
namespace em2 {
class HdrSceneTarget;
struct MaterialCaptureState;
struct MaterialCaptureStats{uint64_t translations=0,replacements=0,failures=0,bindings=0,used=0,fallbacks=0,prepares=0,pending=0,transfers=0,clear_pipelines=0,unsafe_draws=0,surface_transfers=0;};
// GPU-worker creation before shader compilation. Immutable preparation mode;
// outputs are NOT authoritative scene lighting (no resolve/coverage tracking).
bool RegisterMaterialCapture(const rex::graphics::d3d12::D3D12RenderTargetCache*,ID3D12Device*,uint64_t budget=128ull*1024*1024,bool surfaces=false,bool linear_surfaces=false);
// Shutdown only after shader workers and GPU submissions have drained.
void ShutdownMaterialCapture(const rex::graphics::d3d12::D3D12RenderTargetCache*);
MaterialCaptureStats GetMaterialCaptureStats(const rex::graphics::d3d12::D3D12RenderTargetCache*);
// Called by the native deferred draw writer after guest PSO selection.
// Pending asynchronous work is completed through the native submission drain.
void BeforeMaterialCaptureDraw(rex::graphics::d3d12::D3D12CommandProcessor&);
bool SetMaterialCaptureDraw(rex::graphics::d3d12::D3D12RenderTargetCache*,ID3D12PipelineState*);
struct MaterialCaptureBinding{bool active=false;D3D12_CPU_DESCRIPTOR_HANDLE rtv{};std::shared_ptr<HdrSceneTarget> target;};
MaterialCaptureBinding AcquireMaterialCapture(const rex::graphics::d3d12::D3D12RenderTargetCache*,ID3D12Resource*,rex::graphics::d3d12::DeferredCommandList&);
void ClearMaterialCapture(const rex::graphics::d3d12::D3D12RenderTargetCache*,ID3D12Resource*,const D3D12_RECT&,rex::graphics::d3d12::DeferredCommandList&);
struct MaterialCaptureTransfer{std::shared_ptr<HdrSceneTarget> source,destination;uint32_t descriptor_index=0;explicit operator bool()const{return source && destination;}};
bool HasMaterialCapture(const rex::graphics::d3d12::D3D12RenderTargetCache*);
MaterialCaptureTransfer PrepareMaterialCaptureTransfer(const rex::graphics::d3d12::D3D12RenderTargetCache*,ID3D12Resource* source,ID3D12Resource* destination,rex::graphics::d3d12::DeferredCommandList&);
void InvalidateMaterialCaptureDestination(const rex::graphics::d3d12::D3D12RenderTargetCache*,ID3D12Resource*);
void CountMaterialCaptureTransfer(const rex::graphics::d3d12::D3D12RenderTargetCache*);
// Pair with material/HDR plans for the same supported native ownership copy.
MaterialCaptureTransfer PrepareMaterialSurfaceTransfer(const rex::graphics::d3d12::D3D12RenderTargetCache*,ID3D12Resource* source,ID3D12Resource* destination,rex::graphics::d3d12::DeferredCommandList&);
void CountMaterialSurfaceTransfer(const rex::graphics::d3d12::D3D12RenderTargetCache*);
// Owner-thread lookup/reservation for the shared resolve engine. No allocation
// during lookup; invalid and unrecognized surfaces cannot supply resolved data.
MaterialCaptureBinding FindMaterialCaptureSource(const rex::graphics::d3d12::D3D12RenderTargetCache*,ID3D12Resource*);
bool ReserveMaterialCaptureMemory(const rex::graphics::d3d12::D3D12RenderTargetCache*,uint64_t);
void ReleaseMaterialCaptureMemory(const rex::graphics::d3d12::D3D12RenderTargetCache*,uint64_t);
class MaterialPipelineScope {
public:
    explicit MaterialPipelineScope(const rex::graphics::d3d12::D3D12RenderTargetCache*);
    ~MaterialPipelineScope();
private:std::shared_ptr<MaterialCaptureState> previous_;
};
std::shared_ptr<const std::vector<uint8_t>> CurrentMaterialOriginal(const D3D12_SHADER_BYTECODE&,bool* surface=nullptr);
// Deterministic headless fixture gate; never installed by live settings.
struct MaterialCaptureTestGate{std::counting_semaphore<4> entered{0},waiting{0},release{0};};
void SetMaterialCaptureTestGate(const rex::graphics::d3d12::D3D12RenderTargetCache*,std::shared_ptr<MaterialCaptureTestGate>);
void WaitMaterialCaptureTestGate();
void CountMaterialFallback();
bool StampCurrentMaterialPipeline(ID3D12PipelineState*);
bool StampCurrentMaterialInertPipeline(ID3D12PipelineState*);
bool CurrentMaterialCapture();
bool CurrentMaterialSurfaceCapture();
bool CurrentLinearMaterialSurfaceCapture();
// Exact translated variants of the pinned colour-only depth-fog shader.
bool CurrentMaterialSurfacePreserving(const D3D12_SHADER_BYTECODE&);
void CountMaterialClearPipeline();
}

namespace em2 {
// Immutable preparation mode; optional surface target shares the material budget.
bool HasMaterialSurfaceCapture(const rex::graphics::d3d12::D3D12RenderTargetCache*);
bool HasLinearMaterialSurfaceCapture(const rex::graphics::d3d12::D3D12RenderTargetCache*);
bool StampCurrentMaterialSurfacePipeline(ID3D12PipelineState*);
MaterialCaptureBinding AcquireMaterialSurface(const rex::graphics::d3d12::D3D12RenderTargetCache*,ID3D12Resource*,rex::graphics::d3d12::DeferredCommandList&);
MaterialCaptureBinding FindMaterialSurfaceSource(const rex::graphics::d3d12::D3D12RenderTargetCache*,ID3D12Resource*);
void InvalidateMaterialSurface(const rex::graphics::d3d12::D3D12RenderTargetCache*,ID3D12Resource*);
}
