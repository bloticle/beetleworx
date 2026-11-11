#include "pc_shader_library.h"
#include "../private/material_shader_data.h"
#include "../private/scene_shader_data.h"
#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <array>
#include <fstream>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>
#include <span>
REXCVAR_DECLARE(bool,em2_pretranslated_shaders);
REXCVAR_DECLARE(bool,em2_skip_packed_shader_duplicates);
namespace {
using D=rex::graphics::DxbcShader;using T=rex::graphics::DxbcShaderTranslator;
struct Output {std::vector<uint8_t> binary;std::vector<D::TextureBinding> textures;std::vector<D::SamplerBinding> samplers;uint32_t used=0;};
bool Equal(const Output&a,const Output&b){
    if(a.binary!=b.binary || a.used!=b.used || a.textures.size()!=b.textures.size() || a.samplers.size()!=b.samplers.size())return false;
    if(!a.textures.empty() && std::memcmp(a.textures.data(),b.textures.data(),a.textures.size()*sizeof(a.textures[0])))return false;
    for(size_t i=0;i<a.samplers.size();++i){const auto&x=a.samplers[i];const auto&y=b.samplers[i];if(x.bindless_descriptor_index!=y.bindless_descriptor_index || x.fetch_constant!=y.fetch_constant || x.mag_filter!=y.mag_filter || x.min_filter!=y.min_filter || x.mip_filter!=y.mip_filter || x.aniso_filter!=y.aniso_filter)return false;}return true;
}
struct Case {bool bindless=false,rov=false,gamma=false,msaa=true,map=false;unsigned scale=1;};
constexpr std::array<Case,9> cases={Case{},Case{true},Case{false,true},Case{false,false,true},Case{false,false,false,false},Case{false,false,false,true,true},Case{false,false,false,true,false,2},Case{false,false,false,true,false,3},Case{true,false,false,true,false,2}};
}
namespace em2 {
bool TestPcShaderLibrary(const rex::ui::d3d12::D3D12Provider& provider,const std::filesystem::path& directory){
    const bool previous=REXCVAR_GET(em2_pretranslated_shaders);struct Flag{bool previous;~Flag(){REXCVAR_SET(em2_pretranslated_shaders,previous);}}flag{previous};REXCVAR_SET(em2_pretranslated_shaders,true);
    std::filesystem::create_directories(directory);
    const std::array<std::span<const uint32_t>,3> programs={material_shader_data::Gen3_Char_Oswald_LOD0,scene_shader_data::ScreenSpace_DepthFog_0,scene_shader_data::depth_fog_vertex};
    std::vector<Output> references;unsigned failures=0;double native_ms=0,hit_ms=0;
    auto translate=[&](unsigned program,Case config,Output& output){
        const auto code=programs[program];const auto stage=program==2?rex::graphics::xenos::ShaderType::kVertex:rex::graphics::xenos::ShaderType::kPixel;
        D shader(stage,program+1,code.data(),code.size(),std::endian::native);rex::string::StringBuffer text;shader.AnalyzeUcode(text);
        T translator(provider.GetAdapterVendorID(),config.bindless,config.rov,config.gamma,config.msaa,config.scale,config.scale,config.map);
        const auto modification=program==2?translator.GetDefaultVertexShaderModification(0):translator.GetDefaultPixelShaderModification(0);auto* translated=shader.GetOrCreateTranslation(modification);
        if(!translator.TranslateAnalyzedShader(*translated) || !translated->is_valid() || !translated->is_translated())return false;
        output={translated->translated_binary(),shader.GetTextureBindingsAfterTranslation(),shader.GetSamplerBindingsAfterTranslation(),shader.GetUsedTextureMaskAfterTranslation()};return true;
    };
    {PcShaderLibraryScope native(nullptr);for(auto config:cases)for(unsigned p=0;p<programs.size();++p){Output result;auto start=std::chrono::steady_clock::now();if(!translate(p,config,result))return false;native_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();references.push_back(std::move(result));}}
    auto library=OpenPcShaderLibrary(directory);const auto before=GetPcShaderLibraryStats();
    {PcShaderLibraryScope scope(library);size_t i=0;for(auto config:cases)for(unsigned p=0;p<programs.size();++p){Output first,second;if(!translate(p,config,first) || !Equal(first,references[i]))++failures;auto start=std::chrono::steady_clock::now();if(!translate(p,config,second) || !Equal(second,references[i]))++failures;hit_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();++i;}}
    FlushPcShaderLibrary(library);if(!SavePcShaderLibraryPack(library,directory/"pack-seed"))return false;auto populated=GetPcShaderLibraryStats();library.reset();
    auto reopened=OpenPcShaderLibrary(directory/"pack-consumer",directory/"pack-seed");const auto loaded=GetPcShaderLibraryStats();
    {PcShaderLibraryScope scope(reopened);size_t i=0;for(auto config:cases)for(unsigned p=0;p<programs.size();++p){Output result;if(!translate(p,config,result) || !Equal(result,references[i++]))++failures;}}
    std::atomic<unsigned> parallel_failures{0};std::vector<std::thread> threads;
    for(unsigned i=0;i<8;++i)threads.emplace_back([&,i]{PcShaderLibraryScope scope(reopened);Output result;const unsigned p=i%programs.size();if(!translate(p,cases[0],result) || !Equal(result,references[p]))++parallel_failures;});for(auto& thread:threads)thread.join();failures+=parallel_failures.load();
    const auto warm=GetPcShaderLibraryStats();reopened.reset();
    // A complete pack may coexist with its loose originals and newer runtime
    // variants. Skip only the originals; an invalid pack must retain the full
    // loose-record fallback, with exact native binaries and bindings.
    struct DuplicateFlag{bool saved=REXCVAR_GET(em2_skip_packed_shader_duplicates);~DuplicateFlag(){REXCVAR_SET(em2_skip_packed_shader_duplicates,saved);}} duplicate_flag;
    REXCVAR_SET(em2_skip_packed_shader_duplicates,true);
    const auto overlay=directory/"packed-overlay",overlay_records=overlay/"translated-dxbc-v1";
    std::filesystem::create_directories(overlay_records);
    const auto overlay_pack=overlay_records/"library.em2dxbcpack";
    std::filesystem::copy_file(directory/"pack-seed"/"translated-dxbc-v1"/"library.em2dxbcpack",overlay_pack);
    for(const auto& entry:std::filesystem::directory_iterator(directory/"translated-dxbc-v1"))if(entry.path().extension()==L".em2dxbc")
        std::filesystem::copy_file(entry.path(),overlay_records/entry.path().filename());
    Case extra=cases[0];extra.map=true;extra.scale=2;Output extra_reference;
    {PcShaderLibraryScope native(nullptr);if(!translate(0,extra,extra_reference))return false;}
    auto packed=OpenPcShaderLibrary(overlay);
    {PcShaderLibraryScope scope(packed);Output result;if(!translate(0,extra,result) || !Equal(result,extra_reference))++failures;}
    FlushPcShaderLibrary(packed);packed.reset();
    const auto overlay_before=GetPcShaderLibraryStats();packed=OpenPcShaderLibrary(overlay);const auto overlay_loaded=GetPcShaderLibraryStats();
    {PcShaderLibraryScope scope(packed);size_t i=0;for(auto config:cases)for(unsigned p=0;p<programs.size();++p){Output result;if(!translate(p,config,result) || !Equal(result,references[i++]))++failures;}Output result;if(!translate(0,extra,result) || !Equal(result,extra_reference))++failures;}
    const auto overlay_checked=GetPcShaderLibraryStats();packed.reset();
    const auto skipped=overlay_loaded.packed_duplicates_skipped-overlay_before.packed_duplicates_skipped;
    const auto loose=overlay_loaded.file_reads-overlay_before.file_reads;
    if(skipped!=references.size() || loose!=1 || overlay_checked.misses!=overlay_loaded.misses)++failures;
    {std::fstream file(overlay_pack,std::ios::binary|std::ios::in|std::ios::out);file.put('X');if(!file.good())return false;}
    const auto fallback_before=GetPcShaderLibraryStats();packed=OpenPcShaderLibrary(overlay);const auto fallback_loaded=GetPcShaderLibraryStats();
    {PcShaderLibraryScope scope(packed);size_t i=0;for(auto config:cases)for(unsigned p=0;p<programs.size();++p){Output result;if(!translate(p,config,result) || !Equal(result,references[i++]))++failures;}Output result;if(!translate(0,extra,result) || !Equal(result,extra_reference))++failures;}
    const auto fallback_checked=GetPcShaderLibraryStats();packed.reset();
    const auto fallback_reads=fallback_loaded.file_reads-fallback_before.file_reads;
    if(fallback_reads!=references.size()+1 || fallback_loaded.rejected!=fallback_before.rejected+1 || fallback_checked.misses!=fallback_loaded.misses)++failures;
    // Damage only the isolated fixture library, then demand a real native
    // compile rather than accepting any corrupt program or binding layout.
    unsigned damaged=0;for(const auto& entry:std::filesystem::directory_iterator(directory/"translated-dxbc-v1"))if(entry.path().extension()==L".em2dxbc"){
        std::fstream file(entry.path(),std::ios::binary|std::ios::in|std::ios::out);file.seekg(50);char value=0;file.read(&value,1);value^=1;file.seekp(50);file.write(&value,1);if(!file.good())return false;++damaged;
    }
    auto corrupt=OpenPcShaderLibrary(directory);const auto rejected=GetPcShaderLibraryStats();
    {PcShaderLibraryScope scope(corrupt);Output result;if(!translate(0,cases[0],result) || !Equal(result,references[0]))++failures;}
    FlushPcShaderLibrary(corrupt);const auto recovered=GetPcShaderLibraryStats();
    const size_t count=references.size();if(populated.hits-before.hits<count || loaded.loaded-populated.loaded<count || warm.hits-loaded.hits<count+8 || rejected.rejected-warm.rejected<damaged || recovered.misses-rejected.misses!=1)++failures;
    std::ofstream report(directory/"pc-shader-library-results.json");report<<"{\"program_profiles\":"<<count<<",\"reopened_programs\":"<<loaded.loaded-populated.loaded<<",\"parallel_hits\":8,\"packed_duplicates_skipped\":"<<skipped<<",\"new_loose_records_loaded\":"<<loose<<",\"corrupt_pack_loose_reads\":"<<fallback_reads<<",\"corrupt_records_rejected\":"<<rejected.rejected-warm.rejected<<",\"native_recovery_compiles\":"<<recovered.misses-rejected.misses<<",\"failures\":"<<failures<<",\"native_ms\":"<<native_ms<<",\"hit_ms\":"<<hit_ms<<",\"scope\":\"Exact native DXBC and texture/sampler/used-mask equality on fresh shader objects, nine translator profiles including scale, binding model, ROV, gamma, MSAA and source maps; disk reopen, concurrent lookups, packed duplicate skipping, new loose variants, corrupt-pack fallback and damaged-cache native recovery. CPU translation timing only; no gameplay FPS or full offline variant coverage claim.\"}\n";
    REXLOG_INFO("PC shader library: {} profiles, {} reopened, {} corrupt records rejected, {} failures; native {:.3f}ms, warm {:.3f}ms (translation fixture only)",count,loaded.loaded-populated.loaded,rejected.rejected-warm.rejected,failures,native_ms,hit_ms);return !failures && report.good();
}
}
