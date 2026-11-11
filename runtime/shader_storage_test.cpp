#include "screen_upscale_test.h"
#include "pc_settings.h"
#include "scene_indirect_benchmark.h"
#include "built_shaders.h"
#include "shader_storage.h"
#include "pc_shader_library.h"
#include <xxhash.h>
#include <map>
#include <set>
#include "gpu_execution_profile.h"
#include "gpu_constants_internal.h"
#include "gpu_sync.h"
#include "renderer_graph.h"
#include "scaled_resolve_migration.h"
#include "edram_capture.h"
#include "resolve_write_observer.h"
#include "hdr_presenter.h"
#include "presenter_ui_exchange.h"
#include "resolved_storage_capture.h"
#include "heap_budget.h"
#include "shadow_kernel.h"
#include "shadow_pixel_test.h"
#include "scene_shader_audit.h"
#include "scene_raster.h"
#include "scene_replay.h"
#include "bloom_pixel_test.h"
#include "bloom_composite_test.h"
#include "bloom_kernel.h"
#include "hdr_output.h"
#include "hdr_shader_export.h"
#include "hdr_scene_cache.h"
#include "../private/bloom_shader_test_data.h"
#include "../private/shadow_shader_test_data.h"
#include <rex/cvar.h>
REXCVAR_DEFINE_INT32(em2_pc_shader_prepare_scale,1,"Debug","Render scale for archive PC shader preparation (1 through 3)");
#include <rex/logging.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/pipeline_cache.h>
#include <rex/graphics/format/ucode.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/kernel/xmemory.h>
#include <array>
#include <bit>
#include <future>
#include <fstream>
#include <chrono>
#include <atomic>
#include <thread>
#include <span>
#include <spdlog/sinks/base_sink.h>

namespace {
// Test-only read access, using the same explicit-instantiation access exemption
// as the constant-buffer fixture. Observe queues under their native mutex.
template<class Tag,auto Member>struct StorageProbe {friend auto GetStorageMember(Tag){return Member;}};
#define EM2_STORAGE_PROBE(member) struct member##_tag{friend auto GetStorageMember(member##_tag);};template struct StorageProbe<member##_tag,&rex::graphics::d3d12::PipelineCache::member>;
EM2_STORAGE_PROBE(storage_write_request_lock_)
EM2_STORAGE_PROBE(storage_write_shader_queue_)
EM2_STORAGE_PROBE(storage_write_pipeline_queue_)
#undef EM2_STORAGE_PROBE
// Synthetic shaders: ALLOC one export, EXEC_END one ALU, MAX c0,c0 to export.
// This is test code, not game bytecode or a replacement rendering effect.
constexpr std::array<uint32_t,6> vertex={0,0x1001C200,0x20000000,0x000F803E,0,0x02000000};
constexpr std::array<uint32_t,6> pixel ={0,0x1001C400,0x20000000,0x000F8000,0,0x02000000};
std::atomic<unsigned> shader_preloads{0},pipeline_preloads{0},verified_preloads{0};
class PreloadProbe final : public spdlog::sinks::base_sink<std::mutex> {
    void sink_it_(const spdlog::details::log_msg& message) override {
        const std::string_view text(message.payload.data(),message.payload.size());
        if(text.starts_with("Translated 2 shaders from the storage in "))shader_preloads.fetch_add(1,std::memory_order_relaxed);
        if(text.starts_with("Created 1 graphics pipelines (not including reading the descriptions) from the storage in "))pipeline_preloads.fetch_add(1,std::memory_order_relaxed);
    }
    void flush_() override {}
};
auto BigEndianProgram(const std::array<uint32_t,6>& program) {
    auto result=program;for(auto& word:result)word=std::byteswap(word);return result;
}
bool OnGpuThread(rex::graphics::GraphicsSystem& graphics,std::function<bool()> operation) {
    auto done=std::make_shared<std::promise<bool>>();auto result=done->get_future();
    graphics.command_processor()->CallInThread([done,operation=std::move(operation)] {
        try{done->set_value(operation());}
        catch(const std::exception& error){REXLOG_ERROR("Renderer storage GPU diagnostic exception: {}",error.what());done->set_value(false);}
    });
    // Full native cache capture/restore cases compile and execute many SDK PSOs.
    if(result.wait_for(std::chrono::seconds(90))!=std::future_status::ready){
        // This is an observation deadline, not cancellation. The queued job
        // borrows the graphics system; do not let teardown race its completion.
        REXLOG_WARN("Renderer storage GPU diagnostic still running after90s; retaining resources and waiting for worker completion");
        result.wait();
    }
    return result.get();
}
}
bool em2::TestRecordedPcShaderCaches(rex::graphics::GraphicsSystem& graphics,const std::filesystem::path& source,const std::filesystem::path& seed,const std::filesystem::path& output){
    return OnGpuThread(graphics,[&graphics,source,seed,output]{return TestRecordedPcShaderLibrary(*static_cast<rex::graphics::d3d12::D3D12CommandProcessor*>(graphics.command_processor()),source,seed,output,REXCVAR_GET(em2_pc_shader_prepare_scale));});
}
bool em2::TestTargetedScreenUpscale(rex::graphics::GraphicsSystem& graphics,const std::filesystem::path& directory){
    const auto* provider=dynamic_cast<const rex::ui::d3d12::D3D12Provider*>(graphics.provider());
    if(!provider)return false;
    std::atomic<bool> lost{false};auto presenter=rex::ui::d3d12::D3D12Presenter::Create([&](bool,bool){lost=true;},*provider);
    if(!presenter)return false;
    const auto old_mode=PCSetting(kPCScreenUpscaler),old_sharpness=PCSetting(kPCUpscaleSharpness);
    ScreenUpscaleCases configs;
    for(int mode=0;mode<3;++mode)for(int sharpness=0;sharpness<3;++sharpness){
        ChangePCSetting(kPCScreenUpscaler,mode);ChangePCSetting(kPCUpscaleSharpness,sharpness);
        ApplyPCPresentationSettings(*presenter);configs[mode][sharpness]=presenter->GetGuestOutputPaintConfigFromUIThread();
    }
    ChangePCSetting(kPCScreenUpscaler,old_mode);ChangePCSetting(kPCUpscaleSharpness,old_sharpness);ApplyPCPresentationSettings(*presenter);
    const bool success=OnGpuThread(graphics,[&]{return TestScreenUpscalePixels(*presenter,*provider,configs,directory);});
    if(!success || lost.load())return false;
    auto hdr=em2::CreateHdrPresenter([&](bool,bool){lost=true;},*provider);if(!hdr)return false;
    const bool hdr_success=OnGpuThread(graphics,[&]{return TestScreenUpscalePixels(*hdr,*provider,configs,directory/"hdr-upscale",true);});
    const bool presenter_success=hdr_success && OnGpuThread(graphics,[&]{return TestHdrPresenter(*const_cast<rex::ui::d3d12::D3D12Provider*>(provider),directory/"hdr-presenter");});
    return presenter_success && !lost.load();
}
bool em2::TestTargetedSceneLighting(rex::graphics::GraphicsSystem& graphics,const std::filesystem::path& directory) {
    return OnGpuThread(graphics,[&graphics,directory]{
        auto& cp=*static_cast<em2::Em2CommandProcessor*>(graphics.command_processor());
        return em2::TestWorldSurfaceCapture(cp.GetD3D12Provider(),directory/"linear-surface-only",false,false,true) && em2::TestWorldSurfaceCapture(cp.GetD3D12Provider(),directory/"linear-surface",true,false,true) && em2::TestHdrSceneExport(cp,directory/"hdr-scene-export") && em2::TestSceneIndirectLighting(cp,directory/"indirect-lighting") && em2::TestSceneAmbientComposition(cp,directory/"ambient-composition") && em2::TestNativeMaterialCapture(cp,directory/"native-material-hook") && em2::TestNativeFullMaterialLighting(cp.GetD3D12Provider(),directory/"full-material-lighting") && em2::TestNativeMaterialLighting(cp.GetD3D12Provider(),directory/"material-lighting") && em2::AuditNativeMaterialShaders(cp.GetD3D12Provider(),directory/"material-inputs") && em2::AuditNativeSceneShaders(cp.GetD3D12Provider(),directory/"scene-inputs") && em2::TestNativeFogVertex(cp.GetD3D12Provider(),directory/"fog-vertex") && em2::TestSceneUnprojection(cp.GetD3D12Provider(),directory/"scene-unproject");
    });
}
namespace {
template<class Tag,class Type>struct StorageTypeProbe{friend auto StorageType(Tag){return std::type_identity<Type>{};}};
struct ShaderHeaderTag{friend auto StorageType(ShaderHeaderTag);};template struct StorageTypeProbe<ShaderHeaderTag,rex::graphics::d3d12::PipelineCache::ShaderStoredHeader>;
struct PipelineRecordTag{friend auto StorageType(PipelineRecordTag);};template struct StorageTypeProbe<PipelineRecordTag,rex::graphics::d3d12::PipelineCache::PipelineStoredDescription>;
struct ObservedPreparation {uint32_t programs=0,variants=0,failures=0,rejected_files=0;};
struct PreparationTargetsTag{friend auto GetStorageMember(PreparationTargetsTag);};template struct StorageProbe<PreparationTargetsTag,&rex::graphics::d3d12::D3D12CommandProcessor::render_target_cache_>;
struct PreparationBindlessTag{friend auto GetStorageMember(PreparationBindlessTag);};template struct StorageProbe<PreparationBindlessTag,&rex::graphics::d3d12::D3D12CommandProcessor::bindless_resources_used_>;
struct PreparationProfile {rex::ui::GraphicsProvider::GpuVendorID vendor;bool bindless,rov,gamma,msaa,source_map;unsigned scale;};
ObservedPreparation PrepareObservedShaders(const std::filesystem::path& root,const PreparationProfile& profile,const std::shared_ptr<em2::PcShaderLibrary>& library){
    namespace g=rex::graphics;using Header=decltype(StorageType(ShaderHeaderTag{}))::type;using Stored=decltype(StorageType(PipelineRecordTag{}))::type;
    static_assert(sizeof(Header)==12 && sizeof(Stored)==72);
    struct Program{g::xenos::ShaderType stage;std::vector<uint32_t> code;};
    std::map<uint64_t,Program> programs;std::set<std::pair<uint64_t,uint64_t>> variants;ObservedPreparation result;
    if(!std::filesystem::is_directory(root))return result;
    // Read only the SDK-compatible cache namespace. Validate native versions,
    // extents and both stored hashes before feeding any code to the translator.
    for(const auto& entry:std::filesystem::recursive_directory_iterator(root)){
        if(entry.path().extension()!=L".xsh")continue;
        std::ifstream file(entry.path(),std::ios::binary|std::ios::ate);if(!file || file.tellg()>16*1024*1024){++result.rejected_files;continue;}file.seekg(0);
        uint32_t header[2];if(!file.read(reinterpret_cast<char*>(header),sizeof(header)) || header[0]!=0x48534558 || header[1]!=std::byteswap(Header::kVersion)){++result.rejected_files;continue;}
        for(unsigned count=0;file.peek()!=std::char_traits<char>::eof();++count){Header record;if(count>=8192 || !file.read(reinterpret_cast<char*>(&record),sizeof(record)) || !record.ucode_dword_count || record.ucode_dword_count>262144 || record.ucode_dword_count%3){++result.rejected_files;break;}
            Program program{record.type,std::vector<uint32_t>(record.ucode_dword_count)};const auto bytes=program.code.size()*4;
            if(!file.read(reinterpret_cast<char*>(program.code.data()),bytes) || XXH3_64bits(program.code.data(),bytes)!=record.ucode_data_hash){++result.rejected_files;break;}
            if(programs.size()>=8192){++result.rejected_files;break;}
            auto [found,added]=programs.try_emplace(record.ucode_data_hash,std::move(program));if(!added && (found->second.stage!=record.type || found->second.code!=program.code)){++result.rejected_files;break;}
        }
    }
    for(const auto& entry:std::filesystem::recursive_directory_iterator(root)){
        if(!entry.path().filename().wstring().ends_with(L".rtv.d3d12.xpso"))continue;
        std::ifstream file(entry.path(),std::ios::binary|std::ios::ate);if(!file || file.tellg()>16*1024*1024){++result.rejected_files;continue;}file.seekg(0);
        uint32_t header[3];const auto version=std::max(decltype(Stored{}.description)::kVersion,g::DxbcShaderTranslator::Modification::kVersion);
        if(!file.read(reinterpret_cast<char*>(header),sizeof(header)) || header[0]!=0x53504558 || header[1]!=0x54525844 || header[2]!=std::byteswap(version)){++result.rejected_files;continue;}
        for(unsigned count=0;file.peek()!=std::char_traits<char>::eof();++count){Stored record;
            if(count>=65536 || !file.read(reinterpret_cast<char*>(&record),sizeof(record)) || XXH3_64bits(&record.description,sizeof(record.description))!=record.description_hash){++result.rejected_files;break;}
            const auto& d=record.description;variants.emplace(d.vertex_shader_hash,d.vertex_shader_modification);if(d.pixel_shader_hash)variants.emplace(d.pixel_shader_hash,d.pixel_shader_modification);
        }
    }
    result.programs=uint32_t(programs.size());g::DxbcShaderTranslator translator(profile.vendor,profile.bindless,profile.rov,profile.gamma,profile.msaa,profile.scale,profile.scale,profile.source_map);unsigned pending=0;
    std::map<uint64_t,std::unique_ptr<g::DxbcShader>> analyzed;
    for(const auto& [hash,p]:programs){auto shader=std::make_unique<g::DxbcShader>(p.stage,hash,p.code.data(),p.code.size());rex::string::StringBuffer text;shader->AnalyzeUcode(text);
        const auto registers=shader->GetDynamicAddressableRegisterCount(63);variants.emplace(hash,p.stage==g::xenos::ShaderType::kVertex?translator.GetDefaultVertexShaderModification(registers):translator.GetDefaultPixelShaderModification(registers));analyzed.emplace(hash,std::move(shader));}
    for(const auto& [hash,modification]:variants){auto found=analyzed.find(hash);if(found==analyzed.end())continue;auto* translated=found->second->GetOrCreateTranslation(modification);
        ++result.variants;if(!translator.TranslateAnalyzedShader(*translated) || !translated->is_valid())++result.failures;if(++pending%64==0)em2::FlushPcShaderLibrary(library);
    }
    return result;
}
}
bool em2::PrepareArchivePcShaders(rex::graphics::GraphicsSystem& graphics,const std::filesystem::path& catalog,const std::filesystem::path& output,const std::filesystem::path& observed_cache){
    return OnGpuThread(graphics,[&graphics,catalog,output,observed_cache]{
        namespace g=rex::graphics;auto& cp=*static_cast<g::d3d12::D3D12CommandProcessor*>(graphics.command_processor());
        const auto scale=REXCVAR_GET(em2_pc_shader_prepare_scale);if(scale<1 || scale>3)return false;
        const auto* targets=(cp.*GetStorageMember(PreparationTargetsTag{})).get();if(!targets || targets->GetPath()!=g::RenderTargetCache::Path::kHostRenderTargets)return false;
        const PreparationProfile profile{cp.GetD3D12Provider().GetAdapterVendorID(),cp.*GetStorageMember(PreparationBindlessTag{}),false,targets->gamma_render_target_as_srgb(),targets->msaa_2x_supported(),cp.GetD3D12Provider().GetGraphicsAnalysis()!=nullptr,unsigned(scale)};
        const uint32_t native_flags=uint32_t(profile.bindless)|(uint32_t(profile.rov)<<1)|(uint32_t(profile.gamma)<<2)|(uint32_t(profile.msaa)<<3)|(uint32_t(profile.source_map)<<4);
        REXLOG_INFO("Archive PC shader native profile: vendor {}, flags {}, scale {}",uint32_t(profile.vendor),native_flags,scale);
        std::ifstream file(catalog,std::ios::binary|std::ios::ate);if(!file || file.tellg()>16*1024*1024)return false;file.seekg(0);
        char magic[8];if(!file.read(magic,8) || std::memcmp(magic,"EM2UCOD1",8))return false;
        auto word=[&](){unsigned char bytes[4];if(!file.read(reinterpret_cast<char*>(bytes),4))throw std::runtime_error("truncated PC shader catalog");return uint32_t(bytes[0])|(uint32_t(bytes[1])<<8)|(uint32_t(bytes[2])<<16)|(uint32_t(bytes[3])<<24);};
        const auto pairs=word();if(!pairs || pairs>4096)return false;
        auto library=OpenPcShaderLibrary(output);PcShaderLibraryScope scope(library);const auto before=GetPcShaderLibraryStats();
        const auto started=std::chrono::steady_clock::now();uint32_t programs=0,variants=0,failures=0;
        for(uint32_t pair=0;pair<pairs;++pair){
            std::array<std::unique_ptr<g::DxbcShader>,2> shaders;
            for(unsigned stage=0;stage<2;++stage){auto size=word();if(!size || size%12 || size>1024*1024)return false;std::vector<uint32_t> code(size/4);if(!file.read(reinterpret_cast<char*>(code.data()),size))return false;
                shaders[stage]=std::make_unique<g::DxbcShader>(stage?g::xenos::ShaderType::kPixel:g::xenos::ShaderType::kVertex,XXH3_64bits(code.data(),size),code.data(),code.size());rex::string::StringBuffer text;shaders[stage]->AnalyzeUcode(text);
            }
            g::reg::SQ_PROGRAM_CNTL program{};program.vs_export_count=15;g::reg::SQ_CONTEXT_MISC misc{};uint32_t param=0;
            const auto linked=shaders[0]->writes_interpolators() & shaders[1]->GetInterpolatorInputMask(program,misc,param);
            for(unsigned stage=0;stage<2;++stage){auto& shader=*shaders[stage];
                // Native default and effect-linked stage signatures. Other live
                // clip/depth/param-gen/dynamic-register variants remain exact-key
                // cache misses, never substitutions with a different signature.
                g::DxbcShaderTranslator translator(profile.vendor,profile.bindless,profile.rov,profile.gamma,profile.msaa,scale,scale,profile.source_map);
                auto modification=stage?translator.GetDefaultPixelShaderModification(shader.GetDynamicAddressableRegisterCount(63)):translator.GetDefaultVertexShaderModification(shader.GetDynamicAddressableRegisterCount(63));
                for(unsigned profile=0;profile<2;++profile){g::DxbcShaderTranslator::Modification variant(modification);if(profile){if(stage)variant.pixel.interpolator_mask=linked;else variant.vertex.interpolator_mask=linked;}
                    auto* translation=shader.GetOrCreateTranslation(variant.value);if(translation->is_translated())continue;++variants;if(!translator.TranslateAnalyzedShader(*translation) || !translation->is_valid())++failures;
                }++programs;
            }
            // Keep disk persistence bounded even when translation outruns I/O.
            if((pair&63)==63)FlushPcShaderLibrary(library);
            if((pair&255)==255)REXLOG_INFO("Archive PC shader preparation: {}/{} effect programs",pair+1,pairs);
        }
        if(file.peek()!=std::char_traits<char>::eof())return false;
        const auto observed=PrepareObservedShaders(observed_cache,profile,library);
        FlushPcShaderLibrary(library);if(!SavePcShaderLibraryPack(library,output))return false;const auto after=GetPcShaderLibraryStats();
        const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        std::ofstream report(output/"archive-preparation-results.json");report<<"{\"native_flags\":"<<native_flags<<",\"scale\":"<<scale<<",\"pairs\":"<<pairs<<",\"programs\":"<<programs<<",\"variants\":"<<variants<<",\"observed_variants\":"<<observed.variants<<",\"total_requests\":"<<(variants+observed.variants)<<",\"hits\":"<<after.hits-before.hits<<",\"native_compiles\":"<<after.misses-before.misses<<",\"saved\":"<<after.saved-before.saved<<",\"dropped\":"<<after.dropped-before.dropped<<",\"failures\":"<<failures<<",\"milliseconds\":"<<ms<<",\"scope\":\"All original archive programs, native default and effect-linked signatures, current GPU vendor, actual native host-RTV profile at selected scale profile. Prepared PC DXBC plus bindings consumed by normal startup. Other renderer profiles and dynamic native modifications remain exact-key fallback; no full pipeline-state or total runtime shader coverage claim.\"}\n";
        REXLOG_INFO("Archive PC shader preparation: {} programs, {} variants, {} failures, {} dropped, {:.1f}ms",programs,variants,failures,after.dropped-before.dropped,ms);
        std::ofstream observed_report(output/"observed-preparation-results.json");observed_report<<"{\"native_flags\":"<<native_flags<<",\"scale\":"<<scale<<",\"programs\":"<<observed.programs<<",\"variants\":"<<observed.variants<<",\"failures\":"<<observed.failures<<",\"rejected_cache_files\":"<<observed.rejected_files<<",\"scope\":\"Read-only native shader/pipeline cache import; version, extent and native hash validation. Actual observed code and shader modification values, translated for actual native host-RTV profile at selected scale/current GPU. Unobserved runtime patches and other profiles still need preparation.\"}\n";
        REXLOG_INFO("Observed PC shader preparation: {} programs, {} native modifications, {} failures, {} rejected cache files",observed.programs,observed.variants,observed.failures,observed.rejected_files);
        return !failures && !observed.failures && after.dropped==before.dropped && (after.saved-before.saved)+(after.hits-before.hits)==variants+observed.variants && report.good() && observed_report.good();
    });
}
void em2::ConfigureRendererShaderStorageTest() {
    // Process-local diagnostic policy only; never modifies saved preferences.
    REXCVAR_SET(d3d12_pipeline_creation_threads,0);
    // Installed before Runtime::Setup starts GPU threads. Counts only the exact
    // SDK preload-success events needed by this isolated synthetic fixture.
    shader_preloads=0;pipeline_preloads=0;verified_preloads=0;
    if(auto logger=rex::GetLogger(rex::LogCategory::GPU))logger->sinks().push_back(std::make_shared<PreloadProbe>());
}
bool em2::PopulateRendererShaderStorageTest(rex::graphics::GraphicsSystem& graphics,const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);std::ofstream presenter_progress(directory/"presenter-progress.txt");
    presenter_progress<<"source presenter create start"<<std::endl;
    {
        const auto* provider=dynamic_cast<const rex::ui::d3d12::D3D12Provider*>(graphics.provider());
        if(!provider)return false;
        std::atomic<bool> gpu_loss{false};
        auto presenter=rex::ui::d3d12::D3D12Presenter::Create(
            [&](bool,bool){gpu_loss.store(true,std::memory_order_relaxed);},*provider);
        if(!presenter)return false;
        presenter_progress<<"source presenter created"<<std::endl;
        presenter.reset();
        presenter_progress<<"source presenter destroyed"<<std::endl;
        if(gpu_loss.load(std::memory_order_relaxed))return false;
        REXLOG_INFO("Source-built presenter initialized and destroyed with the real D3D12 device: original shader/root-signature/pipeline initialization and fence cleanup; no window, swapchain or HDR output exercised");
    }
    const auto backend=graphics.name();
    if(backend.find("D3D12")==std::string::npos && backend.find("Direct3D 12")==std::string::npos){REXLOG_ERROR("Populated cache diagnostic requires D3D12, got {}",backend);return false;}
    if(!em2::TestNativeHeapBudget(graphics.memory()))return false;
    return OnGpuThread(graphics,[&graphics,directory] {
        auto fixture_library=OpenPcShaderLibrary(directory/"pc-translations");UsePcShaderLibrary(fixture_library);
        namespace g=rex::graphics;
        namespace d=g::d3d12;
        if(!em2::TestHdrOutputPass(static_cast<d::D3D12CommandProcessor*>(graphics.command_processor())->GetD3D12Provider(),directory/"hdr-output"))return false;
        if(!em2::TestHdrCompositionPass(static_cast<d::D3D12CommandProcessor*>(graphics.command_processor())->GetD3D12Provider(),directory/"hdr-composition"))return false;
        if(!em2::TestHdrPresenter(static_cast<d::D3D12CommandProcessor*>(graphics.command_processor())->GetD3D12Provider(),directory/"hdr-presenter"))return false;
        if(!em2::TestHdrNativeSwap(*static_cast<em2::Em2CommandProcessor*>(graphics.command_processor()),directory/"hdr-native-swap"))return false;
        if(!em2::TestHdrSceneExport(*static_cast<d::D3D12CommandProcessor*>(graphics.command_processor()),directory/"hdr-scene-export"))return false;
        if(!em2::TestSceneIndirectLighting(*static_cast<d::D3D12CommandProcessor*>(graphics.command_processor()),directory/"indirect-lighting"))return false;
        if(!em2::TestGpuExecutionTiming(*static_cast<d::D3D12CommandProcessor*>(graphics.command_processor())))return false;
        g::ucode::ControlFlowInstruction cf[2];g::ucode::UnpackControlFlowInstructions(vertex.data(),cf);
        const auto alu=std::bit_cast<g::ucode::AluInstruction>(std::array<uint32_t,3>{vertex[3],vertex[4],vertex[5]});
        if(cf[0].opcode()!=g::ucode::ControlFlowOpcode::kAlloc || cf[1].opcode()!=g::ucode::ControlFlowOpcode::kExecEnd ||
           cf[1].exec.address()!=1 || cf[1].exec.count()!=1 || !alu.is_export() || alu.vector_dest()!=62 || alu.vector_write_mask()!=15)return false;
        auto& cp=*static_cast<d::D3D12CommandProcessor*>(graphics.command_processor());
        if(!em2::TestGraphicsResourceSynchronization(graphics))return false;
        if(!em2::TestResolveWriteObserver(static_cast<em2::Em2CommandProcessor&>(cp)))return false;
        if(!em2::TestResolvedStorageCapture(static_cast<em2::Em2CommandProcessor&>(cp)))return false;
        if(!em2::TestResolvedStorageConversion(static_cast<em2::Em2CommandProcessor&>(cp)))return false;
        if(!em2::TestScaledResolveMigration(cp.GetD3D12Provider()))return false;
        if(!em2::TestEdramScaleMigration(cp.GetD3D12Provider()))return false;
        if(!em2::TestAuxiliaryDepthCapture(static_cast<em2::Em2CommandProcessor&>(cp)))return false;
        if(!em2::TestPackedEdramCapture(static_cast<em2::Em2CommandProcessor&>(cp)))return false;
        if(!em2::TestLiveShaderPolicy(*static_cast<em2::Em2CommandProcessor*>(graphics.command_processor()),*graphics.memory()))return false;
        if(!em2::TestNativeSceneRaster(cp))return false;
        if(!em2::TestNativeSceneReplay(cp))return false;
        if(!em2::TestBuiltShaders(directory/"built-shaders"))return false;
        if(!em2::TestPcShaderLibrary(cp.GetD3D12Provider(),directory/"pc-shader-library"))return false;
        if(!em2::TestRendererGraph(static_cast<em2::Em2CommandProcessor&>(cp),directory/"graph-source"))return false;
        if(!em2::TestPresenterUiExchange(static_cast<em2::Em2CommandProcessor&>(cp),directory/"presenter-ui-exchange"))return false;
        if(!em2::TestNativeShadowPixels(cp.GetD3D12Provider(),directory/"shadow-pixels"))return false;
        if(!em2::TestNativeMaterialCapture(cp,directory/"native-material-hook"))return false;
        if(!em2::TestNativeFullMaterialLighting(cp.GetD3D12Provider(),directory/"full-material-lighting"))return false;
        if(!em2::TestWorldSurfaceCapture(cp.GetD3D12Provider(),directory/"linear-surface-only",false,false,true))return false;
        if(!em2::TestWorldSurfaceCapture(cp.GetD3D12Provider(),directory/"linear-surface",true,false,true))return false;
        if(!em2::TestNativeMaterialLighting(cp.GetD3D12Provider(),directory/"material-lighting"))return false;
        if(!em2::AuditNativeMaterialShaders(cp.GetD3D12Provider(),directory/"material-inputs"))return false;
        if(!em2::AuditNativeSceneShaders(cp.GetD3D12Provider(),directory/"scene-inputs"))return false;
        if(!em2::TestNativeFogVertex(cp.GetD3D12Provider(),directory/"fog-vertex"))return false;
        if(!em2::TestSceneUnprojection(cp.GetD3D12Provider(),directory/"scene-unproject"))return false;
        if(!em2::TestNativeBloomPixels(cp.GetD3D12Provider(),directory/"bloom-pixels"))return false;
        if(!em2::TestNativeBloomCompositePixels(cp.GetD3D12Provider(),directory/"bloom-composite"))return false;
        const bool bindless=REXCVAR_GET(d3d12_bindless) && cp.GetD3D12Provider().GetResourceBindingTier()>=D3D12_RESOURCE_BINDING_TIER_2;
        auto regs=std::make_unique<g::RegisterFile>();std::fill(std::begin(regs->values),std::end(regs->values),0u);
        regs->values[g::reg::RB_MODECONTROL::register_index]=uint32_t(g::xenos::EdramMode::kColorDepth);
        g::reg::RB_SURFACE_INFO surface{};surface.surface_pitch=32;
        regs->values[g::reg::RB_SURFACE_INFO::register_index]=surface.value;
        g::reg::VGT_DRAW_INITIATOR draw{};draw.prim_type=g::xenos::PrimitiveType::kTriangleList;draw.num_indices=3;
        regs->values[g::reg::VGT_DRAW_INITIATOR::register_index]=draw.value;
        g::TraceWriter trace(graphics.memory()->physical_membase());
        d::D3D12RenderTargetCache targets(*regs,*graphics.memory(),trace,1,1,cp,bindless);
        if(!targets.Initialize())return false;
        auto hdr_registration=HdrSceneCacheRegistration::Create(targets,cp.GetD3D12Provider().GetDevice());
        if(!hdr_registration || HdrSceneCacheRegistration::Create(targets,cp.GetD3D12Provider().GetDevice()))return false;
        {
            const auto prior=NativeShadowSamples();
            struct RestoreShadow {unsigned quality;int32_t workers=REXCVAR_GET(d3d12_pipeline_creation_threads);~RestoreShadow(){ConfigureShadowKernel(quality);REXCVAR_SET(d3d12_pipeline_creation_threads,workers);}} restore{prior};
            REXCVAR_SET(d3d12_pipeline_creation_threads,2);
            // Separate transient caches: never mix the replacement with a
            // previous original-hash lookup or the persistence seed below.
            for(unsigned variant=0;variant<5;++variant) {
                ConfigureShadowKernel(variant==2?8:variant==3?16:variant==4?20:4);
                d::PipelineCache shadow_cache(cp,*regs,targets,bindless);
                if(!shadow_cache.Initialize())return false;
                std::vector<uint32_t> native_vs(shadow_test_data::vertex.begin(),shadow_test_data::vertex.end());
                const std::span<const uint32_t> input=variant==1?std::span<const uint32_t>(shadow_test_data::control):
                    std::span<const uint32_t>(shadow_test_data::original);
                std::vector<uint32_t> native_ps(input.begin(),input.end());
                for(auto& word:native_vs)word=std::byteswap(word);
                for(auto& word:native_ps)word=std::byteswap(word);
                auto* shadow_vs=shadow_cache.LoadShader(g::xenos::ShaderType::kVertex,native_vs.data(),native_vs.size());
                auto* shadow_ps=shadow_cache.LoadShader(g::xenos::ShaderType::kPixel,native_ps.data(),native_ps.size());
                if(!shadow_vs || !shadow_ps)return false;
                const std::span<const uint32_t> expected=variant==2?std::span<const uint32_t>(shadow_test_data::samples8):
                    variant==3?std::span<const uint32_t>(shadow_test_data::samples16):variant==4?std::span<const uint32_t>(shadow_test_data::adaptive16):input;
                std::vector<uint32_t> expected_big(expected.begin(),expected.end());
                for(auto& word:expected_big)word=std::byteswap(word);
                if(shadow_ps->ucode_data_hash()!=XXH3_64bits(expected_big.data(),expected_big.size()*4) ||
                    shadow_ps->ucode_dword_count()!=expected.size() ||
                    shadow_cache.LoadShader(g::xenos::ShaderType::kPixel,native_ps.data(),uint32_t(native_ps.size()))!=shadow_ps ||
                    shadow_cache.LoadShader(g::xenos::ShaderType::kPixel,expected_big.data(),uint32_t(expected_big.size()))!=shadow_ps)return false;
                shadow_cache.AnalyzeShaderUcode(*shadow_vs);shadow_cache.AnalyzeShaderUcode(*shadow_ps);
                const unsigned taps=variant<2?4:variant==2?8:variant==3?16:20;
                if(shadow_ps->texture_bindings().size()!=3+3*taps)return false;
                auto* sv=static_cast<d::D3D12Shader::D3D12Translation*>(shadow_vs->GetOrCreateTranslation(
                    shadow_cache.GetCurrentVertexShaderModification(*shadow_vs,g::Shader::HostVertexShaderType::kVertex,0).value));
                auto* sp=static_cast<d::D3D12Shader::D3D12Translation*>(shadow_ps->GetOrCreateTranslation(
                    shadow_cache.GetCurrentPixelShaderModification(*shadow_ps,0,0,g::reg::RB_DEPTHCONTROL{}).value));
                g::PrimitiveProcessor::ProcessingResult shape{};
                shape.guest_primitive_type=shape.host_primitive_type=g::xenos::PrimitiveType::kTriangleList;
                shape.host_vertex_shader_type=g::Shader::HostVertexShaderType::kVertex;shape.host_draw_vertex_count=3;
                const uint32_t formats[5]={};void* handle=nullptr;ID3D12RootSignature* signature=nullptr;
                const bool configured=shadow_cache.ConfigurePipeline(sv,sp,shape,g::reg::RB_DEPTHCONTROL{},15,2,formats,&handle,&signature);
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
                while(shadow_cache.IsCreatingPipelines() && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
                const bool accepted=configured && !shadow_cache.IsCreatingPipelines() && handle && signature && shadow_cache.GetD3D12PipelineByHandle(handle) && sv->is_valid() && sp->is_valid();
                shadow_cache.EndSubmission();
                REXLOG_INFO("Native shadow GPU pipeline variant {}: {} taps/map, bindless {}, device acceptance {} (no pixel output claim)",variant,taps,bindless,accepted);
                if(!accepted)return false;
            }
        }
        const auto hdr_stats=hdr_registration->stats();
        if(!hdr_stats.builds || hdr_stats.worker_builds!=hdr_stats.builds || CurrentHdrScenePipeline())return false;
        REXLOG_INFO("Native per-cache HDR PSO integration: {} augmented builds, {} on background threads, caller context restored; registered transient cache only",hdr_stats.builds,hdr_stats.worker_builds);
        std::ofstream hdr_report(directory/"hdr-native-cache.json");
        hdr_report<<"{\n\"builds\":"<<hdr_stats.builds<<",\n\"background_builds\":"<<hdr_stats.worker_builds
            <<",\n\"caller_context_restored\":true,\n\"scope\":\"native cache pipeline factory only; no live frame binding or HDR presentation\"\n}\n";
        if(!hdr_report.good())return false;
        {
            const auto prior=NativeBloomSampling();
            struct RestoreBloom {unsigned quality;~RestoreBloom(){ConfigureBloomSampling(quality);}}restore{prior};
            d::PipelineCache bloom_cache(cp,*regs,targets,bindless);if(!bloom_cache.Initialize())return false;
            const std::array<std::span<const uint32_t>,6> inputs={bloom_test_data::horizontal_original,bloom_test_data::horizontal_linear,
                bloom_test_data::vertical_original,bloom_test_data::vertical_linear,bloom_test_data::composite,bloom_test_data::composite_linear};
            for(unsigned quality=0;quality<3;++quality)for(unsigned variant=0;variant<inputs.size();++variant) {
                ConfigureBloomSampling(quality);
                const auto wanted=variant>=4?inputs[4+(quality==2)]:quality==0?DisabledBloomShader():inputs[(variant&2)+(quality==2)];
                std::vector<uint32_t> source(inputs[variant].begin(),inputs[variant].end()),expected(wanted.begin(),wanted.end());
                for(auto& word:source)word=std::byteswap(word);for(auto& word:expected)word=std::byteswap(word);
                auto* shader=bloom_cache.LoadShader(g::xenos::ShaderType::kPixel,source.data(),source.size());
                if(!shader || shader->ucode_data_hash()!=XXH3_64bits(expected.data(),expected.size()*4) ||
                    bloom_cache.LoadShader(g::xenos::ShaderType::kPixel,source.data(),source.size())!=shader ||
                    bloom_cache.LoadShader(g::xenos::ShaderType::kPixel,expected.data(),expected.size())!=shader)return false;
            }
            bloom_cache.EndSubmission();
            REXLOG_INFO("Bloom production LoadShader bridge: all18 Gaussian/composite mode/source combinations hash and reuse transformed code correctly");
        }
        d::PipelineCache cache(cp,*regs,targets,bindless);
        if(!cache.Initialize())return false;
        const auto root=RendererShaderStorageRoot(directory);std::filesystem::create_directories(root);
        cache.InitializeShaderStorage(root,0x42560816,true);
        const auto vs_code=BigEndianProgram(vertex),ps_code=BigEndianProgram(pixel);
        auto* vs=cache.LoadShader(g::xenos::ShaderType::kVertex,vs_code.data(),vs_code.size());
        auto* ps=cache.LoadShader(g::xenos::ShaderType::kPixel,ps_code.data(),ps_code.size());
        if(!vs || !ps)return false;
        cache.AnalyzeShaderUcode(*vs);cache.AnalyzeShaderUcode(*ps);
        const auto vs_mod=cache.GetCurrentVertexShaderModification(*vs,g::Shader::HostVertexShaderType::kVertex,0);
        const auto ps_mod=cache.GetCurrentPixelShaderModification(*ps,0,0,g::reg::RB_DEPTHCONTROL{});
        auto* vs_translation=static_cast<d::D3D12Shader::D3D12Translation*>(vs->GetOrCreateTranslation(vs_mod.value));
        auto* ps_translation=static_cast<d::D3D12Shader::D3D12Translation*>(ps->GetOrCreateTranslation(ps_mod.value));
        g::PrimitiveProcessor::ProcessingResult primitive{};
        primitive.guest_primitive_type=primitive.host_primitive_type=g::xenos::PrimitiveType::kTriangleList;
        primitive.host_vertex_shader_type=g::Shader::HostVertexShaderType::kVertex;primitive.host_draw_vertex_count=3;
        const uint32_t formats[5]={};void* handle=nullptr;ID3D12RootSignature* signature=nullptr;
        const bool configured=cache.ConfigurePipeline(vs_translation,ps_translation,primitive,g::reg::RB_DEPTHCONTROL{},0,0,formats,&handle,&signature);
        REXLOG_INFO("Renderer seed pipeline: configured {}, handle {}, signature {}, VS valid {}, PS valid {}",configured,handle!=nullptr,signature!=nullptr,vs_translation->is_valid(),ps_translation->is_valid());
        const bool passed=configured && handle && signature && cache.GetD3D12PipelineByHandle(handle) && vs_translation->is_valid() && ps_translation->is_valid();
        cache.EndSubmission();
        // This fixture disables creation threads, so there are no more producers.
        // Drain pending writes before native shutdown. A popped, in-flight write
        // completes before shutdown joins the writer and closes/flushed FILEs.
        // File size cannot establish this: the writer may still buffer a record.
        bool drained=false;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(passed && std::chrono::steady_clock::now()<deadline) {
            {
                std::lock_guard lock(cache.*GetStorageMember(storage_write_request_lock__tag{}));
                drained=(cache.*GetStorageMember(storage_write_shader_queue__tag{})).empty() &&
                    (cache.*GetStorageMember(storage_write_pipeline_queue__tag{})).empty();
            }
            if(drained)break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        cache.ShutdownShaderStorage();
        REXLOG_INFO("Renderer synthetic pipeline seed drain {} (bounded diagnostic wait; normal cache policy unchanged)",drained?"passed":"failed");
        REXLOG_INFO("Renderer populated storage seed: two synthetic shaders, bindless {}, real D3D12 pipeline {}",bindless,passed?"passed":"failed");
        return passed && drained;
    }) && em2::TestGraphicsResourceSynchronizationDispatch();
}
bool em2::VerifyPreloadedRendererShaderStorageTest(rex::graphics::GraphicsSystem& graphics) {
    return OnGpuThread(graphics,[&graphics] {
        namespace g=rex::graphics;
        auto* memory=graphics.memory();auto* cp=graphics.command_processor();
        auto* heap=memory->LookupHeapByType(true,0x1000);uint32_t packet=0;
        if(!heap || !heap->Alloc(0x1000,0x1000,rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,
            rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite,false,&packet))return false;
        struct Release {rex::memory::BaseHeap* heap;uint32_t address;~Release(){heap->Release(address,nullptr);}} release{heap,packet};
        const auto physical=memory->GetPhysicalAddress(packet);
        if(physical==UINT32_MAX)return false;
        auto* words=memory->TranslateVirtual<uint32_t*>(packet);
        unsigned used=0;
        for(unsigned type=0;type<2;++type) {
            const auto& program=type?pixel:vertex;
            words[used++]=std::byteswap(0xC0000000u|(7u<<16)|(uint32_t(g::xenos::PM4_IM_LOAD_IMMEDIATE)<<8));
            words[used++]=std::byteswap(type);words[used++]=std::byteswap(uint32_t(program.size()));
            for(auto word:program)words[used++]=std::byteswap(word);
        }
        cp->ExecutePacket(physical,used);
        bool passed=true;
        const std::array shaders={cp->active_vertex_shader(),cp->active_pixel_shader()};
        for(unsigned type=0;type<shaders.size();++type) {
            auto* shader=shaders[type];
            if(!shader || !shader->is_ucode_analyzed() || shader->ucode_dword_count()!=6 || shader->ucode_storage_index()==UINT32_MAX || shader->translations().empty()){passed=false;continue;}
            const auto expected=BigEndianProgram(type?pixel:vertex);
            passed&=shader->type()==(type?g::xenos::ShaderType::kPixel:g::xenos::ShaderType::kVertex) &&
                shader->ucode_data_hash()==XXH3_64bits(expected.data(),expected.size()*4);
            bool valid=false;for(const auto& [modification,translation]:shader->translations())valid|=translation->is_valid() && translation->translated_binary().size()>32;
            passed&=valid;
            REXLOG_INFO("Renderer preloaded shader {:016X}: {} words, storage {}, {} translations, valid {}",shader->ucode_data_hash(),shader->ucode_dword_count(),shader->ucode_storage_index(),shader->translations().size(),valid);
        }
        const auto expected_runs=verified_preloads.load()+1;
        const auto shader_runs=shader_preloads.load(),pipeline_runs=pipeline_preloads.load();
        passed&=shader_runs>=expected_runs && pipeline_runs>=expected_runs;
        if(passed)verified_preloads.store(expected_runs);
        REXLOG_INFO("Renderer cache preload events: {} shader batches, {} pipeline batches, expected at least {}",shader_runs,pipeline_runs,expected_runs);
        REXLOG_INFO("Renderer populated shader preload lookup {} (no draw or guest execution)",passed?"passed":"failed");
        return passed;
    });
}

bool em2::BenchmarkSceneIndirect(rex::graphics::GraphicsSystem& graphics,const std::filesystem::path& directory){
    return OnGpuThread(graphics,[&graphics,directory]{
        const auto* provider=dynamic_cast<const rex::ui::d3d12::D3D12Provider*>(graphics.provider());
        return provider && TestSceneIndirectPerformance(*provider,directory);
    });
}
