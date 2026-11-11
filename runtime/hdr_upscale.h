#pragma once
#include <cstdint>
#include <cstddef>
namespace em2 {
struct HdrUpscaleMode {bool filtered=false,perceptual=false;};
// SDK root kinds: 0 bilinear, 1/2 CAS, 3/4 EASU/RCAS. Scan the entire
// production flow: large ratios may append bilinear after the adaptive filter.
template<class Flow,class RootIndex>
HdrUpscaleMode DescribeHdrUpscaleFlow(const Flow& flow,RootIndex root_index){
    HdrUpscaleMode mode;
    for(size_t i=0;i<flow.effect_count;++i){
        const auto root=root_index(flow.effects[i]);
        mode.filtered|=root!=0;mode.perceptual|=root==3 || root==4;
    }
    return mode;
}
// Appended after the SDK effect constants, never overlaid on CAS parameters.
struct HdrUpscaleConstants {
    float paper_white_nits=200,peak_nits=1000;
    uint32_t flags=0,padding=0;
    static constexpr unsigned offset=32,root_size=36;
    static constexpr uint32_t encoded_input=1,final_output=2,sdr_input=4,passthrough=8,perceptual=16;
};
static_assert(sizeof(HdrUpscaleConstants)==16);
}
