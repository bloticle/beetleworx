#include "camera_targets.h"
#include "ppc_context.h"
#include "guest_read_batch.h"
#include <rex/logging.h>
#include <algorithm>
#include <cmath>
#include <immintrin.h>
#include <bit>
#include <cstring>
#include <stdexcept>

PPC_EXTERN_FUNC(__imp__sub_8275C160);
PPC_EXTERN_FUNC(sub_8275C160);


namespace {
bool SameTarget(const em2::CameraTargetPose& a,const em2::CameraTargetPose& b){
    return a.registry==b.registry && a.handle==b.handle && a.entity==b.entity && a.component==b.component;
}
bool ValidTarget(const em2::CameraTargetPose& target){
    return target.registry && target.handle && target.entity && target.component &&
        std::all_of(target.position.begin(),target.position.end(),[](float value){return std::isfinite(value);});
}
struct RestoreMath {unsigned csr=_mm_getcsr();DWORD error=GetLastError();~RestoreMath(){_mm_setcsr(csr);SetLastError(error);}};
bool Readable(uint32_t address,size_t count,uint8_t* base){
    return address>=4096 && uint64_t(address)+count<=0x100000000ull && em2::GuestReadBatch::Readable(base+address,count);
}
}

void em2::CameraTargetHistory::Reset(){previous_={};current_={};tick_=0;valid_=false;}
void em2::CameraTargetHistory::Capture(const CameraTargetPose& target,uint64_t tick,bool discontinuity){
    RestoreMath math;
    if(!ValidTarget(target)){Reset();return;}
    if(!valid_ || !SameTarget(current_,target) || discontinuity || (tick!=tick_ && tick-tick_!=1))previous_=target;
    else if(tick!=tick_)previous_=current_;
    current_=target;tick_=tick;valid_=true;
}
std::optional<std::array<float,3>> em2::CameraTargetHistory::Sample(const CameraTargetPose& target,uint64_t tick,double alpha)const{
    RestoreMath math;
    if(!valid_ || !ValidTarget(target) || tick!=tick_ || !SameTarget(current_,target) || !std::isfinite(alpha) || alpha<0 || alpha>1)return {};
    std::array<float,3> result{};
    for(unsigned axis=0;axis<3;++axis)result[axis]=float(double(previous_.position[axis])+(double(current_.position[axis])-previous_.position[axis])*alpha);
    return result;
}

em2::CameraTargetQueryScope::CameraTargetQueryScope(const CameraTargetHistory& history,const CameraTargetPose& target,uint64_t tick,double alpha)
    :query_{&history,target,tick,alpha},previous_(active_){active_=&query_;}
em2::CameraTargetQueryScope::~CameraTargetQueryScope(){active_=previous_;}
void em2::CameraTargetQueryScope::OverridePosition(uint32_t entity,uint32_t output,uint8_t* base){
    if(!active_ || entity!=active_->target.entity)return;
    RestoreMath math;
    GuestReadBatch inspection;
    if(!Readable(entity,92,base) || !Readable(output,12,base) ||
        PPC_LOAD_U32(0x833DA7E4)!=active_->target.registry)return;
    const auto types=PPC_LOAD_U32(entity+88),components=PPC_LOAD_U32(entity+36);
    const auto type=PPC_LOAD_U8(0x833DBC3C);
    if(!Readable(types,unsigned(type)+1,base))return;
    const auto slot=PPC_LOAD_U8(types+type);
    if(slot==255 || !Readable(components,unsigned(slot)*4+4,base) ||
        PPC_LOAD_U32(components+unsigned(slot)*4)!=active_->target.component+8)return;
    auto target=active_->target;
    for(unsigned axis=0;axis<3;++axis)target.position[axis]=std::bit_cast<float>(PPC_LOAD_U32(output+axis*4));
    if(const auto position=active_->history->Sample(target,active_->tick,active_->alpha))
        for(unsigned axis=0;axis<3;++axis)PPC_STORE_U32(output+axis*4,std::bit_cast<uint32_t>((*position)[axis]));
}

PPC_FUNC_IMPL(sub_8275C160){
    const auto entity=ctx.r3.u32,output=ctx.r4.u32;
    __imp__sub_8275C160(ctx,base);
    em2::CameraTargetQueryScope::OverridePosition(entity,output,base);
}

bool em2::TestCameraTargetHistory(){
    RestoreMath math;unsigned checks=0;
    auto check=[&](bool okay,const char* reason){++checks;if(!okay)REXLOG_ERROR("Camera target history test: {}",reason);return okay;};
    CameraTargetHistory history;
    CameraTargetPose target{1,2,3,4,{10,20,30}};
    if(!check(!history.Sample(target,1,.5),"uninitialized target has no render override"))return false;
    history.Capture(target,1);
    if(!check(history.Sample(target,1,0)==target.position,"initial target does not interpolate from the origin"))return false;
    target.position={14,28,42};history.Capture(target,2);
    for(double alpha:{0.,.25,.5,.75,1.}){
        const std::array<float,3> desired={float(10+4*alpha),float(20+8*alpha),float(30+12*alpha)};
        if(!check(history.Sample(target,2,alpha)==desired,"camera samples the character's completed simulation interval"))return false;
    }
    target.position={18,32,46};history.Capture(target,2);
    if(!check(history.Sample(target,2,.5)==std::array<float,3>{14,26,38},"duplicate capture updates the endpoint without advancing the interval"))return false;
    for(unsigned identity=0;identity<4;++identity){
        auto replacement=target;
        if(identity==0)++replacement.registry;if(identity==1)++replacement.handle;
        if(identity==2)++replacement.entity;if(identity==3)++replacement.component;
        if(!check(!history.Sample(replacement,2,.5),"recycled pointers and changed registry handles cannot reuse an old target"))return false;
    }
    if(!check(!history.Sample(target,3,.5) && !history.Sample(target,2,-.1) && !history.Sample(target,2,1.1) && !history.Sample(target,2,NAN),"stale intervals and invalid presentation phases cannot alter camera queries"))return false;
    target.position={1000,2000,3000};history.Capture(target,3,true);
    if(!check(history.Sample(target,3,.5)==target.position,"explicit teleport discontinuity immediately uses the new target"))return false;
    target.position={1004,2004,3004};history.Capture(target,9);
    if(!check(history.Sample(target,9,.5)==target.position,"loading and missed intervals do not sweep across stale poses"))return false;
    auto invalid=target;invalid.position[1]=NAN;history.Capture(invalid,10);
    if(!check(!history.Sample(target,10,.5),"invalid native pose clears retained target state"))return false;
    history.Capture(target,11);history.Reset();
    if(!check(!history.Sample(target,11,.5),"pause and ownership reset clear target history"))return false;
    auto* memory=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
    struct Release {uint8_t* memory;~Release(){if(memory)VirtualFree(memory,0,MEM_RELEASE);}}release{memory};
    if(!memory || !VirtualAlloc(memory,0x20000,MEM_COMMIT,PAGE_READWRITE) ||
        !VirtualAlloc(memory+0x82000000,0x10000,MEM_COMMIT,PAGE_READWRITE) ||
        !VirtualAlloc(memory+0x833D0000,0x10000,MEM_COMMIT,PAGE_READWRITE))return false;
    auto* base=memory;
    target={0x2000,1,0x3000,0x4000,{10,20,30}};
    constexpr uint32_t types=0x5000,components=0x6000,output=0x7000;
    PPC_STORE_U32(0x833DA7E4,target.registry);PPC_STORE_U8(0x833DBC3C,2);
    PPC_STORE_U32(target.entity+88,types);PPC_STORE_U8(types+2,0);
    PPC_STORE_U32(target.entity+36,components);PPC_STORE_U32(components,target.component+8);
    PPC_STORE_U32(0x82002060,0);
    history.Capture(target,1);target.position={14,28,42};history.Capture(target,2);
    for(unsigned axis=0;axis<3;++axis)PPC_STORE_U32(target.component+172+axis*4,std::bit_cast<uint32_t>(target.position[axis]));
    PPCContext native{};native.fpscr.InitHost();native.r1.u64=0x1F000;native.r3.u64=target.entity;native.r4.u64=output;
    const auto invoke=[&](bool original){
        auto call=native;
        if(original)__imp__sub_8275C160(call,base);else sub_8275C160(call,base);
        return call;
    };
    auto expected=invoke(true);const auto native_math=_mm_getcsr();
    {
        CameraTargetQueryScope scope(history,target,2,.5);
        auto actual=invoke(false);
        const std::array<float,3> desired={12,24,36};
        for(unsigned axis=0;axis<3;++axis){
            if(!check(std::bit_cast<float>(PPC_LOAD_U32(output+axis*4))==desired[axis],"real native entity getter returns the presentation target inside its scope"))return false;
            if(!check(std::bit_cast<float>(PPC_LOAD_U32(target.component+172+axis*4))==target.position[axis],"scoped query leaves authoritative native entity storage intact"))return false;
        }
        if(!check(!std::memcmp(&actual,&expected,sizeof(actual)) && _mm_getcsr()==native_math,"camera query preserves the real getter's complete return context and FP state"))return false;
        {CameraTargetQueryScope nested(history,target,2,.25);invoke(false);
            if(!check(std::bit_cast<float>(PPC_LOAD_U32(output))==11,"nested camera query uses its own render phase"))return false;}
        invoke(false);
        if(!check(std::bit_cast<float>(PPC_LOAD_U32(output))==12,"nested camera query restores the outer render phase"))return false;
        PPC_STORE_U32(0x833DA7E4,target.registry+4);invoke(false);
        if(!check(std::bit_cast<float>(PPC_LOAD_U32(output))==14,"a replaced native entity registry rejects a retained target"))return false;
        PPC_STORE_U32(0x833DA7E4,target.registry);
    }
    invoke(false);
    if(!check(std::bit_cast<float>(PPC_LOAD_U32(output))==14,"queries outside rendering return authoritative native position"))return false;
    try{CameraTargetQueryScope scope(history,target,2,.5);throw std::runtime_error("camera query unwind");}catch(const std::runtime_error&){}
    invoke(false);
    if(!check(std::bit_cast<float>(PPC_LOAD_U32(output))==14,"query scope unwinding restores the native getter"))return false;
    REXLOG_INFO("Camera target history passed {} simulation/presentation interval, identity, discontinuity and reset checks",checks);
    return true;
}
