#include "hdr_shader_export.h"
#include "material_ambient_export.h"
#include "rex_render_target/thirdparty/dxbc/DXBCChecksum.h"
#include <rex/graphics/format/dxbc.h>
#include <cstring>
#include <algorithm>
#include <cstddef>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
namespace {
using namespace rex::graphics::dxbc;
uint32_t Read(const uint8_t* p){uint32_t v;std::memcpy(&v,p,4);return v;}
void Append(std::vector<uint8_t>& out,const void* p,size_t bytes){const auto* a=static_cast<const uint8_t*>(p);out.insert(out.end(),a,a+bytes);}
bool Output(std::span<const uint32_t> instruction,uint32_t index) {
    if(instruction.size()<3 || (instruction[0]&0x80000000))return false;
    const auto operand=instruction[1];
    return !(operand&0x80000000) && ((operand>>12)&255)==2 &&
        ((operand>>20)&3)==1 && ((operand>>22)&7)==0 && instruction[2]==index;
}
}
static std::vector<uint8_t> ExportSceneColour(std::span<const uint8_t> source,uint32_t output,bool zero_rgb,bool linear=false,bool hdr_factor=false,bool unavailable_surface=false) {
    if(source.size()<36 || source.size()>16*1024*1024 || std::memcmp(source.data(),"DXBC",4) ||
       Read(source.data()+24)!=source.size())return {};
    const auto count=Read(source.data()+28);
    if(!count || count>64 || size_t(count)*4>source.size()-32)return {};
    struct Chunk {uint32_t type;std::vector<uint8_t> data;};std::vector<Chunk> chunks;
    bool signature=false,program=false;
    for(unsigned i=0;i<count;++i) {
        const size_t at=Read(source.data()+32+i*4);
        if(at<32+count*4 || at>source.size()-8 || at%4)return {};
        const auto type=Read(source.data()+at),size=Read(source.data()+at+4);
        if(size>source.size()-at-8 || size%4)return {};
        const auto data=source.subspan(at+8,size);
        if(type==MakeFourCC('O','S','G','1') || type==MakeFourCC('O','S','G','5'))return {};
        if(type==MakeFourCC('S','T','A','T'))continue; // Optional stale compiler statistics.
        Chunk chunk{type,{data.begin(),data.end()}};
        if(type==MakeFourCC('O','S','G','N')) {
            if(signature || size<8)return {};signature=true;
            const auto n=Read(data.data()),start=Read(data.data()+4);
            if(start!=8 || !n || n>16 || size_t(n)*sizeof(SignatureParameter)>size-8)return {};
            std::vector<SignatureParameter> parameters(n);std::memcpy(parameters.data(),data.data()+8,n*sizeof(SignatureParameter));
            SignatureParameter extra{};bool found=false,hdr_found=false;
            for(auto& p:parameters) {
                if(p.register_index==output || p.semantic_name_ptr<8+n*sizeof(SignatureParameter) || p.semantic_name_ptr>=size)return {};
                const auto available=size-p.semantic_name_ptr;
                if(!std::memchr(data.data()+p.semantic_name_ptr,0,available))return {};
                if(p.register_index==0 && p.semantic_index==0 && p.component_type==SignatureRegisterComponentType::kFloat32 &&
                   p.mask==15 && !std::strcmp(reinterpret_cast<const char*>(data.data()+p.semantic_name_ptr),"SV_Target")) {
                    if(found)return {};extra=p;found=true;
                }
                if(hdr_factor && p.register_index==4){
                    if(hdr_found || p.semantic_index!=4 || p.component_type!=SignatureRegisterComponentType::kFloat32 || p.mask!=15 || std::strcmp(reinterpret_cast<const char*>(data.data()+p.semantic_name_ptr),"SV_Target"))return {};
                    hdr_found=true;
                }
                p.semantic_name_ptr+=sizeof(SignatureParameter);
            }
            if(!found || (hdr_factor && !hdr_found))return {};
            extra.semantic_name_ptr+=sizeof(SignatureParameter);extra.semantic_index=extra.register_index=output;
            // Keep ordinary register outputs before special outputs such as
            // SV_Coverage/SV_Depth (register UINT_MAX).
            parameters.insert(std::find_if(parameters.begin(),parameters.end(),[&](const auto& p){return p.register_index>output;}),extra);
            chunk.data.clear();const uint32_t header[]={n+1,8};Append(chunk.data,header,sizeof(header));
            Append(chunk.data,parameters.data(),parameters.size()*sizeof(SignatureParameter));
            Append(chunk.data,data.data()+8+n*sizeof(SignatureParameter),size-8-n*sizeof(SignatureParameter));
        } else if(type==MakeFourCC('S','H','E','X') || type==MakeFourCC('S','H','D','R')) {
            if(program || size<8)return {};program=true;
            std::vector<uint32_t> words(size/4);std::memcpy(words.data(),data.data(),size);
            if(words[1]!=words.size() || (words[0]>>16)!=0 || ((words[0]>>4)&15)!=5 || (words[0]&15)>1)return {};
            std::vector<uint32_t> out{words[0],0};unsigned declarations=0,exports=0,captures=0,hdr_declarations=0,hdr_exports=0;uint32_t colour_register=UINT32_MAX;std::vector<uint32_t> controls;
            constexpr uint32_t bias=offsetof(rex::graphics::DxbcShaderTranslator::SystemConstants,color_exp_bias)/16;
            if(linear && ((words[0]&15)!=1 || output!=4 || zero_rgb))return {};
            for(size_t p=2;p<words.size();) {
                const auto op=words[p]&0x7ff;size_t length=(words[p]>>24)&127;
                if(op==53 && !length && p+1<words.size())length=words[p+1];
                if(!length || length>words.size()-p)return {};
                const auto instruction=std::span(words).subspan(p,length);
                if(Output(instruction,output))return {};
                if(hdr_factor && (op==4 || op==5 || op==44 || op==63))return {};
                // The native translator exports through MOV. A partially
                // mirrored arbitrary shader would silently lose some writes.
                if(Output(instruction,0) && op!=101 && op!=54)return {};
                if(linear || hdr_factor){
                    if(op==31 || op==48 || op==76)controls.push_back(op);
                    else if(op==21 || op==22 || op==23){const uint32_t expected=op==21?31:op==22?48:76;if(controls.empty() || controls.back()!=expected)return {};controls.pop_back();}
                    if(op==62 && (linear?!captures:(!exports || !hdr_exports)))return {};
                }
                out.insert(out.end(),instruction.begin(),instruction.end());
                if(hdr_factor && Output(instruction,4)){
                    if((instruction[1]&255)!=0xf2u || (op!=101 && op!=54))return {};
                    if(op==101)++hdr_declarations;
                    else {
                        if(!controls.empty())return {};++hdr_exports;
                        // Match the HDR colour source used by its own blend
                        // unit. Colour0 may have undergone native gamma math.
                        const auto begin=out.size();out.insert(out.end(),instruction.begin(),instruction.end());out[begin+2]=output;out[begin+1]=(out[begin+1]&~0xf0u)|0x70u;
                    }
                }
                if(linear && op==56 && length>=3 && instruction[2]<4096){
                    const auto reg=instruction[2];std::vector<uint32_t> expected;Statistics statistics{};Assembler assembler(expected,statistics);assembler.OpMul(Dest::R(reg),Src::R(reg),Src::CB(0,0,bias,Src::kXXXX));
                    if(std::equal(expected.begin(),expected.end(),instruction.begin(),instruction.end())){if(!controls.empty() || captures++)return {};colour_register=reg;Statistics capture_statistics{};Assembler capture(out,capture_statistics);capture.OpMov(Dest::O(4),Src::R(reg));}
                }
                if((op==101 || op==54) && Output(instruction,0)) {
                    if(op==101)++declarations;else {++exports;if(hdr_factor && (!controls.empty() || (instruction[1]&255)!=0xf2u))return {};if(linear){if(colour_register==UINT32_MAX || !controls.empty())return {};std::vector<uint32_t> expected;Statistics statistics{};Assembler assembler(expected,statistics);assembler.OpMov(Dest::O(0),Src::R(colour_register));if(!std::equal(expected.begin(),expected.end(),instruction.begin(),instruction.end()))return {};p+=length;continue;}}
                    const auto begin=out.size();out.insert(out.end(),instruction.begin(),instruction.end());out[begin+2]=output;
                    if(op==54 && hdr_factor)out[begin+1]=(out[begin+1]&~0xf0u)|0x80u;
                    if(op==54 && zero_rgb){rex::graphics::dxbc::Statistics statistics{};rex::graphics::dxbc::Assembler assembler(out,statistics);assembler.OpMov(rex::graphics::dxbc::Dest::O(output,7),rex::graphics::dxbc::Src::LF(0));if(unavailable_surface)assembler.OpMov(rex::graphics::dxbc::Dest::O(output,8),rex::graphics::dxbc::Src::LF(-1));}
                }
                p+=length;
            }
            if(declarations!=1 || !exports || (linear && (captures!=1 || exports!=1 || !controls.empty())) || (hdr_factor && (exports!=1 || hdr_declarations!=1 || hdr_exports!=1 || !controls.empty())))return {};
            out[1]=uint32_t(out.size());chunk.data.resize(out.size()*4);std::memcpy(chunk.data.data(),out.data(),chunk.data.size());
        }
        chunks.push_back(std::move(chunk));
    }
    if(!signature || !program)return {};
    std::vector<uint8_t> out(32+chunks.size()*4);std::memcpy(out.data(),source.data(),32);
    const uint32_t n=uint32_t(chunks.size());std::memcpy(out.data()+28,&n,4);
    for(unsigned i=0;i<chunks.size();++i) {
        const uint32_t at=uint32_t(out.size()),header[]={chunks[i].type,uint32_t(chunks[i].data.size())};
        std::memcpy(out.data()+32+i*4,&at,4);Append(out,header,sizeof(header));Append(out,chunks[i].data.data(),chunks[i].data.size());
    }
    const uint32_t bytes=uint32_t(out.size());std::memcpy(out.data()+24,&bytes,4);
    unsigned hash[4];CalculateDXBCChecksum(out.data(),bytes,hash);std::memcpy(out.data()+4,hash,sizeof(hash));return out;
}

std::vector<uint8_t> em2::AddHdrSceneExport(std::span<const uint8_t> source){return ExportSceneColour(source,4,false);}
std::vector<uint8_t> em2::ExportZeroMaterialAmbient(std::span<const uint8_t> source){return ExportSceneColour(source,5,true);}
std::vector<uint8_t> em2::ExportHdrAttenuationMaterialAmbient(std::span<const uint8_t> source){return ExportSceneColour(source,5,false,false,true);}

std::vector<uint8_t> em2::AddLinearHdrSceneExport(std::span<const uint8_t> source){return ExportSceneColour(source,4,false,true);}

std::vector<uint8_t> em2::ExportUnavailableMaterialSurface(std::span<const uint8_t> source){return ExportSceneColour(source,6,true,false,false,true);}
