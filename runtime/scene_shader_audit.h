#pragma once
#include <filesystem>
namespace rex::ui::d3d12 { class D3D12Provider; }
namespace rex::graphics::d3d12 {class D3D12CommandProcessor;}
namespace em2 {
// Uses pinned private game programs with the native translator. Does not
// replace a shader or claim that depth encoding/camera conventions are known.
bool AuditNativeSceneShaders(rex::ui::d3d12::D3D12Provider&,const std::filesystem::path&);
bool AuditNativeMaterialShaders(rex::ui::d3d12::D3D12Provider&,const std::filesystem::path&);
bool TestNativeMaterialLighting(rex::ui::d3d12::D3D12Provider&,const std::filesystem::path&);
bool TestWiiBlendPixels(rex::ui::d3d12::D3D12Provider&,const std::filesystem::path&);
bool TestNativeMaterialCapture(rex::graphics::d3d12::D3D12CommandProcessor&,const std::filesystem::path&);
bool TestSceneAmbientComposition(rex::graphics::d3d12::D3D12CommandProcessor&,const std::filesystem::path&);
bool TestMaterialAmbientClear(rex::ui::d3d12::D3D12Provider&,const std::filesystem::path&);
bool TestNativeFullMaterialLighting(rex::ui::d3d12::D3D12Provider&,const std::filesystem::path&);
bool TestSceneUnprojection(rex::ui::d3d12::D3D12Provider&,const std::filesystem::path&);
bool TestNativeFogVertex(rex::ui::d3d12::D3D12Provider&,const std::filesystem::path&);
}

namespace em2 {bool TestWorldSurfaceCapture(rex::ui::d3d12::D3D12Provider&,const std::filesystem::path&,bool combined=false,bool full_reference=false,bool linear_only=false);
bool TestWorldMaterialLighting(rex::ui::d3d12::D3D12Provider&,const std::filesystem::path&,bool full_reference=false,bool base_material=false);}

namespace em2 {bool TestSceneIndirectLighting(rex::graphics::d3d12::D3D12CommandProcessor&,const std::filesystem::path&);}
