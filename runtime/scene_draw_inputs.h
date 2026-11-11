#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <d3d12.h>
#include <rex/graphics/xenos.h>
namespace rex::graphics {class Shader;class RegisterFile;}
namespace em2 {
struct SceneRasterState {
    struct Draw {
        bool indexed=false;
        uint32_t count=0,instances=0,start=0,start_instance=0;
        int32_t base_vertex=0;
        std::optional<D3D12_INDEX_BUFFER_VIEW> indices;
    };
    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissor{};
    std::array<float,3> ndc_scale{},ndc_offset{};
    uint32_t recorded_draws=0;
    Draw last_draw;
};
struct SceneDrawInputs {
    struct VertexInput {
        rex::graphics::xenos::xe_gpu_vertex_fetch_t fetch{};
        uint32_t fetch_constant=0,stride_words=0,offset_words=0;
        rex::graphics::xenos::VertexFormat format{};
    };
    using Row=std::array<float,4>;
    std::array<Row,4> inverse_view_projection;
    std::array<Row,3> inverse_view;
    Row vertex_dimensions,vertex_literals,pixel_literals;
    rex::graphics::xenos::xe_gpu_texture_fetch_t depth_fetch{},fog_fetch{};
    uint64_t frame=0,draw=0;
    std::optional<SceneRasterState> raster;
    std::array<VertexInput,2> vertex_inputs;
};
// Value metadata only: no shader pointers or GPU resource authority/lifetime.
// Exact native fog pixel and original vertex arithmetic with validated runtime-
// linked vertex declarations required. GPU-worker access only.
std::optional<SceneDrawInputs> CaptureSceneDrawInputs(const rex::graphics::Shader*,const rex::graphics::Shader*,const rex::graphics::RegisterFile&,uint64_t frame,uint64_t draw);
bool TestSceneDrawInputs();
}
