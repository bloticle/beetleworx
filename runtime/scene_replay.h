#pragma once
#include "scene_raster.h"
#include <span>
namespace em2 {
// Borrowed native inputs, GPU worker only. Consume immediately in the SAME
// submission before another command/draw/swap/graph change. Does not retain
// native shader cache, constant/index upload pools, or shared-memory owner.
// Validate with IsSceneReplayCurrent before emitting replay commands; retain
// any created PSO/targets/descriptors through their own submission fence.
struct SceneReplayInputs {
    SceneDrawInputs scene;
    uint64_t submission=0,frame=0;
    size_t command_end=0;
    const void* pipeline_identity=nullptr;
    std::span<const uint8_t> vertex_shader;
    std::span<const uint32_t> geometry_shader;
    ID3D12Resource* shared_memory=nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS shared_address=0;
    // System, vertex float, bool/loop, fetch, vertex descriptor indices.
    // Last entry may be0 if the original shader did not require it.
    std::array<D3D12_GPU_VIRTUAL_ADDRESS,5> constants{};
    D3D_PRIMITIVE_TOPOLOGY topology=D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
};
std::optional<SceneReplayInputs> CaptureSceneReplay(rex::graphics::d3d12::D3D12CommandProcessor&,SceneDrawMark);
bool IsSceneReplayCurrent(rex::graphics::d3d12::D3D12CommandProcessor&,const SceneReplayInputs&);
bool TestNativeSceneReplay(rex::graphics::d3d12::D3D12CommandProcessor&);
}
