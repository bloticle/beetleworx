#include "gpu_constants.h"
#include "gpu_constants_internal.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/kernel/xmemory.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/d3d12/texture_cache.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <future>
#include <functional>
#include <vector>
#include <rex/memory/ring_buffer.h>
#include <immintrin.h>

REXCVAR_DECLARE(bool,em2_deduplicate_gpu_constants);
REXCVAR_DECLARE(bool,em2_batch_gpu_constant_reads);
REXCVAR_DECLARE(bool,em2_profile_render);
REXCVAR_DECLARE(bool,em2_profile_gpu_draws);
REXCVAR_DECLARE(bool,em2_inline_gpu_constant_writes);
REXCVAR_DEFINE_BOOL(em2_benchmark_gpu_constants,false,"Debug","Benchmark real shader-constant PM4 packets in the headless renderer diagnostic");
namespace {
using CP=rex::graphics::d3d12::D3D12CommandProcessor;
// Test-only member pointers use the C++ explicit-instantiation access exemption.
// The SDK headers, object layout and production access control are unchanged.
template<class Tag,auto Member>struct ProbeMember {friend auto Get(Tag){return Member;}};
#define EM2_PROBE(member) struct member##_tag{friend auto Get(member##_tag);};template struct ProbeMember<member##_tag,&CP::member>;
EM2_PROBE(frame_open_)
EM2_PROBE(current_float_constant_map_vertex_)
EM2_PROBE(current_float_constant_map_pixel_)
EM2_PROBE(cbuffer_binding_float_vertex_)
EM2_PROBE(cbuffer_binding_float_pixel_)
EM2_PROBE(cbuffer_binding_bool_loop_)
EM2_PROBE(cbuffer_binding_fetch_)
EM2_PROBE(texture_cache_)
#undef EM2_PROBE
struct texture_sync_tag{friend auto Get(texture_sync_tag);};
template struct ProbeMember<texture_sync_tag,&rex::graphics::TextureCache::texture_bindings_in_sync_>;
struct ScopeExit {std::function<void()> fn;~ScopeExit(){fn();}};
struct DetachTextureCache {
    using Owner=std::unique_ptr<rex::graphics::d3d12::D3D12TextureCache>;
    Owner& owner;Owner held;
    DetachTextureCache(Owner& value,bool detach):owner(value){if(detach)held=std::move(owner);}
    ~DetachTextureCache(){if(held)owner=std::move(held);}
};

bool CheckBackend(em2::Em2CommandProcessor& cp,rex::graphics::GraphicsSystem& graphics) {
    using namespace rex::graphics;
    for(uint32_t index=0x4000;index<=0x4927;++index)if((em2::IsDeduplicatedConstantRange(index,1) || (index>=0x4800 && index<=0x48BF)) && !RegisterFile::GetRegisterInfo(index)){
        REXLOG_ERROR("SDK constant metadata missing for {:04X}",index);return false;
    }
    auto& frame=cp.*Get(frame_open__tag{});
    auto& map_v=cp.*Get(current_float_constant_map_vertex__tag{});
    auto& map_p=cp.*Get(current_float_constant_map_pixel__tag{});
    auto& cb_v=cp.*Get(cbuffer_binding_float_vertex__tag{});
    auto& cb_p=cp.*Get(cbuffer_binding_float_pixel__tag{});
    auto& cb_b=cp.*Get(cbuffer_binding_bool_loop__tag{});
    auto& cb_f=cp.*Get(cbuffer_binding_fetch__tag{});
    auto& texture_owner=cp.*Get(texture_cache__tag{});
    if(!texture_owner){REXLOG_ERROR("GPU fetch diagnostic needs an initialized texture cache");return false;}
    auto& texture_sync=static_cast<rex::graphics::TextureCache&>(*texture_owner).*Get(texture_sync_tag{});
    const auto saved_texture_sync=texture_sync;
    auto* regs=graphics.register_file();
    std::array<uint8_t,sizeof(RegisterFile)> saved_regs{};
    std::array<uint8_t,sizeof(map_v)> saved_v{},saved_p{};
    std::array<uint8_t,sizeof(cb_v)> saved_cv{},saved_cp{},saved_cb{},saved_cf{};
    std::memcpy(saved_regs.data(),regs,sizeof(*regs));std::memcpy(saved_v.data(),map_v,sizeof(map_v));std::memcpy(saved_p.data(),map_p,sizeof(map_p));
    std::memcpy(saved_cv.data(),&cb_v,sizeof(cb_v));std::memcpy(saved_cp.data(),&cb_p,sizeof(cb_p));std::memcpy(saved_cb.data(),&cb_b,sizeof(cb_b));std::memcpy(saved_cf.data(),&cb_f,sizeof(cb_f));
    const bool saved_frame=frame,saved_enabled=REXCVAR_GET(em2_deduplicate_gpu_constants),saved_batch=REXCVAR_GET(em2_batch_gpu_constant_reads),saved_inline=REXCVAR_GET(em2_inline_gpu_constant_writes);const auto saved_csr=_mm_getcsr();
    ScopeExit restore{[&]{
        std::memcpy(regs,saved_regs.data(),sizeof(*regs));std::memcpy(map_v,saved_v.data(),sizeof(map_v));std::memcpy(map_p,saved_p.data(),sizeof(map_p));
        std::memcpy(&cb_v,saved_cv.data(),sizeof(cb_v));std::memcpy(&cb_p,saved_cp.data(),sizeof(cb_p));std::memcpy(&cb_b,saved_cb.data(),sizeof(cb_b));std::memcpy(&cb_f,saved_cf.data(),sizeof(cb_f));
        texture_sync=saved_texture_sync;frame=saved_frame;REXCVAR_SET(em2_inline_gpu_constant_writes,saved_inline);REXCVAR_SET(em2_deduplicate_gpu_constants,saved_enabled);REXCVAR_SET(em2_batch_gpu_constant_reads,saved_batch);_mm_setcsr(saved_csr);
    }};
    auto* memory=graphics.memory();auto* heap=memory->LookupHeapByType(true,0x1000);uint32_t packet=0;
    if(!heap || !heap->Alloc(0x4000,0x1000,rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,
        rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite,false,&packet))return false;
    ScopeExit release_packet{[&]{heap->Release(packet,nullptr);}};
    const auto physical=memory->GetPhysicalAddress(packet);if(physical==UINT32_MAX)return false;
    auto* words=memory->TranslateVirtual<uint32_t*>(packet);
    for(bool inline_writes:{false,true}) {
        REXCVAR_SET(em2_inline_gpu_constant_writes,inline_writes);
        texture_sync=UINT32_MAX;
        cb_f.up_to_date=true;
        em2::InvalidateTextureFetchBindings(cp);
        if(texture_sync || cb_f.up_to_date ||
           std::memcmp(saved_regs.data(),regs,sizeof(*regs))) {
            REXLOG_ERROR("Live texture compatibility change did not invalidate all bindings without altering registers");
            return false;
        }
    }
    REXLOG_INFO("Live texture compatibility invalidation passed both GPU register writers");
    constexpr std::array<uint32_t,10> bits={0,0x80000000u,1,0x7F800000u,0xFF800000u,0x7FC12345u,0x7F812345u,0xFFFFFFFFu,0x3F800000u,0xCF000000u};
    unsigned cases=0;
    std::array<uint32_t,RegisterFile::kRegisterCount> before{},expected{};
    for(uint32_t index:{0x4000u,0x4001u,0x43FFu,0x4400u,0x47FEu,0x47FFu,0x4800u,0x4801u,0x4805u,0x4806u,0x48BAu,0x48BFu,0x48C0u,0x4900u,0x4907u,0x4908u,0x4927u})
    for(uint32_t count:{1u,2u,3u,4u,7u,16u,32u,96u,192u,256u,1024u,2048u}) {
        if(uint64_t(index)+count>0x4928 || (index>=0x4800 && index<=0x48BF && uint64_t(index)+count>0x48C0) || (index==0x48C0 && count!=1))continue;
        if(index<0x4800 && uint64_t(index)+count>0x4800 && !(index==0x47FF && count==2))continue;
        for(bool open:{false,true})for(bool valid:{false,true})for(unsigned usage=0;usage<4;++usage)for(unsigned change=0;change<4;++change) {
            std::fill(before.begin(),before.end(),0u);
            for(unsigned i=0;i<count;++i) {
                before[index+i]=bits[i%bits.size()];
                uint32_t value=before[index+i];
                if((change==1 && !i)||(change==2 && i+1==count)||(change==3 && (i&1)==0))value^=1;
                words[i+1]=std::byteswap(value);
            }
            words[0]=std::byteswap(((count-1)<<16)|index);
            auto prepare=[&] {
                std::memcpy(regs->values,before.data(),sizeof(regs->values));
                frame=open;const uint64_t used_v=usage==0?0:usage==1?UINT64_MAX:usage==2?0xA5F001830000FFFFull:0x0001000000000001ull;
                const uint64_t used_p=usage<2?used_v:~used_v;
                std::fill(std::begin(map_v),std::end(map_v),used_v);std::fill(std::begin(map_p),std::end(map_p),used_p);
                cb_v.up_to_date=valid;cb_p.up_to_date=valid;cb_b.up_to_date=valid;cb_f.up_to_date=valid;texture_sync=uint32_t(used_v);
            };
            prepare();REXCVAR_SET(em2_inline_gpu_constant_writes,false);REXCVAR_SET(em2_batch_gpu_constant_reads,false);REXCVAR_SET(em2_deduplicate_gpu_constants,false);cp.ExecutePacket(physical,count+1);std::memcpy(expected.data(),regs->values,sizeof(regs->values));
            const auto reference_sync=texture_sync;
            const std::array reference_flags={cb_v.up_to_date,cb_p.up_to_date,cb_b.up_to_date,cb_f.up_to_date};
            for(bool inline_writes:{false,true})for(bool batch:{false,true})for(bool deduplicate:{false,true}) {
            prepare();REXCVAR_SET(em2_inline_gpu_constant_writes,inline_writes);REXCVAR_SET(em2_batch_gpu_constant_reads,batch);REXCVAR_SET(em2_deduplicate_gpu_constants,deduplicate);
            const auto batches=em2::GpuConstantBatchCount();const auto csr=_mm_getcsr();cp.ExecutePacket(physical,count+1);
            const auto expected_batches=uint64_t(batch && count>=16 && em2::IsBulkGpuRegisterRange(index,count) && (inline_writes || em2::IsDeduplicatedConstantRange(index,count)));
            if(em2::GpuConstantBatchCount()-batches!=expected_batches){REXLOG_ERROR("GPU bulk handler dispatch mismatch: {:04X}+{}, batch {}",index,count,batch);return false;}

            bool changed_v=false,changed_p=false,changed_b=false,touched_fetch=false;
            for(unsigned i=0;i<count;++i) {
                const auto r=index+i;const bool changed=before[r]!=expected[r];
                changed_v|=changed && r>=0x4000 && r<0x4400 && (map_v[(r-0x4000)>>8]&(uint64_t(1)<<(((r-0x4000)>>2)&63)));
                changed_p|=changed && r>=0x4400 && r<0x4800 && (map_p[(r-0x4400)>>8]&(uint64_t(1)<<(((r-0x4400)>>2)&63)));
                changed_b|=changed && r>=0x4900 && r<=0x4927;
                touched_fetch|=r>=0x4800 && r<0x48C0;
            }
            const bool expected_v=deduplicate?(valid && !(open && changed_v)):reference_flags[0],expected_p=deduplicate?(valid && !(open && changed_p)):reference_flags[1];
            const bool expected_b=deduplicate?(valid && !changed_b):reference_flags[2],expected_f=deduplicate?(valid && !touched_fetch):reference_flags[3];
            if(std::memcmp(expected.data(),regs->values,sizeof(regs->values)) || texture_sync!=reference_sync || _mm_getcsr()!=csr || cb_v.up_to_date!=expected_v ||
               cb_p.up_to_date!=expected_p || cb_b.up_to_date!=expected_b || cb_f.up_to_date!=expected_f) {
                REXLOG_ERROR("GPU constant packet mismatch: {:04X}+{}, frame {}, valid {}, usage {}, change {}; flags {}/{}/{}/{} expected {}/{}/{}/{}",index,count,open,valid,usage,change,cb_v.up_to_date,cb_p.up_to_date,cb_b.up_to_date,cb_f.up_to_date,expected_v,expected_p,expected_b,expected_f);return false;
            }
            ++cases;
            }
        }
    }
    REXLOG_INFO("GPU constant backend passed {} real PM4 packet comparisons: complete register state, exact FP bits, dirty-buffer invariants, frame/usage states, fetch boundary side effects and MXCSR; all scalar/bulk, dedup and inline-writer flag combinations; no draw or guest gameplay",cases);
    unsigned ring_cases=0;
    for(uint32_t index:{0x4000u,0x43F8u,0x47E0u,0x4800u,0x4801u,0x4805u,0x4806u,0x48B0u,0x48BFu,0x48C0u,0x4900u})for(uint32_t count:{16u,17u,31u,32u,33u,127u,192u,2048u}) {
        if(uint64_t(index)+count>0x4928 || (!em2::IsBulkGpuRegisterRange(index,count) && count>33))continue;
        const auto bytes=size_t(count)*4,capacity=bytes+16;
        for(size_t start:{size_t(0),size_t(1),size_t(3),capacity-1,capacity-4,capacity-7})for(bool one_register:{false,true})for(bool truncated:{false,true})for(bool inline_writes:{false,true}) {
            std::vector<uint8_t> ring(capacity,0xCD);
            for(unsigned i=0;i<count;++i) {
                const auto be=std::byteswap(bits[i%bits.size()]);
                for(unsigned byte=0;byte<4;++byte)ring[(start+i*4+byte)%capacity]=reinterpret_cast<const uint8_t*>(&be)[byte];
            }
            rex::memory::RingBuffer reader(ring.data(),capacity);
            auto prepare_ring=[&] {
                std::fill(std::begin(regs->values),std::end(regs->values),0u);
                frame=true;std::fill(std::begin(map_v),std::end(map_v),UINT64_MAX);std::fill(std::begin(map_p),std::end(map_p),UINT64_MAX);
                cb_v.up_to_date=cb_p.up_to_date=cb_b.up_to_date=cb_f.up_to_date=true;texture_sync=UINT32_MAX;
                reader.set_read_offset(start);reader.set_write_offset((start+bytes-(truncated?4:0))%capacity);
            };
            const uint32_t header=((count-1)<<16)|index|(one_register?0x8000u:0);
            prepare_ring();REXCVAR_SET(em2_inline_gpu_constant_writes,false);REXCVAR_SET(em2_deduplicate_gpu_constants,true);REXCVAR_SET(em2_batch_gpu_constant_reads,false);
            const bool reference=cp.ExecutePacketType0(&reader,header);const auto reference_offset=reader.read_offset();const auto reference_sync=texture_sync;
            std::memcpy(expected.data(),regs->values,sizeof(regs->values));
            const std::array reference_flags={cb_v.up_to_date,cb_p.up_to_date,cb_b.up_to_date,cb_f.up_to_date};
            prepare_ring();REXCVAR_SET(em2_inline_gpu_constant_writes,inline_writes);REXCVAR_SET(em2_batch_gpu_constant_reads,true);const auto batches=em2::GpuConstantBatchCount();const auto csr=_mm_getcsr();
            const bool optimized=cp.ExecutePacketType0(&reader,header);
            const auto expected_batches=uint64_t(!one_register && !truncated && em2::IsBulkGpuRegisterRange(index,count) && (inline_writes || em2::IsDeduplicatedConstantRange(index,count)));
            if(reference!=optimized || texture_sync!=reference_sync || reference_offset!=reader.read_offset() || std::memcmp(expected.data(),regs->values,sizeof(regs->values)) ||
               reference_flags!=std::array{cb_v.up_to_date,cb_p.up_to_date,cb_b.up_to_date,cb_f.up_to_date} || _mm_getcsr()!=csr ||
               em2::GpuConstantBatchCount()-batches!=expected_batches) {
                REXLOG_ERROR("GPU ring packet mismatch: {:04X}+{}, start {}, single {}, truncated {}",index,count,start,one_register,truncated);return false;
            }
            ++ring_cases;
        }
    }
    REXLOG_INFO("GPU constant bulk ring checks passed {} native-handler comparisons: unaligned/wrapped payloads, tails, single-register flags, truncated packets, fetch boundaries, reader offsets, complete registers, dirty buffers and MXCSR",ring_cases);
    unsigned fetch_cases=0;
    for(bool null_cache:{false,true}) {
        DetachTextureCache detached(texture_owner,null_cache);
        for(uint32_t index=0x47FF;index<=0x48C0;++index)for(uint32_t count:{1u,16u})for(bool repeated:{false,true})for(bool deduplicate:{false,true})
        for(bool open:{false,true})for(uint32_t sync:{0u,UINT32_MAX,0xA5A55A5Au,0x80000001u}) {
            if(count>1 && (index<0x4800 || uint64_t(index)+count>0x48C0))continue;
            const uint32_t value=bits[index%bits.size()];words[0]=std::byteswap(((count-1)<<16)|index);
            for(unsigned i=0;i<count;++i)words[i+1]=std::byteswap(value+i);
            auto prepare=[&] {
                std::fill(std::begin(regs->values),std::end(regs->values),0u);
                for(unsigned i=0;i<count;++i)regs->values[index+i]=repeated?value+i:(value+i)^1;
                frame=open;std::fill(std::begin(map_v),std::end(map_v),UINT64_MAX);std::fill(std::begin(map_p),std::end(map_p),UINT64_MAX);
                cb_v.up_to_date=cb_p.up_to_date=cb_b.up_to_date=cb_f.up_to_date=true;texture_sync=sync;
            };
            prepare();REXCVAR_SET(em2_batch_gpu_constant_reads,true);REXCVAR_SET(em2_deduplicate_gpu_constants,deduplicate);REXCVAR_SET(em2_inline_gpu_constant_writes,false);
            cp.ExecutePacket(physical,count+1);std::memcpy(expected.data(),regs->values,sizeof(regs->values));
            const auto reference_sync=texture_sync;const std::array reference_flags={cb_v.up_to_date,cb_p.up_to_date,cb_b.up_to_date,cb_f.up_to_date};
            prepare();REXCVAR_SET(em2_inline_gpu_constant_writes,true);const auto csr=_mm_getcsr();const auto bulk_before=em2::GpuConstantBatchCount();cp.ExecutePacket(physical,count+1);
            if(em2::GpuConstantBatchCount()-bulk_before!=uint64_t(count==16)) {
                REXLOG_ERROR("Fetch packet did not reach its expected bulk/scalar path: {:04X}+{}",index,count);return false;
            }
            if(std::memcmp(expected.data(),regs->values,sizeof(regs->values)) || reference_sync!=texture_sync ||
               reference_flags!=std::array{cb_v.up_to_date,cb_p.up_to_date,cb_b.up_to_date,cb_f.up_to_date} || csr!=_mm_getcsr()) {
                REXLOG_ERROR("GPU fetch mismatch: {:04X}, repeated {}, dedup {}, open {}, null {}, sync {:08X}/{:08X}",index,repeated,deduplicate,open,null_cache,sync,texture_sync);return false;
            }
            if(index>=0x4800 && index<=0x48BF && (cb_f.up_to_date || (!null_cache && (texture_sync&(uint32_t(1)<<((index-0x4800)/6)))))) {
                REXLOG_ERROR("GPU fetch equal-write invalidation missing for {:04X}",index);return false;
            }
            ++fetch_cases;
        }
    }
    REXLOG_INFO("GPU fetch writer passed {} native-reference packet comparisons: all 192 fetch words, slot boundaries, neighboring registers, equal/changing writes, dedup on/off, open/closed frames, texture sync masks, null/non-null cache, complete registers, CB flags and MXCSR",fetch_cases);
    unsigned type3_cases=0;
    struct ImmediateCase {unsigned form;uint32_t encoded,index;bool supported;};
    std::vector<ImmediateCase> immediate_cases;
    for(unsigned type=0;type<6;++type)for(uint32_t relative:{0u,1u,5u,7u,31u,63u,191u,255u,256u,1023u,1024u,2047u}) {
        constexpr uint32_t bases[]={0x4000,0x4800,0x4900,0x4908,0x2000,0x4000};
        if(type==4 && relative!=256)continue;
        immediate_cases.push_back({0,0x80000000u|(type<<16)|relative,bases[type]+relative,type<5});
    }
    for(unsigned form:{1u,2u})for(uint32_t index:{0x4000u,0x4001u,0x43FFu,0x4400u,0x47FFu,0x4800u,0x4801u,0x4805u,0x4806u,0x48BAu,0x48BFu,0x48C0u,0x4900u,0x4907u,0x4908u,0x4927u})
        immediate_cases.push_back({form,0xA5010000u|index,index,true});
    std::array<uint8_t,0x4000> ring_storage{};
    for(const auto& test:immediate_cases)for(uint32_t count:{0u,1u,7u,15u,16u,17u,31u,32u,96u,192u,1024u,2048u}) {
        if(test.supported && uint64_t(test.index)+count>(test.index<0x4000?0x2110:0x4928))continue;
        // Do not flood the native logger with synthetic writes through gaps in
        // its register table. Mixed float/fetch and bool/loop ranges remain.
        bool mapped=true;
        if(test.supported)for(uint32_t i=0;i<count;++i)if(!RegisterFile::GetRegisterInfo(test.index+i)){mapped=false;break;}
        if(!mapped)continue;
        const uint32_t opcode=std::array{0x2Du,0x55u,0x56u}[test.form];
        words[0]=std::byteswap(0xC0000000u|(count<<16)|(opcode<<8));words[1]=std::byteswap(test.encoded);
        for(bool wrapped:{false,true})for(bool open:{false,true})for(bool valid:{false,true})for(bool changing:{false,true})for(bool deduplicate:{false,true}) {
            std::fill(before.begin(),before.end(),0u);
            for(unsigned i=0;i<count;++i) {
                const auto value=bits[i%bits.size()];
                if(test.index+i<before.size())before[test.index+i]=value;
                words[i+2]=std::byteswap(value^(changing && (i%3==0)?1u:0u));
            }
            rex::memory::RingBuffer reader(ring_storage.data(),ring_storage.size());
            const size_t start=count%3==0?ring_storage.size()-3:count%3;
            const auto* payload=reinterpret_cast<const uint8_t*>(words+1);const size_t bytes=size_t(count+1)*4;
            for(size_t i=0;i<bytes;++i)ring_storage[(start+i)%ring_storage.size()]=payload[i];
            auto prepare=[&] {
                std::memcpy(regs->values,before.data(),sizeof(regs->values));frame=open;
                std::fill(std::begin(map_v),std::end(map_v),valid?UINT64_MAX:0x8001800180018001ull);
                std::fill(std::begin(map_p),std::end(map_p),valid?UINT64_MAX:0x0001000100010001ull);
                cb_v.up_to_date=valid;cb_p.up_to_date=valid;cb_b.up_to_date=valid;cb_f.up_to_date=valid;texture_sync=valid?UINT32_MAX:0xA5C38100u;
                reader.set_read_offset(start);reader.set_write_offset((start+bytes)%ring_storage.size());
            };
            auto execute=[&] {
                if(!wrapped){cp.ExecutePacket(physical,count+2);return true;}
                if(test.form==0)return cp.ExecutePacketType3_SET_CONSTANT(&reader,std::byteswap(words[0]),count+1);
                if(test.form==1)return cp.ExecutePacketType3_SET_CONSTANT2(&reader,std::byteswap(words[0]),count+1);
                return cp.ExecutePacketType3_SET_SHADER_CONSTANTS(&reader,std::byteswap(words[0]),count+1);
            };
            REXCVAR_SET(em2_deduplicate_gpu_constants,deduplicate);REXCVAR_SET(em2_batch_gpu_constant_reads,false);REXCVAR_SET(em2_inline_gpu_constant_writes,false);
            prepare();const bool reference=execute();const auto reference_offset=reader.read_offset();const auto reference_sync=texture_sync;
            std::memcpy(expected.data(),regs->values,sizeof(regs->values));
            const std::array flags={cb_v.up_to_date,cb_p.up_to_date,cb_b.up_to_date,cb_f.up_to_date};
            for(bool batch:{false,true})for(bool specialized:{false,true}) {
                prepare();REXCVAR_SET(em2_batch_gpu_constant_reads,batch);REXCVAR_SET(em2_inline_gpu_constant_writes,specialized);
                const auto csr=_mm_getcsr();const auto batches=em2::GpuConstantBatchCount();const bool result=execute();
                // Independent range check also proves dispatch reaches this
                // replacement rather than merely comparing two native calls.
                const auto end=uint64_t(test.index)+count;
                const bool shader=(test.index>=0x4000 && end<=0x4800)||(test.index>=0x4900 && end<=0x4928);
                const bool fetch=test.index>=0x4800 && end<=0x48C0;
                const bool bulk=batch && count>=16 && count<=2048 && test.supported && (shader||(specialized&&fetch));
                if(result!=reference || reader.read_offset()!=reference_offset || texture_sync!=reference_sync ||
                   std::memcmp(expected.data(),regs->values,sizeof(regs->values)) || flags!=std::array{cb_v.up_to_date,cb_p.up_to_date,cb_b.up_to_date,cb_f.up_to_date} ||
                   csr!=_mm_getcsr() || em2::GpuConstantBatchCount()-batches!=unsigned(bulk)) {
                    REXLOG_ERROR("GPU Type-3 mismatch form {} encoded {:08X}, words {}, wrapped {}, frame {}, valid {}, changing {}, dedup {}, batch {}, specialized {}",test.form,test.encoded,count,wrapped,open,valid,changing,deduplicate,batch,specialized);return false;
                }
                ++type3_cases;
            }
        }
    }
    REXLOG_INFO("GPU Type-3 checks passed {} native-reference comparisons: all three opcodes, unknown types, relative/absolute offsets, reserved bits, zero/tail/large payloads, mixed register ranges, wrapped/unaligned metadata, full dispatch, reader offsets, exact registers, texture invalidation, CB flags, MXCSR and independent bulk-routing checks",type3_cases);
    if(REXCVAR_GET(em2_benchmark_gpu_constants)) {
        using Clock=std::chrono::steady_clock;constexpr unsigned rounds=7,iterations=2000;
        REXCVAR_SET(em2_inline_gpu_constant_writes,false);
        frame=true;std::fill(std::begin(map_v),std::end(map_v),UINT64_MAX);std::fill(std::begin(map_p),std::end(map_p),UINT64_MAX);
        for(unsigned form=0;form<3;++form)for(uint32_t count:{1u,16u,128u,1024u,192u})for(bool changing:{false,true}) {
            const bool fetch=count==192;const uint32_t index=fetch?0x4800:0x4000;
            const auto opcode=std::array{0x2Du,0x55u,0x56u}[form];
            words[0]=std::byteswap(0xC0000000u|(count<<16)|(opcode<<8));
            words[1]=std::byteswap(form?index:(fetch?0x10000u:0u));
            for(unsigned i=0;i<count;++i){regs->values[index+i]=bits[i%bits.size()];words[i+2]=std::byteswap(regs->values[index+i]);}
            std::array<std::array<double,rounds>,2> times{};
            REXCVAR_SET(em2_inline_gpu_constant_writes,true);REXCVAR_SET(em2_deduplicate_gpu_constants,true);
            for(unsigned round=0;round<rounds;++round)for(unsigned order=0;order<2;++order) {
                const auto mode=(round+order)&1;REXCVAR_SET(em2_batch_gpu_constant_reads,mode!=0);
                const auto begin=Clock::now();
                for(unsigned i=0;i<iterations;++i) {
                    cb_v.up_to_date=true;cb_p.up_to_date=true;cb_f.up_to_date=true;texture_sync=UINT32_MAX;
                    if(changing)for(unsigned j=2;j<count+2;++j)words[j]^=0x01000000;
                    cp.ExecutePacket(physical,count+2);
                }
                times[mode][round]=std::chrono::duration<double,std::nano>(Clock::now()-begin).count()/iterations;
            }
            for(auto& mode:times)std::sort(mode.begin(),mode.end());
            REXLOG_INFO("GPU Type-3 bulk PM4 benchmark opcode {:02X}, {} words, fetch {}, changing {}: scalar {:.2f}, bulk {:.2f} ns/packet, {:.3f}x (seven alternating rounds; same specialized writer/dedup; not GPU time or gameplay FPS)",opcode,count,fetch,changing,times[0][3],times[1][3],times[0][3]/times[1][3]);
        }
        REXCVAR_SET(em2_inline_gpu_constant_writes,false);
        for(uint32_t count:{1u,16u,128u,1024u})for(unsigned pattern=0;pattern<5;++pattern) {
            words[0]=std::byteswap(((count-1)<<16)|0x4000u);
            for(unsigned i=0;i<count;++i){regs->values[0x4000+i]=bits[i%bits.size()];words[i+1]=std::byteswap(regs->values[0x4000+i]);}
            std::array<std::array<double,rounds>,2> times{};
            for(unsigned round=0;round<rounds;++round)for(unsigned order=0;order<2;++order) {
                const auto mode=(round+order)&1;REXCVAR_SET(em2_deduplicate_gpu_constants,true);REXCVAR_SET(em2_batch_gpu_constant_reads,mode!=0);
                const auto begin=Clock::now();
                for(unsigned i=0;i<iterations;++i) {
                    if(pattern==1)for(unsigned j=1;j<=count;++j)words[j]^=0x01000000;
                    else if(pattern==2)words[1]^=0x01000000;
                    else if(pattern==3)words[count]^=0x01000000;
                    else if(pattern==4)for(unsigned j=1;j<=count;j+=2)words[j]^=0x01000000;
                    cp.ExecutePacket(physical,count+1);
                }
                times[mode][round]=std::chrono::duration<double,std::nano>(Clock::now()-begin).count()/iterations;
            }
            for(auto& mode:times)std::sort(mode.begin(),mode.end());
            REXLOG_INFO("GPU constant bulk PM4 benchmark {} words, {}: scalar {:.2f}, bulk {:.2f} ns/packet, {:.3f}x (seven alternating rounds; not GPU time or gameplay FPS)",count,std::array{"repeated","all changing","first changing","last changing","alternating changing"}[pattern],times[0][3],times[1][3],times[0][3]/times[1][3]);
        }
        for(uint32_t count:{1u,16u,128u,1024u})for(unsigned pattern=0;pattern<5;++pattern) {
            words[0]=std::byteswap(((count-1)<<16)|0x4000u);
            for(unsigned i=0;i<count;++i){regs->values[0x4000+i]=bits[i%bits.size()];words[i+1]=std::byteswap(regs->values[0x4000+i]);}
            std::array<std::array<double,rounds>,2> times{};
            REXCVAR_SET(em2_deduplicate_gpu_constants,true);REXCVAR_SET(em2_batch_gpu_constant_reads,true);
            for(unsigned round=0;round<rounds;++round)for(unsigned order=0;order<2;++order) {
                const auto mode=(round+order)&1;REXCVAR_SET(em2_inline_gpu_constant_writes,mode!=0);
                const auto begin=Clock::now();
                for(unsigned i=0;i<iterations;++i) {
                    if(pattern==1)for(unsigned j=1;j<=count;++j)words[j]^=0x01000000;
                    else if(pattern==2)words[1]^=0x01000000;
                    else if(pattern==3)words[count]^=0x01000000;
                    else if(pattern==4)for(unsigned j=1;j<=count;j+=2)words[j]^=0x01000000;
                    cp.ExecutePacket(physical,count+1);
                }
                times[mode][round]=std::chrono::duration<double,std::nano>(Clock::now()-begin).count()/iterations;
            }
            for(auto& mode:times)std::sort(mode.begin(),mode.end());
            REXLOG_INFO("GPU constant inline PM4 benchmark {} words, {}: original {:.2f}, inline {:.2f} ns/packet, {:.3f}x (seven alternating rounds; bulk/dedup on; not GPU time or gameplay FPS)",count,std::array{"repeated","all changing","first changing","last changing","alternating changing"}[pattern],times[0][3],times[1][3],times[0][3]/times[1][3]);
        }
        for(uint32_t count:{1u,6u,12u,96u,192u})for(bool changing:{false,true}) {
            words[0]=std::byteswap(((count-1)<<16)|0x4800u);
            for(unsigned i=0;i<count;++i)words[i+1]=std::byteswap(bits[i%bits.size()]);
            std::array<std::array<double,rounds>,2> times{};
            for(unsigned round=0;round<rounds;++round)for(unsigned order=0;order<2;++order) {
                const auto mode=(round+order)&1;REXCVAR_SET(em2_inline_gpu_constant_writes,mode!=0);
                const auto begin=Clock::now();
                for(unsigned i=0;i<iterations;++i) {
                    // Model draw-time reuse: all binding slots become valid
                    // before each packet, so every repeated write must clear it.
                    texture_sync=UINT32_MAX;cb_f.up_to_date=true;
                    if(changing)for(unsigned j=1;j<=count;++j)words[j]^=0x01000000;
                    cp.ExecutePacket(physical,count+1);
                }
                times[mode][round]=std::chrono::duration<double,std::nano>(Clock::now()-begin).count()/iterations;
            }
            for(auto& mode:times)std::sort(mode.begin(),mode.end());
            REXLOG_INFO("GPU fetch inline PM4 benchmark {} words, changing {}: original {:.2f}, inline {:.2f} ns/packet, {:.3f}x (seven alternating rounds; invalidation rearmed every packet; not GPU time or gameplay FPS)",count,changing,times[0][3],times[1][3],times[0][3]/times[1][3]);
        }
    }
    return true;
}
}
bool em2::TestGpuConstantBackend(rex::graphics::GraphicsSystem& graphics) {
    if(IsGpuCommandProfilingEnabled(*graphics.command_processor())!=(REXCVAR_GET(em2_profile_render)||REXCVAR_GET(em2_profile_gpu_draws))){
        REXLOG_ERROR("Graphics command profiling factory selection differs from startup flag");return false;
    }
    auto* cp=dynamic_cast<Em2CommandProcessor*>(graphics.command_processor());
    if(!cp){REXLOG_ERROR("EM2 GPU command-processor factory was not installed");return false;}
    auto done=std::make_shared<std::promise<bool>>();auto result=done->get_future();
    cp->CallInThread([done,cp,&graphics]{done->set_value(CheckBackend(*cp,graphics));});
    return result.get();
}
