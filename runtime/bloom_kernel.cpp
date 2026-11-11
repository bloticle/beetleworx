#include "bloom_kernel.h"
#include "../private/bloom_shader_test_data.h"
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <vector>

namespace {
std::atomic<unsigned> mode{2};
// ALLOC one color; EXEC_END; export constant zero to every oC0 channel.
// The export's scalar_dest_rel bit supplies zero for unwritten channels.
// No texture instructions, interpolators or float constants are consumed.
constexpr std::array<uint32_t,6> disabled={0,0x1001C400,0x20000000,0xC800C000,0,0x02000000};
template<size_t N> auto Big(std::array<uint32_t,N> words){for(auto& word:words)word=std::byteswap(word);return words;}
std::span<const uint32_t> Select(const uint32_t* input,size_t count,bool pixel,std::endian endian,unsigned quality) {
    using namespace em2::bloom_test_data;
    if(pixel && input && count==composite.size()) {
        bool original=true,linear=true;
        for(size_t i=0;i<count;++i) {
            const auto word=endian==std::endian::native?input[i]:std::byteswap(input[i]);
            original&=word==composite[i];linear&=word==composite_linear[i];
        }
        if(!original && !linear)return {};
        if(quality==2?linear:original)return {};
        static const auto co=Big(composite),cl=Big(composite_linear);
        return quality==2?std::span<const uint32_t>(cl):std::span<const uint32_t>(co);
    }
    if(!pixel || !input || count!=horizontal_original.size())return {};
    const std::array<std::span<const uint32_t>,4> programs={horizontal_original,horizontal_linear,vertical_original,vertical_linear};
    unsigned found=4;
    for(unsigned candidate=0;candidate<programs.size();++candidate) {
        bool match=true;
        for(size_t i=0;i<count;++i)if((endian==std::endian::native?input[i]:std::byteswap(input[i]))!=programs[candidate][i]){match=false;break;}
        if(match){found=candidate;break;}
    }
    if(found==4 || (quality==1 && !(found&1)) || (quality==2 && (found&1)))return {};
    static const auto off=Big(disabled);
    static const auto ho=Big(horizontal_original),hl=Big(horizontal_linear),vo=Big(vertical_original),vl=Big(vertical_linear);
    if(quality==0)return off;
    if(quality==1)return found<2?std::span<const uint32_t>(ho):std::span<const uint32_t>(vo);
    return found<2?std::span<const uint32_t>(hl):std::span<const uint32_t>(vl);
}
}
void em2::ConfigureBloomSampling(unsigned value){mode.store(value<3?value:2,std::memory_order_relaxed);}
unsigned em2::NativeBloomSampling(){return mode.load(std::memory_order_relaxed);}
std::span<const uint32_t> em2::DisabledBloomShader(){return disabled;}
std::span<const uint32_t> em2::SelectNativeBloomShader(const uint32_t* input,size_t count,bool pixel){return Select(input,count,pixel,std::endian::big,NativeBloomSampling());}
bool em2::TestBloomKernelSelection() {
    using namespace bloom_test_data;
    const auto previous=NativeBloomSampling();struct Restore{unsigned value;~Restore(){ConfigureBloomSampling(value);}}restore{previous};
    const std::array<std::span<const uint32_t>,4> programs={horizontal_original,horizontal_linear,vertical_original,vertical_linear};
    unsigned checks=0;
    for(unsigned quality=0;quality<3;++quality)for(bool big:{false,true})for(unsigned variant=0;variant<4;++variant) {
        std::vector<uint32_t> input(programs[variant].begin(),programs[variant].end());
        if(big)for(auto& word:input)word=std::byteswap(word);
        const auto result=Select(input.data(),input.size(),true,big?std::endian::big:std::endian::native,quality);
        const bool unchanged=(quality==1 && !(variant&1)) || (quality==2 && (variant&1));
        const std::span<const uint32_t> expected=quality==0?std::span<const uint32_t>(disabled):programs[(variant&2)+(quality==2)];
        if(unchanged?!result.empty():result.size()!=expected.size())return false;
        if(!unchanged)for(size_t i=0;i<result.size();++i)if(std::byteswap(result[i])!=expected[i])return false;
        if(!Select(input.data(),input.size(),false,big?std::endian::big:std::endian::native,quality).empty())return false;
        input[12]^=1;
        if(!Select(input.data(),input.size(),true,big?std::endian::big:std::endian::native,quality).empty())return false;
        ++checks;
    }
    if(!Select(nullptr,54,true,std::endian::big,0).empty())return false;
    for(unsigned quality=0;quality<3;++quality)for(bool big:{false,true})for(bool linear:{false,true}) {
        auto input=linear?composite_linear:composite;if(big)for(auto& word:input)word=std::byteswap(word);
        const auto endian=big?std::endian::big:std::endian::native;
        const auto result=Select(input.data(),input.size(),true,endian,quality);
        const bool unchanged=(quality==2)==linear;
        const auto expected=Big(quality==2?composite_linear:composite);
        if(unchanged?!result.empty():result.size()!=expected.size() || !std::equal(result.begin(),result.end(),expected.begin()))return false;
        if(!Select(input.data(),input.size(),false,endian,quality).empty())return false;
        input[12]^=1;if(!Select(input.data(),input.size(),true,endian,quality).empty())return false;
        ++checks;
    }
    ConfigureBloomSampling(99);if(NativeBloomSampling()!=2)return false;
    REXLOG_INFO("Bloom program selection passed {} combinations: Off/Original/Smooth, endian inputs, both passes, exact-match and unrelated/vertex guards",checks);
    return true;
}
