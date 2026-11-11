#include "built_shaders.h"
#include "auxiliary_depth_resample.h"
#include <rex/logging.h>
#include <climits>
using Microsoft::WRL::ComPtr;
bool em2::AuxiliaryDepthResampler::Initialize(ID3D12Device* device,unsigned samples) {
    if(!device || (samples!=1 && samples!=2 && samples!=4))return false;
    
    
    const auto* vertex=GetBuiltShader(samples>1?BuiltShader::AuxiliaryMSVS:BuiltShader::AuxiliaryVS);
    const auto* depth=GetBuiltShader(samples>1?BuiltShader::AuxiliaryMSDepth:BuiltShader::AuxiliaryDepth);
    const auto* stencil=GetBuiltShader(samples>1?BuiltShader::AuxiliaryMSStencil:BuiltShader::AuxiliaryStencil);
    if(!vertex || !depth || !stencil)return false;
    ComPtr<ID3DBlob> errors,serialized;
    const D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0};
    std::array<D3D12_ROOT_PARAMETER,2> parameters{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants={0,0,5};
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[1].DescriptorTable={1,&range};
    D3D12_ROOT_SIGNATURE_DESC root_desc{2,parameters.data(),0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    if(FAILED(D3D12SerializeRootSignature(&root_desc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors)) ||
       FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root_))))return false;
    for(unsigned pass=0;pass<9;++pass) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};pso.pRootSignature=root_.Get();pso.VS={vertex->GetBufferPointer(),vertex->GetBufferSize()};
        const auto& pixel=pass?stencil:depth;pso.PS={pixel->GetBufferPointer(),pixel->GetBufferSize()};
        pso.SampleMask=UINT_MAX;pso.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pso.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable=FALSE;pso.RasterizerState.MultisampleEnable=samples>1;
        pso.DepthStencilState.DepthEnable=!pass;pso.DepthStencilState.DepthWriteMask=pass?D3D12_DEPTH_WRITE_MASK_ZERO:D3D12_DEPTH_WRITE_MASK_ALL;
        pso.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;pso.DepthStencilState.StencilEnable=pass!=0;
        pso.DepthStencilState.StencilReadMask=0xFF;pso.DepthStencilState.StencilWriteMask=pass?uint8_t(1u<<(pass-1)):0;
        auto& face=pso.DepthStencilState.FrontFace;face.StencilFailOp=face.StencilDepthFailOp=D3D12_STENCIL_OP_KEEP;
        face.StencilPassOp=D3D12_STENCIL_OP_REPLACE;face.StencilFunc=D3D12_COMPARISON_FUNC_ALWAYS;pso.DepthStencilState.BackFace=face;
        pso.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pso.DSVFormat=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;pso.SampleDesc.Count=samples;
        if(FAILED(device->CreateGraphicsPipelineState(&pso,IID_PPV_ARGS(&pipelines_[pass]))))return false;
    }
    samples_=samples;return true;
}
bool em2::AuxiliaryDepthResampler::Record(ID3D12GraphicsCommandList* list,ID3D12Resource* destination,
    D3D12_CPU_DESCRIPTOR_HANDLE dsv,D3D12_GPU_DESCRIPTOR_HANDLE source,unsigned sx,unsigned sy,unsigned dx,unsigned dy) const {
    if(!list || !destination || !root_ || !samples_ || !sx || sx>7 || !sy || sy>7 || !dx || dx>7 || !dy || dy>7)return false;
    const auto desc=destination->GetDesc();if(desc.Format!=DXGI_FORMAT_R32G8X24_TYPELESS || desc.SampleDesc.Count!=samples_)return false;
    const D3D12_VIEWPORT viewport{0,0,float(desc.Width),float(desc.Height),0,1};const D3D12_RECT scissor{0,0,LONG(desc.Width),LONG(desc.Height)};
    list->RSSetViewports(1,&viewport);list->RSSetScissorRects(1,&scissor);list->OMSetRenderTargets(0,nullptr,FALSE,&dsv);
    list->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_STENCIL,0,0,0,nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);list->SetGraphicsRootSignature(root_.Get());
    list->SetGraphicsRootDescriptorTable(1,source);list->OMSetStencilRef(0xFF);
    for(unsigned pass=0;pass<9;++pass) {
        const std::array<unsigned,5> constants={sx,sy,dx,dy,pass?1u<<(pass-1):0};
        list->SetGraphicsRoot32BitConstants(0,5,constants.data(),0);list->SetPipelineState(pipelines_[pass].Get());list->DrawInstanced(3,1,0,0);
    }
    return true;
}
