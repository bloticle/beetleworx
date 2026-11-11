#include "scene_shader_audit.h"
#include "../private/scene_shader_data.h"
#include <rex/graphics/d3d12/shader.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/logging.h>
#include <array>
#include <bit>
#include <fstream>
#include <span>
bool em2::AuditNativeSceneShaders(rex::ui::d3d12::D3D12Provider& provider,const std::filesystem::path& output){
    std::filesystem::create_directories(output);
    const std::array<std::span<const uint32_t>,4> programs={scene_shader_data::ZPrepass_0,scene_shader_data::ZPrepass_1,scene_shader_data::ScreenSpace_DepthFog_0,scene_shader_data::depth_fog_vertex};
    const char* names[]={"z-prepass-0","z-prepass-1","depth-fog","depth-fog-vertex"};
    std::ofstream report(output/"scene-shader-audit.json");report<<"{\"programs\":[";
    for(unsigned i=0;i<programs.size();++i){
        const bool vertex=i==3;
        rex::graphics::DxbcShader shader(vertex?rex::graphics::xenos::ShaderType::kVertex:rex::graphics::xenos::ShaderType::kPixel,i+1,programs[i].data(),programs[i].size(),std::endian::native);
        rex::string::StringBuffer buffer;shader.AnalyzeUcode(buffer);
        rex::graphics::DxbcShaderTranslator translator(provider.GetAdapterVendorID(),false,false,false,true,1,1);
        auto* translated=shader.GetOrCreateTranslation(vertex?translator.GetDefaultVertexShaderModification(0):translator.GetDefaultPixelShaderModification(0));
        if(!translator.TranslateAnalyzedShader(*translated) || !translated->is_valid())return false;
        std::ofstream disasm(output/(std::string(names[i])+".ucode.txt"));disasm<<shader.ucode_disassembly();if(!disasm.good())return false;
        const auto& constants=shader.constant_register_map();
        report<<(i?",":"")<<"{\"name\":\""<<names[i]<<"\",\"float_guest_to_packed\":[";
        bool first=true;for(unsigned c=0;c<256;++c){
            if(!(constants.float_bitmap[c/64]&(uint64_t(1)<<(c%64))))continue;
            report<<(first?"":",")<<"["<<c<<","<<constants.GetPackedFloatConstantIndex(c)<<"]";first=false;
        }
        report<<"],\"texture_fetches\":[";first=true;bool depth=false,fog=false;
        for(const auto& binding:shader.GetTextureBindingsAfterTranslation()){
            report<<(first?"":",")<<binding.fetch_constant;first=false;
            depth|=binding.fetch_constant==0;fog|=binding.fetch_constant==1;
        }
        report<<"],\"vertex_bindings\":[";first=true;
        for(const auto& binding:shader.vertex_bindings()){
            report<<(first?"":",")<<"{\"fetch_constant\":"<<binding.fetch_constant<<",\"stride_words\":"<<binding.stride_words<<",\"attributes\":[";first=false;bool first_attribute=true;
            for(const auto& attribute:binding.attributes){const auto& a=attribute.fetch_instr.attributes;
                report<<(first_attribute?"":",")<<"{\"offset_words\":"<<a.offset<<",\"format\":"<<uint32_t(a.data_format)<<",\"signed\":"<<(a.is_signed?"true":"false")<<",\"integer\":"<<(a.is_integer?"true":"false")<<"}";first_attribute=false;}
            report<<"]}";
        }
        report<<"]}";
        if(i==2 && (!depth || !fog))return false;
    }
    report<<"],\"scope\":\"Pinned original scene pixel programs analyzed and translated through native SDK; guest/packed constant and texture identities only. No reconstruction convention, actual geometry capture, GPU execution or title proven.\"}\n";
    if(!report.good())return false;
    REXLOG_INFO("Native scene shader audit: two Z-prepass pixels and depth-fog vertex/pixel pair translate; depth/fog texture identities present; guest/packed constant maps and native disassembly saved; geometry/depth reconstruction still unverified");return true;
}
