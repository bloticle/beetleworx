#include "material_ambient_export.h"
#include "rex_render_target/thirdparty/dxbc/DXBCChecksum.h"
#include <rex/graphics/format/dxbc.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <algorithm>
#include <cstring>
#include <cstddef>
namespace {
using namespace rex::graphics::dxbc;
uint32_t Read(const uint8_t* p){uint32_t v;std::memcpy(&v,p,4);return v;}
void Append(std::vector<uint8_t>& out,const void* p,size_t bytes){const auto* a=static_cast<const uint8_t*>(p);out.insert(out.end(),a,a+bytes);}
bool Output(std::span<const uint32_t> instruction,uint32_t index){
    if(instruction.size()<3 || (instruction[0]&0x80000000))return false;
    const auto operand=instruction[1];
    return !(operand&0x80000000) && ((operand>>12)&255)==2 &&
        ((operand>>20)&3)==1 && ((operand>>22)&7)==0 && instruction[2]==index;
}
}
std::vector<uint8_t> em2::ExportMaterialAmbient(std::span<const uint8_t> source){
    if(source.size()<36 || source.size()>16*1024*1024 || std::memcmp(source.data(),"DXBC",4) || Read(source.data()+24)!=source.size())return {};
    const auto count=Read(source.data()+28);
    if(!count || count>64 || size_t(count)*4>source.size()-32)return {};
    struct Chunk{uint32_t type;std::vector<uint8_t> data;};std::vector<Chunk> chunks;
    bool signature=false,program=false;
    for(unsigned i=0;i<count;++i){
        const size_t at=Read(source.data()+32+i*4);
        if(at<32+count*4 || at>source.size()-8 || at%4)return {};
        const auto type=Read(source.data()+at),size=Read(source.data()+at+4);
        if(size>source.size()-at-8 || size%4)return {};
        const auto data=source.subspan(at+8,size);
        if(type==MakeFourCC('O','S','G','1') || type==MakeFourCC('O','S','G','5'))return {};
        if(type==MakeFourCC('S','T','A','T'))continue;
        Chunk chunk{type,{data.begin(),data.end()}};
        if(type==MakeFourCC('O','S','G','N')){
            if(signature || size<8)return {};signature=true;
            const auto n=Read(data.data()),start=Read(data.data()+4);
            if(start!=8 || n<3 || n>16 || size_t(n)*sizeof(SignatureParameter)>size-8)return {};
            std::vector<SignatureParameter> parameters;unsigned found=0;
            for(unsigned p=0;p<n;++p){
                SignatureParameter entry;std::memcpy(&entry,data.data()+8+p*sizeof(entry),sizeof(entry));
                if(entry.semantic_name_ptr<8+n*sizeof(entry) || entry.semantic_name_ptr>=size || !std::memchr(data.data()+entry.semantic_name_ptr,0,size-entry.semantic_name_ptr))return {};
                if(entry.register_index==5)return {};
                if(entry.register_index<=2){
                    if(entry.semantic_index!=entry.register_index || entry.component_type!=SignatureRegisterComponentType::kFloat32 || entry.mask!=15 || std::strcmp(reinterpret_cast<const char*>(data.data()+entry.semantic_name_ptr),"SV_Target"))return {};
                    const auto bit=1u<<entry.register_index;if(found&bit)return {};found|=bit;
                    if(entry.register_index==1)continue;
                    if(entry.register_index==2)entry.register_index=entry.semantic_index=5;
                }
                entry.semantic_name_ptr-=sizeof(entry);parameters.push_back(entry);
            }
            if(found!=7 || parameters.size()!=n-1)return {};
            std::stable_sort(parameters.begin(),parameters.end(),[](const auto& a,const auto& b){return a.register_index<b.register_index;});
            chunk.data.clear();const uint32_t header[]={n-1,8};Append(chunk.data,header,sizeof(header));
            Append(chunk.data,parameters.data(),parameters.size()*sizeof(SignatureParameter));
            Append(chunk.data,data.data()+8+n*sizeof(SignatureParameter),size-8-n*sizeof(SignatureParameter));
        }else if(type==MakeFourCC('S','H','E','X') || type==MakeFourCC('S','H','D','R')){
            if(program || size<8)return {};program=true;
            std::vector<uint32_t> words(size/4);std::memcpy(words.data(),data.data(),size);
            if(words[1]!=words.size() || (words[0]>>16)!=0 || ((words[0]>>4)&15)!=5 || (words[0]&15)!=1)return {};
            std::vector<uint32_t> out{words[0],0};unsigned declarations[3]{},exports[3]{},captures=0;uint32_t ambient_register=UINT32_MAX;bool tail=false;unsigned depth=0,returns=0;
            constexpr uint32_t bias=offsetof(rex::graphics::DxbcShaderTranslator::SystemConstants,color_exp_bias)/16;
            static_assert(offsetof(rex::graphics::DxbcShaderTranslator::SystemConstants,color_exp_bias)%16==0);
            for(size_t p=2;p<words.size();){
                const auto op=words[p]&0x7ff;size_t length=(words[p]>>24)&127;
                if(op==53 && !length && p+1<words.size())length=words[p+1];
                if(!length || length>words.size()-p)return {};
                const auto instruction=std::span(words).subspan(p,length);
                if(Output(instruction,5))return {};
                bool omit=false;unsigned target=0;
                for(;target<3;++target)if(Output(instruction,target))break;
                if(target<3){
                    if(op==101){++declarations[target];if(target==1)omit=true;}
                    else if(op==54){++exports[target];
                        if(target==2){
                            if(ambient_register==UINT32_MAX)return {};
                            std::vector<uint32_t> expected;Statistics stat{};Assembler assembler(expected,stat);assembler.OpMov(Dest::O(2),Src::R(ambient_register));
                            if(!std::equal(expected.begin(),expected.end(),instruction.begin(),instruction.end()))return {};
                        }
                        if(target)omit=true;
                    }else return {};
                }
                // Match the native full-vector color2 exposure multiply exactly,
                // rather than guessing a temporary or patching unrelated CB reads.
                if(op==56 && length>=3){
                    const uint32_t reg=instruction[2];std::vector<uint32_t> expected;Statistics stat{};Assembler assembler(expected,stat);
                    assembler.OpMul(Dest::R(reg),Src::R(reg),Src::CB(0,0,bias,Src::kZZZZ));
                    if(std::equal(expected.begin(),expected.end(),instruction.begin(),instruction.end())){
                        if(!tail || depth || captures++)return {};ambient_register=reg;
                        Statistics capture_stat{};Assembler capture(out,capture_stat);
                        capture.OpMul(Dest::O(5,7),Src::R(reg),Src::CB(0,0,bias,Src::kXXXX));
                        capture.OpMov(Dest::O(5,8),Src::LF(1));
                    }
                }
                // Once color0 has been exported the native translator emits
                // only color1/2 exposure+gamma math, then RET. Remove that dead
                // diagnostic tail after checking its side effects/control flow.
                if(tail){
                    if(op==62){if(depth)return {};++returns;}
                    else if(op==54 && Output(instruction,4)){}
                    else {
                        if(op==31){++depth;}
                        else if(op==18){if(!depth)return {};}
                        else if(op==21){if(!depth)return {};--depth;}
                        else if(op==1 || op==29 || op==50 || op==54 || op==55 || op==56 || op==67){
                            if(length<3)return {};const auto type=(instruction[1]>>12)&255;
                            if(type!=0 && !(op==54 && (Output(instruction,1) || Output(instruction,2))))return {};
                        }else return {};
                        omit=true;
                    }
                }
                if(op==54 && Output(instruction,0)){if(tail)return {};tail=true;}
                if(!omit){const auto begin=out.size();out.insert(out.end(),instruction.begin(),instruction.end());if(target==2 && op==101)out[begin+2]=5;}
                p+=length;
            }
            if(captures!=1 || depth || returns!=1 || !tail)return {};
            for(unsigned target=0;target<3;++target)if(declarations[target]!=1 || exports[target]!=1)return {};
            out[1]=uint32_t(out.size());chunk.data.resize(out.size()*4);std::memcpy(chunk.data.data(),out.data(),chunk.data.size());
        }
        chunks.push_back(std::move(chunk));
    }
    if(!signature || !program)return {};
    std::vector<uint8_t> out(32+chunks.size()*4);std::memcpy(out.data(),source.data(),32);
    const uint32_t n=uint32_t(chunks.size());std::memcpy(out.data()+28,&n,4);
    for(unsigned i=0;i<chunks.size();++i){const uint32_t at=uint32_t(out.size()),header[]={chunks[i].type,uint32_t(chunks[i].data.size())};std::memcpy(out.data()+32+i*4,&at,4);Append(out,header,sizeof(header));Append(out,chunks[i].data.data(),chunks[i].data.size());}
    const uint32_t bytes=uint32_t(out.size());std::memcpy(out.data()+24,&bytes,4);unsigned hash[4];CalculateDXBCChecksum(out.data(),bytes,hash);std::memcpy(out.data()+4,hash,sizeof(hash));return out;
}

bool em2::MaterialAmbientPipeline::Supports(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& p,DXGI_FORMAT ambient){
    if(!p.NumRenderTargets || p.NumRenderTargets>5 || !p.pRootSignature || !p.PS.pShaderBytecode || !p.PS.BytecodeLength ||
       (ambient!=DXGI_FORMAT_R16G16B16A16_FLOAT && ambient!=DXGI_FORMAT_R32G32B32A32_FLOAT) ||
       p.BlendState.RenderTarget[0].BlendEnable || p.BlendState.RenderTarget[0].LogicOpEnable || p.BlendState.RenderTarget[0].RenderTargetWriteMask!=15)return false;
    for(unsigned i=1;i<std::min(p.NumRenderTargets,4u);++i){const auto& b=p.BlendState.RenderTarget[p.BlendState.IndependentBlendEnable?i:0];if(p.RTVFormats[i]!=DXGI_FORMAT_UNKNOWN && b.RenderTargetWriteMask)return false;}
    return true;
}
std::unique_ptr<em2::MaterialAmbientPipeline> em2::MaterialAmbientPipeline::Create(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& p,DXGI_FORMAT ambient){
    if(!Supports(p,ambient))return {};
    auto result=std::unique_ptr<MaterialAmbientPipeline>(new MaterialAmbientPipeline);
    result->pixel_=ExportMaterialAmbient({static_cast<const uint8_t*>(p.PS.pShaderBytecode),p.PS.BytecodeLength});if(result->pixel_.empty())return {};
    result->root_=p.pRootSignature;result->desc_=p;auto& d=result->desc_;d.PS={result->pixel_.data(),result->pixel_.size()};d.CachedPSO={};d.NumRenderTargets=6;
    if(!p.BlendState.IndependentBlendEnable)for(unsigned i=1;i<5;++i)d.BlendState.RenderTarget[i]=p.BlendState.RenderTarget[0];
    d.BlendState.IndependentBlendEnable=TRUE;
    for(unsigned i=1;i<5;++i)if(i>=p.NumRenderTargets || p.RTVFormats[i]==DXGI_FORMAT_UNKNOWN){d.RTVFormats[i]=DXGI_FORMAT_UNKNOWN;d.BlendState.RenderTarget[i]={};}
    d.RTVFormats[5]=ambient;d.BlendState.RenderTarget[5]={};d.BlendState.RenderTarget[5].RenderTargetWriteMask=15;return result;
}

bool em2::MaterialAmbientClearPipeline::Supports(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& p){
    if(!p.NumRenderTargets || p.NumRenderTargets>5 || !p.pRootSignature || !p.PS.pShaderBytecode || !p.PS.BytecodeLength || p.RTVFormats[0]==DXGI_FORMAT_UNKNOWN)return false;
    const auto& b=p.BlendState.RenderTarget[0];if(b.LogicOpEnable || !(b.RenderTargetWriteMask&7))return false;
    for(unsigned i=1;i<std::min(p.NumRenderTargets,4u);++i){const auto& other=p.BlendState.RenderTarget[p.BlendState.IndependentBlendEnable?i:0];if(p.RTVFormats[i]!=DXGI_FORMAT_UNKNOWN && other.RenderTargetWriteMask)return false;}
    if(!b.BlendEnable)return true;
    const bool colour_factor=b.DestBlend==D3D12_BLEND_SRC_COLOR || b.DestBlend==D3D12_BLEND_INV_SRC_COLOR;
    // S * destination carries existing ambient through the source term.
    // Inverse destination colour needs a different signed decomposition.
    if(b.SrcBlend==D3D12_BLEND_INV_DEST_COLOR)return false;
    if(colour_factor || b.SrcBlend==D3D12_BLEND_DEST_COLOR){
        if(p.NumRenderTargets!=5 || p.RTVFormats[4]!=DXGI_FORMAT_R16G16B16A16_FLOAT)return false;
        const auto& hdr=p.BlendState.RenderTarget[p.BlendState.IndependentBlendEnable?4:0];
        if(hdr.LogicOpEnable || hdr.BlendEnable!=b.BlendEnable || hdr.SrcBlend!=b.SrcBlend || hdr.DestBlend!=b.DestBlend || hdr.BlendOp!=b.BlendOp || (hdr.RenderTargetWriteMask&7)!=(b.RenderTargetWriteMask&7))return false;
    }
    if(!colour_factor && b.DestBlend!=D3D12_BLEND_ZERO && b.DestBlend!=D3D12_BLEND_ONE && b.DestBlend!=D3D12_BLEND_SRC_ALPHA && b.DestBlend!=D3D12_BLEND_INV_SRC_ALPHA)return false;
    if(b.SrcBlend>=D3D12_BLEND_SRC1_COLOR || b.SrcBlendAlpha>=D3D12_BLEND_SRC1_COLOR || b.DestBlendAlpha>=D3D12_BLEND_SRC1_COLOR)return false;
    return b.BlendOp==D3D12_BLEND_OP_ADD || b.BlendOp==D3D12_BLEND_OP_SUBTRACT || b.BlendOp==D3D12_BLEND_OP_REV_SUBTRACT;
}
std::unique_ptr<em2::MaterialAmbientClearPipeline> em2::MaterialAmbientClearPipeline::Create(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& p){
    if(!Supports(p))return {};auto result=std::unique_ptr<MaterialAmbientClearPipeline>(new MaterialAmbientClearPipeline);
    const auto& blend=p.BlendState.RenderTarget[0];const bool colour_factor=blend.BlendEnable && (blend.DestBlend==D3D12_BLEND_SRC_COLOR || blend.DestBlend==D3D12_BLEND_INV_SRC_COLOR || blend.SrcBlend==D3D12_BLEND_DEST_COLOR);
    const std::span bytes{static_cast<const uint8_t*>(p.PS.pShaderBytecode),p.PS.BytecodeLength};result->pixel_=colour_factor?ExportHdrAttenuationMaterialAmbient(bytes):ExportZeroMaterialAmbient(bytes);if(result->pixel_.empty())return {};
    result->root_=p.pRootSignature;result->desc_=p;auto& d=result->desc_;d.PS={result->pixel_.data(),result->pixel_.size()};d.CachedPSO={};d.NumRenderTargets=6;
    if(!p.BlendState.IndependentBlendEnable)for(unsigned i=1;i<5;++i)d.BlendState.RenderTarget[i]=p.BlendState.RenderTarget[0];d.BlendState.IndependentBlendEnable=TRUE;
    for(unsigned i=1;i<5;++i)if(i>=p.NumRenderTargets || p.RTVFormats[i]==DXGI_FORMAT_UNKNOWN){d.RTVFormats[i]=DXGI_FORMAT_UNKNOWN;d.BlendState.RenderTarget[i]={};}
    d.RTVFormats[5]=DXGI_FORMAT_R16G16B16A16_FLOAT;auto& b=d.BlendState.RenderTarget[5];const auto primary=p.BlendState.RenderTarget[0];b={};b.BlendEnable=primary.BlendEnable;b.SrcBlend=primary.SrcBlend==D3D12_BLEND_DEST_COLOR?D3D12_BLEND_DEST_COLOR:D3D12_BLEND_ZERO;b.DestBlend=primary.DestBlend;b.BlendOp=primary.BlendOp;b.SrcBlendAlpha=D3D12_BLEND_ZERO;b.DestBlendAlpha=D3D12_BLEND_ONE;b.BlendOpAlpha=D3D12_BLEND_OP_ADD;b.RenderTargetWriteMask=primary.RenderTargetWriteMask&7;return result;
}
