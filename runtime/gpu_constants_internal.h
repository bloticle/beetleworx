#pragma once
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/kernel/xmemory.h>
#include "resolve_coverage.h"
#include "scene_draw_inputs.h"
#include "scene_depth_view.h"
#include "scene_replay.h"
extern "C" void EM2OriginalD3DRegister(rex::graphics::d3d12::D3D12CommandProcessor*,uint32_t,uint32_t);
namespace em2 {
struct ResolveWriteObserver;
struct RendererGraph;class GraphicsResourceCheckpoint;
class ResolveRangeLedger;
class ResolveLedgerWatch;
struct HdrSwapState;
class HdrResolvedScene;
class SceneLightingState;
struct HdrOutputSettings;
class Em2CommandProcessor : public rex::graphics::d3d12::D3D12CommandProcessor {
public:
    using rex::graphics::CommandProcessor::ExecutePacketType0;
    using rex::graphics::CommandProcessor::ExecutePacketType3_SET_CONSTANT;
    using rex::graphics::CommandProcessor::ExecutePacketType3_SET_CONSTANT2;
    using rex::graphics::CommandProcessor::ExecutePacketType3_SET_SHADER_CONSTANTS;
    const uint32_t* ConstantRegisterValues()const{return register_file_->values;}
    // GPU-worker metadata for the latest accepted fog draw only. Cleared by
    // any following draw/copy/swap. Does not pin or authorize GPU resources.
    const std::optional<SceneDrawInputs>& scene_draw_inputs()const{return scene_draw_inputs_;}
    const std::shared_ptr<const SceneDepthView>& scene_depth_view()const{return scene_depth_view_;}
    // Borrowed GPU-worker inputs. IsSceneReplayCurrent must pass immediately
    // before consumption; no lifetime beyond another command/draw/submission.
    const std::optional<SceneReplayInputs>& scene_replay()const{return scene_replay_;}
    using D3D12CommandProcessor::D3D12CommandProcessor;
    ~Em2CommandProcessor() override;
    void WriteRegister(uint32_t index,uint32_t value) final;
    // Bulk decoding has already checked the eligible range and exact bits.
    // Retain native dirty-buffer/trace handling without repeating our guard.
    void WriteNativeConstant(uint32_t index,uint32_t value){D3D12CommandProcessor::WriteRegister(index,value);}
    void WriteOriginalConstant(uint32_t index,uint32_t value){EM2OriginalD3DRegister(this,index,value);}
    // Command-processor thread only, outside packet execution.
    bool DrainForResourceChange();
    bool IsGraphicsWorker() const;
    bool SetHdrSwapSettings(const HdrOutputSettings&);
    bool ReleaseHdrSwapBindings();
    ID3D12Resource* RequestSceneSdrSwapTexture(const std::shared_ptr<const HdrResolvedScene>&);
    void ComposeResolvedSceneLighting(rex::graphics::d3d12::D3D12RenderTargetCache*,const rex::graphics::draw_util::ResolveInfo&);
    bool SetResolveWriteObserver(ResolveWriteObserver*);
    // GPU-worker metadata snapshot under the native global memory lock. GPU
    // data capture separately requires the caller to stop producers/invalidation.
    bool CopyTrackedResolveLedger(ResolveRangeLedger&);

protected:
    bool SetupContext() override;
    void ShutdownContext() override;
    rex::graphics::Shader* LoadShader(rex::graphics::xenos::ShaderType,uint32_t,const uint32_t*,uint32_t) override;
    bool IssueDraw(rex::graphics::xenos::PrimitiveType,uint32_t,IndexBufferInfo*,bool) override;
    bool IssueCopy() override;
    void IssueSwap(uint32_t,uint32_t,uint32_t) override;
private:
    std::optional<SceneDrawInputs> scene_draw_inputs_;
    std::optional<SceneReplayInputs> scene_replay_;
    std::shared_ptr<const SceneDepthView> scene_depth_view_,scene_depth_view_cache_;
    uint64_t scene_draw_serial_=0;
    std::shared_ptr<SceneLightingState> scene_lighting_;
    bool scene_lighting_reported_=false,scene_lighting_failed_=false;
    void UpdateSceneLighting();
    bool RefreshLivePixelShader();
    bool resource_checkpoint_=false;
    bool edram_bindings_overridden_=false;
    friend bool PublishRendererGraph(Em2CommandProcessor&,const GraphicsResourceCheckpoint&,
        RendererGraph&,ResolveRangeLedger&,RendererGraph&);
    friend class EdramSystemBindings;
    friend class GraphicsResourceCheckpoint;
    ResolveWriteObserver* resolve_write_observer_=nullptr;
    std::unique_ptr<ResolveRangeLedger> resolve_ledger_;
    std::unique_ptr<ResolveLedgerWatch> resolve_watch_;
    std::shared_ptr<HdrSwapState> hdr_swap_;
    std::vector<uint32_t> live_pixel_source_;
    uint32_t live_pixel_address_=0;
    rex::graphics::Shader* live_pixel_expected_=nullptr;
    unsigned live_shadow_=4,live_bloom_=2;
    friend bool TestResolveWriteObserver(Em2CommandProcessor&);
    friend bool TestPackedEdramCapture(Em2CommandProcessor&);
    friend bool TestLiveShaderPolicy(Em2CommandProcessor&,rex::memory::Memory&);
    friend bool TestRendererGraph(Em2CommandProcessor&,const std::filesystem::path&);
    friend bool TestGraphicsResourceSynchronization(rex::graphics::GraphicsSystem&);
};
bool TestLiveShaderPolicy(Em2CommandProcessor&,rex::memory::Memory&);
}

