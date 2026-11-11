#pragma once
#include <algorithm>
#include <cstdint>

namespace em2 {
// A single conservative rectangle, not a union: a new clear replaces the proof.
// Host pixels include every MSAA sample; partial/unknown writes erase it.
struct StencilZeroProof {
    uint64_t era=0;
    uint32_t left=0,top=0,right=0,bottom=0;
    void Clear(uint64_t next_era,int64_t l,int64_t t,int64_t r,int64_t b,
               uint64_t resource_width,uint32_t resource_height,uint8_t stencil) {
        *this={};
        if(!next_era || stencil || !resource_width || !resource_height)return;
        l=std::max<int64_t>(0,l);t=std::max<int64_t>(0,t);
        r=std::min<int64_t>(std::min<uint64_t>(resource_width,UINT32_MAX),r);
        b=std::min<int64_t>(resource_height,b);
        if(r<=l || b<=t)return;
        era=next_era;left=uint32_t(l);top=uint32_t(t);right=uint32_t(r);bottom=uint32_t(b);
    }
    bool Includes(uint64_t current_era,uint32_t x,uint32_t y,uint32_t width,
                  uint32_t height,uint32_t scale_x,uint32_t scale_y)const {
        if(!era || era!=current_era || !width || !height || !scale_x || !scale_y || scale_x>16 || scale_y>16)return false;
        return uint64_t(x)*scale_x>=left && uint64_t(y)*scale_y>=top &&
            (uint64_t(x)+width)*scale_x<=right && (uint64_t(y)+height)*scale_y<=bottom;
    }
};
bool TestStencilZeroProof();
uint64_t StencilZeroSkippedDraws();
}
