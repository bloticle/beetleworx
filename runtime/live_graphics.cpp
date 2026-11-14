#include "em1_ink_restoration.h"
#include "native_engine.h"
#include "gpu_constants_internal.h"
#include "transfer_benchmark.h"
#include "shadow_kernel.h"
#include "bloom_kernel.h"
#include "scene_raster.h"
#include "em1_preview_capture.h"
#include "em1_preview_runtime.h"
#include "em1_character_lighting.h"
#include <rex/cvar.h>
#include <set>
#include <cmath>
#include <windows.h>
#include "../private/shadow_shader_test_data.h"
#include "../private/bloom_shader_test_data.h"
#include "../private/material_shader_data.h"
#include <rex/logging.h>
#include <algorithm>
#include <bit>
#include <rex/graphics/pipeline/texture/util.h>

REXCVAR_DEFINE_BOOL(em1_trace_draw_state,false,"Debug","Bounded native draw depth and sampler observations after EM1 player readiness");
REXCVAR_DECLARE(bool,em2_restore_ink_drips);
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(bool,em2_online_clothing_bind);

namespace em2 {
namespace {
class ScopedCharacterLighting {
    Em2CommandProcessor& owner_;
    unsigned first_;
    std::array<uint32_t,27> original_{};
    bool applied_=false;
    uint32_t Index(unsigned c,unsigned lane)const{return rex::graphics::XE_GPU_REG_SHADER_CONSTANT_256_X+(first_+c)*4+lane;}
public:
    ScopedCharacterLighting(Em2CommandProcessor& owner,unsigned first):owner_(owner),first_(first){
        for(unsigned c=0;c<9;++c)for(unsigned lane=0;lane<3;++lane)
            original_[c*3+lane]=owner_.ConstantRegisterValues()[Index(c,lane)];
    }
    ScopedCharacterLighting(const ScopedCharacterLighting&)=delete;
    ScopedCharacterLighting& operator=(const ScopedCharacterLighting&)=delete;
    void Apply(const std::array<std::array<float,3>,9>& coefficients){
        applied_=true;
        for(unsigned c=0;c<9;++c)for(unsigned lane=0;lane<3;++lane)
            owner_.WriteNativeConstant(Index(c,lane),std::bit_cast<uint32_t>(coefficients[c][lane]));
    }
    ~ScopedCharacterLighting(){if(applied_)for(unsigned c=0;c<9;++c)for(unsigned lane=0;lane<3;++lane)
        owner_.WriteNativeConstant(Index(c,lane),original_[c*3+lane]);}
};
void TraceEm1Draw(const rex::graphics::Shader* vertex,const rex::graphics::Shader* pixel,
    const rex::graphics::RegisterFile& registers,rex::memory::Memory* memory,uint64_t frame,uint32_t count) noexcept {
    if(!REXCVAR_GET(em1_trace_draw_state) ||
       (!em1::Em1DrawDiagnosticsReady() && !(em1::OriginalInkPakMounted() && NativeConsoleWorldReady())))return;
    try {
        // Ink can first emit after the general mesh trace has exhausted its
        // startup budget. Observe its two pinned programs independently.
        static uint64_t ink_first=frame,ink_window=frame,ink_total=0,ink_draws=0;
        static bool ink_reported=false;
        if(em1::OriginalInkPakMounted() && frame-ink_first<=900) {
            const auto hash=pixel?pixel->ucode_data_hash():0;
            if(hash==0xB5E110AF651A433Eull || hash==0xDFBAF87D3BF6EDE1ull ||
               hash==0xD881DF27BB616E87ull || hash==0xA3CE97D1EBA4F078ull) {
                ++ink_total;++ink_draws;
                if(!ink_reported){
                    ink_reported=true;const auto depth=registers.Get<rex::graphics::reg::RB_DEPTHCONTROL>();
                    REXLOG_INFO("[em1] original ink native draw: frame={} indices={} vs={:016X} ps={:016X} depth_test={} depth_write={} compare={} blend={:08X}",
                        frame,count,vertex?vertex->ucode_data_hash():0,hash,unsigned(depth.z_enable),unsigned(depth.z_write_enable),unsigned(depth.zfunc),registers[rex::graphics::XE_GPU_REG_RB_BLENDCONTROL0]);
                }
            }
            if(frame-ink_window>=30){
                REXLOG_INFO("[em1] original ink draw window: frame={} draws={} total={} option={}",frame,ink_draws,ink_total,REXCVAR_GET(em2_restore_ink_drips));
                ink_window=frame;ink_draws=0;
            }
        }
        static uint64_t first=frame;
        static std::set<std::array<uint64_t,10>> seen;
        static std::set<std::array<uint32_t,3>> uv_seen;
        static bool mask_vertex_reported=false;
        static bool dual_vertex_reported=false;
        if(frame-first>120)return;
        static bool mickey_lighting_reported=false;
        if(!mickey_lighting_reported && pixel) {
            const auto& code=pixel->ucode_data();const auto& expected=material_shader_data::Gen3_Char_Mickey;
            const bool pinned=code.size()==std::size(expected) && std::equal(code.begin(),code.end(),std::begin(expected));
            if(pinned || count==25344) {
                mickey_lighting_reported=true;
                REXLOG_INFO("[em1] character lighting candidate ps={:016X} vs={:016X} pinnedMickey={} words={} expected={}",pixel->ucode_data_hash(),vertex?vertex->ucode_data_hash():0,pinned,code.size(),std::size(expected));
                const auto depth=registers.Get<rex::graphics::reg::RB_DEPTHCONTROL>();
                const auto raster=registers.Get<rex::graphics::reg::PA_SU_SC_MODE_CNTL>();
                REXLOG_INFO("[em1] character draw orientation count={} depth={:08X} test={} write={} compare={} raster={:08X} cullFront={} cullBack={}",
                    count,depth.value,unsigned(depth.z_enable),unsigned(depth.z_write_enable),unsigned(depth.zfunc),raster.value,unsigned(raster.cull_front),unsigned(raster.cull_back));
                REXLOG_INFO("[em1] character candidate PS disassembly\n{}",pixel->ucode_disassembly());
                if(!pinned)for(size_t i=0;i+2<code.size() && i<1536;i+=3)REXLOG_INFO("[em1] character candidate PS instruction {}: {:08X} {:08X} {:08X}",i/3,code[i],code[i+1],code[i+2]);
                for(unsigned c=0;c<32;++c) {
                    const auto at=rex::graphics::XE_GPU_REG_SHADER_CONSTANT_256_X+c*4;
                    REXLOG_INFO("[em1] Mickey lighting c{} raw={:08X}/{:08X}/{:08X}/{:08X}",c,registers[at],registers[at+1],registers[at+2],registers[at+3]);
                }
            }
        }
        const bool mask_candidate=pixel && pixel->ucode_data_hash()==0x463D1C5017F0DC37ull;
        static std::set<std::array<uint32_t,20>> grass_matrices;
        if(count==7155 && vertex && vertex->ucode_data_hash()==0xD932231205D5BC44ull && grass_matrices.size()<120) {
            std::array<uint32_t,20> values{};
            for(unsigned i=0;i<16;++i)values[i]=registers[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_000_X+i];
            for(unsigned i=0;i<4;++i)values[16+i]=registers[rex::graphics::XE_GPU_REG_PA_CL_VPORT_XSCALE+i];
            if(grass_matrices.insert(values).second) {
                for(unsigned row=0;row<5;++row)REXLOG_INFO("[em1] grass projection frame={} row={} raw={:08X}/{:08X}/{:08X}/{:08X}",frame,row,values[row*4],values[row*4+1],values[row*4+2],values[row*4+3]);
            }
        }
        const bool dual_candidate=pixel && pixel->ucode_data_hash()==0x88D11CB3D297D385ull;
        if(dual_candidate && vertex && !dual_vertex_reported){
            dual_vertex_reported=true;const auto& words=vertex->ucode_data();
            for(size_t i=0;i+2<words.size() && i<42;i+=3)
                REXLOG_INFO("[em1] dual linked VS instruction {}: {:08X} {:08X} {:08X}",i/3,words[i],words[i+1],words[i+2]);
            for(unsigned slot:{92u,93u}){
                const auto f=registers.GetVertexFetch(slot);
                std::array<uint32_t,4> head{};SIZE_T read=0;
                const auto address=(f.address<<2)&0x1fffffff;
                const bool copied=memory && f.size>=4 && ReadProcessMemory(GetCurrentProcess(),
                    memory->TranslatePhysical(address),head.data(),sizeof(head),&read) && read==sizeof(head);
                REXLOG_INFO("[em1] dual vertex fetch{} address={:08X} bytes={} endian={} copied={} raw={:08X}/{:08X}/{:08X}/{:08X}",
                    slot,address,f.size*4,unsigned(f.endian),copied,head[0],head[1],head[2],head[3]);
            }
        }
        // Compare complete imported UV buffers against source GX tuples. This is
        // bounded, read-only instrumentation behind em1_trace_draw_state.
        static std::set<std::array<uint32_t,2>> full_uv_seen;
        if(memory && vertex && pixel && vertex->ucode_data_hash()==0xD932231205D5BC44ull &&
            (mask_candidate || pixel->ucode_data_hash()==0x62EA4FFDFD73B787ull) && full_uv_seen.size()<64){
            const auto f=registers.GetVertexFetch(93);
            const uint32_t address=(f.address<<2)&0x1fffffff,bytes=f.size*4;
            if(bytes && bytes<=1024*1024 && full_uv_seen.insert({address,bytes}).second){
                std::vector<uint8_t> data(bytes);SIZE_T read=0;
                if(ReadProcessMemory(GetCurrentProcess(),memory->TranslatePhysical(address),data.data(),bytes,&read) && read==bytes){
                    uint64_t hash=14695981039346656037ull;
                    for(uint8_t byte:data)hash=(hash^byte)*1099511628211ull;
                    REXLOG_INFO("[em1] full UV buffer address={:08X} bytes={} endian={} indices={} fnv={:016X}",address,bytes,unsigned(f.endian),count,hash);
                    static std::set<uint32_t> texture_seen;
                    for(unsigned slot:{1u,3u,2u}){
                        if(!mask_candidate && slot!=1)continue;
                        const auto tf=registers.GetTextureFetch(slot);
                        const auto base=(tf.base_address<<12)&0x1fffffff;
                        const uint32_t w=tf.size_2d.width+1,h=tf.size_2d.height+1,pitch=tf.pitch*32;
                        if(texture_seen.size()>=64 || unsigned(tf.format)!=6 || unsigned(tf.endianness)!=0 ||
                            tf.swizzle!=0x60A || !tf.tiled || w<32 || h<32 || pitch<w || pitch>1024 || h>1024 ||
                            !texture_seen.insert(base).second)continue;
                        const size_t extent=size_t(pitch)*((h+31)&~31u)*4;
                        std::vector<uint8_t> tiled(extent);SIZE_T copied=0;
                        if(!ReadProcessMemory(GetCurrentProcess(),memory->TranslatePhysical(base),tiled.data(),extent,&copied) || copied!=extent)continue;
                        uint64_t digest=14695981039346656037ull;bool valid=true;
                        for(uint32_t y=0;y<h && valid;++y)for(uint32_t x=0;x<w;++x){
                            const int32_t offset=rex::graphics::texture_util::GetTiledOffset2D(x,y,pitch,2);
                            if(offset<0 || size_t(offset)+4>extent){valid=false;break;}
                            for(unsigned channel:{2u,1u,0u,3u})digest=(digest^tiled[offset+channel])*1099511628211ull;
                        }
                        if(valid)REXLOG_INFO("[em1] full texture base={:08X} slot={} width={} height={} rgbaFNV={:016X} repeat={}/{}",base,slot,w,h,digest,unsigned(tf.clamp_x),unsigned(tf.clamp_y));
                        const auto layout=rex::graphics::texture_util::GetGuestTextureLayout(
                            rex::graphics::xenos::DataDimension::k2DOrStacked,tf.pitch,w,h,1,true,tf.format,tf.packed_mips,true,tf.mip_max_level);
                        if(!layout.mips_total_extent_bytes || layout.mips_total_extent_bytes>4*1024*1024)continue;
                        std::vector<uint8_t> mip_data(layout.mips_total_extent_bytes);
                        const uint32_t mip_base=(tf.mip_address<<12)&0x1fffffff;
                        if(!ReadProcessMemory(GetCurrentProcess(),memory->TranslatePhysical(mip_base),mip_data.data(),mip_data.size(),&copied) || copied!=mip_data.size())continue;
                        for(uint32_t mip=1;mip<=layout.max_level;++mip){
                            const uint32_t storage=(std::min)(mip,layout.packed_level);
                            uint32_t ox=0,oy=0,oz=0;
                            if(tf.packed_mips)rex::graphics::texture_util::GetPackedMipOffset(w,h,1,tf.format,mip,ox,oy,oz);
                            const uint32_t mw=(std::max)(1u,w>>mip),mh=(std::max)(1u,h>>mip);
                            uint64_t mhsh=14695981039346656037ull;bool ok=true;
                            for(uint32_t y=0;y<mh && ok;++y)for(uint32_t x=0;x<mw;++x){
                                const int32_t off=rex::graphics::texture_util::GetTiledOffset2D(x+ox,y+oy,layout.mips[storage].row_pitch_bytes/4,2);
                                const uint64_t at=uint64_t(layout.mip_offsets_bytes[storage])+uint32_t(off);
                                if(off<0 || at+4>mip_data.size()){ok=false;break;}
                                for(unsigned channel:{2u,1u,0u,3u})mhsh=(mhsh^mip_data[at+channel])*1099511628211ull;
                            }
                            if(ok)REXLOG_INFO("[em1] full texture mip base={:08X} level={} width={} height={} rgbaFNV={:016X}",base,mip,mw,mh,mhsh);
                        }
                    }
                }
            }
        }
        if(mask_candidate && vertex && !mask_vertex_reported){
            mask_vertex_reported=true;const auto& words=vertex->ucode_data();
            REXLOG_INFO("[em1] masked interpolation program={:08X} context={:08X} interpolator={:08X} vte={:08X}",
                registers[rex::graphics::XE_GPU_REG_SQ_PROGRAM_CNTL],registers[rex::graphics::XE_GPU_REG_SQ_CONTEXT_MISC],
                registers[rex::graphics::XE_GPU_REG_SQ_INTERPOLATOR_CNTL],registers[rex::graphics::XE_GPU_REG_PA_CL_VTE_CNTL]);
            REXLOG_INFO("[em1] masked linked VS disassembly\n{}",vertex->ucode_disassembly());
            for(size_t i=0;i+2<words.size() && i<39;i+=3)
                REXLOG_INFO("[em1] masked linked VS instruction {}: {:08X} {:08X} {:08X}",i/3,words[i],words[i+1],words[i+2]);
        }
        if(memory && vertex && pixel && ((pixel->ucode_data_hash()==0x156CCAE4A1E0E148ull &&
            vertex->ucode_data_hash()==0x10DE0C43AFB711D5ull) || mask_candidate) && uv_seen.size()<128){
            const auto fetch=registers.GetVertexFetch(93);
            const uint32_t address=(fetch.address<<2)&0x1fffffff,bytes=fetch.size*4;
            if(uv_seen.insert({address,bytes,unsigned(fetch.endian)}).second){
                std::array<uint32_t,2048> words{};SIZE_T read=0;
                const size_t wanted=(std::min)(size_t(bytes),sizeof(words));
                const bool copied=wanted>=8 && ReadProcessMemory(GetCurrentProcess(),memory->TranslatePhysical(address),words.data(),wanted,&read) && read==wanted;
                float min_u=INFINITY,max_u=-INFINITY,min_v=INFINITY,max_v=-INFINITY;unsigned finite=0;
                if(copied)for(size_t i=0;i+1<wanted/4;i+=2){
                    const auto u=std::bit_cast<float>(rex::graphics::xenos::GpuSwap(words[i],fetch.endian));
                    const auto v=std::bit_cast<float>(rex::graphics::xenos::GpuSwap(words[i+1],fetch.endian));
                    if(!std::isfinite(u)||!std::isfinite(v))continue;
                    ++finite;min_u=(std::min)(min_u,u);max_u=(std::max)(max_u,u);min_v=(std::min)(min_v,v);max_v=(std::max)(max_v,v);
                }
                REXLOG_INFO("[em1] live UV fetch93 addr={:08X} bytes={} endian={} copied={} finite={} U={}..{} V={}..{} firstRaw={:08X}/{:08X}/{:08X}/{:08X}",
                    address,bytes,unsigned(fetch.endian),copied,finite,min_u,max_u,min_v,max_v,words[0],words[1],words[2],words[3]);
                if(mask_candidate)for(unsigned slot:{1u,3u,2u}){
                    const auto texture=registers.GetTextureFetch(slot);
                    REXLOG_INFO("[em1] masked binding UV={:08X} indices={} slot={} base={:08X} size={}x{} repeat={}/{}",
                        address,count,slot,uint32_t(texture.base_address<<12),unsigned(texture.size_2d.width)+1,
                        unsigned(texture.size_2d.height)+1,unsigned(texture.clamp_x),unsigned(texture.clamp_y));
                    REXLOG_INFO("[em1] masked sampler UV={:08X} slot={} mag={} min={} mip={} lodBias={} signs={}/{}/{}/{} expAdjust={}",address,slot,unsigned(texture.mag_filter),unsigned(texture.min_filter),unsigned(texture.mip_filter),int(texture.lod_bias),unsigned(texture.sign_x),unsigned(texture.sign_y),unsigned(texture.sign_z),unsigned(texture.sign_w),int(texture.exp_adjust));
                    std::array<uint32_t,8> head{};SIZE_T texture_read=0;
                    const bool texture_copied=ReadProcessMemory(GetCurrentProcess(),
                        memory->TranslatePhysical(uint32_t(texture.base_address<<12)),
                        head.data(),sizeof(head),&texture_read) && texture_read==sizeof(head);
                    REXLOG_INFO("[em1] masked texture UV={:08X} slot={} format={} endian={} tiled={} pitch={} swizzle={:03X} mips={}..{} packed={} mipBase={:08X} copied={} raw={:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}",
                        address,slot,unsigned(texture.format),unsigned(texture.endianness),unsigned(texture.tiled),
                        unsigned(texture.pitch),unsigned(texture.swizzle),unsigned(texture.mip_min_level),unsigned(texture.mip_max_level),
                        unsigned(texture.packed_mips),uint32_t(texture.mip_address<<12),texture_copied,
                        head[0],head[1],head[2],head[3],head[4],head[5],head[6],head[7]);
                }
            }
        }
        // Keep cutout raster transitions observable even after the general
        // state budget fills, and include raster state in their identity.
        if(pixel && vertex && pixel->ucode_data_hash()==0x62EA4FFDFD73B787ull && vertex->ucode_data_hash()==0xD932231205D5BC44ull) {
            static std::set<std::array<uint32_t,2>> cutout_raster_seen;
            static std::set<std::array<uint32_t,5>> cutout_fetch_seen;
            const auto raster=registers.Get<rex::graphics::reg::PA_SU_SC_MODE_CNTL>();
            const auto cutout_depth=registers.Get<rex::graphics::reg::RB_DEPTHCONTROL>();
            const auto cutout_mask=registers[rex::graphics::XE_GPU_REG_RB_COLOR_MASK];
            if(cutout_raster_seen.size()<32 && cutout_raster_seen.insert({raster.value,cutout_depth.value}).second)
                REXLOG_INFO("[em1] cutout raster frame={} count={} mode={:08X} cullFront={} cullBack={} depth={:08X}",frame,count,raster.value,unsigned(raster.cull_front),unsigned(raster.cull_back),cutout_depth.value);
            for(unsigned slot:{93u,95u}) {
                const auto fetch=registers.GetVertexFetch(slot);
                const uint32_t address=(fetch.address<<2)&0x1fffffff;
                const uint32_t peer=(registers.GetVertexFetch(slot==93?95:93).address<<2)&0x1fffffff;
                if(cutout_fetch_seen.size()>=64 || !cutout_fetch_seen.insert({slot,address,peer,cutout_depth.value,cutout_mask}).second)continue;
                std::array<uint32_t,6> raw{};SIZE_T read=0;
                const bool copied=memory && fetch.size>=6 && ReadProcessMemory(GetCurrentProcess(),memory->TranslatePhysical(address),raw.data(),sizeof(raw),&read) && read==sizeof(raw);
                REXLOG_INFO("[em1] cutout vertex fetch{} address={:08X} peer={:08X} bytes={} endian={} copied={} count={} raster={:08X} raw={:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X} depth={:08X} colorMask={:08X}",slot,address,peer,fetch.size*4,unsigned(fetch.endian),copied,count,raster.value,raw[0],raw[1],raw[2],raw[3],raw[4],raw[5],cutout_depth.value,cutout_mask);
            }
        }
        if(seen.size()>=512)return;
        const auto depth=registers.Get<rex::graphics::reg::RB_DEPTHCONTROL>();
        const auto raster=registers.Get<rex::graphics::reg::PA_SU_SC_MODE_CNTL>();
        uint64_t clamps=0;
        for(unsigned i=0;i<8;++i){const auto f=registers.GetTextureFetch(i);
            clamps|=(uint64_t(f.clamp_x)|(uint64_t(f.clamp_y)<<3))<<(i*6);}
        const std::array<uint64_t,10> key={vertex?vertex->ucode_data_hash():0,pixel?pixel->ucode_data_hash():0,
            depth.value,registers[rex::graphics::XE_GPU_REG_RB_DEPTH_INFO],registers[rex::graphics::XE_GPU_REG_RB_COLOR_MASK],
            registers[rex::graphics::XE_GPU_REG_RB_BLENDCONTROL0],registers[rex::graphics::XE_GPU_REG_RB_COLORCONTROL],clamps,raster.value,
            registers[rex::graphics::XE_GPU_REG_RB_COLOR_MASK]?count:0};
        if(!seen.insert(key).second)return;
        REXLOG_INFO("[em1] draw state frame={} count={} vs={:016X} ps={:016X} depth={:08X} test={} write={} compare={} depthInfo={:08X} colorMask={:08X} blend={:08X} colorControl={:08X}",
            frame,count,key[0],key[1],depth.value,unsigned(depth.z_enable),unsigned(depth.z_write_enable),unsigned(depth.zfunc),key[3],key[4],key[5],key[6]);
        const auto color_control=registers.Get<rex::graphics::reg::RB_COLORCONTROL>();
        REXLOG_INFO("[em1] draw alpha enabled={} compare={} reference={} cullFront={} cullBack={}",
            unsigned(color_control.alpha_test_enable),unsigned(color_control.alpha_func),
            std::bit_cast<float>(registers[rex::graphics::XE_GPU_REG_RB_ALPHA_REF]),unsigned(raster.cull_front),unsigned(raster.cull_back));
        for(unsigned i=0;i<4;++i){const auto f=registers.GetTextureFetch(i);
            REXLOG_INFO("[em1] draw sampler slot={} clampU={} clampV={} base={:08X} width={} height={}",i,unsigned(f.clamp_x),unsigned(f.clamp_y),uint32_t(f.base_address<<12),unsigned(f.size_2d.width)+1,unsigned(f.size_2d.height)+1);}
    }catch(...){/* Diagnostics must not alter native draw execution. */}
}
bool ControlledPixel(const uint32_t* words,size_t count) {
    const std::array<std::span<const uint32_t>,7> programs={shadow_test_data::original,
        bloom_test_data::horizontal_original,bloom_test_data::horizontal_linear,
        bloom_test_data::vertical_original,bloom_test_data::vertical_linear,
        bloom_test_data::composite,bloom_test_data::composite_linear};
    for(auto program:programs)if(count==program.size()) {
        bool match=true;for(size_t i=0;i<count;++i)if(std::byteswap(words[i])!=program[i]){match=false;break;}
        if(match)return true;
    }
    return false;
}
}
rex::graphics::Shader* Em2CommandProcessor::LoadShader(rex::graphics::xenos::ShaderType type,uint32_t address,const uint32_t* words,uint32_t count) {
    scene_draw_inputs_.reset();
    scene_replay_.reset();
    scene_depth_view_.reset();
    auto* shader=D3D12CommandProcessor::LoadShader(type,address,words,count);
    if(type==rex::graphics::xenos::ShaderType::kPixel) {
        live_pixel_source_.clear();live_pixel_expected_=shader;live_pixel_address_=address;
        if(shader && ControlledPixel(words,count))live_pixel_source_.assign(words,words+count);
        live_shadow_=NativeShadowSamples();live_bloom_=NativeBloomSampling();
    }
    return shader;
}
bool Em2CommandProcessor::RefreshLivePixelShader() {
    if(live_pixel_source_.empty() || !active_pixel_shader_ || active_pixel_shader_!=live_pixel_expected_)return true;
    const auto shadow=NativeShadowSamples(),bloom=NativeBloomSampling();
    if(shadow==live_shadow_ && bloom==live_bloom_)return true;
    // Reload the retained guest source through the normal SDK path. It selects
    // the new program before pipeline, constants, textures and root signatures
    // are prepared by IssueDraw. Old shader/PSO objects stay cache-owned.
    auto* replacement=D3D12CommandProcessor::LoadShader(rex::graphics::xenos::ShaderType::kPixel,
        live_pixel_address_,live_pixel_source_.data(),uint32_t(live_pixel_source_.size()));
    if(!replacement){REXLOG_ERROR("Live pixel-shader settings could not be applied");return false;}
    active_pixel_shader_=live_pixel_expected_=replacement;live_shadow_=shadow;live_bloom_=bloom;return true;
}
bool Em2CommandProcessor::IssueDraw(rex::graphics::xenos::PrimitiveType type,uint32_t count,IndexBufferInfo* indices,bool explicit_mode) {
    scene_draw_inputs_.reset();
    scene_replay_.reset();
    scene_depth_view_.reset();
    const auto mark=MarkSceneDraw(*this);
    static bool grass_geometry_reported=false;
    if(!grass_geometry_reported && REXCVAR_GET(em1_trace_draw_state) && em1::ArchitecturePreviewGraphicsReady() &&
        count==7155 && active_vertex_shader_ && active_vertex_shader_->ucode_data_hash()==0xD932231205D5BC44ull) {
        grass_geometry_reported=true;
        try {
            const auto buffer=[&](unsigned slot,uint32_t address,size_t bytes,unsigned endian) {
                if(!bytes || bytes>1024*1024)return;
                std::vector<uint8_t> data(bytes);SIZE_T read=0;
                if(!ReadProcessMemory(GetCurrentProcess(),memory_->TranslatePhysical(address&0x1fffffff),data.data(),bytes,&read) || read!=bytes)return;
                uint64_t hash=14695981039346656037ull;for(uint8_t byte:data)hash=(hash^byte)*1099511628211ull;
                REXLOG_INFO("[em1] grass geometry slot={} address={:08X} bytes={} endian={} fnv={:016X}",slot,address,bytes,endian,hash);
            };
            for(unsigned slot:{93u,94u,95u}) {const auto f=register_file_->GetVertexFetch(slot);buffer(slot,f.address<<2,f.size*4,unsigned(f.endian));}
            if(indices)buffer(0,indices->guest_base,size_t(count)*(unsigned(indices->format)?4:2),unsigned(indices->endianness));
            REXLOG_INFO("[em1] grass draw primitive={} indexed={} format={} indexOffset={} minVertex={} maxVertex={}",unsigned(type),indices!=nullptr,indices?unsigned(indices->format):0,
                (*register_file_)[rex::graphics::XE_GPU_REG_VGT_INDX_OFFSET],(*register_file_)[rex::graphics::XE_GPU_REG_VGT_MIN_VTX_INDX],(*register_file_)[rex::graphics::XE_GPU_REG_VGT_MAX_VTX_INDX]);
        }catch(...){/* Private diagnostic only. */}
    }
    TraceEm1Draw(active_vertex_shader_,active_pixel_shader_,*register_file_,memory_,GetCurrentFrame(),count);
    const auto binding=active_pixel_shader_ ? em1::CharacterLightingRegister(active_pixel_shader_->ucode_data()):std::nullopt;
    const auto authored=binding ? em1::CurrentPreviewLighting():nullptr;
    const unsigned first=binding.value_or(0);
    std::optional<ScopedCharacterLighting> restore_lighting;
    if(authored){restore_lighting.emplace(*this,first);restore_lighting->Apply(authored->coefficients);}
    static bool authored_lighting_reported=false;
    if(authored && !authored_lighting_reported){authored_lighting_reported=true;REXLOG_INFO("[em1] native Mickey SH bound: first=c{} actor={:08X} position={}/{}/{} ambient={}/{}/{}; guest constants restored after draw",first,authored->entity,authored->position[0],authored->position[1],authored->position[2],authored->coefficients[0][0],authored->coefficients[0][1],authored->coefficients[0][2]);}
    const bool accepted=RefreshLivePixelShader() && D3D12CommandProcessor::IssueDraw(type,count,indices,explicit_mode);
    if(accepted && REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_online_clothing_bind) && active_pixel_shader_ && active_pixel_shader_->is_ucode_analyzed()){
        struct RestoreTraceState {unsigned fp=_mm_getcsr();DWORD error=GetLastError();~RestoreTraceState(){_mm_setcsr(fp);SetLastError(error);}} restore_trace;
        const auto a=register_file_->GetTextureFetch(0),b=register_file_->GetTextureFetch(1),c=register_file_->GetTextureFetch(2);
        const auto& bindings=active_pixel_shader_->texture_bindings();
        constexpr std::array slots{0u,1u,2u};const bool used=std::all_of(slots.begin(),slots.end(),[&](unsigned slot){return std::any_of(bindings.begin(),bindings.end(),[&](const auto& binding){return binding.fetch_constant==slot;});});
        if(used && uint32_t(a.type)==2 && uint32_t(a.dimension)==1 && a.size_2d.width==255 && a.size_2d.height==255){
            static std::vector<std::array<uint64_t,4>> seen;const std::array<uint64_t,4> key{active_pixel_shader_->ucode_data_hash(),(a.base_address<<12)&0x1FFFFFFFu,(b.base_address<<12)&0x1FFFFFFFu,(c.base_address<<12)&0x1FFFFFFFu};
            if(seen.size()<256 && std::find(seen.begin(),seen.end(),key)==seen.end()){seen.push_back(key);REXLOG_INFO("Native Room clothing GPU draw sources: shader {:016X} diffuse {:08X} normal {:08X} specular {:08X} count {}; analyzed original material references all three fetches, accepted native draw",key[0],uint32_t(key[1]),uint32_t(key[2]),uint32_t(key[3]),count);}
        }
    }
    transfer_benchmark_draws+=accepted;
    if(accepted && count>=3)scene_draw_inputs_=CaptureSceneDrawInputs(active_vertex_shader_,active_pixel_shader_,*register_file_,GetCurrentFrame(),++scene_draw_serial_);
    if(scene_draw_inputs_ && !IsSceneColourRaster(*register_file_))scene_draw_inputs_.reset();
    if(scene_draw_inputs_){scene_draw_inputs_->raster=InspectSceneDraw(*this,mark);if(!scene_draw_inputs_->raster)scene_draw_inputs_.reset();}
    if(scene_draw_inputs_){scene_depth_view_=AcquireCurrentSceneDepthView(*this,scene_draw_inputs_->depth_fetch,scene_depth_view_cache_);scene_depth_view_cache_=scene_depth_view_;}
    if(scene_draw_inputs_ && scene_depth_view_){scene_replay_=CaptureSceneReplay(*this,mark);if(scene_replay_)scene_replay_->scene.draw=scene_draw_inputs_->draw;}
    UpdateSceneLighting();
    return accepted;
}
bool TestLiveShaderPolicy(Em2CommandProcessor& cp,rex::memory::Memory& memory) {
    struct Restore {
        Em2CommandProcessor& cp;rex::graphics::Shader* active;std::vector<uint32_t> source;
        uint32_t address;rex::graphics::Shader* expected;unsigned tracked_shadow,tracked_bloom,shadow,bloom;
        ~Restore(){cp.active_pixel_shader_=active;cp.live_pixel_source_=std::move(source);cp.live_pixel_address_=address;cp.live_pixel_expected_=expected;cp.live_shadow_=tracked_shadow;cp.live_bloom_=tracked_bloom;ConfigureShadowKernel(shadow);ConfigureBloomSampling(bloom);}
    }restore{cp,cp.active_pixel_shader_,cp.live_pixel_source_,cp.live_pixel_address_,cp.live_pixel_expected_,cp.live_shadow_,cp.live_bloom_,NativeShadowSamples(),NativeBloomSampling()};
    const auto address=memory.SystemHeapAlloc(16384);if(!address)return false;
    struct Free {rex::memory::Memory& memory;uint32_t address;~Free(){memory.SystemHeapFree(address);}}free{memory,address};
    auto* source=memory.TranslateVirtual<uint32_t*>(address);unsigned checks=0;
    auto matches=[&](std::span<const uint32_t> wanted) {
        ++checks;const auto* shader=cp.active_pixel_shader_;
        return shader && shader->ucode_dword_count()==wanted.size() && std::equal(wanted.begin(),wanted.end(),shader->ucode_data().begin());
    };
    auto bind=[&](std::span<const uint32_t> words){for(size_t i=0;i<words.size();++i)source[i]=std::byteswap(words[i]);cp.active_pixel_shader_=cp.LoadShader(rex::graphics::xenos::ShaderType::kPixel,address,source,uint32_t(words.size()));return cp.active_pixel_shader_!=nullptr;};
    ConfigureShadowKernel(4);if(!bind(shadow_test_data::original))return false;
    for(unsigned quality:{8,16,20,4,20,8,4}) {
        ConfigureShadowKernel(quality);if(!cp.RefreshLivePixelShader())return false;
        const std::span<const uint32_t> wanted=quality==4?std::span<const uint32_t>(shadow_test_data::original):quality==8?std::span<const uint32_t>(shadow_test_data::samples8):quality==16?std::span<const uint32_t>(shadow_test_data::samples16):std::span<const uint32_t>(shadow_test_data::adaptive16);
        if(!matches(wanted)){REXLOG_ERROR("Live shadow policy mismatch at {}",quality);return false;}
    }
    for(bool vertical:{false,true}) {
        ConfigureBloomSampling(0);if(!bind(vertical?std::span<const uint32_t>(bloom_test_data::vertical_linear):std::span<const uint32_t>(bloom_test_data::horizontal_linear)))return false;
        for(unsigned quality:{2,1,0,1,2,0}) {
            ConfigureBloomSampling(quality);if(!cp.RefreshLivePixelShader())return false;
            const auto wanted=quality==0?DisabledBloomShader():vertical?(quality==2?std::span<const uint32_t>(bloom_test_data::vertical_linear):std::span<const uint32_t>(bloom_test_data::vertical_original)):(quality==2?std::span<const uint32_t>(bloom_test_data::horizontal_linear):std::span<const uint32_t>(bloom_test_data::horizontal_original));
            if(!matches(wanted))return false;
        }
    }
    ConfigureBloomSampling(1);if(!bind(bloom_test_data::composite))return false;
    for(unsigned quality:{2,1,0,2}){ConfigureBloomSampling(quality);if(!cp.RefreshLivePixelShader() || !matches(quality==2?std::span<const uint32_t>(bloom_test_data::composite_linear):std::span<const uint32_t>(bloom_test_data::composite)))return false;}
    const auto& unrelated=shadow_test_data::control;
    if(!bind(unrelated))return false;const auto* unchanged=cp.active_pixel_shader_;ConfigureBloomSampling(0);ConfigureShadowKernel(16);
    if(!cp.RefreshLivePixelShader() || cp.active_pixel_shader_!=unchanged)return false;
    // Exercise the production override against the real register file. Distinct
    // sentinels catch accidental W-lane, adjacent-register and stale SH writes.
    constexpr auto pixel_base=rex::graphics::XE_GPU_REG_SHADER_CONSTANT_256_X;
    struct RestorePixelBank {
        Em2CommandProcessor& owner;std::array<uint32_t,1024> original{};
        RestorePixelBank(Em2CommandProcessor& cp):owner(cp){std::copy_n(cp.ConstantRegisterValues()+pixel_base,original.size(),original.begin());}
        ~RestorePixelBank(){for(unsigned i=0;i<original.size();++i)owner.WriteNativeConstant(pixel_base+i,original[i]);}
    }restore_pixels{cp};
    std::array<uint32_t,1024> sentinels{};
    for(unsigned i=0;i<sentinels.size();++i){sentinels[i]=0x3f000000+i;cp.WriteNativeConstant(pixel_base+i,sentinels[i]);}
    std::array<std::array<float,3>,9> coefficients{};
    for(unsigned c=0;c<9;++c)for(unsigned lane=0;lane<3;++lane)coefficients[c][lane]=(float(c*3+lane)-13.f)*.25f;
    for(const auto& entry:em1::character_lighting_data::bindings){
        if(!bind(entry.code) || em1::CharacterLightingRegister(cp.active_pixel_shader_->ucode_data())!=entry.first)return false;
        for(bool unwind:{false,true}){
            bool isolated=true;
            try {
                ScopedCharacterLighting lighting(cp,entry.first);lighting.Apply(coefficients);
                for(unsigned i=0;i<sentinels.size();++i){
                    uint32_t expected=sentinels[i];const unsigned reg=i/4,lane=i%4;
                    if(reg>=entry.first && reg<entry.first+9 && lane<3)expected=std::bit_cast<uint32_t>(coefficients[reg-entry.first][lane]);
                    isolated&=cp.ConstantRegisterValues()[pixel_base+i]==expected;
                }
                if(unwind)throw 1;
            }catch(int){}
            if(!isolated || !std::equal(sentinels.begin(),sentinels.end(),cp.ConstantRegisterValues()+pixel_base))return false;
        }
    }
    REXLOG_INFO("EM1 character lighting register isolation passed: four loaded native variants, RGB-only writes, all 1024 pixel constants restored after normal and exception exits");
    REXLOG_INFO("Live shader policy passed {} real command-processor transitions: shadow modes, both Gaussian orientations across shared Off shader, composite and unrelated preservation; live draw appearance unverified",checks);
    return true;
}
}

