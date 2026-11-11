#include "built_shaders.h"
#include "scene_raster_geometry.h"
#include <rex/logging.h>
#include <cmath>
#include <string>
#include <immintrin.h>
namespace {struct Fp{unsigned csr=_mm_getcsr();~Fp(){_mm_setcsr(csr);}};}
bool em2::PrepareRasterGeometryConstants(const SceneDrawInputs& source,RasterGeometryConstants& output){
    static_assert(sizeof(RasterGeometryConstants)==20*sizeof(float));
    Fp fp;RasterGeometryConstants result{};double camera[3];
    for(unsigned i=0;i<3;++i){camera[i]=source.inverse_view[i][3];if(!std::isfinite(camera[i]))return false;}
    for(unsigned row=0;row<4;++row)for(unsigned column=0;column<4;++column){const auto value=source.inverse_view_projection[row][column];if(!std::isfinite(value))return false;
        result.relative_inverse[row][column]=float(double(value)-(row<3?camera[row]*double(source.inverse_view_projection[3][column]):0));if(!std::isfinite(result.relative_inverse[row][column]))return false;}
    double points[2][3];for(unsigned i=0;i<2;++i){const double z=i?.75:.25;const auto& rows=result.relative_inverse;const double w=double(rows[3][2])*z+rows[3][3];if(!std::isfinite(w)||std::abs(w)<1e-7)return false;
        for(unsigned c=0;c<3;++c)points[i][c]=(double(rows[c][2])*z+rows[c][3])/w;}
    double direction[3],length2=0;for(unsigned c=0;c<3;++c){direction[c]=points[1][c]-points[0][c];length2+=direction[c]*direction[c];}if(!std::isfinite(length2)||length2<1e-20)return false;
    for(unsigned c=0;c<3;++c)result.forward_and_discontinuity[c]=float(direction[c]/std::sqrt(length2));result.forward_and_discontinuity[3]=.25f;output=result;return true;
}
bool em2::SceneRasterGeometryKernel::Initialize(ID3D12Device* device,std::span<const uint8_t> vertex,std::span<const uint32_t> geometry){
    Fp fp;if(!device || vertex.empty())return false;
    Microsoft::WRL::ComPtr<ID3DBlob> errors,serialized;
    const auto* pixel=em2::GetBuiltShader(em2::BuiltShader::SceneRasterGeometry);if(!pixel)return false;
    D3D12_ROOT_PARAMETER parameters[10]{};for(unsigned i=0;i<3;++i){parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;parameters[i].Descriptor.ShaderRegister=i==2?3:i;parameters[i].ShaderVisibility=i?D3D12_SHADER_VISIBILITY_VERTEX:D3D12_SHADER_VISIBILITY_ALL;}
    const D3D12_DESCRIPTOR_RANGE memory_ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0}};
    for(unsigned i=3;i<=4;++i){parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[i].DescriptorTable={1,&memory_ranges[i-3]};parameters[i].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;}
    for(unsigned i=5;i<=6;++i){parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;parameters[i].Descriptor.ShaderRegister=i==5?2:4;parameters[i].ShaderVisibility=i==5?D3D12_SHADER_VISIBILITY_ALL:D3D12_SHADER_VISIBILITY_VERTEX;}
    const D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,1,0},{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,1,0,1,0}};
    for(unsigned i=7;i<=8;++i){parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[i].DescriptorTable={1,&ranges[i-7]};parameters[i].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;}
    parameters[9].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[9].Constants={0,1,20};parameters[9].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    const D3D12_ROOT_SIGNATURE_DESC root{10,parameters};Microsoft::WRL::ComPtr<ID3D12RootSignature> signature;
    if(FAILED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors)) || FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&signature))))return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=signature.Get();pipeline.VS={vertex.data(),vertex.size()};pipeline.PS={pixel->GetBufferPointer(),pixel->GetBufferSize()};if(!geometry.empty())pipeline.GS={geometry.data(),geometry.size_bytes()};
    for(unsigned i=0;i<2;++i)pipeline.BlendState.RenderTarget[i].RenderTargetWriteMask=15;
    pipeline.SampleMask=UINT_MAX;pipeline.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pipeline.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pipeline.RasterizerState.DepthClipEnable=TRUE;pipeline.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pipeline.NumRenderTargets=2;pipeline.RTVFormats[0]=DXGI_FORMAT_R32G32B32A32_FLOAT;pipeline.RTVFormats[1]=DXGI_FORMAT_R16G16B16A16_FLOAT;pipeline.SampleDesc.Count=1;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> state;if(FAILED(device->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&state))))return false;
    root_=std::move(signature);pipeline_=std::move(state);return true;
}
