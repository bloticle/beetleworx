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
static std::vector<uint8_t> ExportSurface(std::span<const uint8_t> source,bool combined,bool linear_only=false){
    const unsigned targets=combined?4:2;
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
            if(start!=8 || n<targets || n>16 || size_t(n)*sizeof(SignatureParameter)>size-8)return {};
            std::vector<SignatureParameter> parameters;unsigned found=0;
            for(unsigned p=0;p<n;++p){
                SignatureParameter entry;std::memcpy(&entry,data.data()+8+p*sizeof(entry),sizeof(entry));
                if(entry.semantic_name_ptr<8+n*sizeof(entry) || entry.semantic_name_ptr>=size || !std::memchr(data.data()+entry.semantic_name_ptr,0,size-entry.semantic_name_ptr))return {};
                if(entry.register_index>=targets && entry.register_index!=4 &&
                   !(entry.register_index==UINT32_MAX && entry.component_type==SignatureRegisterComponentType::kUInt32 && entry.mask==1 && !std::strcmp(reinterpret_cast<const char*>(data.data()+entry.semantic_name_ptr),"SV_Coverage")))return {};
                if(entry.register_index<targets){
                    if(entry.semantic_index!=entry.register_index || entry.component_type!=SignatureRegisterComponentType::kFloat32 || entry.mask!=15 || std::strcmp(reinterpret_cast<const char*>(data.data()+entry.semantic_name_ptr),"SV_Target"))return {};
                    const auto bit=1u<<entry.register_index;if(found&bit)return {};found|=bit;
                    if(combined && entry.register_index==1)continue;
                    if(entry.register_index)entry.register_index=entry.semantic_index=combined?entry.register_index+3:6;
                }
                if(combined)entry.semantic_name_ptr-=sizeof(entry);parameters.push_back(entry);
            }
            if(found!=((1u<<targets)-1) || parameters.size()!=n-unsigned(combined))return {};
            std::stable_sort(parameters.begin(),parameters.end(),[](const auto& a,const auto& b){return a.register_index<b.register_index;});
            chunk.data.clear();const uint32_t header[]={n-unsigned(combined),8};Append(chunk.data,header,sizeof(header));
            Append(chunk.data,parameters.data(),parameters.size()*sizeof(SignatureParameter));
            Append(chunk.data,data.data()+8+n*sizeof(SignatureParameter),size-8-n*sizeof(SignatureParameter));
        }else if(type==MakeFourCC('S','H','E','X') || type==MakeFourCC('S','H','D','R')){
            if(program || size<8)return {};program=true;
            std::vector<uint32_t> words(size/4);std::memcpy(words.data(),data.data(),size);
            if(words[1]!=words.size() || (words[0]>>16)!=0 || ((words[0]>>4)&15)!=5 || (words[0]&15)!=1)return {};
            std::vector<uint32_t> out{words[0],0};unsigned declarations[4]{},exports[4]{},captures[4]{};uint32_t registers[4]{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};bool tail=false;unsigned depth=0,returns=0;uint32_t scratch=UINT32_MAX;
            constexpr uint32_t bias=offsetof(rex::graphics::DxbcShaderTranslator::SystemConstants,color_exp_bias)/16;
            static_assert(offsetof(rex::graphics::DxbcShaderTranslator::SystemConstants,color_exp_bias)%16==0);
            for(size_t p=2;p<words.size();){
                const auto op=words[p]&0x7ff;size_t length=(words[p]>>24)&127;
                if(op==53 && !length && p+1<words.size())length=words[p+1];
                if(!length || length>words.size()-p)return {};
                const auto instruction=std::span(words).subspan(p,length);
                if(Output(instruction,5) || Output(instruction,6))return {};
                bool omit=false;unsigned target=0;
                for(;target<targets;++target)if(Output(instruction,target))break;
                if(target<targets){
                    if(op==101){++declarations[target];if(combined && target==1)omit=true;}
                    else if(op==54){++exports[target];
                        if(target && !(combined && target==1)){
                            if(registers[target]==UINT32_MAX)return {};
                            std::vector<uint32_t> expected;Statistics stat{};Assembler assembler(expected,stat);assembler.OpMov(Dest::O(target),Src::R(registers[target]));
                            if(!std::equal(expected.begin(),expected.end(),instruction.begin(),instruction.end()))return {};
                        }
                        if(target)omit=true;
                    }else return {};
                }
                // Capture each private value at its exact native exposure op.
                if(op==56 && length>=3){
                    const uint32_t reg=instruction[2];
                    for(unsigned t=combined?2:1;t<targets;++t){
                        std::vector<uint32_t> expected;Statistics stat{};Assembler assembler(expected,stat);
                        const uint32_t lanes[]={Src::kXXXX,Src::kYYYY,Src::kZZZZ,Src::kWWWW};
                        assembler.OpMul(Dest::R(reg),Src::R(reg),Src::CB(0,0,bias,lanes[t]));
                        if(std::equal(expected.begin(),expected.end(),instruction.begin(),instruction.end())){
                            if(!tail || depth || captures[t]++)return {};registers[t]=reg;
                            Statistics capture_stat{};Assembler capture(out,capture_stat);
                            if(combined && t==2){capture.OpMul(Dest::O(5,7),Src::R(reg),Src::CB(0,0,bias,Src::kXXXX));capture.OpMov(Dest::O(5,8),Src::LF(1));}
                            else if(!linear_only)capture.OpMov(Dest::O(6),Src::R(reg));
                            else{
                                if(scratch==UINT32_MAX)return {};
                                constexpr auto offset=offsetof(rex::graphics::DxbcShaderTranslator::SystemConstants,texture_swizzled_signs);
                                constexpr uint32_t lane=(offset%16)/4;constexpr uint32_t swizzle[]={Src::kXXXX,Src::kYYYY,Src::kZZZZ,Src::kWWWW};
                                // Pinned Gen3_StaticEnv diffuse uses fetch1,
                                // occupying byte1; two post-swizzle
                                // sign bits per component. Gamma3 in all RGB
                                // lanes guarantees native PWL/sRGB decoding.
                                static_assert(uint32_t(rex::graphics::xenos::TextureSign::kGamma)==3);
                                capture.OpAnd(Dest::R(scratch,1),Src::CB(0,0,uint32_t(offset/16),swizzle[lane]),Src::LU(0x3f00));
                                capture.OpIEq(Dest::R(scratch,1),Src::R(scratch,Src::kXXXX),Src::LU(0x3f00));
                                capture.OpMovC(Dest::O(6,7),Src::R(scratch,Src::kXXXX),Src::R(reg),Src::LF(0));
                                capture.OpMovC(Dest::O(6,8),Src::R(scratch,Src::kXXXX),Src::R(reg,Src::kWWWW),Src::LF(-1));
                            }
                        }
                    }
                }
                // Once color0 has been exported the native translator emits
                // only diagnostic exposure+gamma math, then RET. Remove that dead
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
                            if(type!=0 && !(op==54 && (Output(instruction,1) || (combined && (Output(instruction,2) || Output(instruction,3))))))return {};
                        }else return {};
                        omit=true;
                    }
                }
                if(op==54 && Output(instruction,0)){if(tail)return {};tail=true;}
                if(!omit){const auto begin=out.size();out.insert(out.end(),instruction.begin(),instruction.end());if(target && target<targets && op==101)out[begin+2]=combined?target+3:6;
                    if(linear_only && op==104){if(length!=2 || scratch!=UINT32_MAX || instruction[1]>=4096)return {};scratch=instruction[1];out[begin+1]=scratch+1;}
                }
                p+=length;
            }
            if(depth || returns!=1 || !tail)return {};
            for(unsigned t=combined?2:1;t<targets;++t)if(captures[t]!=1)return {};
            for(unsigned target=0;target<targets;++target)if(declarations[target]!=1 || exports[target]!=1)return {};
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

std::vector<uint8_t> em2::ExportMaterialSurface(std::span<const uint8_t> source){return ExportSurface(source,false);}
std::vector<uint8_t> em2::ExportMaterialLightingSurface(std::span<const uint8_t> source){return ExportSurface(source,true);}
std::vector<uint8_t> em2::ExportLinearMaterialSurface(std::span<const uint8_t> source){return ExportSurface(source,false,true);}
std::vector<uint8_t> em2::ExportLinearMaterialLightingSurface(std::span<const uint8_t> source){return ExportSurface(source,true,true);}

std::unique_ptr<em2::MaterialLightingSurfacePipeline> em2::MaterialLightingSurfacePipeline::Create(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& p,bool linear_only){
    if(!MaterialAmbientPipeline::Supports(p))return {};
    auto result=std::unique_ptr<MaterialLightingSurfacePipeline>(new MaterialLightingSurfacePipeline);
    result->pixel_=linear_only?ExportLinearMaterialLightingSurface({static_cast<const uint8_t*>(p.PS.pShaderBytecode),p.PS.BytecodeLength}):ExportMaterialLightingSurface({static_cast<const uint8_t*>(p.PS.pShaderBytecode),p.PS.BytecodeLength});if(result->pixel_.empty())return {};
    result->root_=p.pRootSignature;auto& d=result->desc_;d=p;d.pRootSignature=result->root_.Get();d.PS={result->pixel_.data(),result->pixel_.size()};d.CachedPSO={};d.NumRenderTargets=7;
    if(!p.BlendState.IndependentBlendEnable)for(unsigned i=1;i<5;++i)d.BlendState.RenderTarget[i]=p.BlendState.RenderTarget[0];
    d.BlendState.IndependentBlendEnable=TRUE;
    for(unsigned i=1;i<5;++i)if(i>=p.NumRenderTargets || p.RTVFormats[i]==DXGI_FORMAT_UNKNOWN){d.RTVFormats[i]=DXGI_FORMAT_UNKNOWN;d.BlendState.RenderTarget[i]={};}
    for(unsigned i=5;i<7;++i){d.RTVFormats[i]=DXGI_FORMAT_R16G16B16A16_FLOAT;d.BlendState.RenderTarget[i]={};d.BlendState.RenderTarget[i].RenderTargetWriteMask=15;}
    return result;
}

std::unique_ptr<em2::MaterialSurfaceClearPipeline> em2::MaterialSurfaceClearPipeline::Create(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& p,bool preserve){
    if(!p.pRootSignature || !p.PS.pShaderBytecode || !p.NumRenderTargets || p.NumRenderTargets>6 || p.RTVFormats[0]==DXGI_FORMAT_UNKNOWN || !(p.BlendState.RenderTarget[0].RenderTargetWriteMask&7))return {};
    auto result=std::unique_ptr<MaterialSurfaceClearPipeline>(new MaterialSurfaceClearPipeline);
    result->pixel_=ExportUnavailableMaterialSurface({static_cast<const uint8_t*>(p.PS.pShaderBytecode),p.PS.BytecodeLength});if(result->pixel_.empty())return {};
    if(preserve){const auto* first=static_cast<const uint8_t*>(p.PS.pShaderBytecode);result->pixel_.assign(first,first+p.PS.BytecodeLength);}
    result->root_=p.pRootSignature;auto& d=result->desc_;d=p;d.pRootSignature=result->root_.Get();d.PS={result->pixel_.data(),result->pixel_.size()};d.CachedPSO={};d.NumRenderTargets=7;
    if(!p.BlendState.IndependentBlendEnable)for(unsigned i=1;i<6;++i)d.BlendState.RenderTarget[i]=p.BlendState.RenderTarget[0];
    d.BlendState.IndependentBlendEnable=TRUE;
    for(unsigned i=1;i<6;++i)if(i>=p.NumRenderTargets || p.RTVFormats[i]==DXGI_FORMAT_UNKNOWN){d.RTVFormats[i]=DXGI_FORMAT_UNKNOWN;d.BlendState.RenderTarget[i]={};}
    d.RTVFormats[6]=DXGI_FORMAT_R16G16B16A16_FLOAT;d.BlendState.RenderTarget[6]={};d.BlendState.RenderTarget[6].RenderTargetWriteMask=preserve?0:15;
    return result;
}
