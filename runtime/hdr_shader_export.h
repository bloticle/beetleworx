#pragma once
#include <cstdint>
#include <span>
#include <vector>
#include <filesystem>
namespace rex::ui::d3d12 {class D3D12Provider;}
namespace rex::graphics::d3d12 {class D3D12CommandProcessor;}
namespace em2 {
// Duplicate the native translator's float color0 MOV exports into SV_Target4.
// This preserves the value before output-merger UNORM conversion, not values
// already saturated by guest instructions or a manual gamma conversion.
// Reject unsupported signatures/programs without changing the original shader.
// Caller must supply matching FP16 MRT4, blend/MSAA state, clears and lifetime.
std::vector<uint8_t> AddHdrSceneExport(std::span<const uint8_t>);
// Strict native SM5.1 epilogue only: capture colour0 after native exposure,
// before native gamma conversion. Does not undo authored guest colour math.
// Reject ambiguous/missing exposure/export sequences; preserve colour0.
std::vector<uint8_t> AddLinearHdrSceneExport(std::span<const uint8_t>);
bool TestHdrSceneExport(rex::graphics::d3d12::D3D12CommandProcessor&,const std::filesystem::path&);
}
