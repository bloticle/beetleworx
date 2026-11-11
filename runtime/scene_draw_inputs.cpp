#include "scene_draw_inputs.h"
#include "../private/scene_shader_data.h"
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/d3d12/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/logging.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <memory>
#include <immintrin.h>
#include <rex/graphics/format/ucode.h>
namespace {
template<size_t N>bool Match(const rex::graphics::Shader* shader,rex::graphics::xenos::ShaderType type,const std::array<uint32_t,N>& code){
    return shader && shader->type()==type && shader->ucode_dword_count()==N && std::equal(code.begin(),code.end(),shader->ucode_data().begin());
}
bool LinkedVertex(const rex::graphics::Shader* shader,std::array<em2::SceneDrawInputs::VertexInput,2>& inputs){
    using namespace rex::graphics;const auto& reference=em2::scene_shader_data::depth_fog_vertex;
    if(!shader || shader->type()!=xenos::ShaderType::kVertex || shader->ucode_dword_count()!=reference.size())return false;
    const auto& actual=shader->ucode_data();
    for(size_t i=0;i<reference.size();++i){const auto slot=i/3,word=i%3;const uint32_t allowed=(slot==3 || slot==4)?(word==0?0x3FF00000u:word==1?0x3F3FF000u:0x7FFFFFFFu):0;
        if((actual[i]^reference[i])&~allowed)return false;}
    for(unsigned slot:{3u,4u}){ucode::VertexFetchInstruction fetch;std::memcpy(&fetch,actual.data()+slot*3,sizeof(fetch));
        bool valid=false;switch(fetch.data_format()){
            case xenos::VertexFormat::k_8_8_8_8:case xenos::VertexFormat::k_2_10_10_10:case xenos::VertexFormat::k_10_11_11:case xenos::VertexFormat::k_11_11_10:
            case xenos::VertexFormat::k_16_16:case xenos::VertexFormat::k_16_16_16_16:case xenos::VertexFormat::k_16_16_FLOAT:case xenos::VertexFormat::k_16_16_16_16_FLOAT:
            case xenos::VertexFormat::k_32_32:case xenos::VertexFormat::k_32_32_32_32:case xenos::VertexFormat::k_32_32_FLOAT:case xenos::VertexFormat::k_32_32_32_32_FLOAT:case xenos::VertexFormat::k_32_32_32_FLOAT:valid=true;break;default:break;}
        if(!valid || fetch.fetch_constant_index()>=96 || !fetch.stride() || fetch.offset()<0 || uint32_t(fetch.offset())>=fetch.stride())return false;
        const auto needed=xenos::GetVertexFormatNeededWords(fetch.data_format(),3);if(uint32_t(fetch.offset())+std::bit_width(needed)>fetch.stride())return false;
        auto& input=inputs[slot-3];input.fetch_constant=fetch.fetch_constant_index();input.stride_words=fetch.stride();input.offset_words=uint32_t(fetch.offset());input.format=fetch.data_format();
    }
    return true;
}
}
std::optional<em2::SceneDrawInputs> em2::CaptureSceneDrawInputs(const rex::graphics::Shader* vertex,const rex::graphics::Shader* pixel,const rex::graphics::RegisterFile& regs,uint64_t frame,uint64_t draw){
    using namespace rex::graphics;
    SceneDrawInputs result;result.frame=frame;result.draw=draw;
    if(!draw || !Match(pixel,xenos::ShaderType::kPixel,scene_shader_data::ScreenSpace_DepthFog_0) || !LinkedVertex(vertex,result.vertex_inputs))return {};
    for(auto& input:result.vertex_inputs){input.fetch=regs.GetVertexFetch(input.fetch_constant);const auto needed=std::bit_width(xenos::GetVertexFormatNeededWords(input.format,3));if(input.fetch.type!=xenos::FetchConstantType::kVertex || input.fetch.size<input.offset_words+needed)return {};}
    auto row=[&](uint32_t base,unsigned index,SceneDrawInputs::Row& output){for(unsigned i=0;i<4;++i){const uint32_t bits=regs.values[base+index*4+i];if((bits&0x7F800000u)==0x7F800000u)return false;output[i]=std::bit_cast<float>(bits);}return true;};
    for(unsigned i=0;i<4;++i)if(!row(XE_GPU_REG_SHADER_CONSTANT_256_X,13+i,result.inverse_view_projection[i]))return {};
    for(unsigned i=0;i<3;++i)if(!row(XE_GPU_REG_SHADER_CONSTANT_256_X,10+i,result.inverse_view[i]))return {};
    if(!row(XE_GPU_REG_SHADER_CONSTANT_000_X,0,result.vertex_dimensions))return {};
    if(!row(XE_GPU_REG_SHADER_CONSTANT_000_X,255,result.vertex_literals) || !row(XE_GPU_REG_SHADER_CONSTANT_256_X,255,result.pixel_literals))return {};
    // Native c255.x/y participate in position reconstruction itself. Exact
    // shader identity alone does not guarantee that guest constants match.
    if((std::bit_cast<uint32_t>(result.pixel_literals[0])&0x7FFFFFFFu)!=0 || std::bit_cast<uint32_t>(result.pixel_literals[1])!=0x3F800000u)return {};
    result.depth_fetch=regs.GetTextureFetch(0);result.fog_fetch=regs.GetTextureFetch(1);
    for(const auto* fetch:{&result.depth_fetch,&result.fog_fetch})if(fetch->type!=xenos::FetchConstantType::kTexture || fetch->dimension!=xenos::DataDimension::k2DOrStacked || fetch->stacked)return {};
    return result;
}
bool em2::TestSceneDrawInputs(){
    using namespace rex::graphics;
    auto regs=std::make_unique<RegisterFile>();std::fill(std::begin(regs->values),std::end(regs->values),0x7FC00000u);
    const auto& vs=scene_shader_data::depth_fog_vertex_linked_example;const auto& ps=scene_shader_data::ScreenSpace_DepthFog_0;
    DxbcShader vertex(xenos::ShaderType::kVertex,1,vs.data(),vs.size(),std::endian::native),pixel(xenos::ShaderType::kPixel,2,ps.data(),ps.size(),std::endian::native);
    for(unsigned c=10;c<=16;++c)for(unsigned lane=0;lane<4;++lane)regs->values[XE_GPU_REG_SHADER_CONSTANT_256_X+c*4+lane]=std::bit_cast<uint32_t>(float(c*10+lane));
    for(unsigned lane=0;lane<4;++lane)regs->values[XE_GPU_REG_SHADER_CONSTANT_000_X+lane]=std::bit_cast<uint32_t>(float(lane)+.5f);
    for(unsigned lane=0;lane<4;++lane){regs->values[XE_GPU_REG_SHADER_CONSTANT_000_X+255*4+lane]=std::bit_cast<uint32_t>(float(lane)+5.f);regs->values[XE_GPU_REG_SHADER_CONSTANT_256_X+255*4+lane]=std::bit_cast<uint32_t>(SceneDrawInputs::Row{0,1,3,2}[lane]);}
    xenos::xe_gpu_texture_fetch_t fetch{};fetch.type=xenos::FetchConstantType::kTexture;fetch.dimension=xenos::DataDimension::k2DOrStacked;fetch.base_address=123;fetch.size_2d.width=639;fetch.size_2d.height=359;
    std::memcpy(&regs->values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],&fetch,sizeof(fetch));fetch.base_address=456;std::memcpy(&regs->values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0+6],&fetch,sizeof(fetch));
    xenos::xe_gpu_vertex_fetch_t stream{};stream.type=xenos::FetchConstantType::kVertex;stream.address=1024;stream.size=12;std::memcpy(&regs->values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0+95*2],&stream,sizeof(stream));
    const auto captured=CaptureSceneDrawInputs(&vertex,&pixel,*regs,17,42);
    if(!captured || captured->frame!=17 || captured->draw!=42 || captured->depth_fetch.base_address!=123 || captured->fog_fetch.base_address!=456)return false;
    if(captured->vertex_inputs[0].fetch_constant!=95 || captured->vertex_inputs[0].stride_words!=4 || captured->vertex_inputs[1].offset_words!=2 || captured->vertex_inputs[1].fetch.address!=1024)return false;
    for(unsigned c=0;c<4;++c)for(unsigned lane=0;lane<4;++lane)if(captured->inverse_view_projection[c][lane]!=float((13+c)*10+lane))return false;
    for(unsigned c=0;c<3;++c)for(unsigned lane=0;lane<4;++lane)if(captured->inverse_view[c][lane]!=float((10+c)*10+lane))return false;
    if(captured->vertex_dimensions!=SceneDrawInputs::Row{.5f,1.5f,2.5f,3.5f})return false;
    if(captured->vertex_literals!=SceneDrawInputs::Row{5,6,7,8} || captured->pixel_literals!=SceneDrawInputs::Row{0,1,3,2})return false;
    auto accepts=[&](const auto& code){DxbcShader candidate(xenos::ShaderType::kVertex,4,code.data(),code.size(),std::endian::native);return CaptureSceneDrawInputs(&candidate,&pixel,*regs,17,43).has_value();};
    if(accepts(scene_shader_data::depth_fog_vertex))return false; // Offline placeholders are not drawable declarations.
    auto half=vs;for(unsigned slot:{3u,4u})half[slot*3+2]=(half[slot*3+2]&~255u)|3u;
    half[13]=(half[13]&~0x3F0000u)|(uint32_t(xenos::VertexFormat::k_16_16_FLOAT)<<16);
    if(!accepts(half))return false;
    for(const auto [word,bit]:{std::pair{size_t(0),0u},std::pair{size_t(9),0u},std::pair{size_t(9),31u},std::pair{size_t(10),22u},std::pair{size_t(15),0u}}){auto bad=vs;bad[word]^=1u<<bit;if(accepts(bad))return false;}
    for(unsigned invalid=0;invalid<5;++invalid){auto bad=vs;
        if(invalid==0)bad[11]&=~255u; // Zero stride.
        else if(invalid==1)bad[10]&=~0x3F0000u; // Undefined format.
        else if(invalid==2)bad[10]=(bad[10]&~0x3F0000u)|(uint32_t(xenos::VertexFormat::k_32_FLOAT)<<16); // Scalar cannot supply XY.
        else if(invalid==3)bad[14]=(bad[14]&0x800000FFu)|(0x7FFFFFu<<8); // Negative offset.
        else bad[14]=(bad[14]&~255u)|3u; // float2 at DWORD2 overruns stride3.
        if(accepts(bad))return false;}
    for(unsigned invalid=0;invalid<2;++invalid){auto bad=stream;if(invalid==0)bad.type=xenos::FetchConstantType::kTexture;else bad.size=3;
        std::memcpy(&regs->values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0+95*2],&bad,sizeof(bad));if(accepts(vs))return false;}
    std::memcpy(&regs->values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0+95*2],&stream,sizeof(stream));
    if(CaptureSceneDrawInputs(nullptr,&pixel,*regs,17,43) || CaptureSceneDrawInputs(&pixel,&vertex,*regs,17,43) || CaptureSceneDrawInputs(&vertex,&pixel,*regs,17,0))return false;
    auto changed=ps;changed.back()^=1;DxbcShader unrelated(xenos::ShaderType::kPixel,3,changed.data(),changed.size(),std::endian::native);
    if(CaptureSceneDrawInputs(&vertex,&unrelated,*regs,17,43))return false;
    for(uint32_t index:{uint32_t(XE_GPU_REG_SHADER_CONSTANT_000_X),uint32_t(XE_GPU_REG_SHADER_CONSTANT_256_X+10*4+3),uint32_t(XE_GPU_REG_SHADER_CONSTANT_256_X+16*4+3)}){const auto saved=regs->values[index];for(uint32_t bits:{0x7FC00000u,0x7F800001u,0x7F800000u,0xFF800000u}){regs->values[index]=bits;const auto csr=_mm_getcsr();if(CaptureSceneDrawInputs(&vertex,&pixel,*regs,17,43) || _mm_getcsr()!=csr)return false;}regs->values[index]=saved;}
    for(unsigned source=0;source<2;++source){const auto offset=XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0+source*6;const auto original=regs->GetTextureFetch(source);
        for(unsigned invalid=0;invalid<3;++invalid){auto bad=original;if(invalid==0)bad.stacked=1;else if(invalid==1)bad.type=xenos::FetchConstantType::kVertex;else bad.dimension=xenos::DataDimension::k3D;
            std::memcpy(&regs->values[offset],&bad,sizeof(bad));if(CaptureSceneDrawInputs(&vertex,&pixel,*regs,17,43))return false;std::memcpy(&regs->values[offset],&original,sizeof(original));}}
    for(unsigned lane=0;lane<2;++lane){const auto index=XE_GPU_REG_SHADER_CONSTANT_256_X+255*4+lane;const auto saved=regs->values[index];regs->values[index]=std::bit_cast<uint32_t>(2.f);if(CaptureSceneDrawInputs(&vertex,&pixel,*regs,17,43))return false;regs->values[index]=saved;}
    if(captured->depth_fetch.base_address!=123 || captured->draw!=42)return false;
    REXLOG_INFO("Scene draw inputs passed: original VS arithmetic with legal linked float/half declarations, placeholder/mutated arithmetic/invalid declaration/unbound stream rejection, exact PS, separate constant banks and independent value lifetime; no GPU resource authority claimed");return true;
}
