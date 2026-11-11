#include "pc_shader_library.h"
#include "gpu_constants_internal.h"
#include "pso_binary_cache.h"
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/pipeline_cache.h>
#include <rex/graphics/d3d12/render_target_cache.h>
#include <rex/graphics/trace_writer.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <chrono>
#include <fstream>
#include <map>
#include <vector>
#include <algorithm>
#include <optional>

REXCVAR_DECLARE(bool,em2_pretranslated_shaders);
REXCVAR_DECLARE(bool,em2_persistent_pso_cache);
REXCVAR_DEFINE_BOOL(em2_test_recorded_pso_cache,false,"Debug","With recorded-cache diagnostic, compare persistent driver blobs against the real native pipeline workload");
namespace {
using CP=rex::graphics::d3d12::D3D12CommandProcessor;
using Base=rex::graphics::CommandProcessor;
using PC=rex::graphics::d3d12::PipelineCache;
template<class Tag,auto Member>struct Access{friend auto Field(Tag){return Member;}};
struct RegsTag{friend auto Field(RegsTag);};template struct Access<RegsTag,&Base::register_file_>;
struct MemoryTag{friend auto Field(MemoryTag);};template struct Access<MemoryTag,&Base::memory_>;
struct BindlessTag{friend auto Field(BindlessTag);};template struct Access<BindlessTag,&CP::bindless_resources_used_>;
struct ShadersTag{friend auto Field(ShadersTag);};template struct Access<ShadersTag,&PC::shaders_>;
struct PipelinesTag{friend auto Field(PipelinesTag);};template struct Access<PipelinesTag,&PC::pipelines_>;
struct Program {
    bool valid=false,translated=false;
    std::vector<uint8_t> binary;
    std::vector<uint32_t> metadata;
    bool operator==(const Program&)const=default;
};
struct Snapshot {
    std::map<std::pair<uint64_t,uint64_t>,Program> programs;
    size_t shaders=0,ready_pipelines=0,failed_pipelines=0;
    double milliseconds=0;
};
Snapshot Capture(const PC& cache){
    Snapshot snapshot;snapshot.shaders=(cache.*Field(ShadersTag{})).size();
    for(const auto& [hash,shader]:cache.*Field(ShadersTag{})){
        for(const auto& [modification,translation]:shader->translations()){
            Program program;program.valid=translation->is_valid();program.translated=translation->is_translated();program.binary=translation->translated_binary();
            program.metadata.push_back(shader->GetUsedTextureMaskAfterTranslation());
            const auto& textures=shader->GetTextureBindingsAfterTranslation();program.metadata.push_back(uint32_t(textures.size()));
            for(const auto& t:textures){program.metadata.insert(program.metadata.end(),{t.bindless_descriptor_index,t.fetch_constant,uint32_t(t.dimension),uint32_t(t.is_signed)});}
            const auto& samplers=shader->GetSamplerBindingsAfterTranslation();program.metadata.push_back(uint32_t(samplers.size()));
            for(const auto& s:samplers){program.metadata.insert(program.metadata.end(),{s.bindless_descriptor_index,s.fetch_constant,uint32_t(s.mag_filter),uint32_t(s.min_filter),uint32_t(s.mip_filter),uint32_t(s.aniso_filter)});}
            for(unsigned i=0;i<256;++i)program.metadata.push_back(shader->constant_register_map().GetPackedFloatConstantIndex(i));
            snapshot.programs.emplace(std::pair{hash,modification},std::move(program));
        }
    }
    for(const auto& [hash,pipeline]:cache.*Field(PipelinesTag{})){if(pipeline->state)++snapshot.ready_pipelines;else ++snapshot.failed_pipelines;}
    return snapshot;
}
}
bool em2::TestRecordedPcShaderLibrary(CP& cp,const std::filesystem::path& source,const std::filesystem::path& seed,const std::filesystem::path& output,unsigned scale){
    namespace g=rex::graphics;namespace d=g::d3d12;
    auto* host=dynamic_cast<em2::Em2CommandProcessor*>(&cp);
    if(scale<1 || scale>3 || !host || !host->IsGraphicsWorker() || !std::filesystem::is_directory(source) || !host->DrainForResourceChange())return false;
    const bool previous=REXCVAR_GET(em2_pretranslated_shaders);struct Restore{bool previous;~Restore(){UsePcShaderLibrary({});REXCVAR_SET(em2_pretranslated_shaders,previous);}}restore{previous};REXCVAR_SET(em2_pretranslated_shaders,true);
    std::vector<std::filesystem::path> files;
    for(const auto& file:std::filesystem::recursive_directory_iterator(source))if(file.path().filename()==L"42560816.xsh" && std::filesystem::exists(file.path().parent_path()/"42560816.rtv.d3d12.xpso"))files.push_back(file.path());
    std::sort(files.begin(),files.end());if(files.empty())return false;
    std::filesystem::create_directories(output);auto library=OpenPcShaderLibrary(output/"learned",seed);
    auto logger=rex::GetLogger(rex::LogCategory::GPU);if(!logger)return false;
    const auto original_level=logger->level();struct LogRestore{std::shared_ptr<spdlog::logger> logger;spdlog::level::level_enum level;~LogRestore(){logger->set_level(level);}}log_restore{logger,original_level};
    uint64_t total_programs=0,total_shaders=0,ready=0,failed=0,hits=0,quiet_hits=0,misses=0,mismatches=0,pso_hits=0,pso_accepted=0,pso_rejected=0;double cold_ms=0,warm_ms=0,quiet_ms=0,pso_fill_ms=0,pso_warm_ms=0;
    auto run=[&](const std::filesystem::path& original,const std::filesystem::path& root)->std::optional<Snapshot>{
        const auto directory=root/"shaders"/"shareable";std::filesystem::create_directories(directory);
        std::filesystem::copy_file(original,directory/"42560816.xsh",std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy_file(original.parent_path()/"42560816.rtv.d3d12.xpso",directory/"42560816.rtv.d3d12.xpso",std::filesystem::copy_options::overwrite_existing);
        auto regs=std::make_unique<g::RegisterFile>(*(cp.*Field(RegsTag{})));auto* memory=cp.*Field(MemoryTag{});g::TraceWriter trace(memory->physical_membase());
        d::D3D12RenderTargetCache targets(*regs,*memory,trace,scale,scale,cp,cp.*Field(BindlessTag{}));if(!targets.Initialize())return {};
        PC cache(cp,*regs,targets,cp.*Field(BindlessTag{}));if(!cache.Initialize())return {};
        const auto start=std::chrono::steady_clock::now();cache.InitializeShaderStorage(root,0x42560816,true);cache.EndSubmission();
        auto snapshot=Capture(cache);snapshot.milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();return snapshot;
    };
    for(size_t i=0;i<files.size();++i){
        logger->set_level(spdlog::level::warn);UsePcShaderLibrary({});auto cold=run(files[i],output/("case-"+std::to_string(i))/"cold");if(!cold)return false;
        logger->set_level(spdlog::level::info);UsePcShaderLibrary(library);const auto before=GetPcShaderLibraryStats();auto prepared=run(files[i],output/("case-"+std::to_string(i))/"prepared");const auto after=GetPcShaderLibraryStats();if(!prepared)return false;
        logger->set_level(spdlog::level::warn);auto quiet=run(files[i],output/("case-"+std::to_string(i))/"quiet");const auto quiet_after=GetPcShaderLibraryStats();if(!quiet)return false;
        const auto count=prepared->programs.size();hits+=after.hits-before.hits;misses+=after.misses-before.misses;
        quiet_hits+=quiet_after.hits-after.hits;misses+=quiet_after.misses-after.misses;
        if(cold->programs!=prepared->programs || cold->shaders!=prepared->shaders || cold->ready_pipelines!=prepared->ready_pipelines || cold->failed_pipelines!=prepared->failed_pipelines)++mismatches;
        if(count && after.hits==before.hits)++mismatches;
        if(quiet->programs!=prepared->programs || quiet->shaders!=prepared->shaders || quiet->ready_pipelines!=prepared->ready_pipelines || quiet->failed_pipelines!=prepared->failed_pipelines || quiet_after.hits-after.hits!=after.hits-before.hits)++mismatches;
        total_programs+=count;total_shaders+=prepared->shaders;ready+=prepared->ready_pipelines;failed+=prepared->failed_pipelines;cold_ms+=cold->milliseconds;warm_ms+=prepared->milliseconds;quiet_ms+=quiet->milliseconds;
        if(REXCVAR_GET(em2_test_recorded_pso_cache)){
            const bool old_pso=REXCVAR_GET(em2_persistent_pso_cache);REXCVAR_SET(em2_persistent_pso_cache,true);std::shared_ptr<PsoBinaryStore> store;
            struct PsoRestore{std::shared_ptr<PsoBinaryStore>& store;bool previous;~PsoRestore(){StopPsoBinaryStore(store);REXCVAR_SET(em2_persistent_pso_cache,previous);}}pso_restore{store,old_pso};
            const auto directory=output/("case-"+std::to_string(i));store=StartPsoBinaryStore(directory/"compiled");if(!store)return false;
            auto fill=run(files[i],directory/"compiled-fill");StopPsoBinaryStore(store);if(!fill)return false;
            store=StartPsoBinaryStore(directory/"compiled");if(!store)return false;const auto pso_before=ReadPsoBinaryStats();auto compiled=run(files[i],directory/"compiled-warm");const auto pso_after=ReadPsoBinaryStats();StopPsoBinaryStore(store);if(!compiled)return false;
            if(fill->programs!=quiet->programs || compiled->programs!=quiet->programs || fill->ready_pipelines!=quiet->ready_pipelines || compiled->ready_pipelines!=quiet->ready_pipelines || fill->failed_pipelines!=quiet->failed_pipelines || compiled->failed_pipelines!=quiet->failed_pipelines)++mismatches;
            pso_hits+=pso_after.hits-pso_before.hits;pso_accepted+=pso_after.accepted-pso_before.accepted;pso_rejected+=pso_after.rejected-pso_before.rejected;pso_fill_ms+=fill->milliseconds;pso_warm_ms+=compiled->milliseconds;
        }
        REXLOG_INFO("Recorded native shader cache {}: {} shaders, {} translations, {} prepared hits, {} native misses, {} ready/{} failed historical pipelines",i,prepared->shaders,count,after.hits-before.hits,after.misses-before.misses,prepared->ready_pipelines,prepared->failed_pipelines);
    }
    FlushPcShaderLibrary(library);const auto removed=cp.GetD3D12Provider().GetDevice()->GetDeviceRemovedReason();
    std::ofstream report(output/"recorded-native-cache-results.json");report<<"{\"scale\":"<<scale<<",\"cache_files\":"<<files.size()<<",\"shaders\":"<<total_shaders<<",\"translations\":"<<total_programs<<",\"prepared_hits\":"<<hits<<",\"quiet_prepared_hits\":"<<quiet_hits<<",\"native_misses\":"<<misses<<",\"ready_pipelines\":"<<ready<<",\"failed_historical_pipelines\":"<<failed<<",\"mismatches\":"<<mismatches<<",\"cold_preload_ms\":"<<cold_ms<<",\"prepared_preload_ms\":"<<warm_ms<<",\"quiet_preload_ms\":"<<quiet_ms<<",\"pso_fill_ms\":"<<pso_fill_ms<<",\"pso_warm_ms\":"<<pso_warm_ms<<",\"pso_hits\":"<<pso_hits<<",\"pso_accepted\":"<<pso_accepted<<",\"pso_rejected\":"<<pso_rejected<<",\"device_removed\":"<<FAILED(removed)<<",\"scope\":\"Actual native PipelineCache shader/pipeline storage preload from isolated copies of recorded game caches. Every translated binary, validity flag, texture/sampler/used-mask/constant-register metadata and native pipeline success/failure count compared to uncached native run; verbose/quiet prepared runs also identical. No source cache writes, draw or gameplay. Wall times include driver/disk warmup and are order-dependent, not an FPS benchmark.\"}\n";
    return !mismatches && !misses && hits && quiet_hits==hits && (!REXCVAR_GET(em2_test_recorded_pso_cache) || (pso_hits && pso_hits==pso_accepted && !pso_rejected)) && !FAILED(removed) && report.good();
}
