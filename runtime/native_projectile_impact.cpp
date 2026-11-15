#include "native_projectile_impact.h"
#include "native_random.h"
#include "online_room_gameplay.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <rex/cvar.h>
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <cstring>
PPC_EXTERN_FUNC(__imp__sub_8293D8A0);
PPC_EXTERN_FUNC(__imp__sub_8275C0C8);
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(bool,em2_probe_room_projectile_impacts);
namespace {
struct Preserve {PPCContext& ctx;PPCContext saved;unsigned fp=_mm_getcsr();DWORD error=GetLastError();explicit Preserve(PPCContext& c):ctx(c),saved(c){}~Preserve(){ctx=saved;_mm_setcsr(fp);SetLastError(error);}};
bool Readable(uint8_t* base,uint32_t p,size_t n){return base && p>=4096 && uint64_t(p)+n<=0x100000000ull && em2::GuestReadBatch::Readable(base+p,n);}
bool Projectile(uint8_t* base,uint32_t component,uint32_t& entity,em2::net::SpawnId& id){
    if(!Readable(base,component,492) || !Readable(base,0x833DF90C,1) || !Readable(base,0x833DBAF0,1))return false;
    const auto root=PPC_LOAD_U32(component+60);if(root<4096 || root>UINT32_MAX-104 || !Readable(base,root+8,96) || PPC_LOAD_U32(root+8)!=0x8208D908)return false;
    entity=root+8;const auto type=PPC_LOAD_U8(0x833DF90C),count=PPC_LOAD_U8(0x833DBAF0);const auto types=PPC_LOAD_U32(entity+88),items=PPC_LOAD_U32(entity+36);
    if(type>=count || !Readable(base,types,count))return false;const auto slot=PPC_LOAD_U8(types+type);
    if(slot==255 || !Readable(base,items,size_t(slot)*4+4) || PPC_LOAD_U32(items+slot*4)!=component+20)return false;
    for(unsigned i=0;i<4;++i)id[i]=PPC_LOAD_U32(entity+4+i*4);return true;
}
bool Position(uint8_t* base,uint32_t entity,std::array<uint32_t,3>& out){
    if(!Readable(base,0x833DBC3C,1) || !Readable(base,0x833DBAF0,1))return false;
    const auto type=PPC_LOAD_U8(0x833DBC3C),count=PPC_LOAD_U8(0x833DBAF0);const auto types=PPC_LOAD_U32(entity+88),items=PPC_LOAD_U32(entity+36);
    if(type>=count || !Readable(base,types,count))return false;const auto slot=PPC_LOAD_U8(types+type);
    if(slot==255 || !Readable(base,items,size_t(slot)*4+4))return false;const auto face=PPC_LOAD_U32(items+slot*4);
    if(face<8 || !Readable(base,face-8,184))return false;
    for(unsigned i=0;i<3;++i)out[i]=PPC_LOAD_U32(face-8+172+i*4);return true;
}
bool Queued(uint8_t* base,uint32_t entity,bool& out){
    if(!Readable(base,0x833DA7E4,4))return false;const auto registry=PPC_LOAD_U32(0x833DA7E4);
    if(!Readable(base,registry,204))return false;const auto count=PPC_LOAD_U32(registry+196),data=PPC_LOAD_U32(registry+192);
    if(count>65536 || (count && !Readable(base,data,size_t(count)*4)))return false;
    out=false;for(unsigned i=0;i<count;++i){const auto value=PPC_LOAD_U32(data+i*4);out|=value==entity || value==entity-8;}return true;
}
struct Scope {em2::net::ProjectileImpact data;bool replay=false,good=true;size_t cursor=0;Scope* previous=nullptr;};
thread_local Scope* current=nullptr;
struct Active {Scope& scope;explicit Active(Scope& s):scope(s){s.previous=current;current=&s;}~Active(){current=scope.previous;}};
}
std::optional<em2::NativeImpactCloneTicket> em2::BeginNativeImpactClone(PPCContext& ctx,uint8_t* base){
    if(!current)return {};Preserve preserve(ctx);GuestReadBatch reads;auto& scope=*current;
    net::ImpactClone clone;const auto source=ctx.r3.u32;
    if(!Readable(base,source,20)){scope.good=false;return {};}
    for(unsigned i=0;i<4;++i)clone.prototype[i]=PPC_LOAD_U32(source+4+i*4);clone.flags={ctx.r4.u32,ctx.r5.u32};
    size_t index=scope.replay?scope.cursor++:scope.data.clones.size();
    if(index>=net::kMaxImpactClones){scope.good=false;return {};}
    if(scope.replay){
        if(index>=scope.data.clones.size()){scope.good=false;return {};}
        const auto expected=scope.data.clones[index];
        if(clone.prototype!=expected.prototype || clone.flags!=expected.flags || !SeedNativeSpawnReplay(ctx,base,expected.random_before)){scope.good=false;return {};}
    }else{
        if(!ReadNativeSpawnRandom(ctx,base,clone.random_before)){scope.good=false;return {};}
        scope.data.clones.push_back(clone);
    }
    return NativeImpactCloneTicket{&scope,index};
}
void em2::EndNativeImpactClone(const NativeImpactCloneTicket& ticket,PPCContext& ctx,uint8_t* base){
    Preserve preserve(ctx);GuestReadBatch reads;auto* scope=static_cast<Scope*>(ticket.scope);
    if(!scope || ticket.index>=scope->data.clones.size())return;
    net::ImpactClone clone=scope->data.clones[ticket.index];const auto root=ctx.r3.u32;
    if(!Readable(base,root,28) || PPC_LOAD_U32(root+8)!=0x8208D908 || !ReadNativeSpawnRandom(ctx,base,clone.random_after)){scope->good=false;return;}
    for(unsigned i=0;i<4;++i)clone.entity[i]=PPC_LOAD_U32(root+12+i*4);
    if(scope->replay){if(clone!=scope->data.clones[ticket.index])scope->good=false;}
    else scope->data.clones[ticket.index]=clone;
}
PPC_FUNC_IMPL(sub_8293D8A0){
    auto* gameplay=em2::net::CurrentNativeRoomGameplay();uint32_t entity=0;em2::net::SpawnId id{};
    if(!gameplay || !REXCVAR_GET(em2_private_profile) || !REXCVAR_GET(em2_probe_room_projectile_impacts)){__imp__sub_8293D8A0(ctx,base);return;}
    em2::net::NativeImpactMode mode=em2::net::NativeImpactMode::Original;
    {Preserve preserve(ctx);em2::GuestReadBatch reads;if(Projectile(base,ctx.r3.u32,entity,id) && !PPC_LOAD_U8(ctx.r3.u32+207))mode=gameplay->ProjectileImpactMode(id,uint32_t(ctx.lr));}
    if(mode==em2::net::NativeImpactMode::Suppress)return; // Audited void callers ignore return registers.
    if(mode==em2::net::NativeImpactMode::Original){__imp__sub_8293D8A0(ctx,base);return;}
    Scope scope;scope.data.projectile=id;const auto component=ctx.r3.u32;
    {Preserve preserve(ctx);em2::GuestReadBatch reads;
        scope.good=Position(base,entity,scope.data.position) && em2::ReadNativeSpawnRandom(ctx,base,scope.data.random_before);
        scope.data.callback=PPC_LOAD_U32(component+484)!=0;
    }
    {Active active(scope);__imp__sub_8293D8A0(ctx,base);}
    {Preserve preserve(ctx);em2::GuestReadBatch reads;
        scope.good&=em2::ReadNativeSpawnRandom(ctx,base,scope.data.random_after) && Queued(base,entity,scope.data.queued);
        if(!scope.good)scope.data.projectile={};
        if(gameplay==em2::net::CurrentNativeRoomGameplay())gameplay->RecordProjectileImpact(std::move(scope.data));
    }
}
bool em2::ReplayNativeProjectileImpact(PPCContext& ctx,uint8_t* base,uint32_t entity,const net::ProjectileImpact& expected,const char*& error){
    Preserve preserve(ctx);GuestReadBatch reads;error=nullptr;
    const auto fail=[&](const char* why){error=why;return false;};
    if(!net::ValidProjectileImpact(expected) || !Readable(base,entity,96) || !Readable(base,0x833DF90C,1))return fail("invalid native impact input");
    const auto types=PPC_LOAD_U32(entity+88),items=PPC_LOAD_U32(entity+36);const auto type=PPC_LOAD_U8(0x833DF90C);
    if(!Readable(base,types,unsigned(type)+1))return fail("projectile type table unavailable");const auto slot=PPC_LOAD_U8(types+type);
    if(slot==255 || !Readable(base,items,size_t(slot)*4+4))return fail("projectile component absent");const auto face=PPC_LOAD_U32(items+slot*4);
    if(face<20)return fail("projectile component base invalid");const auto component=face-20;uint32_t owner=0;net::SpawnId id{};
    if(!Projectile(base,component,owner,id) || owner!=entity || id!=expected.projectile || PPC_LOAD_U8(component+207) || (PPC_LOAD_U32(component+484)!=0)!=expected.callback)
        return fail("projectile identity or retirement mode differs");
    const auto top=ctx.r1.u32&~15u;constexpr size_t bytes=65536;
    if(top<bytes+4096 || !Readable(base,top-uint32_t(bytes),bytes))return fail("impact borrowed stack unavailable");
    std::vector<uint8_t> backup(bytes);auto* memory=base+top-bytes;std::memcpy(backup.data(),memory,bytes);
    struct Restore {uint8_t* memory;const std::vector<uint8_t>& bytes;~Restore(){std::memcpy(memory,bytes.data(),bytes.size());}} restore{memory,backup};
    const auto frame=top-512,position=top-128;
    ctx.r1.u64=frame;PPC_STORE_U32(frame,top);
    for(unsigned i=0;i<3;++i)PPC_STORE_U32(position+i*4,expected.position[i]);PPC_STORE_U32(position+12,0);
    ctx.r3.u64=entity;ctx.r4.u64=position;
    {GuestReadBatch::CallbackBoundary boundary;__imp__sub_8275C0C8(ctx,base);}
    if(!SeedNativeSpawnReplay(ctx,base,expected.random_before))return fail("impact CRT owner unavailable");
    Scope scope;scope.data=expected;scope.replay=true;
    ctx.r1.u64=frame;ctx.r3.u64=component;
    {Active active(scope);GuestReadBatch::CallbackBoundary boundary;__imp__sub_8293D8A0(ctx,base);}
    bool queued=false;
    if(!scope.good || scope.cursor!=expected.clones.size() || !Queued(base,entity,queued) || queued!=expected.queued)return fail("impact native FX or retirement readback differs");
    // Host Damage was already delivered exactly; suppressed local area-query
    // damage can consume a different number of RNG draws on changed geometry.
    // FX clone inputs/outputs are checked above. Commit the host operation's
    // canonical end stream through the original srand, not a counter patch.
    if(!SeedNativeSpawnReplay(ctx,base,expected.random_after))return fail("impact canonical end RNG state unavailable");
    return true;
}
