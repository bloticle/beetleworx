#include "online_actor_reference.h"
#include "online_actor_input.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <array>
#include <cstring>
#include <stdexcept>
#include <windows.h>
#include <immintrin.h>
#include <rex/logging.h>
PPC_EXTERN_FUNC(__imp__sub_82732A18);
PPC_EXTERN_FUNC(__imp__sub_827DDD88);
PPC_EXTERN_FUNC(__imp__sub_827B58D0);
namespace em2::net {
namespace {
bool Readable(uint32_t address,size_t size,uint8_t* base){return base && address>=4096 && uint64_t(address)+size<=0x100000000ull && GuestReadBatch::Readable(base+address,size);}
struct Environment {unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(csr);SetLastError(error);}};
struct NativeScope {
    PPCContext& ctx;PPCContext saved;
    unsigned csr=_mm_getcsr();DWORD error=GetLastError();
    uint8_t* scratch=nullptr;std::array<uint8_t,4096> bytes;
    NativeScope(PPCContext& c,uint8_t* base):ctx(c),saved(c){
        const auto top=ctx.r1.u32&~15u;
        if(top>=8192 && Readable(top-4096,4096,base)){
            scratch=base+top-4096;std::memcpy(bytes.data(),scratch,bytes.size());ctx.r1.u64=top-2048;
        }
    }
    ~NativeScope(){if(scratch)std::memcpy(scratch,bytes.data(),bytes.size());ctx=saved;_mm_setcsr(csr);SetLastError(error);}
};
bool BasicController(const OwnedActorInput& actor,uint8_t* base,uint32_t& basic){
    if(!Readable(actor.entity,96,base) || !Readable(0x833DAE58,1,base))return false;
    const auto kind=PPC_LOAD_U8(0x833DAE58);
    const auto types=PPC_LOAD_U32(actor.entity+88);
    const auto list=PPC_LOAD_U32(actor.entity+36);
    if(!Readable(types,unsigned(kind)+1,base))return false;
    const auto slot=PPC_LOAD_U8(types+kind);
    if(slot==255 || !Readable(list,unsigned(slot)*4+4,base))return false;
    const auto face=PPC_LOAD_U32(list+unsigned(slot)*4);
    if(face<8 || face-8!=actor.controller+16 || !Readable(face-8,84,base))return false;
    basic=face-8;const auto table=PPC_LOAD_U32(basic);
    return table==0x8211DE70 && Readable(table,112,base) && PPC_LOAD_U32(table+108)==0x82A342E8;
}
bool GameplayCameraEntity(uint32_t entity,uint8_t* base){
    if(!Readable(entity,96,base) || PPC_LOAD_U32(entity)!=0x8208D908 || PPC_LOAD_U32(entity+44)!=2)return false;
    const auto list=PPC_LOAD_U32(entity+36);if(!Readable(list,8,base))return false;
    const auto frame=PPC_LOAD_U32(list),camera=PPC_LOAD_U32(list+4);
    return Readable(frame,4,base) && Readable(camera,4,base) &&
        PPC_LOAD_U32(frame)==0x82090340 && PPC_LOAD_U32(camera)==0x820D8970;
}
bool SetReference(uint32_t basic,uint32_t entity,PPCContext& ctx,uint8_t* base){
    NativeScope preserve(ctx,base);if(!preserve.scratch)return false;
    ctx.r3.u64=basic;ctx.r4.u64=entity;
    // Audited derived virtual108 (82A342E8) first assigns basic+80 through
    // NiPointer, then retargets a RETAIL camera via PlayerManager. Extras must
    // not steal that camera. Use the original base reference operation for
    // the identical ownership effect; per-peer movement uses its own basis.
    // No component pointer/refcount is edited manually.
    __imp__sub_827DDD88(ctx,base);return PPC_LOAD_U32(basic+80)==entity;
}
}
ActorReferenceResult AttachOwnedActorMovementReference(uint64_t peer,uint32_t camera_handle,PPCContext& ctx,uint8_t* base,ActorMovementReference& lease){
    Environment environment;
    if(lease.active || !camera_handle)return ActorReferenceResult::Invalid;
    OwnedActorInput actor;if(!OwnedActorInputFor(peer,ctx,base,actor))return ActorReferenceResult::Stale;
    uint32_t basic=0;if(!BasicController(actor,base,basic))return ActorReferenceResult::Invalid;
    if(PPC_LOAD_U32(basic+80))return ActorReferenceResult::ExistingReference;
    uint32_t camera=0;
    {
        NativeScope preserve(ctx,base);if(!preserve.scratch)return ActorReferenceResult::Invalid;
        const auto key=ctx.r1.u32+1024;PPC_STORE_U32(key,camera_handle);
        ctx.r3.u64=actor.registry;ctx.r4.u64=key;__imp__sub_82732A18(ctx,base);camera=ctx.r3.u32;
    }
    // Real camera entity, resolved by handle. Sharing its native reference is
    // safe; the separately supplied peer basis determines ordinary movement.
    // Actor-self references are rejected to avoid reference-count cycles.
    if(camera==actor.entity || !GameplayCameraEntity(camera,base))
        return ActorReferenceResult::Invalid;
    if(!SetReference(basic,camera,ctx,base))return ActorReferenceResult::NativeFailure;
    lease={actor.peer,actor.generation,actor.registry,actor.controller,camera,true};return ActorReferenceResult::Attached;
}
bool OwnedActorMovementReferenceReady(uint64_t peer,PPCContext& ctx,uint8_t* base){
    Environment preserve;OwnedActorInput actor;uint32_t basic=0;
    return OwnedActorInputFor(peer,ctx,base,actor) && BasicController(actor,base,basic) && GameplayCameraEntity(PPC_LOAD_U32(basic+80),base);
}
ActorReferenceResult ReleaseOwnedActorMovementReference(ActorMovementReference& lease,PPCContext& ctx,uint8_t* base){
    Environment environment;
    if(!lease.active)return ActorReferenceResult::Invalid;
    OwnedActorInput actor;
    if(!OwnedActorInputFor(lease.peer,ctx,base,actor) || actor.generation!=lease.generation || actor.registry!=lease.registry || actor.controller!=lease.controller)
        return ActorReferenceResult::Stale;
    uint32_t basic=0;if(!BasicController(actor,base,basic))return ActorReferenceResult::Stale;
    if(PPC_LOAD_U32(basic+80)!=lease.reference){lease={};return ActorReferenceResult::AlreadyReplaced;}
    if(!SetReference(basic,0,ctx,base))return ActorReferenceResult::NativeFailure;
    lease={};return ActorReferenceResult::Released;
}
namespace {
thread_local uint32_t reference_fixture_entity=0;
thread_local unsigned reference_fixture_adds=0,reference_fixture_releases=0;
void FixtureReferenceAdd(PPCContext& ctx,uint8_t* base){
    if(ctx.r3.u32!=reference_fixture_entity)throw std::runtime_error("wrong native reference retain receiver");
    ++reference_fixture_adds;PPC_STORE_U32(ctx.r3.u32+200,PPC_LOAD_U32(ctx.r3.u32+200)+1);
}
void FixtureReferenceRelease(PPCContext& ctx,uint8_t* base){
    if(ctx.r3.u32!=reference_fixture_entity || !PPC_LOAD_U32(ctx.r3.u32+200))throw std::runtime_error("unbalanced native reference release");
    ++reference_fixture_releases;PPC_STORE_U32(ctx.r3.u32+200,PPC_LOAD_U32(ctx.r3.u32+200)-1);
}
}
bool TestOwnedActorMovementReferences(PPCContext& ctx,uint8_t* base,uint64_t first_peer,uint64_t second_peer){
    OwnedActorInput a,b;if(!OwnedActorInputFor(first_peer,ctx,base,a) || !OwnedActorInputFor(second_peer,ctx,base,b))return false;
    // Private mapped fixture addresses; production callers never fabricate a
    // reference entity, replace vtables, or write refcounts.
    constexpr uint32_t camera=0x3C000,node=0x3D000,key=0x3D100,handle=0xF100;
    const auto code=uint32_t(PPC_CODE_BASE)+256;
    NativeScope preserve(ctx,base);if(!preserve.scratch)return false;
    struct Restore {
        uint8_t* base;std::array<OwnedActorInput,2> actors;
        std::array<std::array<uint8_t,800>,2> controllers;
        std::array<uint32_t,2> type_globals;std::array<uint8_t,2> type_entries;
        std::array<uint32_t,4> buckets;uint32_t bucket_address;
        std::array<uint8_t,96> camera_table;
        uint32_t prior_entity=reference_fixture_entity;unsigned adds=reference_fixture_adds,releases=reference_fixture_releases;
        Restore(uint8_t* memory,std::array<OwnedActorInput,2> values):base(memory),actors(values){
            type_globals={PPC_LOAD_U8(0x833DAE58),PPC_LOAD_U32(0x8208D908)};
            for(unsigned i=0;i<2;++i){std::memcpy(controllers[i].data(),base+actors[i].controller,800);const auto types=PPC_LOAD_U32(actors[i].entity+88);type_entries[i]=PPC_LOAD_U8(types+30);}
            bucket_address=PPC_LOAD_U32(actors[0].registry+16);for(unsigned i=0;i<4;++i)buckets[i]=PPC_LOAD_U32(bucket_address+4*i);
            std::memcpy(camera_table.data(),base+0x8208D908,camera_table.size());
        }
        ~Restore(){
            for(unsigned i=0;i<2;++i){std::memcpy(base+actors[i].controller,controllers[i].data(),800);const auto types=PPC_LOAD_U32(actors[i].entity+88);PPC_STORE_U8(types+30,type_entries[i]);}
            PPC_STORE_U8(0x833DAE58,type_globals[0]);for(unsigned i=0;i<4;++i)PPC_STORE_U32(bucket_address+4*i,buckets[i]);
            std::memcpy(base+0x8208D908,camera_table.data(),camera_table.size());reference_fixture_entity=prior_entity;reference_fixture_adds=adds;reference_fixture_releases=releases;
        }
    }restore{base,{a,b}};
    PPC_LOOKUP_FUNC(base,code)=FixtureReferenceAdd;PPC_LOOKUP_FUNC(base,code+4)=FixtureReferenceRelease;
    PPC_STORE_U32(0x8208D908,code);PPC_STORE_U32(0x8208D90C,code+4);
    reference_fixture_entity=camera;reference_fixture_adds=reference_fixture_releases=0;
    std::memset(base+camera,0,256);PPC_STORE_U32(camera,0x8208D908);PPC_STORE_U32(camera+44,2);
    PPC_STORE_U32(camera+36,camera+256);PPC_STORE_U32(camera+256,camera+512);PPC_STORE_U32(camera+260,camera+768);
    PPC_STORE_U32(camera+512,0x82090340);PPC_STORE_U32(camera+768,0x820D8970);
    PPC_STORE_U8(0x833DAE58,30);
    for(const auto actor:{a,b}){
        PPC_STORE_U8(PPC_LOAD_U32(actor.entity+88)+30,0);PPC_STORE_U32(actor.controller+16,0x8211DE70);PPC_STORE_U32(actor.controller+96,0);
    }
    PPC_STORE_U32(key,handle);ctx.r3.u64=key;__imp__sub_827B58D0(ctx,base);
    const auto bucket=restore.bucket_address+(ctx.r3.u32%4)*4;
    PPC_STORE_U32(node,handle);PPC_STORE_U32(node+4,camera);PPC_STORE_U32(node+8,PPC_LOAD_U32(bucket));PPC_STORE_U32(bucket,node);
    bool ok=true;unsigned checks=0;auto check=[&](bool condition,const char* why){++checks;if(!condition){REXLOG_ERROR("Owned actor movement reference: {}",why);ok=false;}};
    ActorMovementReference first,second;
    const auto before=ctx;const auto csr=_mm_getcsr();const auto error=GetLastError();
    check(AttachOwnedActorMovementReference(a.peer,handle,ctx,base,first)==ActorReferenceResult::Attached &&
        AttachOwnedActorMovementReference(b.peer,handle,ctx,base,second)==ActorReferenceResult::Attached &&
        reference_fixture_adds==2 && PPC_LOAD_U32(camera+200)==2,"two actors retain a real reference independently through original NiPointer");
    check(!std::memcmp(&before,&ctx,sizeof(ctx)) && csr==_mm_getcsr() && error==GetLastError(),"full caller/FP/error restoration");
    ActorMovementReference duplicate;
    check(AttachOwnedActorMovementReference(a.peer,handle,ctx,base,duplicate)==ActorReferenceResult::ExistingReference && reference_fixture_adds==2,"do not replace an existing native reference or double-retain");
    check(ReleaseOwnedActorMovementReference(first,ctx,base)==ActorReferenceResult::Released && !first.active && PPC_LOAD_U32(camera+200)==1 && PPC_LOAD_U32(b.controller+96)==camera,"releasing one actor preserves another actor's reference");
    check(AttachOwnedActorMovementReference(a.peer,a.handle,ctx,base,duplicate)==ActorReferenceResult::Invalid && !duplicate.active,"actor-self reference cycle rejected");
    const auto registry=PPC_LOAD_U32(0x833DA7E4);PPC_STORE_U32(0x833DA7E4,registry+4);
    check(ReleaseOwnedActorMovementReference(second,ctx,base)==ActorReferenceResult::Stale && reference_fixture_releases==1,"old world cannot release a reused controller");
    PPC_STORE_U32(0x833DA7E4,registry);
    check(ReleaseOwnedActorMovementReference(second,ctx,base)==ActorReferenceResult::Released && reference_fixture_releases==2 && !PPC_LOAD_U32(camera+200),"balanced native teardown across both owners");
    check(AttachOwnedActorMovementReference(a.peer,handle,ctx,base,first)==ActorReferenceResult::Attached && SetReference(a.controller+16,0,ctx,base),
        "native engine can independently clear its retained reference");
    check(ReleaseOwnedActorMovementReference(first,ctx,base)==ActorReferenceResult::AlreadyReplaced && !first.active && reference_fixture_adds==3 && reference_fixture_releases==3,
        "engine replacement is not released twice by stale bookkeeping");
    check(AttachOwnedActorMovementReference(a.peer,0,ctx,base,first)==ActorReferenceResult::Invalid &&
        AttachOwnedActorMovementReference(0,handle,ctx,base,first)==ActorReferenceResult::Stale,"missing camera and unknown peer rejected");
    PPC_STORE_U32(camera+768,0);
    check(AttachOwnedActorMovementReference(a.peer,handle,ctx,base,first)==ActorReferenceResult::Invalid && !first.active && reference_fixture_adds==3,
        "an arbitrary two-component entity cannot masquerade as the gameplay camera");
    PPC_STORE_U32(camera+768,0x820D8970);
    if(ok)REXLOG_INFO("Owned actor movement reference passed {} checks: original NiPointer retain/release, shared lifetime, caller preservation, existing-reference protection, self-cycle/world rejection and balanced teardown",checks);
    return ok;
}
}
