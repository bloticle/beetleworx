#pragma once
#include <algorithm>
#include <cstdint>

namespace em2 {
struct TransferScissorRect {int32_t left=0,top=0,right=16384,bottom=16384;};
// Native transfer vertices use integer guest-pixel rectangles and a 16384-pixel
// viewport. Keep their exact scaled union; cutouts/gaps remain in the mesh.
class TransferScissorBounds {
public:
    TransferScissorBounds(uint32_t x,uint32_t y):x_(x),y_(y),valid_(x && y && x<=16 && y<=16){}
    void Add(uint32_t x,uint32_t y,uint32_t width,uint32_t height){
        if(!valid_ || !width || !height)return;
        const uint64_t left=std::min<uint64_t>(uint64_t(x)*x_,16384),top=std::min<uint64_t>(uint64_t(y)*y_,16384);
        const uint64_t right=std::min<uint64_t>((uint64_t(x)+width)*x_,16384),bottom=std::min<uint64_t>((uint64_t(y)+height)*y_,16384);
        if(right<=left || bottom<=top)return;
        if(!any_){bounds_={int32_t(left),int32_t(top),int32_t(right),int32_t(bottom)};any_=true;}
        else{bounds_.left=std::min(bounds_.left,int32_t(left));bounds_.top=std::min(bounds_.top,int32_t(top));bounds_.right=std::max(bounds_.right,int32_t(right));bounds_.bottom=std::max(bounds_.bottom,int32_t(bottom));}
    }
    TransferScissorRect Get()const{return valid_ && any_?bounds_:TransferScissorRect{};}
private:
    uint32_t x_,y_;bool valid_,any_=false;TransferScissorRect bounds_{};
};
bool TestTransferScissors();
}
