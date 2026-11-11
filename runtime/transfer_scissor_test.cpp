#include "transfer_scissor.h"
#include <array>
#include <limits>
#include <rex/logging.h>
bool em2::TestTransferScissors(){
    uint32_t random=0x42d9167b;auto next=[&]{random^=random<<13;random^=random>>17;return random^=random<<5;};uint64_t covered=0;
    for(unsigned scale=1;scale<=3;++scale)for(unsigned test=0;test<128;++test){
        struct Rectangle {uint32_t x,y,w,h;};std::array<Rectangle,6> rectangles;TransferScissorBounds bounds(scale,scale);
        for(auto& r:rectangles){r={next()%80,next()%80,next()%32,next()%32};bounds.Add(r.x,r.y,r.w,r.h);}const auto scissor=bounds.Get();
        // Pixel/sample coverage oracle, independent of the bounding-box code.
        // All four standard MSAA sample offsets must survive the new scissor.
        constexpr std::array<std::array<unsigned,2>,4> samples={{{3,1},{7,3},{1,5},{5,7}}};
        for(unsigned y=0;y<96;++y)for(unsigned x=0;x<96;++x)for(const auto& sample:samples){bool original=false;for(const auto& r:rectangles){const auto sx=x*8+sample[0],sy=y*8+sample[1];original|=sx>=r.x*scale*8 && sx<(r.x+r.w)*scale*8 && sy>=r.y*scale*8 && sy<(r.y+r.h)*scale*8;}
            if(original){++covered;if(int(x)<scissor.left || int(x)>=scissor.right || int(y)<scissor.top || int(y)>=scissor.bottom)return false;}}
    }
    TransferScissorBounds overflow(3,3);overflow.Add(UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX);const auto a=overflow.Get();if(a.left || a.top || a.right!=16384 || a.bottom!=16384)return false;
    TransferScissorBounds edge(2,3);edge.Add(8191,5461,UINT32_MAX,UINT32_MAX);const auto b=edge.Get();if(b.left!=16382 || b.top!=16383 || b.right!=16384 || b.bottom!=16384)return false;
    for(auto scale:{0u,17u,UINT32_MAX}){TransferScissorBounds invalid(scale,scale);invalid.Add(1,1,1,1);const auto r=invalid.Get();if(r.left || r.top || r.right!=16384 || r.bottom!=16384)return false;}
    REXLOG_INFO("Transfer scissor coverage passed: {} surviving pixel/sample points, scales 1/2/3, disjoint/cutout mesh bounds, empty/offscreen/overflow fallback and viewport edges",covered);return covered>0;
}
