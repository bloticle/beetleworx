#include "stencil_zero_proof.h"
#include <rex/logging.h>

bool em2::TestStencilZeroProof(){
    StencilZeroProof p;
    if(p.Includes(1,0,0,1,1,1,1))return false;
    // Independent per-sample oracle for clipped clears, partial regions and
    // different scale factors. A positive proof must never cover an uncleared
    // sample, including the edge of each native pixel.
    uint64_t checks=0;
    for(int l=-2;l<=4;++l)for(int t=-2;t<=4;++t)
    for(int r=0;r<=16;r+=4)for(int b=0;b<=16;b+=4){
        p.Clear(11,l,t,r,b,13,12,0);
        for(unsigned sx=1;sx<=3;++sx)for(unsigned sy=1;sy<=3;++sy)
        for(unsigned y=0;y<5;++y)for(unsigned x=0;x<5;++x){
            bool expected=true;
            for(unsigned py=y*sy;py<(y+1)*sy;++py)
            for(unsigned px=x*sx;px<(x+1)*sx;++px)
                expected &= px<13 && py<12 && int(px)>=l && int(py)>=t && int(px)<r && int(py)<b;
            if(p.Includes(11,x,y,1,1,sx,sy)!=expected)return false;
            if(p.Includes(12,x,y,1,1,sx,sy))return false;
            ++checks;
        }
    }
    p.Clear(11,0,0,13,12,13,12,1);if(p.era)return false;
    p.Clear(0,0,0,13,12,13,12,0);if(p.era)return false;
    p.Clear(11,0,0,13,12,13,12,0);
    if(p.Includes(11,UINT32_MAX,0,UINT32_MAX,1,16,1) || p.Includes(11,0,0,1,1,0,1) || p.Includes(11,0,0,1,1,17,1) || p.Includes(11,0,0,0,1,1,1))return false;
    p={};if(p.Includes(11,0,0,1,1,1,1))return false;
    REXLOG_INFO("Stencil zero proof: {} independent coverage checks; epoch, nonzero clear, invalid scale and overflow rejection passed",checks);
    return true;
}
