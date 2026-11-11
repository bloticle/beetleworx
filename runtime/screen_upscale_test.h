#pragma once
#include <rex/ui/presenter.h>
#include <array>
#include <filesystem>
namespace rex::ui::d3d12 {class D3D12Presenter;class D3D12Provider;}
namespace em2 {
void EnableScreenUpscaleValidation();
using ScreenUpscaleCases=std::array<std::array<rex::ui::Presenter::GuestOutputPaintConfig,3>,3>;
bool TestScreenUpscalePixels(rex::ui::d3d12::D3D12Presenter&,const rex::ui::d3d12::D3D12Provider&,
                            const ScreenUpscaleCases&,const std::filesystem::path&,bool hdr=false);
}
