#pragma once
#include <filesystem>
namespace rex::ui::d3d12 {class D3D12Provider;}
namespace rex::graphics::d3d12 {class D3D12CommandProcessor;}
namespace em2 {bool TestTransferScissorGpu(const rex::ui::d3d12::D3D12Provider&,const std::filesystem::path&);}
namespace em2 {bool TestNativeStencilZero(rex::graphics::d3d12::D3D12CommandProcessor&,const std::filesystem::path&);}
namespace em2 {bool ProbeStencilPlaneCopies(const rex::ui::d3d12::D3D12Provider&,const std::filesystem::path&,bool* r8_accepted);}
namespace em2 {bool TestStencilPlaneCopyPixels(const rex::ui::d3d12::D3D12Provider&,const std::filesystem::path&);}
