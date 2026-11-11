#include "emr_ability_animation_probe.h"
#include "ppc_context.h"
#include "em1_havok_pack.h"
#include "guest_read_batch.h"
#include <rex/kernel/kernel_state.h>
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>

PPC_EXTERN_FUNC(sub_822F2900);
PPC_EXTERN_FUNC(sub_822FE000);

namespace {
uint32_t BoundAbilityAnimation(uint8_t* base,uint32_t actor,std::string_view filename){
    em2::GuestReadBatch inspection;
    auto mapped=[&](uint32_t at,size_t n){return at>=4096 && uint64_t(at)+n<=0x100000000ull && em2::GuestReadBatch::Readable(PPC_RAW_ADDR(at),n);};
    auto word=[&](uint32_t at){em2::em1::hkx::Need(mapped(at,4),"Bound animation pointer is unavailable");return PPC_LOAD_U32(at);};
    em2::em1::hkx::Need(mapped(actor,212),"Native animation component unavailable");
    const auto graph=word(actor+192),context=word(actor+208),character=word(context);
    const auto bindings=word(character+68),items=word(bindings+8),count=word(bindings+12);
    em2::em1::hkx::Need(count && count<1024 && mapped(items,size_t(count)*4),"Invalid native animation binding set");
    const auto root=word(graph+84),states=word(root+100),number=word(root+104);
    em2::em1::hkx::Need(number && number<512 && mapped(states,size_t(number)*4),"Invalid native movement graph");
    for(unsigned i=0;i<number;++i){
        const auto state=word(states+i*4);if(!mapped(state,80))continue;
        const auto clip=word(state+52);if(!mapped(clip,112))continue;
        const auto name=word(clip+48);if(!mapped(name,160))continue;
        std::string path;for(unsigned k=0;k<160;++k){const auto c=PPC_LOAD_U8(name+k);if(!c)break;if(c<32 || c>126){path.clear();break;}path+=char(c);}
        const auto slash=path.find_last_of("/\\");if(slash==path.npos || std::string_view(path).substr(slash+1)!=filename)continue;
        const auto index=PPC_LOAD_U16(clip+80);
        em2::em1::hkx::Need(index<count,"Ability clip is not linked to a native binding");
        const auto wrapper=word(items+index*4),binding=word(wrapper+8),animation=word(binding+12);
        em2::em1::hkx::Need(mapped(animation,132) && word(animation)==0x8200360C && word(animation+16)==95 && !word(animation+20),"Ability resolved to an incompatible or fallback animation");
        return animation;
    }
    throw std::runtime_error("Ability clip is missing from the native movement graph");
}
}

namespace em2 {
bool ProbeEMRAbilityClips(PPCContext& ctx,uint8_t* base,const std::filesystem::path& directory,std::string& error,uint32_t actor_animation) {
    struct Restore {PPCContext& ctx;PPCContext saved;unsigned csr=_mm_getcsr();DWORD last=GetLastError();~Restore(){ctx=saved;_mm_setcsr(csr);SetLastError(last);}} restore{ctx,ctx};
    namespace hkx=em1::hkx;
    auto read=[](const std::filesystem::path& file,size_t maximum){
        std::ifstream stream(file,std::ios::binary|std::ios::ate);
        hkx::Need(bool(stream),"Cannot open converted animation fixture");const auto size=stream.tellg();
        hkx::Need(size>0 && uint64_t(size)<=maximum,"Animation fixture exceeds budget");
        std::vector<uint8_t> data(size_t(size),uint8_t{});stream.seekg(0);stream.read(reinterpret_cast<char*>(data.data()),data.size());
        hkx::Need(bool(stream),"Incomplete animation fixture read");return data;
    };
    unsigned clips=0,samples=0;float maximum_position=0,maximum_scale=0,maximum_angle=0;
    try {
        auto* kernel=rex::kernel::kernel_state();hkx::Need(kernel && base,"Native animation probe requires initialized kernel");
        const auto expected=read(directory/"native-sampling-expected.bin",2*1024*1024);size_t cursor=0;
        auto word=[&](){hkx::Need(cursor+4<=expected.size(),"Truncated sampling fixture");uint32_t value;std::memcpy(&value,expected.data()+cursor,4);cursor+=4;return value;};
        hkx::Need(expected.size()>12 && !std::memcmp(expected.data(),"EMRQA001",8),"Wrong sampling fixture version");cursor=8;
        const auto count=word();hkx::Need(count==6,"Expected five authored clips and final-pose hold");
        for(unsigned clip=0;clip<count;++clip) {
            const auto name_size=word(),sample_count=word();hkx::Need(name_size && name_size<=96 && sample_count && sample_count<=16 && cursor+name_size<=expected.size(),"Invalid animation fixture record");
            const std::string name(reinterpret_cast<const char*>(expected.data()+cursor),name_size);cursor+=name_size;
            hkx::Need(name.find_first_not_of("abcdefghijklmnopqrstuvwxyz_.")==name.npos && name.ends_with(".hkx"),"Invalid animation fixture filename");
            const auto bytes=read(directory/name,2*1024*1024);hkx::Pack pack(bytes,false);hkx::Location animation{UINT32_MAX,0};
            for(const auto& [key,klass]:pack.classes)if(klass=="hkaSplineCompressedAnimation"){
                hkx::Need(animation.section==UINT32_MAX,"Multiple animation objects");animation={uint32_t(key>>32),uint32_t(key)};
            }
            hkx::Need(animation.section!=UINT32_MAX,"No native spline object");
            const uint32_t output_offset=uint32_t((144+bytes.size()+15)&~size_t(15)),end=output_offset+95*48+64;
            struct Scratch {rex::memory::Memory& memory;uint32_t at;Scratch(rex::memory::Memory& m,uint32_t n):memory(m),at(m.SystemHeapAlloc(n,32)){}~Scratch(){if(at)memory.SystemHeapFree(at);}} scratch(*kernel->memory(),end);
            hkx::Need(scratch.at!=0,"Cannot allocate private native animation probe");
            const auto owner=scratch.at,packed=owner+144,output=owner+output_offset;
            std::memset(base+owner,0,end);std::memcpy(base+packed,bytes.data(),bytes.size());
            for(const auto& [key,target]:pack.pointers)PPC_STORE_U32(packed+pack.sections[uint32_t(key>>32)].start+uint32_t(key),packed+pack.sections[target.section].start+target.offset);
            std::memcpy(base+owner,base+packed+pack.sections[animation.section].start+animation.offset,132);
            hkx::Need(PPC_LOAD_U32(owner+16)==95 && PPC_LOAD_U32(owner+20)==0,"Unexpected ability track counts");
            ctx.r3.u64=owner;ctx.r4.u64=1;sub_822F2900(ctx,base);
            hkx::Need(PPC_LOAD_U32(owner)==0x8200360c,"Native spline finish did not install the expected sampler");
            const auto sampler=actor_animation?BoundAbilityAnimation(base,actor_animation,name):owner;
            for(unsigned sample=0;sample<sample_count;++sample) {
                const float time=std::bit_cast<float>(word());hkx::Need(std::isfinite(time) && time>=0,"Invalid expected sample time");
                std::memset(base+output,0xff,95*48+64);
                ctx.r3.u64=sampler;ctx.f1.f64=time;ctx.r5.u64=output;ctx.r6.u64=output+95*48;ctx.r7.u64=0;
                sub_822FE000(ctx,base);
                for(unsigned bone=0;bone<95;++bone) {
                    std::array<float,12> source{},actual{};
                    for(unsigned k=0;k<12;++k){source[k]=std::bit_cast<float>(word());actual[k]=std::bit_cast<float>(PPC_LOAD_U32(output+bone*48+k*4));if(k!=3 && k!=11)hkx::Need(std::isfinite(actual[k]),"Nonfinite native ability pose");}
                    for(unsigned k=0;k<3;++k){maximum_position=std::max(maximum_position,std::abs(actual[k]-source[k]));maximum_scale=std::max(maximum_scale,std::abs(actual[8+k]-source[8+k]));}
                    double dot=0,a=0,b=0;for(unsigned k=4;k<8;++k){dot+=double(source[k])*actual[k];a+=double(source[k])*source[k];b+=double(actual[k])*actual[k];}
                    if(!(a>1e-12 && b>1e-12)){
                        REXLOG_ERROR("EMR sampler {} time {} bone {} expected quaternion {} {} {} {} actual {} {} {} {}",name,time,bone,source[4],source[5],source[6],source[7],actual[4],actual[5],actual[6],actual[7]);
                        hkx::Need(false,"Degenerate native ability quaternion");
                    }
                    const auto angle=float(2*std::acos(std::clamp(std::abs(dot)/std::sqrt(a*b),0.,1.))*180/3.141592653589793);
                    maximum_angle=std::max(maximum_angle,angle);
                }
                for(unsigned k=0;k<64;++k)hkx::Need(base[output+95*48+k]==0xff,"Native ability sampler overwrote guard");
                ++samples;
            }
            ++clips;
        }
        hkx::Need(cursor==expected.size(),"Trailing animation fixture bytes");
        REXLOG_INFO("EMR {} sampler comparison: clips {} samples {} bones95 maximum position {} scale {} rotation_degrees {}",actor_animation?"live bound":"native fixture",clips,samples,maximum_position,maximum_scale,maximum_angle);
        hkx::Need(maximum_position<.0025f && maximum_scale<.0001f && maximum_angle<.15f,"Native ability samples differ from authored GLB beyond import tolerance");
        error.clear();return true;
    } catch(const std::exception& failure) {
        error=failure.what();REXLOG_ERROR("EMR native sampler failed after {} clips/{} samples: {}",clips,samples,error);return false;
    }
}
}
