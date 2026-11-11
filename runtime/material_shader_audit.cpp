#include "scene_shader_audit.h"
#include "../private/material_shader_data.h"
#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/logging.h>
#include <bit>
#include <fstream>
#include <span>
bool em2::AuditNativeMaterialShaders(rex::ui::d3d12::D3D12Provider& provider,const std::filesystem::path& output){
    std::filesystem::create_directories(output);
    const std::array<std::span<const uint32_t>,7> programs={material_shader_data::Gen3_Char_Oswald_LOD0,material_shader_data::Gen3_Char_Oswald,material_shader_data::Gen3_Char_Mickey,material_shader_data::Gen3_StaticEnv_DL,material_shader_data::world_baked_difference,material_shader_data::Gen3_StaticEnv,material_shader_data::world_base_baked_difference};const char* names[]={"oswald-lod","oswald-full","mickey-full","world-original","world-baked-difference","world-base-original","world-base-baked-difference"};
    std::ofstream report(output/"material-shader-audit.json");report<<"{\"programs\":[";
    for(unsigned i=0;i<programs.size();++i){rex::graphics::DxbcShader shader(rex::graphics::xenos::ShaderType::kPixel,i+1,programs[i].data(),programs[i].size(),std::endian::native);rex::string::StringBuffer buffer;shader.AnalyzeUcode(buffer);rex::graphics::DxbcShaderTranslator translator(provider.GetAdapterVendorID(),false,false,false,true,1,1);auto* translated=shader.GetOrCreateTranslation(translator.GetDefaultPixelShaderModification(0));if(!translator.TranslateAnalyzedShader(*translated) || !translated->is_valid())return false;
        std::ofstream text(output/(std::string(names[i])+".ucode.txt"));text<<shader.ucode_disassembly();if(!text.good())return false;
        if(i>=3){std::ofstream binary(output/(std::string(names[i])+".dxbc"),std::ios::binary);const auto& bytes=translated->translated_binary();binary.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());if(!binary.good())return false;}
        report<<(i?",":"")<<"{\"name\":\""<<names[i]<<"\",\"float_guest_to_packed\":[";bool first=true;const auto& constants=shader.constant_register_map();for(unsigned c=0;c<256;++c)if(constants.float_bitmap[c/64]&(uint64_t(1)<<(c%64))){report<<(first?"":",")<<"["<<c<<","<<constants.GetPackedFloatConstantIndex(c)<<"]";first=false;}
        report<<"],\"texture_fetches\":[";first=true;for(const auto& binding:shader.GetTextureBindingsAfterTranslation()){report<<(first?"":",")<<binding.fetch_constant;first=false;}report<<"]}";
    }
    {const auto& words=material_shader_data::full_vertex_linked;rex::graphics::DxbcShader vertex(rex::graphics::xenos::ShaderType::kVertex,3,words.data(),words.size(),std::endian::native);rex::string::StringBuffer text;vertex.AnalyzeUcode(text);rex::graphics::DxbcShaderTranslator translator(provider.GetAdapterVendorID(),false,false,false,true,1,1);auto* translated=vertex.GetOrCreateTranslation(translator.GetDefaultVertexShaderModification(0));if(!translator.TranslateAnalyzedShader(*translated)||!translated->is_valid())return false;std::ofstream file(output/"oswald-full-vertex.ucode.txt");file<<vertex.ucode_disassembly();if(!file.good())return false;}
    report<<"],\"scope\":\"Pinned original material shaders translated and native disassembly saved. Reflection proves constant/texture identities only, not separated ambient contribution or GPU execution.\"}\n";REXLOG_INFO("Native material shader audit: original Oswald LOD/full and Mickey full SH, normal and specular programs translate; no lighting replacement or GPU execution");return report.good();
}
