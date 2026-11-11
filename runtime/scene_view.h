#pragma once
#include "scene_draw_inputs.h"
#include <memory>
namespace rex::graphics::d3d12 {class D3D12CommandProcessor;class D3D12RenderTargetCache;}
namespace em2 {
// Immutable metadata for one captured native fog draw. Shared object identity,
// not equal dimensions/matrices, links its camera, AO and resolved scene layers.
// Contains no authority to replay borrowed constant or vertex upload pools.
struct SceneViewIdentity {
    uint64_t frame=0,submission=0,draw=0;
    uint32_t width=0,height=0;
    std::array<SceneDrawInputs::Row,4> inverse_view_projection{};
    std::array<SceneDrawInputs::Row,3> inverse_view{};
    rex::graphics::xenos::xe_gpu_texture_fetch_t depth_fetch{};
    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissor{};
};
struct SceneViewTarget {
    const rex::graphics::d3d12::D3D12RenderTargetCache* cache=nullptr;
    ID3D12Resource* legacy=nullptr; // Borrowed only within the current native draw.
};
SceneViewTarget CaptureSceneViewTarget(rex::graphics::d3d12::D3D12CommandProcessor&);
}
