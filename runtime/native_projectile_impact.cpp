#include "native_projectile_impact.h"
#include "native_random.h"
#include "online_room_gameplay.h"
#include "guest_read_batch.h"
#include "native_component_schema.h"
#include "native_entity_probe.h"
#include "native_entity_lifetime.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <rex/cvar.h>
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <cstring>
PPC_EXTERN_FUNC(__imp__sub_8293D8A0);
PPC_EXTERN_FUNC(__imp__sub_8275C0C8);
PPC_EXTERN_FUNC(__imp__sub_828B8DD0);
PPC_EXTERN_FUNC(__imp__sub_82AA86E8);
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(bool,em2_probe_room_projectile_impacts);
REXCVAR_DECLARE(bool,em2_probe_room_shooter_launches);
REXCVAR_DECLARE(bool,em2_probe_room_world_damage_events);
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
struct Scope {std::vector<em2::net::ImpactClone> clones;bool replay=false,good=true;size_t cursor=0;Scope* previous=nullptr;};
thread_local Scope* current=nullptr;
struct Active {Scope& scope;explicit Active(Scope& s):scope(s){s.previous=current;current=&s;}~Active(){current=scope.previous;}};
bool RewardOwner(uint8_t* base,uint32_t component,em2::net::DeathReward& reward,uint32_t& handle){
    std::string type;
    if(!Readable(base,component,92) || !em2::ReadNativeComponentTypeName(base,component,type) || type!="DeathRewardComponent" || PPC_LOAD_U8(component+73))return false;
    const auto root=PPC_LOAD_U32(component+48);
    if(root<4096 || root>UINT32_MAX-104 || !Readable(base,root+8,96) || PPC_LOAD_U32(root+8)!=0x8208D908)return false;
    em2::NativeEntitySnapshot snapshot;const char* error=nullptr;uint32_t address=0;
    if(!em2::ReadNativeEntitySnapshot(base,snapshot,error,address,false))return false;
    const auto row=std::find_if(snapshot.records.begin(),snapshot.records.end(),[&](const auto& r){return r.debug_address==root+8;});
    if(row==snapshot.records.end())return false;
    reward.owner=row->id;handle=row->handle;
    for(unsigned i=0;i<4;++i)reward.before[i]=PPC_LOAD_U32(component+76+i*4);
    return true;
}
}
std::optional<em2::NativeImpactCloneTicket> em2::BeginNativeImpactClone(PPCContext& ctx,uint8_t* base){
    if(!current)return {};Preserve preserve(ctx);GuestReadBatch reads;auto& scope=*current;
    net::ImpactClone clone;const auto source=ctx.r3.u32;
    if(!Readable(base,source,20)){scope.good=false;return {};}
    for(unsigned i=0;i<4;++i)clone.prototype[i]=PPC_LOAD_U32(source+4+i*4);clone.flags={ctx.r4.u32,ctx.r5.u32};
    size_t index=scope.replay?scope.cursor++:scope.clones.size();
    if(index>=net::kMaxImpactClones){scope.good=false;return {};}
    if(scope.replay){
        if(index>=scope.clones.size()){scope.good=false;return {};}
        const auto expected=scope.clones[index];
        if(clone.prototype!=expected.prototype || clone.flags!=expected.flags || !SeedNativeSpawnReplay(ctx,base,expected.random_before)){scope.good=false;return {};}
    }else{
        if(!ReadNativeSpawnRandom(ctx,base,clone.random_before)){scope.good=false;return {};}
        scope.clones.push_back(clone);
    }
    return NativeImpactCloneTicket{&scope,index};
}
void em2::EndNativeImpactClone(const NativeImpactCloneTicket& ticket,PPCContext& ctx,uint8_t* base){
    Preserve preserve(ctx);GuestReadBatch reads;auto* scope=static_cast<Scope*>(ticket.scope);
    if(!scope || ticket.index>=scope->clones.size())return;
    net::ImpactClone clone=scope->clones[ticket.index];const auto root=ctx.r3.u32;
    if(!Readable(base,root,28) || PPC_LOAD_U32(root+8)!=0x8208D908 || !ReadNativeSpawnRandom(ctx,base,clone.random_after)){scope->good=false;return;}
    for(unsigned i=0;i<4;++i)clone.entity[i]=PPC_LOAD_U32(root+12+i*4);
    if(scope->replay){if(clone!=scope->clones[ticket.index])scope->good=false;}
    else scope->clones[ticket.index]=clone;
}
PPC_FUNC_IMPL(sub_8293D8A0){
    auto* gameplay=em2::net::CurrentNativeRoomGameplay();uint32_t entity=0;em2::net::SpawnId id{};
    if(!gameplay || !REXCVAR_GET(em2_private_profile) || !REXCVAR_GET(em2_probe_room_projectile_impacts)){__imp__sub_8293D8A0(ctx,base);return;}
    em2::net::NativeImpactMode mode=em2::net::NativeImpactMode::Original;
    {Preserve preserve(ctx);em2::GuestReadBatch reads;if(Projectile(base,ctx.r3.u32,entity,id) && !PPC_LOAD_U8(ctx.r3.u32+207))mode=gameplay->ProjectileImpactMode(id,uint32_t(ctx.lr));}
    if(mode==em2::net::NativeImpactMode::Suppress)return; // Audited void callers ignore return registers.
    if(mode==em2::net::NativeImpactMode::Original){__imp__sub_8293D8A0(ctx,base);return;}
    Scope scope;em2::net::ProjectileImpact impact;impact.projectile=id;const auto component=ctx.r3.u32;
    {Preserve preserve(ctx);em2::GuestReadBatch reads;
        scope.good=Position(base,entity,impact.position) && em2::ReadNativeSpawnRandom(ctx,base,impact.random_before);
        impact.callback=PPC_LOAD_U32(component+484)!=0;
    }
    {Active active(scope);__imp__sub_8293D8A0(ctx,base);}
    {Preserve preserve(ctx);em2::GuestReadBatch reads;
        scope.good&=em2::ReadNativeSpawnRandom(ctx,base,impact.random_after) && Queued(base,entity,impact.queued);
        impact.clones=std::move(scope.clones);
        if(!scope.good)impact.projectile={};
        if(gameplay==em2::net::CurrentNativeRoomGameplay())gameplay->RecordProjectileImpact(std::move(impact));
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
    Scope scope;scope.clones=expected.clones;scope.replay=true;
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
PPC_FUNC_IMPL(sub_82AA86E8){
    auto* gameplay=em2::net::CurrentNativeRoomGameplay();
    if(!gameplay || !REXCVAR_GET(em2_private_profile) || !REXCVAR_GET(em2_probe_room_world_damage_events)){__imp__sub_82AA86E8(ctx,base);return;}
    em2::net::DeathReward reward;uint32_t handle=0,owner=0;const auto component=ctx.r3.u32>=64?ctx.r3.u32-64:0,event=ctx.r4.u32;
    em2::net::NativeImpactMode mode=em2::net::NativeImpactMode::Original;
    {Preserve preserve(ctx);em2::GuestReadBatch reads;
        if(Readable(base,event,20) && PPC_LOAD_U16(event+4)==16 && RewardOwner(base,component,reward,handle) && PPC_LOAD_U32(event+8)==handle){
            owner=PPC_LOAD_U32(component+48)+8;reward.reason=PPC_LOAD_U32(event+16);mode=gameplay->DeathRewardMode(owner);
        }
    }
    // Suppress the outer void event callback, including its handled flag. An
    // early guest death must not consume the reward before the host's tick.
    if(mode==em2::net::NativeImpactMode::Suppress)return;
    if(mode==em2::net::NativeImpactMode::Original){__imp__sub_82AA86E8(ctx,base);return;}
    Scope scope;
    {Preserve preserve(ctx);scope.good=em2::ReadNativeSpawnRandom(ctx,base,reward.random_before);}
    {Active active(scope);__imp__sub_82AA86E8(ctx,base);}
    {Preserve preserve(ctx);em2::GuestReadBatch reads;
        scope.good&=Readable(base,component,92) && PPC_LOAD_U8(component+73)==1 && em2::ReadNativeSpawnRandom(ctx,base,reward.random_after);
        if(Readable(base,component,92))for(unsigned i=0;i<4;++i)reward.after[i]=PPC_LOAD_U32(component+76+i*4);
        reward.clones=std::move(scope.clones);if(!scope.good)reward.owner={};
        if(gameplay==em2::net::CurrentNativeRoomGameplay())gameplay->RecordDeathReward(std::move(reward));
    }
}
bool em2::ReplayNativeDeathReward(PPCContext& ctx,uint8_t* base,uint32_t component,const net::DeathReward& expected,const char*& error){
    Preserve preserve(ctx);GuestReadBatch reads;error=nullptr;const auto fail=[&](const char* why){error=why;return false;};
    net::DeathReward local;uint32_t handle=0;
    if(!net::ValidDeathReward(expected) || !RewardOwner(base,component,local,handle) || local.owner!=expected.owner || local.before!=expected.before)
        return fail("death reward identity, one-shot flag or pre-state differs");
    const auto top=ctx.r1.u32&~15u;constexpr size_t bytes=65536;
    if(top<bytes+4096 || !Readable(base,top-uint32_t(bytes),bytes))return fail("death reward borrowed stack unavailable");
    std::vector<uint8_t> backup(bytes);auto* memory=base+top-bytes;std::memcpy(backup.data(),memory,bytes);
    struct Restore {uint8_t* memory;const std::vector<uint8_t>& bytes;~Restore(){std::memcpy(memory,bytes.data(),bytes.size());}} restore{memory,backup};
    const auto frame=top-512,event=top-128;std::memset(base+event,0,20);
    // The original callback reads only type +4 and owner handle +8; its original
    // distribution reads the scalar +16. No fabricated vtable is dispatched.
    PPC_STORE_U16(event+4,16);PPC_STORE_U32(event+8,handle);PPC_STORE_U32(event+16,expected.reason);
    if(!SeedNativeSpawnReplay(ctx,base,expected.random_before))return fail("death reward CRT owner unavailable");
    Scope scope;scope.clones=expected.clones;scope.replay=true;ctx.r1.u64=frame;PPC_STORE_U32(frame,top);ctx.r3.u64=component+64;ctx.r4.u64=event;
    {Active active(scope);GuestReadBatch::CallbackBoundary boundary;__imp__sub_82AA86E8(ctx,base);}
    uint32_t random=0;
    if(!Readable(base,component,92) || PPC_LOAD_U8(component+73)!=1)return fail("native death reward callback did not commit its handled flag");
    if(!scope.good || scope.cursor!=expected.clones.size())return fail("native death reward ordered clone sequence differs");
    if(!ReadNativeSpawnRandom(ctx,base,random) || random!=expected.random_after)return fail("native death reward end RNG differs");
    for(unsigned i=0;i<4;++i)if(PPC_LOAD_U32(component+76+i*4)!=expected.after[i])return fail("native death reward post-state differs");
    return true;
}
bool em2::ReadNativeShooterPending(uint8_t* base,uint32_t shooter,net::ShooterLaunch& out,uint32_t& owner){
    GuestReadBatch reads;std::string name;
    if(!Readable(base,shooter,96) || !ReadNativeComponentTypeName(base,shooter,name) || name!="ShooterComponent")return false;
    const auto root=PPC_LOAD_U32(shooter+48);if(root<4096 || root>UINT32_MAX-104 || !Readable(base,root+8,96) || PPC_LOAD_U32(root+8)!=0x8208D908)return false;
    const auto count=PPC_LOAD_U32(shooter+84),data=PPC_LOAD_U32(shooter+80);
    if(count>net::kMaxSpawnChildren || (count && !Readable(base,data,size_t(count)*4)))return false;
    net::ShooterLaunch value;for(unsigned i=0;i<4;++i)value.owner.id[i]=PPC_LOAD_U32(root+12+i*4);
    if(count){
        NativeEntitySnapshot snapshot;const char* error=nullptr;uint32_t address=0;uint64_t before=0,after=0;
        if(!SampleNativeEntityLifetimeRevision(before) || !ReadNativeEntitySnapshot(base,snapshot,error,address,false) ||
           !SampleNativeEntityLifetimeRevision(after) || before!=after)return false;
        for(unsigned i=0;i<count;++i){const auto handle=PPC_LOAD_U32(data+i*4);
            const auto record=std::find_if(snapshot.records.begin(),snapshot.records.end(),[&](const auto& row){return row.handle==handle;});
            if(record==snapshot.records.end())return false;value.projectiles.push_back(record->id);
        }
    }
    owner=root+8;out=std::move(value);return true;
}
PPC_FUNC_IMPL(sub_828B8DD0){
    auto* gameplay=em2::net::CurrentNativeRoomGameplay();
    if(!gameplay || !REXCVAR_GET(em2_private_profile) || !REXCVAR_GET(em2_probe_room_shooter_launches)){__imp__sub_828B8DD0(ctx,base);return;}
    em2::net::ShooterLaunch launch;uint32_t owner=0;const auto shooter=ctx.r3.u32;
    em2::net::NativeImpactMode mode=em2::net::NativeImpactMode::Original;
    {Preserve preserve(ctx);if(em2::ReadNativeShooterPending(base,shooter,launch,owner) && !launch.projectiles.empty())mode=gameplay->ShooterLaunchMode(launch.projectiles,uint32_t(ctx.lr));}
    if(mode==em2::net::NativeImpactMode::Original){__imp__sub_828B8DD0(ctx,base);return;}
    // The audited callers discard the result. Keep the pending list untouched
    // until the matching host launch, rather than clearing it to fake progress.
    if(mode==em2::net::NativeImpactMode::Suppress){ctx.r3.u64=0;return;}
    Scope scope;
    {Preserve preserve(ctx);scope.good=em2::ReadNativeSpawnRandom(ctx,base,launch.random_before);}
    {Active active(scope);__imp__sub_828B8DD0(ctx,base);}
    {Preserve preserve(ctx);em2::GuestReadBatch reads;
        launch.success=(ctx.r3.u32&255)!=0;launch.clones=std::move(scope.clones);
        scope.good&=em2::ReadNativeSpawnRandom(ctx,base,launch.random_after) && Readable(base,shooter,96) && !PPC_LOAD_U32(shooter+84);
        if(!scope.good){launch.owner={};launch.projectiles.clear();}
        if(gameplay==em2::net::CurrentNativeRoomGameplay())gameplay->RecordShooterLaunch(std::move(launch),owner);
    }
}
bool em2::ReplayNativeShooterLaunch(PPCContext& ctx,uint8_t* base,uint32_t shooter,const net::ShooterLaunch& expected,const char*& error){
    Preserve preserve(ctx);GuestReadBatch reads;error=nullptr;const auto fail=[&](const char* why){error=why;return false;};
    net::ShooterLaunch pending;uint32_t owner=0;
    if(!net::ValidShooterLaunch(expected) || !ReadNativeShooterPending(base,shooter,pending,owner) || pending.projectiles!=expected.projectiles ||
       (!expected.owner.peer && pending.owner.id!=expected.owner.id))return fail("launch pending entities or owner differ");
    const auto top=ctx.r1.u32&~15u;constexpr size_t bytes=65536;
    if(top<bytes+4096 || !Readable(base,top-uint32_t(bytes),bytes))return fail("launch borrowed stack unavailable");
    std::vector<uint8_t> backup(bytes);auto* memory=base+top-bytes;std::memcpy(backup.data(),memory,bytes);
    struct Restore {uint8_t* memory;const std::vector<uint8_t>& bytes;~Restore(){std::memcpy(memory,bytes.data(),bytes.size());}} restore{memory,backup};
    if(!SeedNativeSpawnReplay(ctx,base,expected.random_before))return fail("launch CRT owner unavailable");
    Scope scope;scope.replay=true;scope.clones=expected.clones;
    ctx.r1.u64=top-512;PPC_STORE_U32(ctx.r1.u32,top);ctx.r3.u64=shooter;
    {Active active(scope);GuestReadBatch::CallbackBoundary boundary;__imp__sub_828B8DD0(ctx,base);}
    if(!scope.good || scope.cursor!=expected.clones.size() || ((ctx.r3.u32&255)!=0)!=expected.success ||
       !Readable(base,shooter,96) || PPC_LOAD_U32(shooter+84))return fail("native launch result, FX or pending-list readback differs");
    if(!SeedNativeSpawnReplay(ctx,base,expected.random_after))return fail("launch canonical end RNG unavailable");
    return true;
}
