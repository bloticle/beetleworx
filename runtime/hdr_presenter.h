#pragma once
#include <rex/ui/d3d12/d3d12_presenter.h>
namespace em2 {
// Explicit FP16 linear-scRGB presenter. Caller supplies already calibrated
// output, bypassing the command processor's legacy gamma conversion. Mode is
// immutable for this presenter; live switching must replace it after draining.
std::unique_ptr<rex::ui::d3d12::D3D12Presenter> CreateHdrPresenter(rex::ui::Presenter::HostGpuLossCallback,const rex::ui::d3d12::D3D12Provider&);
bool IsHdrPresenter(const rex::ui::d3d12::D3D12Presenter*);
bool SetHdrPresenterPaperWhite(const rex::ui::d3d12::D3D12Presenter*,float nits);
float HdrPresenterPaperWhite(const rex::ui::d3d12::D3D12Presenter*);
bool SetHdrPresenterPeak(const rex::ui::d3d12::D3D12Presenter*,float nits);
float HdrPresenterPeak(const rex::ui::d3d12::D3D12Presenter*);
// Native SDR gamma callbacks retain their 10-bit mailbox contract in this
// scope. HDR painting then decodes the display-encoded result to scRGB.
class HdrLegacyRefreshScope {public:HdrLegacyRefreshScope();~HdrLegacyRefreshScope();HdrLegacyRefreshScope(const HdrLegacyRefreshScope&)=delete;HdrLegacyRefreshScope& operator=(const HdrLegacyRefreshScope&)=delete;private:bool previous_;};
bool IsHdrLegacyRefresh();
bool CreateHdrSdrPaintPipeline(const rex::ui::d3d12::D3D12Presenter*,ID3D12Device*,const D3D12_GRAPHICS_PIPELINE_STATE_DESC&);
ID3D12PipelineState* HdrSdrPaintPipeline(const rex::ui::d3d12::D3D12Presenter*);
void RegisterHdrPresenter(const rex::ui::d3d12::D3D12Presenter*);
void ForgetHdrPresenter(const rex::ui::d3d12::D3D12Presenter*);
// Paint thread only. Caller waits for all prior paint submissions before an
// extent change. UI layer starts/ends in PIXEL_SHADER_RESOURCE, rendered using
// existing SDR UI pipelines into BGRA8, then linearized onto the HDR target.
ID3D12Resource* PrepareHdrPresenterUi(const rex::ui::d3d12::D3D12Presenter*,ID3D12Device*,uint32_t,uint32_t,D3D12_CPU_DESCRIPTOR_HANDLE&);
bool DrawHdrPresenterUi(const rex::ui::d3d12::D3D12Presenter*,ID3D12GraphicsCommandList*);
bool HdrPresenterUiExtentMatches(const rex::ui::d3d12::D3D12Presenter*,uint32_t,uint32_t);
bool TestHdrPresenter(rex::ui::d3d12::D3D12Provider&,const std::filesystem::path&);
class Em2CommandProcessor;
bool TestHdrNativeSwap(Em2CommandProcessor&,const std::filesystem::path&);
}
