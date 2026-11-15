#include "native_actor_health.h"
#include "guest_read_batch.h"
#include "native_entity_probe.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <array>
#include <cstring>
PPC_EXTERN_FUNC(__imp__sub_827C9CE0); // entity int-property read, vtable+204
PPC_EXTERN_FUNC(__imp__sub_827CA5F0); // entity int-property write, vtable+268
PPC_EXTERN_FUNC(__imp__sub_82761F40); // original Damage event constructor
PPC_EXTERN_FUNC(__imp__sub_82821AC8); // original AddStimulus
PPC_EXTERN_FUNC(__imp__sub_828218B0); // original effective damage calculation
PPC_EXTERN_FUNC(sub_827340E0);       // original event dispatch, native ordering hook
PPC_EXTERN_FUNC(__imp__sub_827621C0); // non-deleting Damage event destructor
namespace {
thread_local unsigned damage_replay_depth=0;
bool Readable(uint8_t* base,uint32_t at,size_t size){return base && at>=4096 && uint64_t(at)+size<=0x100000000ull && em2::GuestReadBatch::Readable(base+at,size);}
struct Borrow {
    PPCContext& ctx;PPCContext saved;DWORD error=GetLastError();unsigned csr=_mm_getcsr();
    uint8_t* memory=nullptr;uint8_t* base=nullptr;uint32_t frame=0,scratch=0;std::array<uint8_t,8192> bytes;
    Borrow(PPCContext& c,uint8_t* guest_base):ctx(c),saved(c),base(guest_base){
        const auto top=c.r1.u32&~15u;if(top<12288 || !Readable(base,top-8192,bytes.size()))return;
        memory=base+top-8192;std::memcpy(bytes.data(),memory,bytes.size());frame=top-4096;scratch=frame+512;
    }
    void Reset(){ctx=saved;ctx.r1.u64=frame;PPC_STORE_U32(frame,saved.r1.u32);}
    ~Borrow(){if(memory)std::memcpy(memory,bytes.data(),bytes.size());ctx=saved;_mm_setcsr(csr);SetLastError(error);}
};
bool Route(uint8_t* base,uint32_t entity,uint32_t& property){
    if(!Readable(base,entity,96) || PPC_LOAD_U32(entity)!=0x8208D908 || !Readable(base,0x8208D908,272) ||
        PPC_LOAD_U32(0x8208D908+204)!=0x827C9CE0 || PPC_LOAD_U32(0x8208D908+268)!=0x827CA5F0 || !Readable(base,0x833DBB14,4))return false;
    const auto count=PPC_LOAD_U32(entity+44),components=PPC_LOAD_U32(entity+36);
    if(!count || count>256 || !Readable(base,components,size_t(count)*4))return false;
    for(unsigned i=0;i<count;++i){const auto component=PPC_LOAD_U32(components+i*4);
        if(!Readable(base,component,4) || !Readable(base,PPC_LOAD_U32(component),272))return false;}
    property=PPC_LOAD_U32(0x833DBB14);return Readable(base,property,8);
}
// The retail Damage Lua export (827D06A8) uses this exact native sequence.
// At a completed-update boundary a numeric Health=0 write postpones death
// notification until the next update. Keep the immediate notification path for
// that phase and legacy health-only correction. Full event authority can stage
// direct native zero-health state before the upcoming original health update.
// This does not claim to reproduce the host's particular damage stimulus or
// replace general combat-event replication.
bool NativeLethalCorrection(Borrow& borrow,uint32_t entity){
    struct Replay {unsigned saved=damage_replay_depth;Replay(){++damage_replay_depth;}~Replay(){damage_replay_depth=saved;}} replay;
    auto* base=borrow.base;auto& ctx=borrow.ctx;
    em2::NativeEntitySnapshot entities;const char* error=nullptr;uint32_t address=0;
    if(!em2::ReadNativeEntitySnapshot(base,entities,error,address,false)){
        REXLOG_ERROR("Native lethal correction registry rejected: {} at {:08X}; target {:08X}",error?error:"unknown",address,entity);return false;
    }
    if(!Readable(base,0x833DA6F4,4))return false;
    const auto found=std::find_if(entities.records.begin(),entities.records.end(),[&](const auto& value){return value.debug_address==entity;});
    const auto manager=PPC_LOAD_U32(0x833DA6F4);
    if(found==entities.records.end() || !found->handle || !Readable(base,manager,1016)){
        REXLOG_ERROR("Native lethal correction binding rejected: entity {:08X}, handle {:08X}, manager {:08X}",entity,found==entities.records.end()?0:found->handle,manager);return false;
    }
    const auto event=borrow.scratch,target=event+128,instigator=target+4,position=event+144;
    std::memset(base+event,0,176);PPC_STORE_U32(target,found->handle);
    borrow.Reset();ctx.r3.u64=event;ctx.r4.u64=target;ctx.r5.u64=instigator;
    {em2::GuestReadBatch::CallbackBoundary callback;__imp__sub_82761F40(ctx,base);}
    const auto destroy=[&]{borrow.Reset();ctx.r3.u64=event;em2::GuestReadBatch::CallbackBoundary callback;__imp__sub_827621C0(ctx,base);};
    try{
        borrow.Reset();ctx.r3.u64=event;ctx.r4.u64=2;ctx.f1.f64=1000.0;
        {em2::GuestReadBatch::CallbackBoundary callback;__imp__sub_82821AC8(ctx,base);}
        borrow.Reset();ctx.r3.u64=event;ctx.r4.u64=position;ctx.r5.u64=0;
        {em2::GuestReadBatch::CallbackBoundary callback;__imp__sub_828218B0(ctx,base);}
        borrow.Reset();ctx.r3.u64=manager;ctx.r4.u64=event;ctx.r5.u64=0;
        {em2::GuestReadBatch::CallbackBoundary callback;sub_827340E0(ctx,base);}
    }catch(...){destroy();throw;}
    destroy();return true;
}
}
bool em2::NativeDamageReplayActive()noexcept{return damage_replay_depth!=0;}
void em2::DispatchNativeAuthorityDamage(PPCContext& ctx,uint8_t* base){
    struct Replay {unsigned saved=damage_replay_depth;Replay(){++damage_replay_depth;}~Replay(){damage_replay_depth=saved;}} replay;
    sub_827340E0(ctx,base);
}
std::optional<int32_t> em2::ReadNativeActorHealth(PPCContext& ctx,uint8_t* base,uint32_t entity){
    GuestReadBatch reads;Borrow borrow(ctx,base);uint32_t property=0;
    if(!borrow.memory || !Route(base,entity,property))return {};
    borrow.Reset();PPC_STORE_U32(borrow.scratch,0xCDCDCDCD);
    ctx.r3.u64=entity;ctx.r4.u64=property+4;ctx.r5.u64=borrow.scratch;ctx.r6.u64=0;
    {GuestReadBatch::CallbackBoundary callback;__imp__sub_827C9CE0(ctx,base);}
    if(!ctx.r3.u32)return {};return int32_t(PPC_LOAD_U32(borrow.scratch));
}
bool em2::CorrectNativeActorHealth(PPCContext& ctx,uint8_t* base,uint32_t entity,int32_t health,NativeHealthCorrectionPhase phase){
    GuestReadBatch reads;Borrow borrow(ctx,base);uint32_t property=0;
    if(!borrow.memory || !Route(base,entity,property))return false;
    const auto current=ReadNativeActorHealth(ctx,base,entity);if(!current)return false;if(*current==health)return true;
    // A fabricated hit can be ignored by native immunity during respawn.
    // Before an update, use the authoritative property value and let original
    // HealthComponent update 827CF7F0 process its own zero-health behavior once.
    if(health==0 && *current>0 && *current<=1000 && phase==NativeHealthCorrectionPhase::CompletedUpdate){
        if(!NativeLethalCorrection(borrow,entity))return false;
        const auto corrected=ReadNativeActorHealth(ctx,base,entity);
        if(!corrected || *corrected!=0)REXLOG_ERROR("Native lethal correction readback rejected: entity {:08X}, health before {}, after present {}, after {}; original Damage dispatched",entity,*current,bool(corrected),corrected.value_or(-1));
        return corrected && *corrected==0;
    }
    borrow.Reset();ctx.r3.u64=entity;ctx.r4.u64=property+4;ctx.r5.s64=health;ctx.r6.u64=0;
    {GuestReadBatch::CallbackBoundary callback;__imp__sub_827CA5F0(ctx,base);}
    if(!ctx.r3.u32)return false;
    const auto corrected=ReadNativeActorHealth(ctx,base,entity);return corrected && *corrected==health;
}
