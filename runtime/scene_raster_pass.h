#pragma once
#include "scene_geometry.h"
#include "scene_replay.h"
#include "scene_raster_geometry.h"
#include <vector>
#include <filesystem>
namespace em2 {
struct SceneRasterProgram;
struct NativeSceneSampler {
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_GPU_DESCRIPTOR_HANDLE handle{};
};
// Reads the actual original pixel TF0 sampler slot, not an assumed slot0 or
// reconstructed filter. Does not allocate or mutate native descriptors.
std::optional<NativeSceneSampler> CaptureNativeSceneSampler(rex::graphics::d3d12::D3D12CommandProcessor&,const SceneReplayInputs&);
bool TestNativeSceneSampler(rex::graphics::d3d12::D3D12CommandProcessor&,const SceneReplayInputs&);
bool TestDeferredSceneReplay(rex::graphics::d3d12::D3D12CommandProcessor&,std::shared_ptr<const SceneDepthView>,const std::filesystem::path&);
class SceneRasterBindings {
public:
    const std::shared_ptr<SceneGeometryBindings>& geometry()const{return geometry_;}
private:
    friend class SceneRasterGeometryPass;
    std::shared_ptr<SceneGeometryBindings> geometry_;
    std::shared_ptr<SceneRasterProgram> program_;
    NativeSceneSampler sampler_;
    rex::graphics::xenos::xe_gpu_texture_fetch_t depth_fetch_{};
    SceneDrawMark capture_; // stream_offset is captured replay.command_end
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvs_,depth_srv_;
    Microsoft::WRL::ComPtr<ID3D12Resource> unused_uav_;
    Microsoft::WRL::ComPtr<ID3D12Resource> shared_memory_;
};
// GPU worker only. Retain bindings (including kernel/RTVs/source/sampler heap)
// through submission fence. CreateBindings emits no commands. Record requires
// a still-current captured native draw; consumes borrowed native constant/index
// pools in the SAME submission. Output feeds SceneOcclusionPass directly.
// Reused outputs are overwritten in queue order; consume before next Record.
class SceneRasterGeometryPass {
public:
    bool Initialize(ID3D12Device*);
    uint32_t program_builds()const{return program_builds_;}
    std::shared_ptr<SceneRasterBindings> CreateBindings(rex::graphics::d3d12::D3D12CommandProcessor&,const SceneReplayInputs&,std::shared_ptr<const SceneDepthView>,std::shared_ptr<const SceneRasterBindings> previous={});
    bool Record(rex::graphics::d3d12::D3D12CommandProcessor&,SceneRasterBindings&,const SceneReplayInputs&)const;
private:
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12Resource> unused_uav_;
    uint32_t program_builds_=0;
    std::vector<std::shared_ptr<SceneRasterProgram>> programs_; // LRU, at most8
};
}
