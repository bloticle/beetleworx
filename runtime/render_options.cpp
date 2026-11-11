#include "render_options.h"
#include "shadow_kernel.h"
#include "bloom_kernel.h"
#include "pc_settings.h"
#include "physical_arena.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/graphics/d3d12/texture_cache.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/format/ucode.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include "../private/bloom_shader_test_data.h"
#include "../private/shadow_shader_test_data.h"
#include <fstream>
#include <span>
#include <atomic>
#include <bit>
#include <chrono>
#include <array>
#include <cstring>
#include <mutex>
#include <utility>
#include <immintrin.h>
#include <windows.h>

PPC_EXTERN_FUNC(__imp__sub_8279F040);
PPC_EXTERN_FUNC(__imp__sub_827A42F0);
PPC_EXTERN_FUNC(__imp__sub_827A17C0);
PPC_EXTERN_FUNC(__imp__sub_829C3868);
REXCVAR_DECLARE(int32_t,draw_resolution_scale_x);
REXCVAR_DECLARE(int32_t,draw_resolution_scale_y);
REXCVAR_DEFINE_BOOL(em2_trace_bloom,true,"Debug","Record bounded native bloom target setup diagnostics");

REXCVAR_DEFINE_INT32(em2_anisotropic_override,-1,"GPU","EM2 texture anisotropy; -1 original, 2..5 selects 2x..16x");
REXCVAR_DEFINE_BOOL(em2_trace_lighting,false,"Debug","Log initialized native lighting configuration for rendering QA");
REXCVAR_DEFINE_BOOL(em2_profile_lighting,false,"Debug","Sample native lighting call wall time every 32 calls; not a GPU or full CPU profiler");

namespace {
struct LightingProfile {
    std::atomic<uint64_t> calls{0},samples{0},nanoseconds{0};
};
LightingProfile lighting_distance_profile,lighting_component_profile;
template<typename Function>
void ProfileLightingCall(PPCContext& ctx,uint8_t* base,LightingProfile& profile,Function original) {
    if(!REXCVAR_GET(em2_profile_lighting)) {original(ctx,base);return;}
    const auto call=profile.calls.fetch_add(1,std::memory_order_relaxed);
    if(call%32!=0) {original(ctx,base);return;}
    const auto start=std::chrono::steady_clock::now();
    original(ctx,base);
    const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
    profile.nanoseconds.fetch_add(uint64_t(ns),std::memory_order_relaxed);
    profile.samples.fetch_add(1,std::memory_order_release);
}
}

PPC_FUNC_IMPL(sub_829C3868) {
    // This is native screen-effect target setup, not a per-draw GPU callback.
    // The descriptor owns its interned name and source throughout this call.
    // Query the same source dimensions used by the original routine, preserving
    // the caller's complete register and host floating-point state.
    const auto descriptor=ctx.r4.u32;
    if(REXCVAR_GET(em2_trace_bloom) && descriptor) {
        const auto name=PPC_LOAD_U32(descriptor+40);
        std::array<char,96> label{};
        if(name) for(unsigned i=0;i<label.size()-1;++i) {
            label[i]=char(PPC_LOAD_U8(name+i));if(!label[i])break;
        }
        if(std::strstr(label.data(),"Bloom") || std::strstr(label.data(),"bloom")) {
            static std::atomic<unsigned> reports{0};
            if(reports.fetch_add(1,std::memory_order_relaxed)<16) {
                const auto source=PPC_LOAD_U32(descriptor+28),divisor=PPC_LOAD_U32(descriptor+32);
                if(source && divisor) {
                    const auto saved=ctx;const auto csr=_mm_getcsr();
                    const auto table=PPC_LOAD_U32(source);
                    ctx.r3.u64=source;ctx.r4.u64=0;
                    PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(table+76));
                    const auto width=ctx.r3.u32;
                    ctx=saved;_mm_setcsr(csr);
                    ctx.r3.u64=source;ctx.r4.u64=0;
                    PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(table+80));
                    const auto height=ctx.r3.u32;
                    ctx=saved;_mm_setcsr(csr);
                    REXLOG_INFO("Bloom target setup '{}': source {}x{}, divisor {}, guest target {}x{}, host draw scale {}x{}; native format words {:08X}/{:08X}/{:08X}",
                        label.data(),width,height,divisor,width/divisor,height/divisor,
                        REXCVAR_GET(draw_resolution_scale_x),REXCVAR_GET(draw_resolution_scale_y),
                        PPC_LOAD_U32(descriptor+16),PPC_LOAD_U32(descriptor+20),PPC_LOAD_U32(descriptor+24));
                }
            }
        }
    }
    __imp__sub_829C3868(ctx,base);
}

PPC_FUNC_IMPL(sub_8279F040) {
    ProfileLightingCall(ctx,base,lighting_distance_profile,__imp__sub_8279F040);
}
PPC_FUNC_IMPL(sub_827A17C0) {
    ProfileLightingCall(ctx,base,lighting_component_profile,__imp__sub_827A17C0);
}

void em2::LogLightingProfile() {
    if(!REXCVAR_GET(em2_profile_lighting)) return;
    // Cumulative counters; no destructive resets racing engine worker threads.
    // Timed calls include their descendants and any waits, plus clock overhead.
    for(const auto& entry : {std::pair{"distance gate 8279F040",&lighting_distance_profile},
                            std::pair{"component update 827A17C0",&lighting_component_profile}}) {
        const auto samples=entry.second->samples.load(std::memory_order_acquire);
        const auto ns=entry.second->nanoseconds.load(std::memory_order_relaxed);
        REXLOG_INFO("Lighting wall-time profile {}: {} calls, {} timed samples, {} ns sampled total (cumulative, one in 32 calls)",
            entry.first,entry.second->calls.load(std::memory_order_relaxed),samples,ns);
    }
}

PPC_FUNC_IMPL(sub_827A42F0) {
    __imp__sub_827A42F0(ctx,base);
    em2::ApplyLiveLightingSettings(base);
    if(REXCVAR_GET(em2_trace_lighting)) {
        // Report actual cached guest values, not the requested host settings.
        // Bits avoid FP conversions and keep the guest's floating-point mode.
        REXLOG_INFO("Native lighting initialized: ray casts {}, squared update distance bits {:08X}, ray length bits {:08X}, fade step bits {:08X}",
            PPC_LOAD_U8(0x8339A1C4)!=0,PPC_LOAD_U32(0x8339A1CC),
            PPC_LOAD_U32(0x8339A1C8),PPC_LOAD_U32(0x8339A25C));
    }
}

namespace {
void ApplyLightingSelection(uint8_t* base,unsigned settings) {
    constexpr uint32_t squared[]={0x3F800000,0x3E800000,0x3D800000};
    PPC_STORE_U8(0x8339A1C4,settings&1);
    PPC_STORE_U32(0x8339A1CC,squared[(settings>>1)%3]);
}
}
void em2::ApplyLiveLightingSettings(uint8_t* base){ApplyLightingSelection(base,NativeLightingSettings());}

using Cache=rex::graphics::d3d12::D3D12TextureCache;
using Binding=rex::graphics::d3d12::D3D12Shader::SamplerBinding;
// SDK 0.1.1 MS x64 member ABI: this in RCX, hidden result in RDX,
// binding in R8. The union's user-defined constructor requires an indirect
// result even though its payload is four bytes (verified in SDK disassembly).
static_assert(sizeof(Cache::SamplerParameters)==sizeof(uint32_t));
extern "C" void* EM2OriginalSamplerParameters(const Cache*,Cache::SamplerParameters*,const Binding&);

namespace {
bool TestLightingDistances() {
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));
    if(!base) return false;
    struct Release {
        uint8_t* base; unsigned mxcsr=_mm_getcsr();
        ~Release() {_mm_setcsr(mxcsr);VirtualFree(base,0,MEM_RELEASE);}
    } release{base};
    if(!VirtualAlloc(base,0x10000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0x83390000,0x10000,MEM_COMMIT,PAGE_READWRITE)) return false;
    for(unsigned selection=0;selection<6;++selection) {
        std::memset(base+0x83390000,0xA5,0x10000);
        auto expected=std::make_unique<std::array<uint8_t,0x10000>>();expected->fill(0xA5);
        auto* expected_base=expected->data();
        (*expected)[0xA1C4]=uint8_t(selection&1);
        constexpr uint32_t bits[]={0x3F800000,0x3E800000,0x3D800000};
        const auto big=std::byteswap(bits[selection>>1]);std::memcpy(expected_base+0xA1CC,&big,4);
        const auto csr=_mm_getcsr();ApplyLightingSelection(base,selection);
        if(_mm_getcsr()!=csr || std::memcmp(expected_base,base+0x83390000,0x10000))return false;
    }
    unsigned comparisons=0;
    for(float threshold : {1.0f,0.5f,0.25f})
        for(bool uninitialized : {false,true})
            for(unsigned axis=0;axis<3;++axis)
                for(float factor : {0.0f,0.5f,1.0f,1.5f}) {
                    constexpr uint32_t node=0x1000,position=0x2000;
                    PPC_STORE_U32(0x8339A1CC,std::bit_cast<uint32_t>(threshold*threshold));
                    PPC_STORE_U32(node+40,uninitialized?0xFFFFFFFFu:0u);
                    for(unsigned i=0;i<3;++i) {
                        PPC_STORE_U32(node+44+i*4,0);
                        PPC_STORE_U32(position+i*4,std::bit_cast<uint32_t>(i==axis?threshold*factor:0.0f));
                    }
                    PPCContext ctx{};ctx.fpscr.InitHost();ctx.r3.u32=node;ctx.r4.u32=position;
                    __imp__sub_8279F040(ctx,base);
                    const bool update=uninitialized || factor>1.0f;
                    if(ctx.r3.u32!=unsigned(update)) return false;
                    for(unsigned i=0;i<3;++i)
                        if(PPC_LOAD_U32(node+44+i*4)!=(update?PPC_LOAD_U32(position+i*4):0u)) return false;
                    ++comparisons;
                }
    REXLOG_INFO("Native lighting update tests passed: six live selections change only the native flag/distance bytes and preserve FP; {} original-code checks across distance presets, boundary equality, axes and first update",comparisons);
    return true;
}

Cache::SamplerParameters Upgrade(Cache::SamplerParameters p,bool has_mips,int quality) {
    // Point-sampled UI, lookup textures, and base-map-only effects retain their
    // native sampling. Only already-filtered mipmapped materials are eligible.
    if(quality>=2 && quality<=5 && has_mips && !p.mip_base_map && p.min_linear && p.mag_linear) {
        p.aniso_filter=rex::graphics::xenos::AnisoFilter(quality);
        p.mip_linear=1;
    }
    return p;
}
}

namespace rex::graphics::d3d12 {
D3D12TextureCache::SamplerParameters D3D12TextureCache::GetSamplerParameters(const D3D12Shader::SamplerBinding& binding) const {
    SamplerParameters original;
    EM2OriginalSamplerParameters(this,&original,binding);
    const auto quality=REXCVAR_GET(em2_anisotropic_override);
    if(quality<2 || quality>5) return original;
    uint32_t first=0,last=0;
    texture_util::GetSubresourcesFromFetchConstant(register_file().GetTextureFetch(binding.fetch_constant),
        nullptr,nullptr,nullptr,nullptr,nullptr,&first,&last);
    const auto upgraded=Upgrade(original,last>first,quality);
    static std::atomic<bool> reported{false};
    if(upgraded!=original && !reported.exchange(true,std::memory_order_relaxed))
        REXLOG_INFO("EM2 anisotropic sampling active: {}x, original sampler key {:08X}, upgraded key {:08X}",
            1u<<(quality-1),original.value,upgraded.value);
    return upgraded;
}
}

namespace em2 {
namespace {
class ShadowAuditTranslator final:public rex::graphics::DxbcShaderTranslator {
public:
    using DxbcShaderTranslator::DxbcShaderTranslator;
    rex::string::StringBuffer audit;
protected:
    void ProcessExecInstructionBegin(const rex::graphics::ParsedExecInstruction& instruction) override {
        instruction.Disassemble(&audit);audit.Append('\n');
        DxbcShaderTranslator::ProcessExecInstructionBegin(instruction);
    }
    void ProcessTextureFetchInstruction(const rex::graphics::ParsedTextureFetchInstruction& instruction) override {
        instruction.Disassemble(&audit);audit.Append('\n');
        DxbcShaderTranslator::ProcessTextureFetchInstruction(instruction);
    }
    void ProcessAluInstruction(const rex::graphics::ParsedAluInstruction& instruction,uint8_t exports) override {
        instruction.Disassemble(&audit);audit.Append('\n');
        DxbcShaderTranslator::ProcessAluInstruction(instruction,exports);
    }
};
}
bool TestNativeShadowShader(const std::filesystem::path& output) {
    const std::array<std::span<const uint32_t>,5> programs={shadow_test_data::original,shadow_test_data::control,
        shadow_test_data::samples8,shadow_test_data::samples16,shadow_test_data::adaptive16};
    const std::array<const char*,5> labels={"original","control","8","16","adaptive16"};
    for(unsigned variant=0;variant<programs.size();++variant)
    for(unsigned scale=1;scale<=4;++scale)for(bool bindless:{false,true}) {
        const auto words=programs[variant];
        rex::graphics::DxbcShader shadow(rex::graphics::xenos::ShaderType::kPixel,0x534841444F57,
            words.data(),words.size(),std::endian::native);
        rex::string::StringBuffer buffer;shadow.AnalyzeUcode(buffer);
        std::array<unsigned,6> fetches{};
        for(const auto& binding:shadow.texture_bindings()) {
            if(binding.fetch_constant>=fetches.size())return false;
            ++fetches[binding.fetch_constant];
        }
        const unsigned taps=variant<2?4:variant==2?8:variant==3?16:20;
        if(fetches!=std::array<unsigned,6>{1,1,1,taps,taps,taps} || shadow.writes_color_targets()!=1) {
            REXLOG_ERROR("Shadow {} binding/export signature mismatch",labels[variant]);return false;
        }
            ShadowAuditTranslator translator(rex::ui::GraphicsProvider::GpuVendorID::kNvidia,
                bindless,false,false,true,scale,scale);
            auto* translation=shadow.GetOrCreateTranslation(translator.GetDefaultPixelShaderModification(0));
            if(!translator.TranslateAnalyzedShader(*translation) || !translation->is_valid()) {
                REXLOG_ERROR("Native screen-space shadow shader translation failed at scale {} bindless {}",scale,bindless);
                return false;
            }
        if(!output.empty()) {
            std::filesystem::create_directories(output);
            const auto& binary=translation->translated_binary();
            std::ofstream file(output/("shadow-"+std::string(labels[variant])+"-"+std::to_string(scale)+(bindless?"-bindless":"-bound")+".dxbc"),std::ios::binary);
            file.write(reinterpret_cast<const char*>(binary.data()),binary.size());file.close();
            if(!file || binary.size()<32 || std::memcmp(binary.data(),"DXBC",4))return false;
            if(scale==1 && !bindless) {
                std::ofstream text(output/("shadow-"+std::string(labels[variant])+".xenos.txt"),std::ios::binary);
                text<<translator.audit.to_string();text.close();
                if(!text || translator.audit.length()<1000)return false;
            }
        }
    }
    REXLOG_INFO("Shadow original/control/8/16/adaptive programs translated at scales 1..4, bound/bindless, with exact depth-fetch counts and color export; GPU output still needs validation");
    return true;
}
bool TestNativeBloomShader(const std::filesystem::path& output) {
    // Exercise the actual shader analyzer and D3D12 DXBC translator with the
    // verified original/patched Gaussian programs, without a window or GPU.
    for(unsigned scale=1;scale<=4;++scale) for(bool bindless:{false,true}) {
        const std::array<std::span<const uint32_t>,8> programs={bloom_test_data::horizontal_original,bloom_test_data::horizontal_linear,
                                  bloom_test_data::vertical_original,bloom_test_data::vertical_linear,DisabledBloomShader(),bloom_test_data::threshold,bloom_test_data::composite,bloom_test_data::composite_linear};
        for(unsigned i=0;i<programs.size();++i) {
            rex::graphics::DxbcShader shader(rex::graphics::xenos::ShaderType::kPixel,i+1,
                programs[i].data(),programs[i].size(),std::endian::native);
            rex::string::StringBuffer disassembly;
            shader.AnalyzeUcode(disassembly);
            ShadowAuditTranslator translator(rex::ui::GraphicsProvider::GpuVendorID::kNvidia,
                bindless,false,false,true,scale,scale);
            auto* translation=shader.GetOrCreateTranslation(translator.GetDefaultPixelShaderModification(0));
            if(!translator.TranslateAnalyzedShader(*translation) || !translation->is_valid() ||
               translation->translated_binary().size()<32 ||
               std::memcmp(translation->translated_binary().data(),"DXBC",4)) {
                REXLOG_ERROR("Bloom shader translation failed: program {}, scale {}, bindless {}",i,scale,bindless);
                return false;
            }
            const auto& samplers=shader.GetSamplerBindingsAfterTranslation();
            if(i==7 && (samplers.size()!=2 || !std::all_of(samplers.begin(),samplers.end(),[](const auto& sampler){
                return sampler.fetch_constant<=1 && sampler.min_filter==rex::graphics::xenos::TextureFilter::kLinear &&
                    sampler.mag_filter==rex::graphics::xenos::TextureFilter::kLinear;
            })))return false;
            const auto expected=i&1?rex::graphics::xenos::TextureFilter::kLinear:rex::graphics::xenos::TextureFilter::kUseFetchConst;
            if(i>=5 ? shader.writes_color_targets()!=1 || shader.texture_bindings().empty() :
               i==4 ? !samplers.empty() || !shader.texture_bindings().empty() || shader.writes_color_targets()!=1 :
               samplers.size()!=1 || samplers[0].fetch_constant!=0 ||
               samplers[0].min_filter!=expected || samplers[0].mag_filter!=expected) {
                REXLOG_ERROR("Bloom translated sampler layout differs from shader patch intent");return false;
            }
            if(!output.empty()) {
                std::filesystem::create_directories(output);
                const auto stem="bloom-"+std::to_string(i)+"-"+std::to_string(scale)+(bindless?"-bindless":"-bound");
                const auto& binary=translation->translated_binary();
                std::ofstream file(output/(stem+".dxbc"),std::ios::binary);
                file.write(reinterpret_cast<const char*>(binary.data()),binary.size());file.close();if(!file)return false;
                if(scale==1 && !bindless){std::ofstream text(output/(stem+".xenos.txt"));text<<translator.audit.to_string();text.close();if(!text)return false;}
            }
        }
    }
    REXLOG_INFO("Bloom original/patched programs translated to DXBC at scales 1..4 with bound/bindless resources; sampler layouts verified");
    // Validate the bloom asset patch against the runtime's actual instruction
    // decoder. The seven fetches differ only in registers; word 1 is shared.
    using Fetch=rex::graphics::ucode::TextureFetchInstruction;
    for(uint32_t first:{0x100850A1u,0x10087001u,0x10081021u,0x10082041u,0x10083061u,0x10084081u,0x100800C1u}) {
        const auto original=std::bit_cast<Fetch>(std::array<uint32_t,3>{first,0x1F1FF688u,0x4000u});
        const auto patched=std::bit_cast<Fetch>(std::array<uint32_t,3>{first,0x1F1F5688u,0x4000u});
        if(original.has_min_filter() || original.has_mag_filter() ||
           patched.min_filter()!=rex::graphics::xenos::TextureFilter::kLinear ||
           patched.mag_filter()!=rex::graphics::xenos::TextureFilter::kLinear ||
           original.mip_filter()!=patched.mip_filter() ||
           original.offset_x()!=patched.offset_x() || original.offset_y()!=patched.offset_y()) {
            REXLOG_ERROR("Bloom shader filter override decoding failed");return false;
        }
    }
    REXLOG_INFO("Bloom shader sampling override matches runtime instruction decoder for all seven Gaussian taps");
    return true;
}
bool TestRenderOptions() {
    if(!TestPhysicalArenaStats())return false;
    if(!TestShadowKernelSelection() || !TestNativeShadowShader() || !TestBloomKernelSelection() || !TestNativeBloomShader())return false;
    if(!TestLightingDistances()) {
        REXLOG_ERROR("Native lighting update distance tests failed");
        return false;
    }
    Cache::SamplerParameters p; p.min_linear=p.mag_linear=p.mip_linear=1;
    p.clamp_x=rex::graphics::xenos::ClampMode::kClampToEdge;
    p.mip_min_level=2;
    bool ok=Upgrade(p,true,-1)==p && Upgrade(p,true,6)==p && Upgrade(p,false,5)==p;
    for(int quality=2;quality<=5;++quality) {
        const auto upgraded=Upgrade(p,true,quality);
        auto expected=p; expected.aniso_filter=rex::graphics::xenos::AnisoFilter(quality);
        ok&=upgraded==expected;
    }
    auto special=p; special.mag_linear=0;
    ok&=Upgrade(special,true,5)==special;
    special=p; special.min_linear=0;
    ok&=Upgrade(special,true,5)==special;
    special=p; special.mip_base_map=1;
    ok&=Upgrade(special,true,5)==special;
    if(ok) REXLOG_INFO("Renderer sampler tests passed: 2x..16x, native mode, point/base-only/nonmipped preservation");
    else REXLOG_ERROR("Renderer sampler tests failed");
    return ok;
}
}
