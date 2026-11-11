#pragma once
#include <filesystem>
namespace rex::graphics {class GraphicsSystem;}
namespace rex::ui::d3d12 {class D3D12Provider;}
namespace em2 {
bool TestSceneIndirectPerformance(const rex::ui::d3d12::D3D12Provider&,const std::filesystem::path&);
bool BenchmarkSceneIndirect(rex::graphics::GraphicsSystem&,const std::filesystem::path&);
}
