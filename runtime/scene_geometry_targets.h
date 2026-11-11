#pragma once
#include <d3d12.h>
#include <wrl/client.h>
namespace em2 {
struct SceneGeometryTargets {
    Microsoft::WRL::ComPtr<ID3D12Resource> positions,normals;
    // Initial state UAV; after each producer write both outputs are PSR|NPSR.
    bool written=false;
};
}
