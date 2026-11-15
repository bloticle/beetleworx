#include "native_behavior_checkpoint.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/kernel/kernel_state.h>
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <array>
#include <cstring>
#include <stdexcept>
#include <set>

PPC_EXTERN_FUNC(__imp__sub_823B57B8); // graph.getInternalState(output)
PPC_EXTERN_FUNC(__imp__sub_823AE398); // nondeleting state destructor
PPC_EXTERN_FUNC(__imp__sub_823B6190); // graph.deactivate(context)
PPC_EXTERN_FUNC(__imp__sub_823B6428); // inactive graph.setInternalState(context,state)
namespace em2 {namespace {
void Need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
bool Readable(uint8_t* base,uint32_t at,size_t size){return base && at>=4096 && size && uint64_t(at)+size<=0x100000000ull && GuestReadBatch::Readable(base+at,size);}
struct Calls {
    PPCContext& ctx;PPCContext saved;uint8_t* base;unsigned csr=_mm_getcsr();DWORD error=GetLastError();uint32_t low=0;
    std::array<uint8_t,32768> bytes{};
    Calls(PPCContext& c,uint8_t* b):ctx(c),saved(c),base(b){
        Need(c.r1.u32>=65536 && Readable(base,c.r1.u32-32768,bytes.size()),"Native behavior checkpoint requires initialized owner stack");
        low=c.r1.u32-32768;std::memcpy(bytes.data(),base+low,bytes.size());
    }
    ~Calls(){std::memcpy(base+low,bytes.data(),bytes.size());ctx=saved;_mm_setcsr(csr);SetLastError(error);}
    void Reset(){ctx=saved;ctx.r1.u64=low+16384;}
};
struct Graph {
    uint32_t animation=0,definition=0,graph=0,character=0,context=0,state=0;
    bool active=false;
};
void CheckGraph(uint8_t* base,const Graph& g){
    Need(Readable(base,g.animation,212) && PPC_LOAD_U32(g.animation)==0x820B34F0 && PPC_LOAD_U32(g.animation+8)==0x820B3360 &&
        PPC_LOAD_U32(g.animation+48)==g.definition && PPC_LOAD_U32(g.animation+192)==g.graph && PPC_LOAD_U32(g.animation+196)==g.character && PPC_LOAD_U32(g.animation+208)==g.context,
        "Native behavior component/graph/character/context ownership changed");
    if(!g.graph){Need(!g.active && !g.state,"Null native behavior graph has captured state");return;}
    Need(Readable(base,g.graph,224) && PPC_LOAD_U32(g.graph)==0x8201E16C && Readable(base,g.character,64) && PPC_LOAD_U32(g.character+60)==g.graph &&
        Readable(base,g.context,40) && PPC_LOAD_U32(g.context)==g.character && !PPC_LOAD_U32(g.context+4),"Native behavior graph/context differs from audited completed-boundary layout");
    Need(bool(PPC_LOAD_U8(g.graph+218))==g.active,"Native behavior graph activation changed; lifecycle restoration required");
}
unsigned CheckActiveNodes(uint8_t* base,const Graph& g){
    const auto header=PPC_LOAD_U32(g.graph+100);Need(Readable(base,header,12),"Native behavior active-node array unavailable");
    const auto data=PPC_LOAD_U32(header),count=PPC_LOAD_U32(header+4),capacity=PPC_LOAD_U32(header+8)&0x3FFFFFFF;
    Need(count<=capacity && capacity<=8192 && (!count || Readable(base,data,size_t(count)*4)),"Native behavior active-node extent exceeds checkpoint bound");
    for(unsigned i=0;i<count;++i){const auto info=PPC_LOAD_U32(data+i*4);Need(Readable(base,info,40),"Native behavior runtime node info unavailable");
        const auto source=PPC_LOAD_U32(info),node=PPC_LOAD_U32(info+4);Need(Readable(base,source,48),"Native behavior source node unavailable");
        // getInternalState deliberately skips source-node types48..63 before
        // dereferencing their runtime node. They may have no runtime clone.
        const auto type=PPC_LOAD_U8(source+39);if(type<48 || type>=64)Need(Readable(base,node,48),"Native behavior active runtime node unavailable");
    }return count;
}
unsigned CheckState(uint8_t* base,const Graph& g){
    Need(g.state && Readable(base,g.state,24) && PPC_LOAD_U32(g.state)==0x8201C308,"Native retained behavior state container unavailable");
    const auto data=PPC_LOAD_U32(g.state+8),count=PPC_LOAD_U32(g.state+12),capacity=PPC_LOAD_U32(g.state+16)&0x3FFFFFFF;
    Need(count<=capacity && capacity<=16384 && (!count || Readable(base,data,size_t(count)*4)),"Native retained behavior node state extent invalid");
    std::set<uint16_t> ids;
    for(unsigned i=0;i<count;++i){const auto info=PPC_LOAD_U32(data+i*4);Need(Readable(base,info,24) && PPC_LOAD_U32(info)==0x8201BF98,"Native behavior node state class differs");
        Need(ids.insert(PPC_LOAD_U16(info+20)).second,"Native retained behavior state has duplicate node IDs");
        const auto state=PPC_LOAD_U32(info+16);Need(!state || Readable(base,state,8),"Native node internal state unavailable");
    }return count;
}
uint32_t EmptyState(uint8_t* base){
    auto* kernel=rex::kernel::kernel_state();Need(kernel && kernel->memory(),"Native behavior scratch allocator unavailable");
    const auto at=kernel->memory()->SystemHeapAlloc(32,16);Need(at && Readable(base,at,32),"Native behavior state header allocation failed");
    // Header only is SystemHeap-owned. All retained members are constructed and
    // destroyed by original Havok routines. Never run its deleting destructor.
    std::memset(base+at,0,32);PPC_STORE_U32(at,0x8201C308);PPC_STORE_U16(at+4,24);PPC_STORE_U16(at+6,1);PPC_STORE_U32(at+16,0x80000000);return at;
}
}
struct NativeBehaviorCheckpoint::Impl {
    NativeEntityOwnership ownership;std::vector<Graph> graphs;uint64_t epoch=0;DWORD thread=0;
};
NativeBehaviorCheckpoint::NativeBehaviorCheckpoint():impl_(std::make_unique<Impl>()){}
NativeBehaviorCheckpoint::~NativeBehaviorCheckpoint(){for(const auto& g:impl_->graphs)if(g.state){REXLOG_ERROR("Native behavior checkpoint requires explicit owner-thread Release");break;}}
bool NativeBehaviorCheckpoint::ValidateOwnership(uint8_t* base,uint64_t epoch,std::string& error)const{
    GuestReadBatch reads;try{const auto& s=*impl_;Need(epoch && epoch==s.epoch && s.thread==GetCurrentThreadId(),"Native behavior checkpoint belongs to another epoch/thread");
        NativeEntityOwnership now;const char* why=nullptr;uint32_t where=0;Need(ReadNativeEntityOwnership(base,now,why,where) && now==s.ownership,"Native behavior entity/component inventory changed");
        for(const auto& g:s.graphs){CheckGraph(base,g);if(g.state)CheckState(base,g);}
        error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool NativeBehaviorCheckpoint::Capture(PPCContext& ctx,uint8_t* base,const NativeEntityOwnership& ownership,uint64_t epoch,std::string& error){
    GuestReadBatch reads;try{auto& s=*impl_;Need(!s.epoch && epoch,"Native behavior checkpoint requires an empty/nonzero scope");
        s.ownership=ownership;s.epoch=epoch;s.thread=GetCurrentThreadId();Calls calls(ctx,base);std::set<uint32_t> unique;unsigned nodes=0;
        for(const auto& e:ownership.entities)for(const auto& c:e.components){const auto object=c[0]-8;
            if(c[1]!=0x820B3360)continue;Need(Readable(base,object,212) && PPC_LOAD_U32(object)==0x820B34F0,"Native behavior component class differs");
            Graph g;g.animation=object;g.definition=PPC_LOAD_U32(object+48);g.graph=PPC_LOAD_U32(object+192);g.character=PPC_LOAD_U32(object+196);g.context=PPC_LOAD_U32(object+208);
            Need(!g.graph || Readable(base,g.graph,224),"Native behavior graph header unavailable");g.active=g.graph && PPC_LOAD_U8(g.graph+218);CheckGraph(base,g);
            Need(!g.graph || unique.insert(g.graph).second,"Native behavior graph shared across component owners");s.graphs.push_back(g);auto& retained=s.graphs.back();
            if(!g.active)continue;CheckActiveNodes(base,g);retained.state=EmptyState(base);
            calls.Reset();ctx.r3.u64=g.graph;ctx.r4.u64=retained.state;__imp__sub_823B57B8(ctx,base);nodes+=CheckState(base,retained);
        }
        Need(ValidateOwnership(base,epoch,error),"Native behavior ownership changed during native capture");
        REXLOG_INFO("Native behavior state captured: {} animation graph owners, {} retained node states; original getInternalState and allocator-aware cleanup, no portable state or full semantic comparison",s.graphs.size(),nodes);
        error.clear();return true;
    }catch(const std::exception& e){error=e.what();Release(ctx,base);return false;}
}
bool NativeBehaviorCheckpoint::Restore(PPCContext& ctx,uint8_t* base,uint64_t epoch,std::string& error)const{
    GuestReadBatch reads;try{Need(ValidateOwnership(base,epoch,error),"Native behavior restore ownership rejected before calls");Calls calls(ctx,base);
        // All graph owners are checked before the first native mutation. These
        // calls can still partially mutate/fail and fire activation events. The
        // private replay owner stops on failure, never resumes a partial world.
        for(const auto& g:impl_->graphs)if(g.state){
            calls.Reset();ctx.r3.u64=g.graph;ctx.r4.u64=g.context;__imp__sub_823B6190(ctx,base);
            Need(!PPC_LOAD_U8(g.graph+218) && !PPC_LOAD_U32(g.graph+100) && !PPC_LOAD_U32(g.context+4),"Native behavior graph did not deactivate cleanly");
            calls.Reset();ctx.r3.u64=g.graph;ctx.r4.u64=g.context;ctx.r5.u64=g.state;__imp__sub_823B6428(ctx,base);
            CheckGraph(base,g);CheckActiveNodes(base,g);
        }
        error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool NativeBehaviorCheckpoint::Release(PPCContext& ctx,uint8_t* base){
    auto& s=*impl_;if(s.thread && s.thread!=GetCurrentThreadId())return false;
    bool retained=false;for(const auto& g:s.graphs)retained|=g.state!=0;
    if(!retained){s.graphs.clear();s.epoch=0;return true;}
    try{Calls calls(ctx,base);for(auto& g:s.graphs)if(g.state){calls.Reset();ctx.r3.u64=g.state;__imp__sub_823AE398(ctx,base);rex::kernel::kernel_state()->memory()->SystemHeapFree(g.state);g.state=0;}
        s.graphs.clear();s.epoch=0;return true;
    }catch(const std::exception& e){REXLOG_ERROR("Native behavior checkpoint release failed: {}",e.what());return false;}
}
}
