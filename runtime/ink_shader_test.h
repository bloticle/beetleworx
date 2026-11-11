#pragma once
#include <filesystem>
namespace rex::ui::d3d12 { class D3D12Provider; }
namespace em2 {
// Executes all four actual PS programs read from the staged ink FXLO through the
// native Xenos->DXBC translator. This does not certify native ribbon binding.
bool TestOriginalInkPixels(rex::ui::d3d12::D3D12Provider&,
    const std::filesystem::path& candidate_effect,
    const std::filesystem::path& output);
}
