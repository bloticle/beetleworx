#include "native_deferred_entity_checkpoint.h"
#include "native_entity_lifetime.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/kernel/kernel_state.h>
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <array>
#include <vector>
#include <cstring>
#include <stdexcept>

PPC_EXTERN_FUNC(__imp__sub_827475D8); // Resize original pointer/count/capacity array.
PPC_EXTERN_FUNC(__imp__sub_8250E198); // Original NiPointer assignment.
PPC_EXTERN_FUNC(__imp__sub_826547C8); // Original array allocator's free.
namespace em2 {
namespace {
void Need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
bool Readable(uint8_t* base,uint32_t address,size_t bytes){return base && address>=4096 && bytes && uint64_t(address)+bytes<=0x100000000ull && GuestReadBatch::Readable(base+address,bytes);}
struct Calls {
    PPCContext& ctx;PPCContext saved;uint8_t* base;unsigned csr=_mm_getcsr();DWORD error=GetLastError();uint32_t low=0;
    std::array<uint8_t,32768> bytes{};
    Calls(PPCContext& c,uint8_t* b):ctx(c),saved(c),base(b){
        Need(c.r1.u32>=65536 && Readable(base,c.r1.u32-32768,bytes.size()),"Deferred queue requires an initialized owner stack");
        low=c.r1.u32-32768;std::memcpy(bytes.data(),base+low,bytes.size());
    }
    ~Calls(){std::memcpy(base+low,bytes.data(),bytes.size());ctx=saved;_mm_setcsr(csr);SetLastError(error);}
    void Reset(){ctx=saved;ctx.r1.u64=low+16384;}
    void Resize(uint32_t header,unsigned count){Reset();ctx.r3.u64=header;ctx.r4.u64=count;__imp__sub_827475D8(ctx,base);}
    void Assign(uint32_t target,uint32_t source){Reset();ctx.r3.u64=target;ctx.r4.u64=source;__imp__sub_8250E198(ctx,base);}
};
struct Identity {
    uint32_t entity=0,vtable=0;std::array<uint32_t,4> guid{};
    bool operator==(const Identity&)const=default;
};
Identity Identify(uint8_t* base,uint32_t entity){
    Need(!(entity&3) && entity>=8 && Readable(base,entity-8,104),"Deferred entity interface is unavailable");
    Identity id;id.entity=entity;id.vtable=PPC_LOAD_U32(entity);
    Need(id.vtable>=0x82000000 && id.vtable<0x83400000 && !(id.vtable&3) && PPC_LOAD_U32(entity-4),"Deferred entity interface/reference is invalid");
    for(unsigned i=0;i<4;++i)id.guid[i]=PPC_LOAD_U32(entity+4+i*4);
    return id;
}
std::vector<Identity> Inventory(uint8_t* base,uint32_t header){
    Need(Readable(base,header,12),"Deferred queue header unavailable");
    const auto array=PPC_LOAD_U32(header),count=PPC_LOAD_U32(header+4),capacity=PPC_LOAD_U32(header+8);
    Need(count<=65536 && capacity<=1048576 && count<=capacity && (!count || Readable(base,array,size_t(count)*4)),"Deferred queue bounds invalid");
    std::vector<Identity> result;result.reserve(count);
    for(unsigned i=0;i<count;++i)result.push_back(Identify(base,PPC_LOAD_U32(array+i*4)));
    return result;
}
uint32_t AllocateScratch(){auto* kernel=rex::kernel::kernel_state();Need(kernel && kernel->memory(),"Deferred queue allocation context unavailable");const auto at=kernel->memory()->SystemHeapAlloc(64,16);Need(at!=0,"Deferred queue scratch allocation failed");return at;}
void FreeScratch(uint32_t at){rex::kernel::kernel_state()->memory()->SystemHeapFree(at);}
// The header/scratch is SystemHeap memory; the array itself always belongs to
// the original game's allocator. Never free either through the other allocator.
void Clear(Calls& calls,uint32_t header){
    auto* base=calls.base;
    calls.Resize(header,0);const auto array=PPC_LOAD_U32(header);
    if(array){calls.Reset();calls.ctx.r3.u64=array;__imp__sub_826547C8(calls.ctx,calls.base);}
    std::memset(calls.base+header,0,12);
}
void Fill(Calls& calls,uint32_t header,const std::vector<Identity>& items){
    auto* base=calls.base;
    calls.Resize(header,unsigned(items.size()));
    const auto array=PPC_LOAD_U32(header);
    Need(items.empty() || Readable(calls.base,array,items.size()*4),"Deferred queue native array allocation failed");
    for(unsigned i=0;i<items.size();++i){PPC_STORE_U32(header+16,items[i].entity);calls.Assign(array+i*4,header+16);}
}
struct Temporary {
    Calls& calls;uint32_t at;
    explicit Temporary(Calls& c):calls(c),at(AllocateScratch()){std::memset(calls.base+at,0,64);}
    ~Temporary(){Clear(calls,at);FreeScratch(at);}
};
}
struct NativeDeferredEntityCheckpoint::Impl {
    uint32_t thread=0,registry=0,owner=0,world=0,header=0;uint64_t epoch=0,revision=0;
    std::vector<Identity> entries;
};
NativeDeferredEntityCheckpoint::NativeDeferredEntityCheckpoint():impl_(std::make_unique<Impl>()){}
NativeDeferredEntityCheckpoint::~NativeDeferredEntityCheckpoint(){if(impl_->header)REXLOG_ERROR("Deferred entity checkpoint requires explicit owner-thread Release; retained native references not freed on an arbitrary thread");}
unsigned NativeDeferredEntityCheckpoint::size()const{return unsigned(impl_->entries.size());}
bool NativeDeferredEntityCheckpoint::Capture(PPCContext& ctx,uint8_t* base,uint32_t registry,uint64_t epoch,std::string& error){
    error.clear();try{
        auto& s=*impl_;Need(!s.header && epoch && Readable(base,registry,232),"Deferred checkpoint capture scope unavailable or already captured");
        Calls calls(ctx,base);uint64_t revision=0;
        Need(SampleNativeEntityLifetimeRevision(revision),"Entity lifecycle mutation in progress");
        Need(PPC_LOAD_U32(registry+4)==0x827B58D0 && PPC_LOAD_U32(registry+8)==0x82A69948,"Deferred queue registry layout differs from audited image");
        const auto owner=PPC_LOAD_U32(registry+152),world=PPC_LOAD_U32(registry+228);
        Need(owner && world,"Deferred queue native owners unavailable");
        auto entries=Inventory(base,registry+192);Temporary retained(calls);Fill(calls,retained.at,entries);
        uint64_t completed=0;Need(SampleNativeEntityLifetimeRevision(completed) && completed==revision && Inventory(base,registry+192)==entries &&
            PPC_LOAD_U32(registry+152)==owner && PPC_LOAD_U32(registry+228)==world,"Deferred queue changed during capture");
        const auto header=AllocateScratch();std::memset(base+header,0,64);
        std::memcpy(base+header,base+retained.at,12);std::memset(base+retained.at,0,12);
        s.thread=GetCurrentThreadId();s.registry=registry;s.owner=owner;s.world=world;s.epoch=epoch;s.revision=revision;s.header=header;s.entries=std::move(entries);return true;
    }catch(const std::exception& ex){error=ex.what();return false;}
}
bool NativeDeferredEntityCheckpoint::Restore(PPCContext& ctx,uint8_t* base,uint32_t registry,uint64_t epoch,std::string& error)const{
    error.clear();try{
        const auto& s=*impl_;Need(s.header && s.thread==GetCurrentThreadId() && s.registry==registry && s.epoch==epoch,"Deferred restore owner/registry/epoch mismatch");
        Calls calls(ctx,base);uint64_t revision=0;
        Need(SampleNativeEntityLifetimeRevision(revision) && revision==s.revision && Readable(base,registry,232) &&
            PPC_LOAD_U32(registry+152)==s.owner && PPC_LOAD_U32(registry+228)==s.world,"Deferred restore lifetime or native owner changed");
        Inventory(base,registry+192); // Validate the old queue before swapping/releasing it.
        for(const auto& entry:s.entries)Need(Identify(base,entry.entity)==entry,"Deferred entity identity changed");
        Temporary replacement(calls);Fill(calls,replacement.at,s.entries);
        Need(SampleNativeEntityLifetimeRevision(revision) && revision==s.revision,"Entity lifecycle changed while preparing deferred restore");
        std::array<uint8_t,12> old{};std::memcpy(old.data(),base+registry+192,12);
        std::memcpy(base+registry+192,base+replacement.at,12);std::memcpy(base+replacement.at,old.data(),12);
        // Temporary releases the old queue through the original NiPointer and
        // allocator routines. The complete world owner supplies its transaction.
        return true;
    }catch(const std::exception& ex){error=ex.what();return false;}
}
bool NativeDeferredEntityCheckpoint::Matches(uint8_t* base,uint32_t registry,uint64_t epoch,std::string& error)const{
    const auto previous=GetLastError();const auto csr=_mm_getcsr();
    struct Host{DWORD error;unsigned csr;~Host(){SetLastError(error);_mm_setcsr(csr);}}host{previous,csr};GuestReadBatch reads;
    try{const auto& s=*impl_;uint64_t revision=0;
        Need(s.header && s.thread==GetCurrentThreadId() && s.registry==registry && s.epoch==epoch &&
            SampleNativeEntityLifetimeRevision(revision) && revision==s.revision && Readable(base,registry,232) &&
            PPC_LOAD_U32(registry+152)==s.owner && PPC_LOAD_U32(registry+228)==s.world && Inventory(base,registry+192)==s.entries,
            "Native deferred queue differs from captured scope/entries");
        error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool NativeDeferredEntityCheckpoint::Release(PPCContext& ctx,uint8_t* base){
    auto& s=*impl_;if(!s.header)return true;if(s.thread!=GetCurrentThreadId())return false;
    try{Calls calls(ctx,base);Clear(calls,s.header);FreeScratch(s.header);s=Impl{};return true;}catch(...){return false;}
}
bool TestNativeDeferredEntityCheckpoint(PPCContext& ctx,uint8_t* base){
    try{
        Calls calls(ctx,base);auto* memory=rex::kernel::kernel_state()->memory();
        const auto scratch=memory->SystemHeapAlloc(1024,16);Need(Readable(base,scratch,1024),"Deferred fixture scratch unavailable");
        struct Free {rex::memory::Memory* memory;uint32_t at;~Free(){memory->SystemHeapFree(at);}}free{memory,scratch};
        std::memset(base+scratch,0,1024);const auto registry=scratch,first=scratch+520,second=scratch+648;
        PPC_STORE_U32(registry+4,0x827B58D0);PPC_STORE_U32(registry+8,0x82A69948);PPC_STORE_U32(registry+152,1);PPC_STORE_U32(registry+228,2);
        for(auto entity:{first,second}){PPC_STORE_U32(entity,0x8208D908);PPC_STORE_U32(entity-4,1);PPC_STORE_U32(entity+4,entity);}
        struct Cleanup {Calls& calls;uint32_t header;~Cleanup(){Clear(calls,header);}}cleanup{calls,registry+192};
        std::vector<Identity> entries{Identify(base,first),Identify(base,second)};Fill(calls,registry+192,entries);
        NativeDeferredEntityCheckpoint checkpoint;
        struct Release {NativeDeferredEntityCheckpoint& value;Calls& calls;~Release(){value.Release(calls.ctx,calls.base);}}release{checkpoint,calls};
        std::string error;if(!checkpoint.Capture(ctx,base,registry,1,error))throw std::runtime_error(error);
        Need(PPC_LOAD_U32(first-4)==3 && PPC_LOAD_U32(second-4)==3,"Capture did not retain original native references");
        calls.Resize(registry+192,0);Fill(calls,registry+192,{entries[1]});
        Need(!checkpoint.Restore(ctx,base,registry,2,error) && Inventory(base,registry+192)==std::vector<Identity>{entries[1]},"Wrong epoch changed the live queue");
        if(!checkpoint.Restore(ctx,base,registry,1,error))throw std::runtime_error(error);
        Need(Inventory(base,registry+192)==entries && PPC_LOAD_U32(first-4)==3 && PPC_LOAD_U32(second-4)==3,"Restored queue order/references differ");
        Need(checkpoint.Release(ctx,base),"Deferred checkpoint release failed");calls.Resize(registry+192,0);
        Need(PPC_LOAD_U32(first-4)==1 && PPC_LOAD_U32(second-4)==1,"Deferred checkpoint leaked native references");
        REXLOG_INFO("Native deferred queue checkpoint passed: original native arrays/refcounts, capture, changed count/order restore, epoch rejection and balanced release; allocated interfaces, not game actor resurrection");return true;
    }catch(const std::exception& ex){REXLOG_ERROR("Native deferred queue checkpoint failed: {}",ex.what());return false;}
}
}
