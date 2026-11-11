#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
namespace rex::ui::d3d12 {class D3D12Provider;}
namespace em2 {
// One contiguous native range (8-byte units for 8bpp, 16-byte otherwise).
// GPU root offsets remain 16-byte aligned. The caller must establish its
// texel format and retain both allocations until the GPU fence completes.
// This is texture resolve storage, not the differently organized EDRAM buffer.
struct ScaledResolveMigration {
    uint32_t source_x,source_y,destination_x,destination_y;
    uint32_t bytes_per_texel,native_bytes;
};
struct EdramScaleMigration {
    uint32_t source_x,source_y,destination_x,destination_y;
    uint32_t tile_count;
    bool pixels_are_64_bit;
    // Native EDRAM interleaves 2x samples vertically and 4x in both axes.
    uint32_t samples_x=1,samples_y=1;
};
class ScaledResolveMigrator {
public:
    bool Initialize(ID3D12Device*);
    // Source must be NON_PIXEL_SHADER_RESOURCE (or GENERIC_READ), destination
    // UNORDERED_ACCESS, with non-overlapping backing allocations. Does not
    // submit, wait or change resource states. It replaces compute root/PSO
    // bindings; callers must invalidate their caches and rebind them. This
    // migrator, source and destination must outlive the completion fence.
    bool Record(ID3D12GraphicsCommandList*,ID3D12Resource* source,uint64_t source_offset,
        ID3D12Resource* destination,uint64_t destination_offset,const ScaledResolveMigration&) const;
    // One uniform-format range of complete EDRAM tiles. For mixed ownership,
    // issue separate calls with offsets/ranges obtained from the native owner
    // table. Depth uses the 32-bit path; its bank swap commutes with scaling.
    bool RecordEdram(ID3D12GraphicsCommandList*,ID3D12Resource* source,uint64_t source_offset,
        ID3D12Resource* destination,uint64_t destination_offset,const EdramScaleMigration&) const;
    // Untile a range into a canonical 1280-pixel-wide texture copy footprint.
    // Destination is the complete 10 MiB * destination scale area footprint;
    // source_offset selects the first tile. Native tile positions are retained.
    bool RecordEdramLinear(ID3D12GraphicsCommandList*,ID3D12Resource* source,uint64_t source_offset,
        ID3D12Resource* destination,const EdramScaleMigration&,uint32_t first_tile) const;
private:
    bool RecordStorage(ID3D12GraphicsCommandList*,ID3D12Resource*,uint64_t,
        ID3D12Resource*,uint64_t,const ScaledResolveMigration&,uint32_t layout,uint32_t first_tile=0,
        uint32_t samples_x=1,uint32_t samples_y=1) const;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
};
bool TestScaledResolveMigration(rex::ui::d3d12::D3D12Provider&);
bool TestEdramScaleMigration(rex::ui::d3d12::D3D12Provider&);
}
