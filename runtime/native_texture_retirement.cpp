#include "native_texture_retirement.h"
#include "guest_read_batch.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <limits>

PPC_EXTERN_FUNC(EM2RetireTextureData);
PPC_EXTERN_FUNC(EM2RetireTextureDataBase);
PPC_EXTERN_FUNC(EM2DrainNativeResources);
PPC_EXTERN_FUNC(__imp__RtlEnterCriticalSection);
PPC_EXTERN_FUNC(__imp__RtlLeaveCriticalSection);
REXCVAR_DECLARE(bool,em2_trace_shadow_preparation);
namespace {
bool Readable(uint8_t* base,uint32_t address,size_t bytes){
    return base && address>=4096 && uint64_t(address)+bytes<=0x100000000ull &&
        em2::GuestReadBatch::Readable(PPC_RAW_ADDR(address),bytes);
}
// Caller holds the resource manager's native +520 critical section. This is
// the same bounded list consumed by 826811B8, which frees headers at 82681788.
bool QueuedHeader(uint8_t* base,uint32_t manager,uint32_t header){
    if(!Readable(base,manager,572) || !Readable(base,header,52) || (PPC_LOAD_U32(header)&15)!=3)return false;
    const auto type=PPC_LOAD_U32(manager);
    if(!Readable(base,type,60) || PPC_LOAD_U32(type+56)!=0x826811B8)return false;
    const auto count=PPC_LOAD_U16(manager+566),capacity=PPC_LOAD_U16(manager+564);
    if(!count || count>capacity || capacity>16384)return false;
    const auto items=PPC_LOAD_U32(manager+560);
    if(!Readable(base,items,size_t(count)*4))return false;
    for(unsigned i=0;i<count;++i)if(PPC_LOAD_U32(items+i*4)==header)return true;
    return false;
}
struct TextureOwner {uint32_t owner=0,header=0,backing=0;uint64_t epoch=0;bool destroying=false,retired=false;};
struct TextureOwners {
    std::mutex mutex;std::unordered_map<uint32_t,TextureOwner> owners;
    std::unordered_map<uint32_t,std::pair<uint32_t,uint64_t>> headers;uint64_t epoch=0;
    bool Clear(uint8_t* base,const TextureOwner& value){
        if(!Readable(base,value.owner,40) || PPC_LOAD_U32(value.owner)!=0x82055A3C ||
           (PPC_LOAD_U32(value.owner+28)!=value.header && PPC_LOAD_U32(value.owner+28)!=0) ||
           (PPC_LOAD_U32(value.owner+32)!=value.backing && PPC_LOAD_U32(value.owner+32)!=0))return false;
        PPC_STORE_U32(value.owner+28,0);PPC_STORE_U32(value.owner+32,0);return true;
    }
    void Remove(uint32_t object,uint64_t token){
        auto it=owners.find(object);if(it==owners.end() || it->second.epoch!=token)return;
        const auto header=it->second.header;const auto found=headers.find(header);
        if(found!=headers.end() && found->second==std::pair(object,token))headers.erase(found);
        owners.erase(it);
    }
    uint64_t Register(uint8_t* base,uint32_t object){
        if(!Readable(base,object,40) || PPC_LOAD_U32(object)!=0x82055A3C)return 0;
        const auto header=PPC_LOAD_U32(object+28),backing=PPC_LOAD_U32(object+32);
        if(!header || !Readable(base,header,52) || (PPC_LOAD_U32(header)&15)!=3)return 0;
        std::lock_guard lock(mutex);
        if(epoch==UINT64_MAX || owners.size()>=65536)return 0;
        if(const auto old=owners.find(object);old!=owners.end())Remove(object,old->second.epoch);
        if(const auto old=headers.find(header);old!=headers.end()){
            auto previous=owners.find(old->second.first);
            if(previous!=owners.end() && previous->second.epoch==old->second.second){
                previous->second.retired=true;if(!previous->second.destroying)Clear(base,previous->second);
            }
            headers.erase(old);
        }
        const auto token=++epoch;owners.emplace(object,TextureOwner{object,header,backing,token});headers[header]={object,token};return token;
    }
    uint64_t Begin(uint32_t object){std::lock_guard lock(mutex);const auto it=owners.find(object);if(it==owners.end())return 0;it->second.destroying=true;return it->second.epoch;}
    void End(uint32_t object,uint64_t token){std::lock_guard lock(mutex);Remove(object,token);}
    bool Retire(uint8_t* base,uint32_t header){
        std::lock_guard lock(mutex);const auto found=headers.find(header);if(found==headers.end())return false;
        const auto binding=found->second;headers.erase(found);const auto it=owners.find(binding.first);
        if(it==owners.end() || it->second.epoch!=binding.second || it->second.header!=header)return false;
        auto& value=it->second;value.retired=true;
        return !value.destroying && Clear(base,value);
    }
    bool Transfer(uint8_t* base,uint32_t object,bool queued){
        std::lock_guard lock(mutex);const auto it=owners.find(object);if(it==owners.end() || (!it->second.retired && !queued))return false;
        if(!Clear(base,it->second))return false;it->second.retired=true;return true;
    }
    bool Retired(uint32_t object){std::lock_guard lock(mutex);const auto it=owners.find(object);return it!=owners.end() && it->second.retired;}
};
TextureOwners texture_owners;
struct Environment {DWORD error=GetLastError();unsigned csr=_mm_getcsr();~Environment(){SetLastError(error);_mm_setcsr(csr);}};
uint32_t Manager(uint8_t* base){
    if(!Readable(base,0x833CE200,4))return 0;
    const auto renderer=PPC_LOAD_U32(0x833CE200);
    if(!Readable(base,renderer,1892) || PPC_LOAD_U32(renderer)!=0x82053F28)return 0;
    const auto manager=PPC_LOAD_U32(renderer+1888);
    return Readable(base,manager,572)?manager:0;
}
}
bool em2::DeferQueuedTextureHeaderFree(PPCContext& ctx,uint8_t* base){
    if((ctx.lr!=0x8267CC74 && ctx.lr!=0x8267BE04) || !ctx.r3.u32)return false;
    struct Restore {PPCContext& ctx;PPCContext saved=ctx;DWORD error=GetLastError();unsigned csr=_mm_getcsr();~Restore(){ctx=saved;SetLastError(error);_mm_setcsr(csr);}} restore{ctx};
    const auto header=ctx.r3.u32,object=ctx.r31.u32;
    uint32_t manager=0;
    {
        GuestReadBatch inspection;
        if(!Readable(base,object,40) || PPC_LOAD_U32(object)!=0x82055A3C || PPC_LOAD_U32(object+28)!=header)return false;
        manager=Manager(base);if(!manager)return false;
        const auto type=PPC_LOAD_U32(manager);
        if(!Readable(base,type,60) || PPC_LOAD_U32(type+56)!=0x826811B8)return false;
    }
    ctx.r3.u64=manager+520;__imp__RtlEnterCriticalSection(ctx,base);
    struct Unlock {PPCContext& ctx;uint8_t* base;uint32_t manager;~Unlock(){ctx.r3.u64=manager+520;__imp__RtlLeaveCriticalSection(ctx,base);}} unlock{ctx,base,manager};
    GuestReadBatch inspection;
    const bool queued=Manager(base)==manager && QueuedHeader(base,manager,header);
    if(queued && REXCVAR_GET(em2_trace_shadow_preparation))
        REXLOG_INFO("Native texture header ownership deferred: object={:08X} header={:08X} resource={:08X} backing={:08X}; exact pending resource queue match, immediate free omitted, native fenced cleanup retained",object,header,PPC_LOAD_U32(object+36),PPC_LOAD_U32(object+32));
    return queued;
}
void em2::TrackNativeTextureData(uint32_t owner,uint8_t* base){Environment preserve;GuestReadBatch inspection;texture_owners.Register(base,owner);}
void em2::RetireNativeTextureHeader(PPCContext& ctx,uint8_t* base){
    Environment preserve;GuestReadBatch inspection;
    // The original consumer has already popped/swapped this entry out of
    // +560 and decremented +566 before freeing backing/header storage.
    // Queue membership is therefore NOT the ownership proof at this site.
    const auto manager=ctx.r23.u32,header=ctx.r3.u32;
    if(ctx.lr!=0x82681788 || header!=ctx.r28.u32 || !Readable(base,manager,572) || !Readable(base,header,52) ||
       (PPC_LOAD_U32(header)&15)!=3)return;
    const auto type=PPC_LOAD_U32(manager);
    if(!Readable(base,type,60) || PPC_LOAD_U32(type+56)!=0x826811B8)return;
    if(texture_owners.Retire(base,header))REXLOG_INFO("Native texture ownership retired: header {:08X}; stale wrapper header/backing cleared before original fenced free",header);
}
void em2::TransferQueuedTextureOwnership(PPCContext& ctx,uint8_t* base){
    struct Restore {PPCContext& ctx;PPCContext saved=ctx;Environment host;~Restore(){ctx=saved;}} restore{ctx};
    const auto object=ctx.r31.u32;uint32_t manager=0,header=0;
    {GuestReadBatch inspection;if(!Readable(base,object,40) || PPC_LOAD_U32(object)!=0x82055A3C)return;
        header=PPC_LOAD_U32(object+28);if(!header)return;
        if(texture_owners.Retired(object)){texture_owners.Transfer(base,object,false);return;}
        manager=Manager(base);if(!manager)return;
    }
    ctx.r3.u64=manager+520;__imp__RtlEnterCriticalSection(ctx,base);
    struct Unlock {PPCContext& ctx;uint8_t* base;uint32_t manager;~Unlock(){ctx.r3.u64=manager+520;__imp__RtlLeaveCriticalSection(ctx,base);}}unlock{ctx,base,manager};
    GuestReadBatch inspection;
    const bool queued=Manager(base)==manager && QueuedHeader(base,manager,header);
    if(texture_owners.Transfer(base,object,queued) && REXCVAR_GET(em2_trace_shadow_preparation))
        REXLOG_INFO("Native texture wrapper transferred: owner {:08X} header {:08X}; original queue retains backing/header ownership",object,header);
}
namespace {
void DestroyTexture(PPCContext& ctx,uint8_t* base,PPCFunc* original){
    const auto object=ctx.r3.u32;uint64_t token=0;
    {Environment preserve;token=texture_owners.Begin(object);}
    struct Finish {uint32_t object;uint64_t token;~Finish(){Environment preserve;texture_owners.End(object,token);}}finish{object,token};
    original(ctx,base);
}
}
PPC_FUNC_IMPL(sub_8267CC20){DestroyTexture(ctx,base,EM2RetireTextureData);}
PPC_FUNC_IMPL(sub_8267BDB8){DestroyTexture(ctx,base,EM2RetireTextureDataBase);}
PPC_FUNC_IMPL(sub_826811B8){EM2DrainNativeResources(ctx,base);}

bool em2::TestNativeTextureRetirement(){
    std::vector<uint8_t> memory(0x20000);auto* base=memory.data();
    constexpr uint32_t manager=0x10000,type=0x11000,items=0x12000,header=0x13000;
    PPC_STORE_U32(manager,type);PPC_STORE_U32(type+56,0x826811B8);
    PPC_STORE_U32(manager+560,items);PPC_STORE_U16(manager+564,4);PPC_STORE_U16(manager+566,2);
    PPC_STORE_U32(header,3);PPC_STORE_U32(items,0x14000);PPC_STORE_U32(items+4,header);
    auto check=[](bool valid,const char* description){if(!valid)REXLOG_ERROR("Native texture retirement: {}",description);return valid;};
    if(!check(QueuedHeader(base,manager,header),"exact pending header admitted"))return false;
    PPC_STORE_U16(manager+566,1);
    if(!check(!QueuedHeader(base,manager,header),"unused capacity entries do not transfer ownership"))return false;
    PPC_STORE_U16(manager+566,5);
    if(!check(!QueuedHeader(base,manager,header),"count beyond capacity rejected"))return false;
    PPC_STORE_U16(manager+566,2);PPC_STORE_U32(header,4);
    if(!check(!QueuedHeader(base,manager,header),"foreign header kind rejected"))return false;
    PPC_STORE_U32(header,3);PPC_STORE_U32(type+56,0x12345678);
    if(!check(!QueuedHeader(base,manager,header),"foreign resource manager rejected"))return false;
    PPCContext ctx{};ctx.fpscr.InitHost();ctx.r3.u64=header;ctx.lr=0x82681788;
    const auto before=ctx;const auto saved=memory;const auto csr=_mm_getcsr();SetLastError(0x11223344);
    if(!check(!DeferQueuedTextureHeaderFree(ctx,base) && !std::memcmp(&before,&ctx,sizeof(ctx)) && memory==saved && _mm_getcsr()==csr && GetLastError()==0x11223344,
        "deferred cleanup itself remains untouched, with context/memory/FP/error unchanged"))return false;
    TextureOwners ownership;constexpr uint32_t object=0x15000,backing=0x17000,resource=0x16000,other_header=0x18000;
    const auto prepare=[&](uint32_t h=header){PPC_STORE_U32(object,0x82055A3C);PPC_STORE_U32(object+28,h);PPC_STORE_U32(object+32,backing);PPC_STORE_U32(object+36,resource);PPC_STORE_U32(h,3);};
    prepare();auto token=ownership.Register(base,object);const auto header_before=PPC_LOAD_U32(header);
    if(!check(token && ownership.Retire(base,header) && !PPC_LOAD_U32(object+28) && !PPC_LOAD_U32(object+32) &&
        PPC_LOAD_U32(object+36)==resource && PPC_LOAD_U32(header)==header_before,"completed queue retirement clears retained wrapper references, preserving resource/header storage"))return false;
    ownership.End(object,token);
    prepare();token=ownership.Register(base,object);if(!check(ownership.Begin(object)==token,"destructor captures exact owner incarnation"))return false;
    if(!check(!ownership.Retire(base,header) && PPC_LOAD_U32(object+28)==header && ownership.Retired(object),"concurrent retirement marks active destructor without writing through it"))return false;
    if(!check(ownership.Transfer(base,object,false) && !PPC_LOAD_U32(object+28) && !PPC_LOAD_U32(object+32),"destructor consumes already-completed retirement before its own frees"))return false;
    ownership.End(object,token);
    prepare();token=ownership.Register(base,object);ownership.Begin(object);
    if(!check(ownership.Transfer(base,object,true) && !PPC_LOAD_U32(object+28) && !PPC_LOAD_U32(object+32),"pending queue gets sole backing/header ownership"))return false;
    ownership.End(object,token);
    prepare();const auto old_token=ownership.Register(base,object);prepare(other_header);const auto new_token=ownership.Register(base,object);
    ownership.End(object,old_token);
    if(!check(new_token!=old_token && ownership.Retire(base,other_header) && !PPC_LOAD_U32(object+28),"old destructor completion cannot unregister a reused owner address"))return false;
    ownership.End(object,new_token);
    prepare();token=ownership.Register(base,object);PPC_STORE_U32(object+28,other_header);
    if(!check(!ownership.Retire(base,header) && PPC_LOAD_U32(object+28)==other_header && PPC_LOAD_U32(object+32)==backing,"changed header ownership is not overwritten"))return false;
    ownership.End(object,token);
    prepare();PPC_STORE_U32(object,0x12345678);
    if(!check(!ownership.Register(base,object),"foreign wrapper class is not tracked"))return false;
    prepare();PPC_STORE_U32(type+56,0x826811B8);PPC_STORE_U16(manager+566,0);
    TrackNativeTextureData(object,base);
    PPCContext consumer{};consumer.r3.u64=header;consumer.r28.u64=header;consumer.r23.u64=manager;consumer.lr=0x82681788;
    const auto consumer_before=consumer;const auto storage_before=memory;
    SetLastError(0x55667788);const auto consumer_csr=_mm_getcsr();
    RetireNativeTextureHeader(consumer,base);
    auto storage_expected=storage_before;std::memset(storage_expected.data()+object+28,0,8);
    if(!check(memory==storage_expected && !std::memcmp(&consumer,&consumer_before,sizeof(consumer)) &&
        GetLastError()==0x55667788 && _mm_getcsr()==consumer_csr,
        "actual post-dequeue callback retires owner despite empty queue; only owned reference fields change"))return false;
    texture_owners.End(object,texture_owners.Begin(object));
    REXLOG_INFO("Native texture retirement checks passed: exact pending queue lookup, used extent, overflow/capacity/type rejection and ordinary/deferred free-site isolation; owner generations, already-retired and pending ownership transfer; live original fenced frees remain acceptance");return true;
}
