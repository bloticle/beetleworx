#include "native_gameplay_component_checkpoint.h"
#include "native_component_checkpoint.h"
#include "native_behavior_checkpoint.h"
#include "native_random.h"
#include "native_physics_bridge_checkpoint.h"
#include <cstring>
#include <array>
#include <sstream>
#include "native_simulation_clock_checkpoint.h"
#include "native_input_history_checkpoint.h"
#include "native_rigid_body_checkpoint.h"
#include "native_deferred_entity_checkpoint.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <windows.h>
#include <stdexcept>
namespace em2 {namespace {
void Need(bool value,const char* error){if(!value)throw std::runtime_error(error);}
}
struct NativeGameplayComponentCheckpoint::Impl {
    NativeEntityOwnership ownership;NativeComponentFieldCheckpoint fields;NativeBehaviorCheckpoint behavior;
    NativeRigidBodyCheckpoint physics;NativeDeferredEntityCheckpoint deferred;NativeRandomCheckpoint random;NativeInputHistoryCheckpoint input;NativeSimulationClockCheckpoint clock;NativePhysicsBridgeCheckpoint bridge;
    std::array<uint8_t,8> physics_time{};uint32_t physics_step=0,manager_table=0;
    uint32_t world=0,thread=0;uint64_t epoch=0;bool ready=false;
    bool Scope(uint8_t* base,uint64_t expected,std::string& error)const{
        NativeEntityOwnership now;const char* why=nullptr;uint32_t where=0;
        if(!ready || !expected || expected!=epoch || thread!=GetCurrentThreadId() ||
            !ReadNativeEntityOwnership(base,now,why,where) || now!=ownership ||
            !GuestReadBatch::Readable(base+ownership.world,84) || PPC_LOAD_U32(ownership.world+4)!=world || PPC_LOAD_U32(ownership.world)!=manager_table || PPC_LOAD_U32(ownership.world+72)!=physics_step){
            error="Gameplay component bundle owner/epoch/inventory changed";return false;
        }return true;
    }
};
NativeGameplayComponentCheckpoint::NativeGameplayComponentCheckpoint():impl_(std::make_unique<Impl>()){}
NativeGameplayComponentCheckpoint::~NativeGameplayComponentCheckpoint()=default;
size_t NativeGameplayComponentCheckpoint::fields()const{return impl_->fields.fields();}
unsigned NativeGameplayComponentCheckpoint::bodies()const{return impl_->physics.bodies();}
bool NativeGameplayComponentCheckpoint::Capture(PPCContext& ctx,uint8_t* base,uint64_t epoch,std::string& error){
    auto& s=*impl_;try{
        Need(!s.epoch && epoch,"Gameplay component bundle requires an empty checkpoint/nonzero epoch");
        const char* why=nullptr;uint32_t where=0;
        Need(ReadNativeEntityOwnership(base,s.ownership,why,where),"Gameplay bundle entity ownership unavailable");
        Need(s.ownership.world && GuestReadBatch::Readable(base+s.ownership.world,84),"Gameplay bundle physics manager unavailable");
        s.world=PPC_LOAD_U32(s.ownership.world+4);s.epoch=epoch;s.thread=GetCurrentThreadId();
        s.manager_table=PPC_LOAD_U32(s.ownership.world);s.physics_step=PPC_LOAD_U32(s.ownership.world+72);std::memcpy(s.physics_time.data(),base+s.ownership.world+76,8);
        std::vector<NativeComponentSchema> schemas;
        if(!s.bridge.Capture(base,s.ownership,epoch,error) || !s.clock.Capture(base,epoch,error) || !s.random.Capture(ctx,base,epoch,error) || !s.input.Capture(base,epoch,error) || !ReadNativeComponentSchemas(base,s.ownership,schemas,error) || !s.fields.Capture(base,s.ownership,schemas,epoch,error) ||
            !s.physics.Capture(ctx,base,s.world,epoch,error) || !s.deferred.Capture(ctx,base,s.ownership.registry,epoch,error) || !s.behavior.Capture(ctx,base,s.ownership,epoch,error)){
            Release(ctx,base);return false;
        }
        s.ready=true;
        if(!Matches(ctx,base,epoch,error)){Release(ctx,base);return false;}
        return true;
    }catch(const std::exception& e){error=e.what();Release(ctx,base);return false;}
}
bool NativeGameplayComponentCheckpoint::Matches(PPCContext& ctx,uint8_t* base,uint64_t epoch,std::string& error)const{
    const auto& s=*impl_;
    if(!s.Scope(base,epoch,error))return false;
    std::string problems,why;
    auto check=[&](const char* name,bool result){if(!result){if(!problems.empty())problems+="; ";problems+=name;problems+=": ";problems+=why;}why.clear();};
    if(std::memcmp(base+s.ownership.world+76,s.physics_time.data(),8)){why="EM2 physics residual/advanced time differs from checkpoint";check("physics scheduler",false);}
    check("fields",s.fields.Matches(base,epoch,why));
    // This only validates retained native owners, not behavior-state equivalence.
    check("behavior ownership",s.behavior.ValidateOwnership(base,epoch,why));
    check("physics",s.physics.Matches(ctx,base,s.world,epoch,why));
    check("EM2 physics data",s.bridge.Matches(base,epoch,why));
    check("deferred",s.deferred.Matches(base,s.ownership.registry,epoch,why));
    check("RNG",s.random.Matches(ctx,base,epoch,why));
    check("input",s.input.Matches(base,epoch,why));
    check("clock",s.clock.Matches(base,epoch,why));
    if(!problems.empty()){error=std::move(problems);return false;}
    if(!s.Scope(base,epoch,error))return false;error.clear();return true;
}
bool NativeGameplayComponentCheckpoint::Restore(PPCContext& ctx,uint8_t* base,uint64_t epoch,std::string& error)const{
    const auto& s=*impl_;
    if(!s.Scope(base,epoch,error))return false;
    // Native calls may partially write before failure: the complete world owner
    // must provide recovery. The private combined probe stops instead of resuming.
    // Original 827ADCC8 consumes +76 before scheduling split Havok phases and
    // publishes total advanced time at +80. Both are scalar owner state.
    std::memcpy(base+s.ownership.world+76,s.physics_time.data(),8);
    if(!s.clock.Restore(base,epoch,error) || !s.behavior.Restore(ctx,base,epoch,error) || !s.bridge.Restore(base,epoch,error) || !s.physics.Restore(ctx,base,s.world,epoch,error) || !s.deferred.Restore(ctx,base,s.ownership.registry,epoch,error) ||
        !s.fields.Restore(base,epoch,error) || !s.bridge.Restore(base,epoch,error) || !s.input.Restore(base,epoch,error) || !s.random.Restore(ctx,base,epoch,error) || !s.clock.Restore(base,epoch,error))return false;
    return Matches(ctx,base,epoch,error);
}
std::string NativeGameplayComponentCheckpoint::DescribePhysicsBody(uint8_t* base,uint32_t body)const{
    std::ostringstream out;if(!GuestReadBatch::Readable(base+body,16))return "body unreadable";
    const auto data=PPC_LOAD_U32(body+12);out<<"userdata 0x"<<std::hex<<data;
    if(data>=4096 && uint64_t(data)+8<=0x100000000ull && GuestReadBatch::Readable(base+data,8)){
        out<<" "<<impl_->fields.DescribeEntity(PPC_LOAD_U32(data));const auto root=PPC_LOAD_U32(data+4);out<<" root 0x"<<std::hex<<root;
        if(root>=4096 && uint64_t(root)+12<=0x100000000ull && GuestReadBatch::Readable(base+root,12)){
            out<<" root table 0x"<<PPC_LOAD_U32(root);const auto name=PPC_LOAD_U32(root+8);
            if(name>=4096 && uint64_t(name)+96<=0x100000000ull && GuestReadBatch::Readable(base+name,96)){
                const auto* p=reinterpret_cast<const char*>(base+name);const auto* end=static_cast<const char*>(std::memchr(p,0,96));if(end){std::string text(p,end);for(auto& c:text)if(uint8_t(c)<32 || uint8_t(c)>126)c='?';out<<" root name '"<<text<<"'";}
            }
        }
    }return out.str();
}
bool NativeGameplayComponentCheckpoint::SamplePhysicsPhase(uint8_t* base,uint32_t object,unsigned phase,std::vector<NativePhysicsMotionSample>& samples,std::string& error)const{
    const auto& s=*impl_;const bool manager=phase==1 || phase==4;
    if(phase==11 || phase==12){
        if(object<4096 || uint64_t(object)+28>0x100000000ull || !GuestReadBatch::Readable(base+object,28) || PPC_LOAD_U32(object)!=0x8203098C || PPC_LOAD_U32(object+24)!=s.world){samples.clear();error.clear();return true;}
        return s.physics.SampleMotion(base,samples,error);
    }
    if(phase==9 || phase==10){
        // 82487770 receives this island's entity array in r4. Admit only an
        // array belonging to a live island of this captured world.
        bool found=false;
        const auto array=PPC_LOAD_U32(s.world+40),count=PPC_LOAD_U32(s.world+44);
        if(count>4096 || (count && (array<4096 || uint64_t(array)+size_t(count)*4>0x100000000ull || !GuestReadBatch::Readable(base+array,size_t(count)*4)))){error="Private solver active-island list unavailable";return false;}
        for(unsigned i=0;i<count;++i){const auto island=PPC_LOAD_U32(array+i*4);if(island>=4096 && uint64_t(island)+84<=0x100000000ull && GuestReadBatch::Readable(base+island,84) && PPC_LOAD_U32(island+76)==object){found=true;break;}}
        if(!found){samples.clear();error.clear();return true;}return s.physics.SampleMotion(base,samples,error);
    }
    const auto expected=phase==7 || phase==8?s.world+480:manager?s.ownership.world:PPC_LOAD_U32(s.world+8);
    if(object!=expected){samples.clear();error.clear();return true;}
    return s.physics.SampleMotion(base,samples,error);
}
bool NativeGameplayComponentCheckpoint::TestPhysicsBridgeRoundTrip(uint8_t* base,uint64_t epoch,std::string& error)const{
    return impl_->Scope(base,epoch,error) && impl_->bridge.TestRoundTrip(base,epoch,error);
}
bool NativeGameplayComponentCheckpoint::TestInputRoundTrip(PPCContext& ctx,uint8_t* base,uint64_t epoch,std::string& error)const{
    return impl_->Scope(base,epoch,error) && impl_->input.TestRoundTrip(base,epoch,error) && impl_->input.TestButtonEdges(ctx,base,epoch,error) && Matches(ctx,base,epoch,error);
}
bool NativeGameplayComponentCheckpoint::TestRandomRoundTrip(PPCContext& ctx,uint8_t* base,uint64_t epoch,std::string& error)const{
    return impl_->Scope(base,epoch,error) && impl_->random.TestRoundTrip(ctx,base,epoch,error) && Matches(ctx,base,epoch,error);
}
bool NativeGameplayComponentCheckpoint::TestRoundTrip(PPCContext& ctx,uint8_t* base,uint64_t epoch,std::string& error)const{
    if(!Matches(ctx,base,epoch,error))return false;
    return impl_->fields.TestRoundTrip(base,epoch,error,[&](std::string& why){return Restore(ctx,base,epoch,why);}) && Matches(ctx,base,epoch,error);
}
bool NativeGameplayComponentCheckpoint::Release(PPCContext& ctx,uint8_t* base){
    auto& s=*impl_;if(s.thread && s.thread!=GetCurrentThreadId())return false;
    const auto behavior=s.behavior.Release(ctx,base);const auto physics=s.physics.Release(ctx,base);const auto deferred=s.deferred.Release(ctx,base);s.ready=false;
    return behavior && physics && deferred;
}
}
