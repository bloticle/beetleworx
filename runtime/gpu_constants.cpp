#include "gpu_constants.h"
#include "gpu_constants_internal.h"
#include "gpu_command_profile.h"
#include "gpu_execution_profile.h"
#include "gpu_draw_profile.h"
#include "resolve_write_observer.h"
#include "em1_preview_capture.h"
#include "shadow_readback.h"
#include <rex/logging.h>
#include <rex/cvar.h>
#include <rex/graphics/d3d12/graphics_system.h>
#include <rex/graphics/d3d12/texture_cache.h>
#include <rex/memory/ring_buffer.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <immintrin.h>

REXCVAR_DEFINE_BOOL(em2_deduplicate_gpu_constants,true,"GPU","Keep current GPU constant buffers valid when float/bool/loop register writes repeat their exact current bits");
REXCVAR_DEFINE_BOOL(em2_batch_gpu_constant_reads,true,"GPU","Read large immediate shader/fetch packets in bulk; fetch words always retain invalidation");
REXCVAR_DEFINE_BOOL(em2_inline_gpu_constant_writes,true,"GPU","Inline known shader/fetch register stores and native invalidation; false uses original SDK writer");
REXCVAR_DECLARE(bool,em2_profile_render);
REXCVAR_DECLARE(bool,em2_profile_gpu_draws);
REXCVAR_DECLARE(bool,em2_trace_shadow_gpu_state);
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(bool,em1_trace_draw_state);
namespace {
thread_local rex::memory::RingBuffer* em1_packet_reader=nullptr;
thread_local uint32_t em1_packet_header=0;
struct Em1PacketScope {
    rex::memory::RingBuffer* previous;uint32_t header;bool enabled;
    Em1PacketScope(rex::memory::RingBuffer* reader,uint32_t packet)
        :previous(nullptr),header(0),enabled(REXCVAR_GET(em1_trace_draw_state)) {
        if(enabled){previous=em1_packet_reader;header=em1_packet_header;em1_packet_reader=reader;em1_packet_header=packet;}
    }
    ~Em1PacketScope(){if(enabled){em1_packet_reader=previous;em1_packet_header=header;}}
};
}
extern "C" bool EM2OriginalPacketType0(rex::graphics::CommandProcessor*,rex::memory::RingBuffer*,uint32_t);
extern "C" bool EM2OriginalSET_CONSTANT(rex::graphics::CommandProcessor*,rex::memory::RingBuffer*,uint32_t,uint32_t);
extern "C" bool EM2OriginalSET_CONSTANT2(rex::graphics::CommandProcessor*,rex::memory::RingBuffer*,uint32_t,uint32_t);
extern "C" bool EM2OriginalSET_SHADER_CONSTANTS(rex::graphics::CommandProcessor*,rex::memory::RingBuffer*,uint32_t,uint32_t);
// These definitions use the SDK's declared member interface and private fields,
// not object-layout casts. Its original writer remains available as a bridge.
__attribute__((always_inline)) void rex::graphics::d3d12::D3D12CommandProcessor::WriteRegister(uint32_t index,uint32_t value) {
    if(index>=XE_GPU_REG_SHADER_CONSTANT_000_X && index<=XE_GPU_REG_SHADER_CONSTANT_511_W) {
        const_cast<volatile uint32_t&>(register_file_->values[index])=value;
        if(frame_open_) {
            const uint32_t slot=(index-XE_GPU_REG_SHADER_CONSTANT_000_X)>>2;
            if(slot>=256) {
                const auto pixel=slot-256;
                if(cbuffer_binding_float_pixel_.up_to_date && (current_float_constant_map_pixel_[pixel>>6]&(uint64_t(1)<<(pixel&63))))cbuffer_binding_float_pixel_.up_to_date=false;
            }else if(cbuffer_binding_float_vertex_.up_to_date && (current_float_constant_map_vertex_[slot>>6]&(uint64_t(1)<<(slot&63))))cbuffer_binding_float_vertex_.up_to_date=false;
        }
        return;
    }
    if(index>=XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 && index<=XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
        const_cast<volatile uint32_t&>(register_file_->values[index])=value;
        cbuffer_binding_bool_loop_.up_to_date=false;return;
    }
    if(index>=XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 && index<=XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
        // Equal fetch writes still invalidate. This is the installed/pinned
        // SDK's full fetch path, not a newer upstream backend's ABI.
        const_cast<volatile uint32_t&>(register_file_->values[index])=value;
        cbuffer_binding_fetch_.up_to_date=false;
        if(texture_cache_)texture_cache_->TextureFetchConstantWritten((index-XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0)/6);
        return;
    }
    EM2OriginalD3DRegister(this,index,value);
}
namespace {
// The generic SDK writer only stores these registers and checks their metadata.
// Every eligible constant is known, so that lookup cannot log an unknown write.
constexpr uint32_t known_registers[]={
#define XE_GPU_REGISTER(index,type,name) index,
#include <rex/graphics/register_table.inc>
#undef XE_GPU_REGISTER
};
static_assert([] {
    std::array<bool,rex::graphics::RegisterFile::kRegisterCount> known{};
    for(auto index:known_registers)if(index<known.size())known[index]=true;
    for(uint32_t i=0x4000;i<=0x47FF;++i)if(!known[i])return false;
    for(uint32_t i=0x4800;i<=0x48BF;++i)if(!known[i])return false;
    for(uint32_t i=0x4900;i<=0x4927;++i)if(!known[i])return false;
    return true;
}(),"Revalidate native constant-register metadata before skipping its lookup");
// Only selected at startup with profiling enabled. Ordinary draws share the
// live graphics binding path without the profiling scopes.
class ProfiledCommandProcessor final:public em2::Em2CommandProcessor {
public:
    using Em2CommandProcessor::Em2CommandProcessor;
protected:
    bool SetupContext() override {
        if(!Em2CommandProcessor::SetupContext())return false;
        try{execution_scope_=std::make_unique<em2::GpuExecutionScope>(execution_,*this,REXCVAR_GET(em2_profile_gpu_draws)?&draws_:nullptr);}catch(...){}
        return true;
    }
    void ShutdownContext() override {
        // Installed SDK starts shutdown by CheckSubmissionFence(current).
        // Retain timestamp resources until that native wait has completed.
        Em2CommandProcessor::ShutdownContext();
        const auto csr=_mm_getcsr();
        try{execution_.Poll(UINT64_MAX);execution_.FlushReport();draws_.Poll(UINT64_MAX);draws_.FlushReport();}catch(...){}
        draws_.ResetAfterIdle();
        execution_scope_.reset();execution_.ResetAfterIdle();_mm_setcsr(csr);
    }
    bool IssueDraw(rex::graphics::xenos::PrimitiveType type,uint32_t count,IndexBufferInfo* indices,bool explicit_mode) override {
        em2::GpuCommandProfiler::Call call(profile_,em2::GpuCommandKind::Draw);
        if(REXCVAR_GET(em2_trace_shadow_gpu_state) && REXCVAR_GET(em2_private_profile)){
            // Inspect only bindings referenced by an analyzed guest shader.
            // A bound R32 texture alone does not establish a shadow receiver,
            // nor does a declared fetch prove that a particular pixel samples it.
            const auto trace_textures=[&](const rex::graphics::Shader* shader,unsigned stage){
                if(!shader || !shader->is_ucode_analyzed())return;
                static std::array<std::array<uint64_t,3>,64> seen{};static unsigned used=0;
                for(const auto& binding:shader->texture_bindings()){
                    if(binding.fetch_constant>=32 || used==seen.size())continue;
                    const auto fetch=register_file_->GetTextureFetch(binding.fetch_constant);
                    const auto address=(fetch.base_address<<12)&0x1FFFFFFFu;
                    const bool probe_match=address && address==em2::ShadowPixelProbeAddress();
                    if(uint32_t(fetch.type)!=2 || (!probe_match && uint32_t(fetch.format)!=36) || uint32_t(fetch.dimension)!=1 ||
                       fetch.size_2d.width!=fetch.size_2d.height || fetch.size_2d.width<127 || fetch.size_2d.width>2047)continue;
                    const std::array<uint64_t,3> key{shader->ucode_data_hash(),address,(uint64_t(stage)<<32)|binding.fetch_constant};
                    if(std::find(seen.begin(),seen.begin()+used,key)!=seen.begin()+used)continue;
                    seen[used++]=key;
                    REXLOG_INFO("Private shadow candidate texture use: stage={} ucode={:016X} fetch={} physical={:08X} side={} format={} probe_match={} endian={} tiled={} words={:08X},{:08X},{:08X},{:08X},{:08X},{:08X}; referenced analyzed shader binding, not proof of executed receiver pixels",stage,shader->ucode_data_hash(),binding.fetch_constant,address,fetch.size_2d.width+1,uint32_t(fetch.format),probe_match,uint32_t(fetch.endianness),fetch.tiled,fetch.dword_0,fetch.dword_1,fetch.dword_2,fetch.dword_3,fetch.dword_4,fetch.dword_5);
                }
            };
            trace_textures(active_vertex_shader_,0);trace_textures(active_pixel_shader_,1);
            if(active_vertex_shader_ && active_vertex_shader_->ucode_data_hash()==0x9185E8B84A9AAF05ull && active_vertex_shader_->is_ucode_analyzed()){
                static std::array<uint32_t,32> inspected{};static unsigned inspected_count=0;
                const auto position=register_file_->GetVertexFetch(95);const auto address=position.address<<2;
                if(inspected_count<inspected.size() && position.size>=3 && position.size<=262144 && position.size%3==0 && std::find(inspected.begin(),inspected.begin()+inspected_count,address)==inspected.begin()+inspected_count){
                    inspected[inspected_count++]=address;
                    const auto csr=_mm_getcsr();
                    try{
                        const auto& map=active_vertex_shader_->constant_register_map();
                        std::array<std::array<float,4>,4> matrix{};unsigned rows=0;
                        for(unsigned i=0;i<256;++i){const auto packed=map.GetPackedFloatConstantIndex(i);if(packed<4){for(unsigned j=0;j<4;++j)matrix[packed][j]=std::bit_cast<float>((*register_file_)[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_000_X+i*4+j]);++rows;}}
                        std::vector<uint32_t> data(position.size);SIZE_T read=0;
                        if(rows==4 && ReadProcessMemory(GetCurrentProcess(),memory_->TranslatePhysical(address&0x1FFFFFFF),data.data(),data.size()*4,&read) && read==data.size()*4){
                            std::array<unsigned,6> outside{};unsigned valid=0,positive_w=0;std::vector<uint8_t> masks(data.size()/3);
                            const auto clip_control=register_file_->Get<rex::graphics::reg::PA_CL_CLIP_CNTL>();
                            float min_x=INFINITY,max_x=-INFINITY,min_y=INFINITY,max_y=-INFINITY;
                            for(size_t v=0;v<data.size();v+=3){std::array<float,4> input{0,0,0,1},clip{};
                                for(unsigned j=0;j<3;++j){auto word=data[v+j];switch(uint32_t(position.endian)){case 1:word=((word&0x00FF00FF)<<8)|((word>>8)&0x00FF00FF);break;case 2:word=std::byteswap(word);break;case 3:word=(word<<16)|(word>>16);break;}input[j]=std::bit_cast<float>(word);}
                                bool finite=true;for(unsigned r=0;r<4;++r){for(unsigned j=0;j<4;++j)clip[r]+=matrix[r][j]*input[j];finite&=std::isfinite(clip[r]);}if(!finite)continue;++valid;
                                const std::array tests{clip[0]<-clip[3],clip[0]>clip[3],clip[1]<-clip[3],clip[1]>clip[3],clip[2]<(clip_control.dx_clip_space_def?0:-clip[3]),clip[2]>clip[3]};
                                for(unsigned p=0;p<6;++p){outside[p]+=tests[p];masks[v/3]|=uint8_t(tests[p])<<p;}
                                if(clip[3]>0){++positive_w;min_x=std::min(min_x,clip[0]/clip[3]);max_x=std::max(max_x,clip[0]/clip[3]);min_y=std::min(min_y,clip[1]/clip[3]);max_y=std::max(max_y,clip[1]/clip[3]);}
                            }
                            unsigned common=0;for(unsigned p=0;p<6;++p)if(valid==data.size()/3 && outside[p]==valid)common|=1u<<p;
                            REXLOG_INFO("Private shadow raw clip bounds: address={:08X} vertices={} finite={} positive_w={} common_plane_mask={:02X} x_over_w={}..{} y_over_w={}..{} vte={:08X}; actual position buffer and packed shader matrix, pre-system-conversion bounds only",address,data.size()/3,valid,positive_w,common,min_x,max_x,min_y,max_y,(*register_file_)[rex::graphics::XE_GPU_REG_PA_CL_VTE_CNTL]);
                            if(indices && type==rex::graphics::xenos::PrimitiveType::kTriangleList && uint32_t(indices->format)==0 && uint32_t(indices->endianness)==1 && count && count%3==0 && count<=262144 && !(*register_file_)[rex::graphics::XE_GPU_REG_VGT_INDX_OFFSET]){
                                std::vector<uint16_t> index(count);SIZE_T got=0;
                                if(ReadProcessMemory(GetCurrentProcess(),memory_->TranslatePhysical(indices->guest_base&0x1FFFFFFF),index.data(),index.size()*2,&got) && got==index.size()*2){
                                    unsigned rejected=0,xy=0,z=0,unknown=0,inside=0;for(auto mask:masks)inside+=!mask;
                                    for(unsigned i=0;i<count;i+=3){uint8_t mask=63;bool known=true;
                                        for(unsigned j=0;j<3;++j){const auto v=std::byteswap(index[i+j]);if(v>=masks.size()){known=false;break;}mask&=masks[v];}
                                        if(!known){++unknown;continue;}rejected+=mask!=0;xy+=bool(mask&15);z+=bool(mask&48);
                                    }
                                    REXLOG_INFO("Private shadow indexed clip coverage: address={:08X} triangles={} rejected={} rejected_xy={} rejected_z={} unknown={} inside_vertices={} clip_control={:08X}; original indexed triangles, raw pre-system clip coordinates only",address,count/3,rejected,xy,z,unknown,inside,clip_control.value);
                                }
                            }
                        }
                    }catch(...){}
                    _mm_setcsr(csr);
                }
                static bool captured=false;
                if(!captured){captured=true;const auto csr=_mm_getcsr();
                    const auto& map=active_vertex_shader_->constant_register_map();
                    REXLOG_INFO("Private shadow vertex input: float_count={} VS_const={:08X} index_base={:08X} index_format={} index_endian={} index_offset={}",map.float_count,(*register_file_)[rex::graphics::XE_GPU_REG_SQ_VS_CONST],indices?indices->guest_base:0,indices?uint32_t(indices->format):0,indices?uint32_t(indices->endianness):0,(*register_file_)[rex::graphics::XE_GPU_REG_VGT_INDX_OFFSET]);
                    for(unsigned i=0;i<256;++i)if(map.GetPackedFloatConstantIndex(i)!=UINT32_MAX){const auto* words=register_file_->values+rex::graphics::XE_GPU_REG_SHADER_CONSTANT_000_X+i*4;REXLOG_INFO("Private shadow vertex constant c{} packed{}: {:08X} {:08X} {:08X} {:08X}",i,map.GetPackedFloatConstantIndex(i),words[0],words[1],words[2],words[3]);}
                    const auto fetch=register_file_->GetVertexFetch(95);
                    REXLOG_INFO("Private shadow vertex fetch95: address={:08X} bytes={} endian={}",fetch.address<<2,fetch.size*4,uint32_t(fetch.endian));
                    if(fetch.size>=12){std::array<uint32_t,12> words{};SIZE_T read=0;
                        if(ReadProcessMemory(GetCurrentProcess(),memory_->TranslatePhysical((fetch.address<<2)&0x1FFFFFFF),words.data(),sizeof(words),&read) && read==sizeof(words))
                            for(unsigned i=0;i<4;++i)REXLOG_INFO("Private shadow position raw{}: {:08X} {:08X} {:08X}",i,words[i*3],words[i*3+1],words[i*3+2]);}
                    _mm_setcsr(csr);
                }
            }
            const auto color=register_file_->Get<rex::graphics::reg::RB_COLOR_INFO>();
            const auto pitch=register_file_->Get<rex::graphics::reg::RB_SURFACE_INFO>().surface_pitch;
            const auto xs=(*register_file_)[rex::graphics::XE_GPU_REG_PA_CL_VPORT_XSCALE]&0x7FFFFFFFu,ys=(*register_file_)[rex::graphics::XE_GPU_REG_PA_CL_VPORT_YSCALE]&0x7FFFFFFFu;
            if(pitch==1024 || (xs==std::bit_cast<uint32_t>(512.0f) && ys==xs)){
                static std::atomic<unsigned> reports{0};
                if(reports.fetch_add(1)<32)REXLOG_INFO("Private shadow guest draw: count={} pitch={} color_format={} mode={:08X} color_mask={:08X} depth_control={:08X} depth_clear={:08X} color_info={:08X} vertex_ucode={:016X} pixel_ucode={:016X}; guest state before backend normalization, unchanged",count,pitch,uint32_t(color.color_format),register_file_->Get<rex::graphics::reg::RB_MODECONTROL>().value,(*register_file_)[rex::graphics::XE_GPU_REG_RB_COLOR_MASK],register_file_->Get<rex::graphics::reg::RB_DEPTHCONTROL>().value,(*register_file_)[rex::graphics::XE_GPU_REG_RB_DEPTH_CLEAR],color.value,active_vertex_shader_?active_vertex_shader_->ucode_data_hash():0,active_pixel_shader_?active_pixel_shader_->ucode_data_hash():0);
            }
        }
        const bool result=Em2CommandProcessor::IssueDraw(type,count,indices,explicit_mode);call.End(result);return result;
    }
    bool IssueCopy() override {
        em2::GpuCommandProfiler::Call call(profile_,em2::GpuCommandKind::Copy);
        const bool result=Em2CommandProcessor::IssueCopy();call.End(result);return result;
    }
    void IssueSwap(uint32_t frontbuffer,uint32_t width,uint32_t height) override {
        em2::GpuCommandProfiler::Call call(profile_,em2::GpuCommandKind::Swap);
        Em2CommandProcessor::IssueSwap(frontbuffer,width,height);call.End();
    }
private:
    em2::GpuCommandProfiler profile_;
    em2::GpuExecutionProfiler execution_;
    em2::GpuDrawProfiler draws_;
    std::unique_ptr<em2::GpuExecutionScope> execution_scope_;
};
constinit thread_local uint64_t constant_batches=0;
// Retain the previous complete bulk reader as the disabled-path A/B control.
__declspec(noinline) bool LegacyBatchConstants(em2::Em2CommandProcessor& cp,rex::memory::RingBuffer& reader,uint32_t index,uint32_t count,const uint32_t* registers) {
    alignas(32) std::array<uint32_t,2048> words;
    const auto saved=reader.read_offset();
    if(reader.Read(words.data(),size_t(count)*4)!=size_t(count)*4){reader.set_read_offset(saved);return false;}
    ++constant_batches;
    const bool deduplicate=REXCVAR_GET(em2_deduplicate_gpu_constants);
    const auto swap=_mm256_setr_epi8(3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12,3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12);
    uint32_t i=0;
    for(;count-i>=8;i+=8) {
        const auto incoming=_mm256_shuffle_epi8(_mm256_load_si256(reinterpret_cast<const __m256i*>(words.data()+i)),swap);
        _mm256_store_si256(reinterpret_cast<__m256i*>(words.data()+i),incoming);
        unsigned changed=0xFF;
        if(deduplicate) {
            const auto current=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(registers+i));
            changed^=unsigned(_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(current,incoming))));
        }
        while(changed){const auto lane=std::countr_zero(changed);cp.WriteOriginalConstant(index+i+lane,words[i+lane]);changed&=changed-1;}
    }
    for(;i<count;++i){const auto value=std::byteswap(words[i]);if(!deduplicate || registers[i]!=value)cp.WriteOriginalConstant(index+i,value);}
    return true;
}
// Keep native private-field bookkeeping out of the decoder's unchanged path.
// One outlined call handles all changed lanes of a vector, not one call per word.
__declspec(noinline) void ApplyConstantMask(em2::Em2CommandProcessor& cp,uint32_t index,const uint32_t* words,unsigned changed) {
    while(changed) {
        const auto lane=std::countr_zero(changed);
        cp.WriteNativeConstant(index+lane,words[lane]);changed&=changed-1;
    }
}
__declspec(noinline) bool BatchConstants(em2::Em2CommandProcessor& cp,rex::memory::RingBuffer& reader,uint32_t index,uint32_t count,const uint32_t* registers) {
    alignas(32) std::array<uint32_t,2048> words;
    const auto saved=reader.read_offset();
    if(reader.Read(words.data(),size_t(count)*4)!=size_t(count)*4){reader.set_read_offset(saved);return false;}
    ++constant_batches;
    const bool deduplicate=em2::IsDeduplicatedConstantRange(index,count) && REXCVAR_GET(em2_deduplicate_gpu_constants);
    const auto swap=_mm256_setr_epi8(3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12,3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12);
    uint32_t i=0;
    for(;count-i>=8;i+=8) {
        const auto incoming=_mm256_shuffle_epi8(_mm256_load_si256(reinterpret_cast<const __m256i*>(words.data()+i)),swap);
        _mm256_store_si256(reinterpret_cast<__m256i*>(words.data()+i),incoming);
        unsigned changed=0xFF;
        if(deduplicate) {
            const auto current=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(registers+i));
            changed^=unsigned(_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(current,incoming))));
        }
        if(changed)ApplyConstantMask(cp,index+i,words.data()+i,changed);
    }
    const auto tail=i;unsigned changed=0;
    for(;i<count;++i){words[i]=std::byteswap(words[i]);if(!deduplicate || registers[i]!=words[i])changed|=1u<<(i-tail);}
    if(changed)ApplyConstantMask(cp,index+tail,words.data()+tail,changed);
    return true;
}
__declspec(noinline) bool ConstantPacketOrOriginal(rex::graphics::CommandProcessor* base,rex::memory::RingBuffer* reader,uint32_t packet,const uint32_t* registers) {
    const auto count=((packet>>16)&0x3FFF)+1,index=packet&0x7FFF;
    if(em2::IsBulkGpuRegisterRange(index,count) && reader->capacity()>=size_t(count)*4 && reader->read_count()>=size_t(count)*4)
        if(auto* cp=dynamic_cast<em2::Em2CommandProcessor*>(base)) {
            const bool specialized=REXCVAR_GET(em2_inline_gpu_constant_writes);
            // The original bulk reader may suppress equal shader writes. Fetch
            // packets instead retain the original scalar handler when disabled.
            if(specialized){if(BatchConstants(*cp,*reader,index,count,registers))return true;}
            else if(em2::IsDeduplicatedConstantRange(index,count) && LegacyBatchConstants(*cp,*reader,index,count,registers))return true;
        }
    return EM2OriginalPacketType0(base,reader,packet);
}
template<unsigned Form> bool OriginalType3(rex::graphics::CommandProcessor* cp,rex::memory::RingBuffer* reader,uint32_t packet,uint32_t count) {
    if constexpr(Form==0)return EM2OriginalSET_CONSTANT(cp,reader,packet,count);
    else if constexpr(Form==1)return EM2OriginalSET_CONSTANT2(cp,reader,packet,count);
    else return EM2OriginalSET_SHADER_CONSTANTS(cp,reader,packet,count);
}
template<unsigned Form> __declspec(noinline) bool Type3ConstantsOrOriginal(rex::graphics::CommandProcessor* base,rex::memory::RingBuffer* reader,uint32_t packet,uint32_t count) {
    const auto saved=reader->read_offset();
    if(reader->capacity()>=size_t(count)*4 && reader->read_count()>=size_t(count)*4)
        if(auto* cp=dynamic_cast<em2::Em2CommandProcessor*>(base)) {
            uint32_t encoded=0;
            if(reader->Read(&encoded,4)==4) {
                encoded=std::byteswap(encoded);uint32_t index=encoded&0xFFFF;bool supported=true;
                if constexpr(Form==0) {
                    constexpr uint32_t bases[]={0x4000,0x4800,0x4900,0x4908,0x2000};
                    const auto type=(encoded>>16)&0xFF;
                    if(type<5)index=bases[type]+(encoded&0x7FF);else supported=false;
                }
                if(supported && em2::IsBulkGpuRegisterRange(index,count-1)) {
                    const bool specialized=REXCVAR_GET(em2_inline_gpu_constant_writes);
                    const auto* registers=cp->ConstantRegisterValues()+index;
                    if(specialized){if(BatchConstants(*cp,*reader,index,count-1,registers))return true;}
                    else if(em2::IsDeduplicatedConstantRange(index,count-1) && LegacyBatchConstants(*cp,*reader,index,count-1,registers))return true;
                }
            }
            // Peeking at an unsupported offset/type must not consume it before
            // native handling; no writes happen until a complete bulk read.
            reader->set_read_offset(saved);
        }
    return OriginalType3<Form>(base,reader,packet,count);
}
}
uint64_t em2::GpuConstantBatchCount(){return constant_batches;}
bool em2::IsGpuCommandProfilingEnabled(const rex::graphics::CommandProcessor& cp){return dynamic_cast<const ProfiledCommandProcessor*>(&cp)!=nullptr;}
bool rex::graphics::CommandProcessor::ExecutePacketType0(rex::memory::RingBuffer* reader,uint32_t packet) {
    Em1PacketScope trace(reader,packet);
    const auto count=((packet>>16)&0x3FFF)+1,index=packet&0x7FFF;
    if(count>=16 && count<=2048 && !(packet&0x8000) && index>=0x4000 && index<=0x4927 &&
       REXCVAR_GET(em2_batch_gpu_constant_reads))
        return ConstantPacketOrOriginal(this,reader,packet,register_file_->values+index);
    return EM2OriginalPacketType0(this,reader,packet);
}

bool rex::graphics::CommandProcessor::ExecutePacketType3_SET_CONSTANT(rex::memory::RingBuffer* reader,uint32_t packet,uint32_t count) {
    Em1PacketScope trace(reader,packet);
    if(count>=17 && count<=2049 && REXCVAR_GET(em2_batch_gpu_constant_reads))return Type3ConstantsOrOriginal<0>(this,reader,packet,count);
    return EM2OriginalSET_CONSTANT(this,reader,packet,count);
}
bool rex::graphics::CommandProcessor::ExecutePacketType3_SET_CONSTANT2(rex::memory::RingBuffer* reader,uint32_t packet,uint32_t count) {
    Em1PacketScope trace(reader,packet);
    if(count>=17 && count<=2049 && REXCVAR_GET(em2_batch_gpu_constant_reads))return Type3ConstantsOrOriginal<1>(this,reader,packet,count);
    return EM2OriginalSET_CONSTANT2(this,reader,packet,count);
}
bool rex::graphics::CommandProcessor::ExecutePacketType3_SET_SHADER_CONSTANTS(rex::memory::RingBuffer* reader,uint32_t packet,uint32_t count) {
    Em1PacketScope trace(reader,packet);
    if(count>=17 && count<=2049 && REXCVAR_GET(em2_batch_gpu_constant_reads))return Type3ConstantsOrOriginal<2>(this,reader,packet,count);
    return EM2OriginalSET_SHADER_CONSTANTS(this,reader,packet,count);
}
bool em2::IsDeduplicatedConstantRange(uint32_t index,uint32_t count) {
    using namespace rex::graphics;
    if(!count)return false;
    const auto end=uint64_t(index)+count-1;
    return (index>=XE_GPU_REG_SHADER_CONSTANT_000_X && end<=XE_GPU_REG_SHADER_CONSTANT_511_W) ||
        (index>=XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 && end<=XE_GPU_REG_SHADER_CONSTANT_LOOP_31);
}
bool em2::IsBulkGpuRegisterRange(uint32_t index,uint32_t count) {
    return IsDeduplicatedConstantRange(index,count) || (count && index>=0x4800 && uint64_t(index)+count-1<=0x48BF);
}
void em2::Em2CommandProcessor::WriteRegister(uint32_t index,uint32_t value) {
    if(index==rex::graphics::XE_GPU_REG_PA_SU_SC_MODE_CNTL && REXCVAR_GET(em1_trace_draw_state) && em1::ArchitecturePreviewGraphicsReady()) {
        try {
            static uint64_t first=GetCurrentFrame();static unsigned writes=0;
            if(GetCurrentFrame()-first<8 && writes<512 && register_file_->values[index]!=value) {
                uintptr_t address=0;
                if(em1_packet_reader && em1_packet_reader->capacity()>=4)
                    address=reinterpret_cast<uintptr_t>(em1_packet_reader->buffer())+(em1_packet_reader->read_offset()+em1_packet_reader->capacity()-4)%em1_packet_reader->capacity();
                const auto origin=reinterpret_cast<uintptr_t>(memory_->physical_membase());
                const uint32_t physical=address>=origin && address-origin<0x20000000 ? uint32_t(address-origin) : UINT32_MAX;
                REXLOG_INFO("[em1] raster write sequence={} frame={} old={:08X} new={:08X} packet={:08X} wordPhysical={:08X} ps={:016X}",++writes,GetCurrentFrame(),register_file_->values[index],value,em1_packet_header,physical,active_pixel_shader_?active_pixel_shader_->ucode_data_hash():0);
            }
        }catch(...){}
    }
    if(REXCVAR_GET(em2_deduplicate_gpu_constants) && IsDeduplicatedConstantRange(index,1) && register_file_->values[index]==value)return;
    if(REXCVAR_GET(em2_inline_gpu_constant_writes))D3D12CommandProcessor::WriteRegister(index,value);
    else EM2OriginalD3DRegister(this,index,value);
}
std::unique_ptr<rex::graphics::CommandProcessor> rex::graphics::d3d12::D3D12GraphicsSystem::CreateCommandProcessor() {
    // A real derived instance uses the SDK's original constructor and lifecycle.
    static_assert(sizeof(D3D12CommandProcessor)==0x1288,"Revalidate SDK command-processor ABI before replacing its factory");
    if(REXCVAR_GET(em2_profile_render) || REXCVAR_GET(em2_profile_gpu_draws))return std::make_unique<ProfiledCommandProcessor>(this,kernel_state_);
    return std::make_unique<em2::Em2CommandProcessor>(this,kernel_state_);
}
