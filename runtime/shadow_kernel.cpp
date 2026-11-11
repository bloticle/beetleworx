#include "shadow_kernel.h"
#include "bloom_kernel.h"
#include "../private/shadow_shader_test_data.h"
#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/graphics/d3d12/shader.h>
#include <rex/graphics/d3d12/pipeline_cache.h>
#include <rex/logging.h>
#include <atomic>
#include <bit>
#include <span>
#include <algorithm>

// Substitute before the SDK computes its hash, lookup identity and storage
// record. Constructors, native cache logic and unwind metadata stay intact.
extern "C" rex::graphics::d3d12::D3D12Shader* EM2OriginalLoadShader(
    rex::graphics::d3d12::PipelineCache*,rex::graphics::xenos::ShaderType,const uint32_t*,uint32_t);
namespace {
std::atomic<unsigned> samples{4};
std::span<const uint32_t> Select(rex::graphics::xenos::ShaderType type,const uint32_t* input,size_t count,std::endian endian,unsigned quality) {
    if(quality==4 || type!=rex::graphics::xenos::ShaderType::kPixel || count!=em2::shadow_test_data::original.size())return {};
    for(size_t i=0;i<count;++i) {
        const auto word=endian==std::endian::native?input[i]:std::byteswap(input[i]);
        if(word!=em2::shadow_test_data::original[i])return {};
    }
    if(quality==8)return em2::shadow_test_data::samples8;
    if(quality==16)return em2::shadow_test_data::samples16;
    if(quality==20)return em2::shadow_test_data::adaptive16;
    return {};
}
}
void em2::ConfigureShadowKernel(unsigned quality) {samples.store(quality==8 || quality==16 || quality==20?quality:4,std::memory_order_relaxed);}
unsigned em2::NativeShadowSamples(){return samples.load(std::memory_order_relaxed);}
const char* em2::NativeShadowCacheKey() {
    const auto quality=NativeShadowSamples();
    return quality==8?shadow_test_data::cache_samples8:quality==16?shadow_test_data::cache_samples16:quality==20?shadow_test_data::cache_adaptive16:"";
}
rex::graphics::d3d12::D3D12Shader* rex::graphics::d3d12::PipelineCache::LoadShader(
    rex::graphics::xenos::ShaderType type,const uint32_t* words,uint32_t count) {
    const auto quality=em2::NativeShadowSamples();
    const auto bloom=em2::SelectNativeBloomShader(words,count,type==rex::graphics::xenos::ShaderType::kPixel);
    if(!bloom.empty()){words=bloom.data();count=uint32_t(bloom.size());}
    const auto replacement=Select(type,words,count,std::endian::big,quality);
    if(!replacement.empty()) {
        static const auto eight=[] {auto code=em2::shadow_test_data::samples8;for(auto& word:code)word=std::byteswap(word);return code;}();
        static const auto sixteen=[] {auto code=em2::shadow_test_data::samples16;for(auto& word:code)word=std::byteswap(word);return code;}();
        static const auto adaptive=[] {auto code=em2::shadow_test_data::adaptive16;for(auto& word:code)word=std::byteswap(word);return code;}();
        words=quality==8?eight.data():quality==16?sixteen.data():adaptive.data();count=uint32_t(replacement.size());
        static std::atomic<bool> reported{false};
        if(!reported.exchange(true))REXLOG_INFO("Native shadow receiver: {} depth reads per map; depth-adaptive {}",em2::NativeShadowSamples(),quality==20);
    }
    return EM2OriginalLoadShader(this,type,words,count);
}
bool em2::TestShadowKernelSelection() {
    const auto previous=NativeShadowSamples();
    struct Restore {unsigned value;~Restore(){ConfigureShadowKernel(value);}} restore{previous};
    unsigned comparisons=0;
    if(!Select(rex::graphics::xenos::ShaderType::kPixel,nullptr,0,std::endian::big,16).empty())return false;
    for(unsigned quality:{4,8,16,20})for(bool big:{false,true}) {
        auto input=shadow_test_data::original;
        if(big)for(auto& word:input)word=std::byteswap(word);
        ConfigureShadowKernel(quality);
        if(quality==4?NativeShadowCacheKey()[0]!=0:std::string_view(NativeShadowCacheKey()).find("shadow-")!=0)return false;
        const auto selected=Select(rex::graphics::xenos::ShaderType::kPixel,input.data(),input.size(),
            big?std::endian::big:std::endian::native,quality);
        const std::span<const uint32_t> expected=quality==4?std::span<const uint32_t>(shadow_test_data::original):
            quality==8?std::span<const uint32_t>(shadow_test_data::samples8):quality==16?std::span<const uint32_t>(shadow_test_data::samples16):std::span<const uint32_t>(shadow_test_data::adaptive16);
        if(quality==4?!selected.empty():selected.size()!=expected.size() || !std::equal(expected.begin(),expected.end(),selected.begin()))return false;
        for(bool vertex:{false,true}) {
            auto different=input;if(!vertex)different[12]^=1;
            if(!Select(vertex?rex::graphics::xenos::ShaderType::kVertex:rex::graphics::xenos::ShaderType::kPixel,
                different.data(),different.size(),big?std::endian::big:std::endian::native,quality).empty())return false;
            ++comparisons;
        }
        ++comparisons;
    }
    REXLOG_INFO("Shadow microcode selection passed {} cases: original/8/16/adaptive, both endian inputs, exact program and unrelated/vertex preservation; actual cache/load bridge requires GPU diagnostic",comparisons);
    return true;
}
