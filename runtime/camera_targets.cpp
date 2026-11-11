#include "camera_targets.h"
#include "ppc_context.h"
#include "guest_read_batch.h"
#include <rex/logging.h>
#include <rex/cvar.h>
#include <algorithm>
#include <cmath>
#include <immintrin.h>
#include <bit>
#include <cstring>
#include <stdexcept>

PPC_EXTERN_FUNC(__imp__sub_8275C160);
PPC_EXTERN_FUNC(sub_8275C160);
REXCVAR_DECLARE(bool,em2_trace_camera);


namespace {
bool SameTarget(const em2::CameraTargetPose& a,const em2::CameraTargetPose& b){
    return a.registry==b.registry && a.handle==b.handle && a.entity==b.entity && a.component==b.component && a.scenegraph==b.scenegraph && a.root==b.root;
}
bool ValidTarget(const em2::CameraTargetPose& target){
    return target.registry && target.handle && target.entity && target.component &&
        std::all_of(target.position.begin(),target.position.end(),[](float value){return std::isfinite(value);});
}
struct RestoreMath {unsigned csr=_mm_getcsr();DWORD error=GetLastError();~RestoreMath(){_mm_setcsr(csr);SetLastError(error);}};
bool Readable(uint32_t address,size_t count,uint8_t* base){
    return address>=4096 && uint64_t(address)+count<=0x100000000ull && em2::GuestReadBatch::Readable(base+address,count);
}
bool VisualIdentity(const em2::CameraTargetPose& target,uint8_t* base,uint32_t& graph,uint32_t& root,unsigned* failure=nullptr){
    auto reject=[&](unsigned reason){if(failure)*failure=reason;return false;};
    if(!Readable(target.entity,92,base))return reject(1);
    const auto types=PPC_LOAD_U32(target.entity+88),components=PPC_LOAD_U32(target.entity+36);
    const auto kind=PPC_LOAD_U8(0x833DB0EC);
    if(!Readable(types,unsigned(kind)+1,base))return reject(2);
    const auto slot=PPC_LOAD_U8(types+kind);
    if(slot==255 || !Readable(components,unsigned(slot)*4+4,base))return reject(3);
    const auto face=PPC_LOAD_U32(components+unsigned(slot)*4);
    if(face<8 || !Readable(face-8,152,base))return reject(4);
    graph=face-8;
    // Membership in this entity's typed component table proves ownership.
    // 831D6708 uses its own interface's +84; that ABI must not be applied to
    // the full SceneGraphComponent base without proving the interface offset.
    root=PPC_LOAD_U32(graph+88);
    return Readable(root,208,base) || reject(5);
}
bool LargeRelocation(const std::array<float,3>& previous,const std::array<float,3>& current){
    double squared=0;for(unsigned i=0;i<3;++i){const double delta=double(current[i])-previous[i];squared+=delta*delta;}
    return squared>32.*32.;
}
}

bool em2::ReadCameraVisualTarget(CameraTargetPose& target,uint8_t* base){
    RestoreMath preserve;GuestReadBatch reads;
    uint32_t graph=0,root=0;unsigned failure=0;
    auto failed=[&](unsigned reason){
        if(REXCVAR_GET(em2_trace_camera)){
            struct Failure {uint32_t entity=0,graph=0,root=0;unsigned reason=0;bool operator==(const Failure&)const=default;};
            static thread_local std::array<Failure,8> seen{};
            const Failure current{target.entity,graph,root,reason};
            if(std::find(seen.begin(),seen.end(),current)==seen.end()){
                const auto free=std::find_if(seen.begin(),seen.end(),[](const Failure& value){return !value.reason;});
                if(free!=seen.end()){*free=current;REXLOG_INFO("Native visual camera target rejected: stage {}, entity {:08X}, graph {:08X}, root {:08X}, handle {:08X} (1 entity,2 type map,3 component slot,4 component,5 root,6 position,7 input)",reason,target.entity,graph,root,target.handle);}
            }
        }
        return false;
    };
    if(!ValidTarget(target))return failed(7);
    if(!VisualIdentity(target,base,graph,root,&failure))return failed(failure);
    std::array<float,3> visible{};
    for(unsigned axis=0;axis<3;++axis){visible[axis]=std::bit_cast<float>(PPC_LOAD_U32(root+192+axis*4));if(!std::isfinite(visible[axis]))return failed(6);}
    target.physical_position=target.position;target.position=visible;target.scenegraph=graph;target.root=root;
    return true;
}

void em2::CameraTargetHistory::Reset(){previous_={};current_={};tick_=0;valid_=false;}
void em2::CameraTargetHistory::Capture(const CameraTargetPose& target,uint64_t tick,bool discontinuity){
    RestoreMath math;
    if(!ValidTarget(target)){Reset();return;}
    if(valid_ && target.root && SameTarget(current_,target))
        discontinuity|=LargeRelocation(current_.position,target.position) || LargeRelocation(current_.physical_position,target.physical_position);
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
    if(target.root){
        uint32_t graph=0,root=0;
        if(!VisualIdentity(target,base,graph,root) || graph!=target.scenegraph || root!=target.root)return;
    }
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
    for(unsigned identity=0;identity<6;++identity){
        auto replacement=target;
        if(identity==0)++replacement.registry;if(identity==1)++replacement.handle;
        if(identity==2)++replacement.entity;if(identity==3)++replacement.component;
        if(identity==4)++replacement.scenegraph;if(identity==5)++replacement.root;
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
    // Real retail entity getter + owned scenegraph type map. Physics has
    // already advanced one tick beyond the actor root visible in this frame.
    constexpr uint32_t graph=0x8000,root=0x9000,replacement_root=0xA000;
    PPC_STORE_U8(0x833DB0EC,3);PPC_STORE_U8(types+3,1);PPC_STORE_U32(components+4,graph+8);
    PPC_STORE_U32(graph+84,0xBAD0BAD0);PPC_STORE_U32(graph+88,root);
    auto capture_visual=[&](std::array<float,3> physical,std::array<float,3> visual,uint64_t tick){
        CameraTargetPose pose{0x2000,1,0x3000,0x4000,physical};
        for(unsigned axis=0;axis<3;++axis){
            PPC_STORE_U32(pose.component+172+axis*4,std::bit_cast<uint32_t>(physical[axis]));
            PPC_STORE_U32(root+192+axis*4,std::bit_cast<uint32_t>(visual[axis]));
        }
        if(!ReadCameraVisualTarget(pose,base))throw std::runtime_error("visual target fixture resolution failed");
        history.Capture(pose,tick);return pose;
    };
    history.Reset();target=capture_visual({20,40,60},{10,20,30},100);
    target=capture_visual({24,48,72},{14,28,42},101);
    {
        const auto physics=target.physical_position;
        CameraTargetQueryScope scope(history,target,101,.5);invoke(false);
        if(!check(std::bit_cast<float>(PPC_LOAD_U32(output))==12 &&
            std::bit_cast<float>(PPC_LOAD_U32(output+4))==24 && std::bit_cast<float>(PPC_LOAD_U32(output+8))==36,
            "native camera query follows actual visible actor-root phase, not next-tick physical endpoints"))return false;
        for(unsigned axis=0;axis<3;++axis)if(!check(std::bit_cast<float>(PPC_LOAD_U32(target.component+172+axis*4))==physics[axis],
            "visual target query leaves authoritative movement and physics coordinates untouched"))return false;
        PPC_STORE_U32(graph+88,replacement_root);invoke(false);
        if(!check(std::bit_cast<float>(PPC_LOAD_U32(output))==24,"replacement scene root cannot receive a query from the previous root's history"))return false;
        PPC_STORE_U32(graph+88,root);PPC_STORE_U32(components+4,graph+16);invoke(false);
        if(!check(std::bit_cast<float>(PPC_LOAD_U32(output))==24,"replacing the entity's typed scenegraph component rejects the old target identity"))return false;
        PPC_STORE_U32(components+4,graph+8);PPC_STORE_U8(types+3,255);invoke(false);
        if(!check(std::bit_cast<float>(PPC_LOAD_U32(output))==24,"detaching the typed scenegraph slot rejects a retained visual target"))return false;
        PPC_STORE_U8(types+3,1);
    }
    target=capture_visual({1000,1000,1000},{14,28,42},102);
    if(!check(history.Sample(target,102,.5)==target.position,"physical teleport resets view history even before the scenegraph catches up"))return false;
    target=capture_visual({1001,1001,1001},{1000,1000,1000},103);
    if(!check(history.Sample(target,103,.5)==target.position,"scenegraph teleport never sweeps through the intervening world"))return false;
    auto swapped=target;swapped.handle=2;swapped.position={70,80,90};
    history.Capture(swapped,104);
    if(!check(history.Sample(swapped,104,.5)==swapped.position && !history.Sample(target,104,.5),
        "character swap or reused scene-root address cannot retain the previous actor's camera target"))return false;
    auto changed=swapped;changed.root=replacement_root;changed.position={72,82,92};history.Capture(changed,105);
    if(!check(history.Sample(changed,105,.5)==changed.position,"root identity replacement resets instead of blending unrelated model resources"))return false;
    REXLOG_INFO("Camera target history passed {} simulation/presentation interval, identity, discontinuity and reset checks",checks);
    return true;
}
