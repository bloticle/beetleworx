#pragma once
#include "scene_draw_inputs.h"
#include <cstddef>
namespace rex::graphics::d3d12 {class D3D12CommandProcessor;}
namespace em2 {
struct SceneDrawMark {uint64_t submission=0,frame=0;size_t stream_offset=0;};
SceneDrawMark MarkSceneDraw(rex::graphics::d3d12::D3D12CommandProcessor&);
std::optional<SceneRasterState> InspectSceneDraw(rex::graphics::d3d12::D3D12CommandProcessor&,SceneDrawMark);
bool IsSceneColourRaster(const rex::graphics::RegisterFile&);
bool TestNativeSceneRaster(rex::graphics::d3d12::D3D12CommandProcessor&);
}
