#include "skinning.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <immintrin.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <chrono>
#include <windows.h>

PPC_EXTERN_FUNC(__imp__sub_82968430);
PPC_EXTERN_FUNC(__imp__sub_82540678);
REXCVAR_DEFINE_BOOL(em2_native_skinning,true,"Debug","Use SIMD game skinning with translated fallback");
REXCVAR_DEFINE_UINT32(em2_skin_batch_vertices,256,"Debug","Skinning vertices per job, 0 uses the original heuristic (maximum 256)");
REXCVAR_DEFINE_BOOL(em2_benchmark_skinning,false,"Debug","Benchmark palette caching during runtime regression tests");
REXCVAR_DEFINE_BOOL(em2_packed_skin_streams,true,"CPU","Use exact 12-byte SIMD position/normal transfers in native skinning");
REXCVAR_DEFINE_BOOL(em2_wide_skin_palette,true,"CPU","Decode adjacent skinning matrix rows with exact-bit AVX2 loads");
REXCVAR_DEFINE_BOOL(em2_reuse_skin_blends,true,"CPU","Reuse exact adjacent skinning index/weight blends within a workload");
REXCVAR_DEFINE_BOOL(em2_benchmark_skin_blends,false,"Debug","Benchmark exact skin blend reuse in the existing regression executable");
namespace {
void PrepareSkinPartition(PPCContext& context,uint8_t* base,uint32_t requested) {
    const auto task=context.r4.u32;
    const auto kernel=PPC_LOAD_U32(task+104);
    const auto original=PPC_LOAD_U32(task+128);
    const bool selected=requested && original==0xFFFFFFFF && kernel &&
        PPC_LOAD_U32(PPC_LOAD_U32(kernel)+8)==0x82968430;
    if(selected) PPC_STORE_U32(task+128,std::min(requested,256u));
    __imp__sub_82540678(context,base);
    if(selected) PPC_STORE_U32(task+128,original);
}
struct Stream { uint32_t address,stride; };
bool HasFma() {
    static const bool result=[] { int cpu[4]; __cpuid(cpu,1); return (cpu[2]&(1<<12))!=0; }();
    return result;
}
float ReadFloat(uint32_t address,uint8_t* base) { return std::bit_cast<float>(PPC_LOAD_U32(address)); }
void WriteFloat(uint32_t address,float value,uint8_t* base) { PPC_STORE_U32(address,std::bit_cast<uint32_t>(value)); }
template<bool Packed> __m128 LoadThree(uint32_t address,uint8_t* base) {
    if constexpr(!Packed)return _mm_set_ps(0,ReadFloat(address+8,base),ReadFloat(address+4,base),ReadFloat(address,base));
    // Do not read a fourth float: a 12-byte stream may end at a guard page.
    const auto low=_mm_loadl_epi64(reinterpret_cast<const __m128i*>(PPC_RAW_ADDR(address)));
    uint32_t high;std::memcpy(&high,PPC_RAW_ADDR(address+8),4);
    const auto bytes=_mm_unpacklo_epi64(low,_mm_cvtsi32_si128(int(high)));
    return _mm_castsi128_ps(_mm_shuffle_epi8(bytes,_mm_setr_epi8(3,2,1,0,7,6,5,4,11,10,9,8,-1,-1,-1,-1)));
}
__m128 LoadRow(const uint8_t* address) {
    const auto swap=_mm_setr_epi8(3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12);
    const auto bytes=_mm_loadu_si128(reinterpret_cast<const __m128i*>(address));
    // Palette padding is not used by the Xbox kernel; don't do arithmetic on it.
    return _mm_blend_ps(_mm_castsi128_ps(_mm_shuffle_epi8(bytes,swap)),_mm_setzero_ps(),8);
}
template<bool Packed> void StoreThree(uint32_t address,__m128 value,uint8_t* base) {
    if constexpr(!Packed) {
        alignas(16) float xyz[4]; _mm_store_ps(xyz,value);
        for(unsigned i=0;i<3;++i) WriteFloat(address+i*4,xyz[i],base);
    } else {
        const auto bytes=_mm_shuffle_epi8(_mm_castps_si128(value),_mm_setr_epi8(3,2,1,0,7,6,5,4,11,10,9,8,-1,-1,-1,-1));
        _mm_storel_epi64(reinterpret_cast<__m128i*>(PPC_RAW_ADDR(address)),bytes);
        const uint32_t high=uint32_t(_mm_extract_epi32(bytes,2));std::memcpy(PPC_RAW_ADDR(address+8),&high,4);
    }
}

template<bool CachePalette=true,bool Packed=true,bool LazyPalette=false,bool WidePalette=false,bool ReuseBlend=false>
__attribute__((target("avx2,fma")))
bool Execute(uint32_t workload,uint8_t* base) {
    if(PPC_LOAD_U16(workload+8)!=10 || PPC_LOAD_U16(workload+10)!=3) return false;
    const auto table=PPC_LOAD_U32(workload),count=PPC_LOAD_U32(workload+12);
    if(!count || count>4096) return false;
    std::array<Stream,13> streams;
    for(unsigned i=0;i<streams.size();++i) {
        const auto descriptor=table+i*16;
        streams[i]={PPC_LOAD_U32(descriptor)+PPC_LOAD_U32(descriptor+4),PPC_LOAD_U32(descriptor+12)};
    }
    const auto params=streams[0].address;
    if(!params) return false;
    // Resolve the SDK physical-view adjustment once, not once per matrix row.
    // Discontinuous/wrapping parameter blocks retain the original-kernel path.
    const auto palette_start=uint32_t(params+4);
    if(uint64_t(params)+8216>0x100000000ull ||
       PPC_PHYS_HOST_OFFSET(palette_start)!=PPC_PHYS_HOST_OFFSET(uint32_t(palette_start+8191)))return false;
    const auto* palette_source=PPC_RAW_ADDR(palette_start);
    const bool tangent=PPC_LOAD_U32(params)!=0 && streams[4].address && streams[12].address;
    for(auto i:{1u,2u,10u,11u}) if(!streams[i].address || streams[i].stride!=12) return false;
    if(!streams[3].address || streams[3].stride!=24 || !streams[5].address || streams[5].stride!=1) return false;
    if(tangent && (streams[4].stride!=16 || streams[12].stride!=16)) return false;
    const auto morphs=PPC_LOAD_U32(params+8196);
    if constexpr(ReuseBlend) {
        const uint64_t end=uint64_t(streams[3].address)+uint64_t(count)*24;
        if(end>0x100000000ull || PPC_PHYS_HOST_OFFSET(streams[3].address)!=PPC_PHYS_HOST_OFFSET(uint32_t(end-1)))
            return Execute<CachePalette,Packed,LazyPalette,WidePalette,false>(workload,base);
    }
    if constexpr(Packed) {
        auto safe=[&](unsigned i) {
            const auto end=uint64_t(streams[i].address)+uint64_t(count-1)*streams[i].stride+12;
            return end<=0x100000000ull && PPC_PHYS_HOST_OFFSET(streams[i].address)==PPC_PHYS_HOST_OFFSET(uint32_t(end-1));
        };
        for(auto i:{1u,2u,10u,11u})if(!safe(i))return Execute<CachePalette,false,LazyPalette,WidePalette,ReuseBlend>(workload,base);
        if(tangent && (!safe(4) || !safe(12)))return Execute<CachePalette,false,LazyPalette,WidePalette,ReuseBlend>(workload,base);
        if(morphs>=1 && morphs<=4)for(unsigned i=0;i<morphs;++i)if(!safe(6+i))return Execute<CachePalette,false,LazyPalette,WidePalette,ReuseBlend>(workload,base);
    }
    // In-place streams can make the original's load/store order observable.
    // Preserve that behavior through the translated fallback.
    for(unsigned out=10;out<(tangent?13u:12u);++out) for(unsigned in=0;in<10;++in) {
        if(!streams[in].address) continue;
        const uint64_t input_begin=streams[in].address,output_begin=streams[out].address;
        const uint64_t input_end=input_begin+(in==0?8216ull:uint64_t(count)*streams[in].stride);
        const uint64_t output_end=output_begin+uint64_t(count)*streams[out].stride;
        if(input_begin<output_end && output_begin<input_end) return false;
    }
    // The alias checks above make the palette immutable for this workload.
    // Keep the cache local: animation and render replay can replace it between
    // jobs, and engine workers may execute several workloads concurrently.
    __m128 palette[128][4];
    std::array<uint8_t,128> palette_ready{};
    if constexpr(CachePalette && !LazyPalette) {
        if constexpr(WidePalette) {
            // Same raw row bytes and zero padding as LoadRow; never widen across
            // a 32-bit guest address wrap. No floating-point arithmetic here.
            {
                const auto swap=_mm256_setr_epi8(3,2,1,0,7,6,5,4,11,10,9,8,-1,-1,-1,-1,3,2,1,0,7,6,5,4,11,10,9,8,-1,-1,-1,-1);
                for(unsigned bone=0;bone<128;++bone)for(unsigned row=0;row<4;row+=2) {
                    const auto bytes=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(palette_source+bone*64+row*16));
                    _mm256_storeu_si256(reinterpret_cast<__m256i*>(&palette[bone][row]),_mm256_shuffle_epi8(bytes,swap));
                }
            }
        }else for(unsigned bone=0;bone<128;++bone) for(unsigned row=0;row<4;++row)
            palette[bone][row]=LoadRow(palette_source+bone*64+row*16);
    }
    bool blend_active=ReuseBlend;unsigned consecutive_misses=0;
    for(unsigned i=0;i<count;) {
        const auto skin=streams[3].address+i*24;
        uint32_t first_index=0;
        __m128 bones[3][4],weights[3];
        for(unsigned influence=0;influence<3;++influence) {
            const auto index=int32_t(PPC_LOAD_U32(skin+influence*4));
            if(influence==0)first_index=uint32_t(index);
            if(index>=128) return false;
            const auto weight=ReadFloat(skin+12+influence*4,base);
            weights[influence]=_mm_set1_ps(weight>=0?weight:0.f);
            if constexpr(CachePalette) {
                if constexpr(LazyPalette) {
                    if(index>=0 && !palette_ready[index]) {
                        for(unsigned row=0;row<4;++row)palette[index][row]=LoadRow(palette_source+index*64+row*16);
                        palette_ready[index]=1;
                    }
                }
                for(unsigned row=0;row<4;++row)
                    bones[influence][row]=index<0?_mm_setzero_ps():palette[index][row];
            } else {
                if constexpr(WidePalette) {
                    if(index<0)for(auto& row:bones[influence])row=_mm_setzero_ps();
                    else {
                        const auto swap=_mm256_setr_epi8(3,2,1,0,7,6,5,4,11,10,9,8,-1,-1,-1,-1,3,2,1,0,7,6,5,4,11,10,9,8,-1,-1,-1,-1);
                        for(unsigned row=0;row<4;row+=2) {
                            const auto bytes=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(palette_source+index*64+row*16));
                            const auto pair=_mm256_castsi256_ps(_mm256_shuffle_epi8(bytes,swap));
                            bones[influence][row]=_mm256_castps256_ps128(pair);
                            bones[influence][row+1]=_mm256_extractf128_ps(pair,1);
                        }
                    }
                }else for(unsigned row=0;row<4;++row)
                    bones[influence][row]=index<0?_mm_setzero_ps():LoadRow(palette_source+index*64+row*16);
            }
        }
        __m128 rows[4];
        for(unsigned row=0;row<4;++row)
            rows[row]=_mm_fmadd_ps(bones[0][row],weights[0],_mm_fmadd_ps(bones[2][row],weights[2],_mm_mul_ps(bones[1][row],weights[1])));
        // The original schedules M00 in a different accumulation order.
        const auto first=_mm_fmadd_ps(bones[1][0],weights[1],_mm_fmadd_ps(bones[0][0],weights[0],_mm_mul_ps(bones[2][0],weights[2])));
        rows[0]=_mm_blend_ps(rows[0],first,1);
        do {
        auto position=LoadThree<Packed>(streams[1].address+i*12,base);
        if(int8_t(PPC_LOAD_U8(streams[5].address+i))>0 && morphs>=1 && morphs<=4) {
            for(int morph=int(morphs)-1;morph>=0;--morph) {
                if(!streams[6+morph].address || streams[6+morph].stride!=12) return false;
                position=_mm_fmadd_ps(LoadThree<Packed>(streams[6+morph].address+i*12,base),
                    _mm_set1_ps(ReadFloat(params+8200+morph*4,base)),position);
            }
        }
        const auto px=_mm_shuffle_ps(position,position,_MM_SHUFFLE(0,0,0,0));
        const auto py=_mm_shuffle_ps(position,position,_MM_SHUFFLE(1,1,1,1));
        const auto pz=_mm_shuffle_ps(position,position,_MM_SHUFFLE(2,2,2,2));
        const auto transformed=_mm_add_ps(_mm_fmadd_ps(rows[0],px,_mm_fmadd_ps(rows[2],pz,_mm_mul_ps(rows[1],py))),rows[3]);
        StoreThree<Packed>(streams[10].address+i*12,transformed,base);
        const auto normal=LoadThree<Packed>(streams[2].address+i*12,base);
        const auto nx=_mm_shuffle_ps(normal,normal,_MM_SHUFFLE(0,0,0,0));
        const auto ny=_mm_shuffle_ps(normal,normal,_MM_SHUFFLE(1,1,1,1));
        const auto nz=_mm_shuffle_ps(normal,normal,_MM_SHUFFLE(2,2,2,2));
        StoreThree<Packed>(streams[11].address+i*12,_mm_fmadd_ps(rows[1],ny,_mm_fmadd_ps(rows[2],nz,_mm_mul_ps(rows[0],nx))),base);
        if(tangent) {
            const auto source=streams[4].address+i*16;
            const auto t=LoadThree<Packed>(source,base);
            const auto tx=_mm_shuffle_ps(t,t,_MM_SHUFFLE(0,0,0,0));
            const auto ty=_mm_shuffle_ps(t,t,_MM_SHUFFLE(1,1,1,1));
            const auto tz=_mm_shuffle_ps(t,t,_MM_SHUFFLE(2,2,2,2));
            StoreThree<Packed>(streams[12].address+i*16,_mm_fmadd_ps(rows[1],ty,_mm_fmadd_ps(rows[2],tz,_mm_mul_ps(rows[0],tx))),base);
            PPC_STORE_U32(streams[12].address+i*16+12,PPC_LOAD_U32(source+12));
        }
        ++i;
        if constexpr(ReuseBlend)if(blend_active && i<count) {
            const auto next_skin=streams[3].address+i*24;
            const auto next_index=PPC_LOAD_U32(next_skin);
            if(int32_t(next_index)>=128)return false;
            bool same=false;
            if(next_index==first_index) {
                // Exact24-byte signature; never read a fourth weight. A bad
                // first index is rejected before widening a potentially short
                // invalid record. Equal leading16 bytes imply all indices valid.
                const auto left=_mm_loadu_si128(reinterpret_cast<const __m128i*>(PPC_RAW_ADDR(skin)));
                const auto right=_mm_loadu_si128(reinterpret_cast<const __m128i*>(PPC_RAW_ADDR(next_skin)));
                if(_mm_movemask_epi8(_mm_cmpeq_epi8(left,right))==0xFFFF) {
                    uint64_t left_tail,right_tail;
                    std::memcpy(&left_tail,PPC_RAW_ADDR(skin+16),8);std::memcpy(&right_tail,PPC_RAW_ADDR(next_skin+16),8);
                    same=left_tail==right_tail;
                }
            }
            if(same){consecutive_misses=0;continue;}
            if(++consecutive_misses>=8)blend_active=false;
        }
        break;
        }while(true);
    }
    return true;
}
bool ExecuteScalarSelected(uint32_t workload,uint8_t* base) {
    return PPC_LOAD_U32(workload+12)<128 ? Execute<false,false>(workload,base):Execute<true,false>(workload,base);
}
bool ExecuteSelected(uint32_t workload,uint8_t* base) {
    // Keep dispatch outside the kernels so small-job fallback is not inlined
    // into the cached kernel's inner-loop register allocation.
    if(PPC_LOAD_U32(workload+12)<16 || !REXCVAR_GET(em2_packed_skin_streams))return ExecuteScalarSelected(workload,base);
    // After hoisting the palette base, narrow uncached rows win for small jobs.
    // Widen only eager palette initialization; keep the per-vertex path narrow.
    if(PPC_LOAD_U32(workload+12)<128)return Execute<false>(workload,base);
    if(REXCVAR_GET(em2_reuse_skin_blends))
        return REXCVAR_GET(em2_wide_skin_palette)?Execute<true,true,false,true,true>(workload,base):Execute<true,true,false,false,true>(workload,base);
    return REXCVAR_GET(em2_wide_skin_palette)?Execute<true,true,false,true>(workload,base):Execute<true>(workload,base);
}
}
PPC_FUNC_IMPL(sub_82540678) {
    PrepareSkinPartition(ctx,base,REXCVAR_GET(em2_skin_batch_vertices));
}
namespace em2 {
bool TryNativeSkinning(PPCContext& context,uint8_t* base) {
    if(!REXCVAR_GET(em2_native_skinning) || !HasFma()) return false;
    context.fpscr.disableFlushMode();
    return ExecuteSelected(context.r4.u32,base);
}

bool TestNativeSkinning() {
    if(!HasFma()) { REXLOG_INFO("SIMD skinning tests skipped: FMA unavailable"); return true; }
    // Reserve the guest address space sparsely so the original translated
    // kernel can use its real zero constant and stack helpers unchanged.
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100010000ull,MEM_RESERVE,PAGE_READWRITE));
    if(!base) return false;
    struct Release { uint8_t* base; unsigned mxcsr=_mm_getcsr();bool packed=REXCVAR_GET(em2_packed_skin_streams),wide=REXCVAR_GET(em2_wide_skin_palette); ~Release(){REXCVAR_SET(em2_wide_skin_palette,wide);REXCVAR_SET(em2_packed_skin_streams,packed); _mm_setcsr(mxcsr); VirtualFree(base,0,MEM_RELEASE); } } release{base};
    REXCVAR_SET(em2_packed_skin_streams,true);
    if(!VirtualAlloc(base,0x40000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0x82002000,0x1000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0x8203D000,0x1000,MEM_COMMIT,PAGE_READWRITE)) return false;
    PPC_STORE_U32(0x8203D220,0xFFFFFFFF);
    uint32_t random=0xE21C360;
    auto value=[&] { random=random*1664525u+1013904223u; return float(int32_t(random>>8)-0x800000)/0x800000; };
    constexpr uint32_t workload=0x80,table=0x100,params=0x1000;
    PPC_STORE_U32(workload,table); PPC_STORE_U16(workload+8,10); PPC_STORE_U16(workload+10,3);
    const std::array<uint32_t,13> strides={8216,12,12,24,16,1,12,12,12,12,12,12,16};
    for(unsigned stream=0;stream<13;++stream) {
        const uint32_t address=stream==0?params:(stream+3)*0x2000;
        PPC_STORE_U32(table+stream*16,address); PPC_STORE_U32(table+stream*16+4,0);
        PPC_STORE_U32(table+stream*16+12,strides[stream]);
    }
    bool ok=true; unsigned comparisons=0,packed_comparisons=0,lazy_comparisons=0;
    for(unsigned vertices:{1u,8u,15u,16u,43u,64u,128u,256u}) for(unsigned tangents=0;tangents<2;++tangents) for(unsigned morphs=0;morphs<=5;++morphs) {
        PPC_STORE_U32(workload+12,vertices); PPC_STORE_U32(params,tangents); PPC_STORE_U32(params+8196,morphs);
        for(unsigned bone=0;bone<128;++bone) for(unsigned component=0;component<16;++component)
            WriteFloat(params+4+bone*64+component*4,component%4==3?NAN:value()*3,base);
        for(unsigned morph=0;morph<4;++morph) WriteFloat(params+8200+morph*4,value(),base);
        for(unsigned vertex=0;vertex<vertices;++vertex) {
            for(auto stream:{1u,2u,4u,6u,7u,8u,9u}) for(unsigned component=0;component<strides[stream]/4;++component)
                WriteFloat((stream+3)*0x2000+vertex*strides[stream]+component*4,value()*5,base);
            for(unsigned influence=0;influence<3;++influence) {
                const auto address=0xC000+vertex*24;
                PPC_STORE_U32(address+influence*4,vertex%5==0?uint32_t(-1):uint32_t((vertex*7+influence*29)%128));
                WriteFloat(address+12+influence*4,value(),base);
            }
            PPC_STORE_U8(0x10000+vertex,uint8_t(vertex%3==0?-1:vertex%3));
        }
        std::memset(base+0x1A000,0xA5,0x6000);
        PPCContext reference{}; reference.r1.u64=0x3F000; reference.r4.u64=workload; reference.fpscr.InitHost();
        __imp__sub_82968430(reference,base);
        std::array<uint8_t,0x6000> expected; std::memcpy(expected.data(),base+0x1A000,expected.size());
        std::memset(base+0x1A000,0xA5,0x6000);
        if(!(vertices<128?Execute<false,true>(workload,base):Execute<true,true>(workload,base))) { REXLOG_ERROR("SIMD skinning rejected reference fixture"); ok=false; break; }
        std::array<uint8_t,0x6000> packed;std::memcpy(packed.data(),base+0x1A000,packed.size());
        std::memset(base+0x1A000,0xA5,0x6000);
        if(!(vertices<128?Execute<false,false>(workload,base):Execute<true,false>(workload,base)) ||
           std::memcmp(packed.data(),base+0x1A000,packed.size())) {
            REXLOG_ERROR("Packed skin stream bytes differ from scalar SIMD: vertices {}, tangents {}, morphs {}",vertices,tangents,morphs);ok=false;goto finished;
        }
        ++packed_comparisons;
        for(bool wide:{false,true})for(bool enabled:{false,true}) {
            REXCVAR_SET(em2_wide_skin_palette,wide);REXCVAR_SET(em2_packed_skin_streams,enabled);std::memset(base+0x1A000,0xA5,0x6000);
            if(!ExecuteSelected(workload,base) || std::memcmp(packed.data(),base+0x1A000,packed.size())) {
                REXLOG_ERROR("Selected skin stream path mismatch: vertices {}, enabled {}",vertices,enabled);ok=false;goto finished;
            }
        }
        for(unsigned output=0;output<3;++output) for(unsigned vertex=0;vertex<vertices;++vertex)
            for(unsigned component=0;component<(output==2?4u:3u);++component) {
                const auto offset=output*0x2000+vertex*strides[10+output]+component*4;
                uint32_t bits; std::memcpy(&bits,expected.data()+offset,4);
                const auto original=std::bit_cast<float>(std::byteswap(bits));
                const auto actual=ReadFloat(0x1A000+offset,base);
                ++comparisons;
                if(output==2 && !tangents) {
                    if(PPC_LOAD_U32(0x1A000+offset)!=0xA5A5A5A5) ok=false;
                } else if(!std::isfinite(actual) || std::abs(original-actual)>std::max(1e-4f,std::abs(original)*2e-6f)) {
                    REXLOG_ERROR("SIMD skinning mismatch: tangent {}, morph {}, output {}, vertex {}, component {}: {} vs {}",
                        tangents,morphs,output,vertex,component,actual,original); ok=false; goto finished;
                }
            }
    }
    if(ok) {
        const auto entry_csr=_mm_getcsr();
        std::vector<uint8_t> snapshot(0x40000);std::memcpy(snapshot.data(),base,snapshot.size());
        constexpr std::array<uint32_t,12> special={0,0x80000000u,0x3F800000u,0x7F800000u,0xFF800000u,0x7FC12345u,0x7F812345u,0xFFC54321u,1,0x80000001u,0x00800000u,0x7F7FFFFFu};
        for(unsigned vertices:{16u,43u,64u,127u,128u,256u})for(unsigned used:{1u,8u,32u,128u})for(unsigned tangents:{0u,1u})
        for(unsigned morphs:{0u,1u,4u})for(unsigned bits:{0u,1u,2u})for(unsigned rounding:{0u,0x2000u,0x4000u,0x6000u}) {
            PPC_STORE_U32(workload+12,vertices);PPC_STORE_U32(params,tangents);PPC_STORE_U32(params+8196,morphs);
            for(unsigned i=0;i<vertices;++i)for(unsigned influence=0;influence<3;++influence) {
                const auto key_vertex=bits?i/8:i;
                PPC_STORE_U32(0xC000+i*24+influence*4,key_vertex%7==0?0xFFFFFFFFu:(key_vertex*7+influence*29)%used);
                if(bits==2)PPC_STORE_U32(0xC000+i*24+12+influence*4,special[(key_vertex+influence)%special.size()]);
                else WriteFloat(0xC000+i*24+12+influence*4,influence?0.25f:0.5f,base);
            }
            if(bits) {
                for(unsigned bone=0;bone<128;++bone)for(unsigned component=0;component<16;++component)
                    PPC_STORE_U32(params+4+bone*64+component*4,special[(bone+component)%special.size()]);
                for(unsigned i=0;i<vertices;++i)for(unsigned component=0;component<3;++component)
                    PPC_STORE_U32(0x8000+i*12+component*4,special[(i+component*3)%special.size()]);
            }else {std::memcpy(base+params+4,snapshot.data()+params+4,8192);std::memcpy(base+0x8000,snapshot.data()+0x8000,0x2000);}
            const unsigned entry=0x1F80u|rounding|(tangents?0x8040u:0u);
            std::memset(base+0x1A000,0xA5,0x6000);_mm_setcsr(entry);
            const bool reference=vertices<128?Execute<false,true>(workload,base):Execute<true,true>(workload,base);
            const unsigned csr=_mm_getcsr();std::array<uint8_t,0x6000> expected;std::memcpy(expected.data(),base+0x1A000,expected.size());
            std::memset(base+0x1A000,0xA5,0x6000);_mm_setcsr(entry);
            const bool actual=Execute<true,true,true>(workload,base);const auto actual_csr=_mm_getcsr();
            if(reference!=actual || csr!=actual_csr || std::memcmp(expected.data(),base+0x1A000,expected.size())) {
                REXLOG_ERROR("Lazy skin palette mismatch: vertices {}, bones {}, tangents {}, morphs {}, special {}, CSR {:X}/{:X}",vertices,used,tangents,morphs,bits,csr,actual_csr);ok=false;break;
            }
            std::memset(base+0x1A000,0xA5,0x6000);_mm_setcsr(entry);
            const bool wide=vertices<128?Execute<false,true,false,true>(workload,base):Execute<true,true,false,true>(workload,base);const auto wide_csr=_mm_getcsr();
            if(reference!=wide || csr!=wide_csr || std::memcmp(expected.data(),base+0x1A000,expected.size())) {
                REXLOG_ERROR("Wide skin palette mismatch: vertices {}, bones {}, tangents {}, morphs {}, special {}",vertices,used,tangents,morphs,bits);ok=false;break;
            }
            std::memset(base+0x1A000,0xA5,0x6000);_mm_setcsr(entry);
            const bool reused=vertices<128?Execute<false,true>(workload,base):Execute<true,true,false,true,true>(workload,base);
            if(reference!=reused || csr!=_mm_getcsr() || std::memcmp(expected.data(),base+0x1A000,expected.size())) {
                REXLOG_ERROR("Skin blend reuse mismatch: vertices {}, bones {}, tangents {}, morphs {}, special {}, rounding {}",vertices,used,tangents,morphs,bits,rounding);ok=false;break;
            }
            ++lazy_comparisons;
        }
        std::memcpy(base,snapshot.data(),snapshot.size());_mm_setcsr(entry_csr);
        REXLOG_INFO("On-demand/wide skin palette: {} complete raw-output/MXCSR comparisons including concentrated/spread bones, missing indices, morphs, tangent, NaNs, infinity and flush/round modes; {}",lazy_comparisons,ok?"passed":"failed");
    }
    if(ok) {
        struct RestoreGuardTest {
            uint8_t* base;std::vector<uint8_t> saved;DWORD protection=0;bool protected_page=false;
            bool reuse=REXCVAR_GET(em2_reuse_skin_blends);
            ~RestoreGuardTest(){if(protected_page){DWORD ignored;VirtualProtect(base+0x2B000,0x1000,protection,&ignored);}std::memcpy(base,saved.data(),saved.size());REXCVAR_SET(em2_reuse_skin_blends,reuse);}
        }restore_guard{base,std::vector<uint8_t>(base,base+0x40000)};
        if(!VirtualProtect(base+0x2B000,0x1000,PAGE_NOACCESS,&restore_guard.protection))ok=false;
        else {
            restore_guard.protected_page=true;
            for(unsigned scenario=0;scenario<3;++scenario) {
                constexpr uint32_t boundary=0x2B000,vertices=128;
                const auto skin_start=boundary-(scenario==0?vertices*24:scenario==1?28u:40u);
                PPC_STORE_U32(table+3*16,skin_start);PPC_STORE_U32(workload+12,vertices);
                PPC_STORE_U32(params,1);PPC_STORE_U32(params+8196,0);
                const auto records=scenario?1u:vertices;
                for(unsigned vertex=0;vertex<records;++vertex)for(unsigned influence=0;influence<3;++influence) {
                    PPC_STORE_U32(skin_start+vertex*24+influence*4,influence);
                    WriteFloat(skin_start+vertex*24+12+influence*4,influence?.25f:.5f,base);
                }
                if(scenario==1)PPC_STORE_U32(boundary-4,128); // Reject before any wide probe.
                if(scenario==2) {
                    PPC_STORE_U32(boundary-16,0);PPC_STORE_U32(boundary-12,128);
                    PPC_STORE_U32(boundary-8,2);WriteFloat(boundary-4,.5f,base);
                }
                std::array<uint8_t,0x6000> expected;
                std::memset(base+0x1A000,0xA5,expected.size());_mm_setcsr(0x1F80);
                const auto accepted=Execute<true,true,false,true>(workload,base);const auto csr=_mm_getcsr();
                std::memcpy(expected.data(),base+0x1A000,expected.size());
                for(bool enabled:{false,true}) {
                    REXCVAR_SET(em2_reuse_skin_blends,enabled);std::memset(base+0x1A000,0xA5,expected.size());_mm_setcsr(0x1F80);
                    const auto actual=ExecuteSelected(workload,base);
                    if(actual!=accepted || actual!=(scenario==0) || csr!=_mm_getcsr() || std::memcmp(expected.data(),base+0x1A000,expected.size())) {
                        REXLOG_ERROR("Skin blend guard-page mismatch: scenario {}, reuse {}",scenario,enabled);ok=false;
                    }
                }
            }
            if(ok)REXLOG_INFO("Skin blend exact signature guard passed: repeated128-record stream ends at inaccessible page, invalid next first/second indices reject before inaccessible weights, full output/MXCSR parity through both production toggle states (six comparisons)");
        }
    }
    if(ok && REXCVAR_GET(em2_benchmark_skin_blends)) {
        struct Affinity {DWORD_PTR previous=SetThreadAffinityMask(GetCurrentThread(),DWORD_PTR(1)<<GetCurrentProcessorNumber());~Affinity(){if(previous)SetThreadAffinityMask(GetCurrentThread(),previous);}} affinity;
        for(unsigned vertices:{128u,256u})for(unsigned run:{1u,2u,4u,16u,256u})for(unsigned tangents:{0u,1u}) {
            PPC_STORE_U32(workload+12,vertices);PPC_STORE_U32(params,tangents);PPC_STORE_U32(params+8196,0);
            for(unsigned i=0;i<vertices;++i)for(unsigned influence=0;influence<3;++influence) {
                PPC_STORE_U32(0xC000+i*24+influence*4,((i/run)*7+influence*29)%128);
                WriteFloat(0xC000+i*24+12+influence*4,influence?.25f:.5f,base);
            }
            auto measure=[&](bool candidate) {
                const auto start=std::chrono::steady_clock::now();uint64_t sum=0;
                for(unsigned repeat=0;repeat<4000;++repeat) {
                    const bool accepted=candidate?Execute<true,true,false,true,true>(workload,base):Execute<true,true,false,true>(workload,base);
                    if(!accepted)throw std::runtime_error("Skin blend benchmark rejected workload");
                    sum+=PPC_LOAD_U32(0x1A000+(repeat%vertices)*12);
                }
                return std::pair(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count()/4000,sum);
            };
            measure(false);measure(true);std::array<double,7> old{},candidate{};
            for(unsigned round=0;round<7;++round)for(unsigned phase=0;phase<2;++phase) {
                const bool enabled=(round+phase)&1;const auto sample=measure(enabled);
                (enabled?candidate:old)[round]=sample.first;
                if(sample.second!=measure(!enabled).second)throw std::runtime_error("Skin blend benchmark output checksum mismatch");
            }
            std::sort(old.begin(),old.end());std::sort(candidate.begin(),candidate.end());
            REXLOG_INFO("Skin blend reuse benchmark: {} vertices, run {}, tangent {}, baseline {:.6f} us, reuse {:.6f} us, {:.6f}x (7 alternating rounds,4000 workloads, pinned core; kernel only, not game FPS)",vertices,run,tangents,old[3],candidate[3],old[3]/candidate[3]);
        }
    }
    if(ok && REXCVAR_GET(em2_benchmark_skinning)) {
        // Same final randomized 256-vertex fixture and output memory in both
        // paths; run alternating rounds to reduce warm-cache/order bias.
        std::array<double,5> uncached{},cached{};
        auto measure=[&](auto execute) {
            const auto begin=std::chrono::steady_clock::now();
            for(unsigned i=0;i<10000;++i) if(!execute(workload,base)) return -1.0;
            return std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-begin).count()/10000;
        };
        for(unsigned round=0;round<5;++round) {
            if(round%2) { cached[round]=measure(Execute<true>); uncached[round]=measure(Execute<false>); }
            else { uncached[round]=measure(Execute<false>); cached[round]=measure(Execute<true>); }
        }
        std::sort(uncached.begin(),uncached.end()); std::sort(cached.begin(),cached.end());
        REXLOG_INFO("Skin palette benchmark: 256 vertices, median uncached {} us, cached {} us per workload (5 alternating rounds of 10000)",uncached[2],cached[2]);
        for(unsigned vertices:{128u,256u})for(unsigned tangents:{0u,1u}) {
            PPC_STORE_U32(workload+12,vertices);PPC_STORE_U32(params,tangents);PPC_STORE_U32(params+8196,0);
            std::array<double,7> narrow{},wide{};
            for(unsigned round=0;round<7;++round)for(unsigned phase=0;phase<2;++phase) {
                const bool candidate=(round+phase)%2;
                REXCVAR_SET(em2_wide_skin_palette,candidate);
                (candidate?wide:narrow)[round]=measure(ExecuteSelected);
            }
            std::sort(narrow.begin(),narrow.end());std::sort(wide.begin(),wide.end());
            REXLOG_INFO("Skin wide palette benchmark: {} vertices, tangent {}, narrow {} us, wide {} us, speedup {}x (7 alternating rounds of 10000)",vertices,tangents,narrow[3],wide[3],narrow[3]/wide[3]);
        }
        // Separate lazy/eager policy measurement; the old packed-stream benchmark
        // below keeps lazy mode off so it continues to isolate stream transfers.
        for(unsigned vertices:{16u,43u,64u,128u,256u})for(unsigned used:{1u,8u,32u,128u})for(unsigned tangents:{0u,1u}) {
            PPC_STORE_U32(workload+12,vertices);PPC_STORE_U32(params,tangents);PPC_STORE_U32(params+8196,0);
            for(unsigned i=0;i<vertices;++i)for(unsigned influence=0;influence<3;++influence)
                PPC_STORE_U32(0xC000+i*24+influence*4,(i*7+influence*29)%used);
            std::array<double,5> legacy{},lazy{};
            for(unsigned round=0;round<5;++round)for(unsigned phase=0;phase<2;++phase) {
                const bool candidate=(round+phase)%2;
                auto execute=candidate?Execute<true,true,true>:(vertices<128?Execute<false,true>:Execute<true,true>);
                (candidate?lazy:legacy)[round]=measure(execute);
            }
            std::sort(legacy.begin(),legacy.end());std::sort(lazy.begin(),lazy.end());
            REXLOG_INFO("Skin lazy palette benchmark: {} vertices, {} bones, tangent {}, legacy {} us, lazy {} us, speedup {}x (5 alternating rounds of 10000)",vertices,used,tangents,legacy[2],lazy[2],legacy[2]/lazy[2]);
        }
        REXCVAR_SET(em2_wide_skin_palette,false); // Keep stream-transfer benchmark independent.
        for(unsigned vertices:{1u,8u,15u,16u,43u,64u,128u,256u})for(unsigned tangents:{0u,1u}) {
            PPC_STORE_U32(workload+12,vertices);PPC_STORE_U32(params,tangents);
            std::array<double,7> scalar{},packed{};
            for(unsigned round=0;round<7;++round)for(unsigned phase=0;phase<2;++phase) {
                const bool use_packed=(round+phase)%2;
                auto execute=use_packed?ExecuteSelected:ExecuteScalarSelected;
                (use_packed?packed:scalar)[round]=measure(execute);
            }
            std::sort(scalar.begin(),scalar.end());std::sort(packed.begin(),packed.end());
            REXLOG_INFO("Skin stream benchmark: {} vertices, tangent {}, scalar {} us, packed {} us, speedup {}x (7 alternating rounds of 10000)",vertices,tangents,scalar[3],packed[3],scalar[3]/packed[3]);
        }
    }
finished:
    unsigned boundary_comparisons=0;
    if(!VirtualAlloc(base+0xDFFF0000,0x30000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0xFFFF0000,0x11000,MEM_COMMIT,PAGE_READWRITE))ok=false;
    else {
        // Run complete kernels with input/output streams crossing the SDK's
        // physical-map gap and 4-GiB wrap, comparing all fixture memory.
        constexpr size_t sizes[]={0x40000,0x30000,0x11000};
        uint8_t* regions[]={base,base+0xDFFF0000,base+0xFFFF0000};
        std::array<std::vector<uint8_t>,3> before,expected;
        for(unsigned i=0;i<3;++i){before[i].resize(sizes[i]);expected[i].resize(sizes[i]);}
        std::array<uint32_t,48> positions;
        for(unsigned i=0;i<positions.size();++i)positions[i]=PPC_LOAD_U32(0x8000+i*4);
        for(bool output:{false,true})for(uint32_t address:{0xDFFFFFA8u,0xFFFFFFA8u}) {
            PPC_STORE_U32(workload+12,16);PPC_STORE_U32(params,0);PPC_STORE_U32(params+8196,0);
            PPC_STORE_U32(table+16,output?0x8000:address);PPC_STORE_U32(table+10*16,output?address:0x1A000);
            if(!output)for(unsigned i=0;i<positions.size();++i)PPC_STORE_U32(uint32_t(address+i*4),positions[i]);
            for(unsigned i=0;i<3;++i)std::memcpy(before[i].data(),regions[i],sizes[i]);
            if(!ExecuteScalarSelected(workload,base))ok=false;
            for(unsigned i=0;i<3;++i){std::memcpy(expected[i].data(),regions[i],sizes[i]);std::memcpy(regions[i],before[i].data(),sizes[i]);}
            REXCVAR_SET(em2_packed_skin_streams,true);
            if(!ExecuteSelected(workload,base))ok=false;
            for(unsigned i=0;i<3;++i)if(std::memcmp(expected[i].data(),regions[i],sizes[i]))ok=false;
            ++boundary_comparisons;
        }
        PPC_STORE_U32(table+16,0x8000);PPC_STORE_U32(table+10*16,0x1A000);
    }
    unsigned palette_mapping_comparisons=0;
    if(ok)for(unsigned mapped_vertices:{16u,128u}) {
        PPC_STORE_U32(workload+12,mapped_vertices);PPC_STORE_U32(params,0);PPC_STORE_U32(params+8196,0);
        std::memset(base+0x1A000,0xA5,0x6000);
        PPCContext low{};low.r1.u64=0x3F000;low.r4.u64=workload;low.fpscr.InitHost();
        __imp__sub_82968430(low,base);
        std::array<uint8_t,0x6000> expected;std::memcpy(expected.data(),base+0x1A000,expected.size());
        for(uint32_t address:{0xE0001000u,0xE0001004u,0xE0001008u,0xE000100Cu}) {
            for(unsigned offset=0;offset<8216;offset+=4)PPC_STORE_U32(address+offset,PPC_LOAD_U32(params+offset));
            const auto old_row=LoadRow(base+uint32_t(address+4)),mapped_row=LoadRow(PPC_RAW_ADDR(uint32_t(address+4)));
            if(!std::memcmp(&old_row,&mapped_row,sizeof(old_row))){REXLOG_ERROR("E0 fixture does not distinguish the old raw-offset reader");ok=false;break;}
            PPC_STORE_U32(table,address);std::memset(base+0x1A000,0xA5,0x6000);
            PPCContext original{};original.r1.u64=0x3F000;original.r4.u64=workload;original.fpscr.InitHost();
            __imp__sub_82968430(original,base);
            if(std::memcmp(expected.data(),base+0x1A000,expected.size())) {REXLOG_ERROR("Original skin kernel E0 palette disagrees with equivalent low-memory fixture");ok=false;break;}
            for(bool wide:{false,true}) {
                REXCVAR_SET(em2_wide_skin_palette,wide);std::memset(base+0x1A000,0xA5,0x6000);
                if(!ExecuteSelected(workload,base))ok=false;
                for(unsigned out=0;out<2;++out)for(unsigned i=0;i<mapped_vertices*3;++i) {
                    uint32_t bits;std::memcpy(&bits,expected.data()+out*0x2000+i*4,4);
                    const auto reference=std::bit_cast<float>(std::byteswap(bits)),actual=ReadFloat(0x1A000+out*0x2000+i*4,base);
                    if(!std::isfinite(actual) || std::abs(actual-reference)>std::max(1e-4f,std::abs(reference)*2e-6f))ok=false;
                }
                ++palette_mapping_comparisons;
            }
        }
        for(uint32_t address:{0xDFFFF000u,0xFFFFF000u})for(bool wide:{false,true}) {
            PPC_STORE_U32(table,address);REXCVAR_SET(em2_wide_skin_palette,wide);std::memset(base+0x1A000,0xA5,0x6000);
            if(ExecuteSelected(workload,base) || !std::all_of(base+0x1A000,base+0x20000,[](uint8_t v){return v==0xA5;}))ok=false;
            ++palette_mapping_comparisons;
        }
        PPC_STORE_U32(table,params);
        REXLOG_INFO("Skin palette physical mapping: {} E0 aligned/unaligned original-kernel comparisons and gap/wrap no-write fallback checks; {}",palette_mapping_comparisons,ok?"passed":"failed");
    }
    // Check raw payloads, every byte alignment, neighboring bytes, and a stream
    // ending exactly at an inaccessible page. NaNs must remain raw bits.
    for(unsigned alignment=0;alignment<16;++alignment)for(uint32_t bits:{0u,0x80000000u,0x3F800000u,0x7F800001u,0x7FC12345u,0xFFFFFFFFu}) {
        const uint32_t source=0x30000+alignment,destination=0x32000+alignment;
        for(unsigned i=0;i<3;++i)PPC_STORE_U32(source+i*4,bits+i);
        const auto scalar=LoadThree<false>(source,base),packed=LoadThree<true>(source,base);
        alignas(16) uint32_t a[4],b[4];_mm_store_si128(reinterpret_cast<__m128i*>(a),_mm_castps_si128(scalar));_mm_store_si128(reinterpret_cast<__m128i*>(b),_mm_castps_si128(packed));
        std::memset(base+0x32000,0xA5,64);StoreThree<true>(destination,packed,base);
        if(std::memcmp(a,b,16) || std::memcmp(base+source,base+destination,12))ok=false;
        for(unsigned i=0;i<64;++i)if((i<alignment || i>=alignment+12) && base[0x32000+i]!=0xA5)ok=false;
    }
    DWORD old_source=0,old_destination=0;
    if(!VirtualProtect(base+0x31000,0x1000,PAGE_NOACCESS,&old_source))ok=false;
    else {
        if(!VirtualProtect(base+0x33000,0x1000,PAGE_NOACCESS,&old_destination))ok=false;
        else {
            for(unsigned i=0;i<3;++i)PPC_STORE_U32(0x30FF4+i*4,0xDEADBEEFu+i);
            StoreThree<true>(0x32FF4,LoadThree<true>(0x30FF4,base),base);
            if(std::memcmp(base+0x30FF4,base+0x32FF4,12))ok=false;
            DWORD ignored;VirtualProtect(base+0x33000,0x1000,old_destination,&ignored);
        }
        DWORD ignored;VirtualProtect(base+0x31000,0x1000,old_source,&ignored);
    }
    REXLOG_INFO("Packed skin stream validation: {} complete output comparisons, {} complete physical-gap/wrap input/output comparisons; raw-bit/alignment/canary/guard-page tests {}",packed_comparisons,boundary_comparisons,ok?"passed":"failed");
    PPC_STORE_U32(table+10*16,PPC_LOAD_U32(table+16));
    if(ExecuteSelected(workload,base)) { REXLOG_ERROR("SIMD skinning accepted aliased input/output"); ok=false; }
    PPC_STORE_U32(table+10*16,0x1A000);
    PPC_STORE_U32(table+3*16+12,20);
    if(ExecuteSelected(workload,base)) { REXLOG_ERROR("SIMD skinning accepted an unsupported vertex layout"); ok=false; }
    // Exercise the actual native partition prepare method, including the
    // default 1024-byte heuristic, rather than duplicating its arithmetic.
    constexpr uint32_t task=0x22000,policy=0x23000,stream=0x24000,array=0x25000,kernel=0x26000,vtable=0x27000;
    PPC_STORE_U32(task+48,array); PPC_STORE_U32(task+56,1); PPC_STORE_U32(task+68,0);
    PPC_STORE_U32(array,stream); PPC_STORE_U16(stream+6,24); PPC_STORE_U32(stream+60,5200);
    PPC_STORE_U32(task+104,kernel); PPC_STORE_U32(kernel,vtable); PPC_STORE_U32(vtable+8,0x82968430);
    auto prepare=[&](uint32_t requested,uint32_t authored) {
        PPC_STORE_U32(task+128,authored);
        PPCContext context{}; context.r1.u64=0x3F000; context.r3.u64=policy; context.r4.u64=task;
        PrepareSkinPartition(context,base,requested);
        return PPC_LOAD_U32(policy+4);
    };
    if(prepare(0,0xFFFFFFFF)!=43 || prepare(256,0xFFFFFFFF)!=256 ||
       PPC_LOAD_U32(task+128)!=0xFFFFFFFF || PPC_LOAD_U32(policy+12)!=5200 ||
       prepare(10000,0xFFFFFFFF)!=256 || prepare(256,17)!=17) {
        REXLOG_ERROR("Skin batch preparation or override restoration failed"); ok=false;
    }
    PPC_STORE_U32(vtable+8,0x8253EE80);
    if(prepare(256,0xFFFFFFFF)!=43) { REXLOG_ERROR("Skin batching affected a different kernel"); ok=false; }
    if(ok) REXLOG_INFO("SIMD skinning matched original kernel in {} component comparisons",comparisons);
    return ok;
}
}
