#pragma once
#include <filesystem>
namespace rex::ui::d3d12 {class D3D12Provider;}
namespace em2 {
bool TestNativeBloomPixels(rex::ui::d3d12::D3D12Provider&,const std::filesystem::path&);
}
