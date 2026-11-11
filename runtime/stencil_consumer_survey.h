#pragma once
#include <d3d12.h>
#include <cstdint>
#include <span>
namespace rex::graphics {class RegisterFile;class Shader;class CommandProcessor;}
namespace em2 {
struct StencilSurveyContext {
  const void* owner;
  uint64_t frame,submission;
};
enum class StencilSurveyReason : unsigned {
  GuestStencilDraw,DepthTransferRead,ResolveDumpRead,ResolveClear,
  NativeBitOverwrite,ProvenZeroOverwrite,NativeStencilReferenceOverwrite,Count
};
struct StencilSurveyDrawState {
  uint32_t normalized_depth_control,front_refmask,back_refmask;
  D3D12_RECT scissor;
  uint64_t vertex_shader_hash=0,pixel_shader_hash=0;
  uint32_t draw_initiator=0;
  bool pixel_analyzed=false,pixel_kills=false,pixel_writes_depth=false;
};
// Default-Off CPU observer. No GPU commands, waits, readbacks or retained COM
// references. False transitions finish a session instead of joining gaps.
bool StencilSurveyEnabled();
void StencilSurveyDraw(StencilSurveyContext,bool rasterization,bool stencil_enabled,
                       ID3D12Resource* depth,const StencilSurveyDrawState* state=nullptr);
void StencilSurveyCopy(StencilSurveyContext,ID3D12Resource*,std::span<const D3D12_RECT>);
void StencilSurveyRead(StencilSurveyContext,ID3D12Resource*,std::span<const D3D12_RECT>,
                       StencilSurveyReason,bool whole_resource=false);
void StencilSurveyOverwrite(StencilSurveyContext,ID3D12Resource*,std::span<const D3D12_RECT>,
                            StencilSurveyReason);
void StencilSurveyProbeClear(StencilSurveyContext,rex::graphics::CommandProcessor&,
    const rex::graphics::RegisterFile&,const rex::graphics::Shader&,ID3D12Resource*,
    uint32_t normalized_depth_control,uint32_t normalized_color_mask);
void ShutdownStencilSurvey(const void* owner);
bool TestStencilConsumerSurvey();
}
