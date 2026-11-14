#include "online_actor_roster.h"
#include "online_actor_frames.h"
#include "online_actor_jump.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <windows.h>
#include <immintrin.h>
#include <cstring>
#include <optional>
#include <algorithm>
PPC_EXTERN_FUNC(__imp__sub_82732A18);
namespace em2::net {
namespace {
struct Environment {unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(csr);SetLastError(error);}};
bool Readable(uint32_t address,size_t size,uint8_t* base){return base && address>=4096 && uint64_t(address)+size<=0x100000000ull && GuestReadBatch::Readable(base+address,size);}
std::optional<uint32_t> Resolve(uint32_t registry,uint32_t handle,PPCContext& ctx,uint8_t* base){
    Environment environment;
    const auto top=ctx.r1.u32&~15u;
    if(!Readable(registry,24,base) || top<8192 || !Readable(top-4096,4096,base))return {};
    struct Restore {PPCContext& ctx;PPCContext saved;uint8_t* memory;std::array<uint8_t,4096> bytes;
        Restore(PPCContext& c,uint8_t* p):ctx(c),saved(c),memory(p){std::memcpy(bytes.data(),p,bytes.size());}
        ~Restore(){std::memcpy(memory,bytes.data(),bytes.size());ctx=saved;}}restore(ctx,base+top-4096);
    const auto key=top-512;PPC_STORE_U32(key,handle);
    ctx.r1.u64=top-2048;ctx.r3.u64=registry;ctx.r4.u64=key;
    __imp__sub_82732A18(ctx,base);return ctx.r3.u32;
}
}
bool OwnedActorRoster::Owner()const{return thread_ && thread_==GetCurrentThreadId();}
bool OwnedActorRoster::Reject(const char* why){error_=why;return false;}
void OwnedActorRoster::Disable(){for(unsigned i=0;i<count_;++i){SetOwnedActorInputEnabled(actors_[i].actor.peer,actors_[i].actor.generation,false);actors_[i].actor.enabled=false;}active_=false;}
OwnedActorRoster::~OwnedActorRoster(){Environment preserve;if(!retired_ && Owner())Disable();}
bool OwnedActorRoster::Add(uint8_t room_peer,uint64_t native_peer,uint32_t handle,unsigned action_user,PPCContext& ctx,uint8_t* base){
    Environment preserve;
    if(thread_ && !Owner())return false;
    if(active_ || retiring_ || retired_ || !room_peer || room_peer>16 || count_==actors_.size() || action_user>1)
        return Reject("Invalid extra-human roster registration phase or identity");
    if(mask_&(1u<<(room_peer-1)))return Reject("Lobby participant already owns an extra actor");
    for(unsigned i=0;i<count_;++i)if(actors_[i].actor.peer==native_peer || actors_[i].actor.handle==handle)
        return Reject("Extra actor peer or native handle is duplicated");
    if(!Readable(0x833DA7E4,4,base))return Reject("Native actor registry is unavailable");
    const auto registry=PPC_LOAD_U32(0x833DA7E4);
    if(registry_ && registry_!=registry)return Reject("Native world changed while registering the actor group");
    OwnedActorInput actor;
    if(!RegisterOwnedActorInput(native_peer,handle,ctx,base,actor,action_user))return Reject("Native extra-human registration failed");
    thread_=GetCurrentThreadId();registry_=registry;
    actors_[count_++]={room_peer,actor,{}};mask_|=uint16_t(1u<<(room_peer-1));
    if(!SetOwnedActorInputEnabled(native_peer,actor.generation,false))return Reject("Could not quarantine the new actor before admission");
    actors_[count_-1].actor.enabled=false;
    return true;
}
bool OwnedActorRoster::Activate(uint16_t expected,uint32_t camera,PPCContext& ctx,uint8_t* base){
    std::array<uint32_t,16> cameras{};cameras.fill(camera);
    return Activate(expected,std::span<const uint32_t>(cameras.data(),count_),ctx,base);
}
bool OwnedActorRoster::Activate(uint16_t expected,std::span<const uint32_t> cameras,PPCContext& ctx,uint8_t* base){
    Environment preserve;
    if(!Owner())return false;
    if(active_ || retiring_ || retired_ || !expected || expected!=mask_ || cameras.size()!=count_ || std::any_of(cameras.begin(),cameras.end(),[](auto handle){return !handle;}))
        return Reject("Extra-human roster does not match the frozen admission mask");
    auto fail=[&](const char* why){Disable();return Reject(why);};
    for(unsigned i=0;i<count_;++i){auto& entry=actors_[i];OwnedActorInput current;
        if(!OwnedActorInputFor(entry.actor.peer,ctx,base,current) || current.generation!=entry.actor.generation || current.registry!=registry_)
            return fail("An extra actor changed during group admission");
        // No frame binding is exposed until the entire group activates. Even
        // while enabled here, ordinary native broadcasts remain quarantined.
        if(!SetOwnedActorInputEnabled(current.peer,current.generation,true) || !ApplyOwnedActorTaskPermissions(current.peer,ctx,base))
            return fail("Could not apply existing native human ability permissions");
        const auto result=AttachOwnedActorMovementReference(current.peer,cameras[i],ctx,base,entry.reference);
        if((result!=ActorReferenceResult::Attached && result!=ActorReferenceResult::ExistingReference) ||
            !OwnedActorMovementReferenceReady(current.peer,ctx,base))return fail("Could not attach a usable native movement reference");
    }
    for(unsigned i=0;i<count_;++i)actors_[i].actor.enabled=true;
    active_=true;error_.clear();return true;
}
bool OwnedActorRoster::BindFrames(OwnedMovementFrames& frames,PPCContext& ctx,uint8_t* base){
    Environment preserve;if(!Owner())return false;
    if(!active_ || retiring_)return Reject("Actor roster is not ready for authoritative input");
    for(unsigned i=0;i<count_;++i)if(!frames.Bind(actors_[i].room_peer,actors_[i].actor.peer,ctx,base))return Reject("Could not bind every participant to its native actor");
    return true;
}
bool OwnedActorRoster::BeginRetirement(PPCContext& ctx,uint8_t* base){
    Environment preserve;if(!Owner())return false;if(retired_)return true;
    Disable();retiring_=true;bool complete=true;
    if(!Readable(0x833DA7E4,4,base) || PPC_LOAD_U32(0x833DA7E4)!=registry_)return false;
    for(unsigned i=0;i<count_;++i){auto& reference=actors_[i].reference;if(!reference.active)continue;
        const auto entity=Resolve(registry_,actors_[i].actor.handle,ctx,base);
        if(!entity){complete=false;continue;}
        if(!*entity){reference={};continue;} //Native actor destruction released its own NiPointer.
        if(*entity!=actors_[i].actor.entity){complete=false;continue;}
        const auto result=ReleaseOwnedActorMovementReference(reference,ctx,base);
        complete&=result==ActorReferenceResult::Released || result==ActorReferenceResult::AlreadyReplaced;
    }
    return complete;
}
bool OwnedActorRoster::CompleteRetirement(PPCContext& ctx,uint8_t* base){
    Environment preserve;if(!Owner())return false;if(retired_)return true;
    if(!retiring_ || !Readable(0x833DA7E4,4,base) || PPC_LOAD_U32(0x833DA7E4)!=registry_)return false;
    for(unsigned i=0;i<count_;++i)if(actors_[i].reference.active)return false;
    for(unsigned i=0;i<count_;++i){const auto resolved=Resolve(registry_,actors_[i].actor.handle,ctx,base);
        if(!resolved || *resolved)return false;
    }
    for(unsigned i=0;i<count_;++i){const auto& actor=actors_[i].actor;
        if(!UnregisterOwnedActorInput(actor.peer,actor.generation))return Reject("Native peer was reassigned before group retirement");
        if(!ForgetOwnedActorJump(actor.peer,actor.generation,ctx,base))return Reject("Could not retire an extra actor's native Jump history");
        actors_[i].reference={};
    }
    retired_=true;return true;
}
bool OwnedActorRoster::AbandonDestroyedWorld(PPCContext& ctx,uint8_t* base){
    Environment preserve;
    if(!Owner() || !Readable(0x833DA7E4,4,base) || PPC_LOAD_U32(0x833DA7E4)==registry_)return false;
    Disable();retiring_=true;
    for(unsigned i=0;i<count_;++i){const auto& actor=actors_[i].actor;if(!UnregisterOwnedActorInput(actor.peer,actor.generation))return false;
        if(!ForgetOwnedActorJump(actor.peer,actor.generation,ctx,base))return false;
        actors_[i].reference={};
    }
    retired_=true;return true;
}
bool OwnedActorRoster::ReleaseForWorldUnload(PPCContext& ctx,uint8_t* base){
    Environment preserve;
    if(!Owner() || !BeginRetirement(ctx,base))return false;
    for(unsigned i=0;i<count_;++i){const auto& actor=actors_[i].actor;
        if(actors_[i].reference.active || !UnregisterOwnedActorInput(actor.peer,actor.generation) ||
           !ForgetOwnedActorJump(actor.peer,actor.generation,ctx,base))return false;
    }
    retired_=true;return true;
}
}
