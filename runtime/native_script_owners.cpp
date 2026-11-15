#include "native_script_owners.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <array>
#include <unordered_set>
#include <unordered_map>
#include <stdexcept>
#include <cstring>
PPC_EXTERN_FUNC(__imp__sub_826D26B8); // Native LuaObject assignment/list rebind.
namespace em2 {
namespace {
bool Readable(uint8_t* base,uint32_t at,size_t size){return base && at>=4096 && size && uint64_t(at)+size<=0x100000000ull && GuestReadBatch::Readable(base+at,size);}
void Need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
struct Environment {unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(csr);SetLastError(error);}};
}
bool ReadNativeScriptOwners(uint8_t* base,uint32_t state,NativeScriptOwners& out,std::string& error)try{
    Environment environment;GuestReadBatch inspection;error.clear();
    Need(Readable(base,state,128),"Native script owner VM unavailable");
    NativeScriptOwners sample;sample.global=PPC_LOAD_U32(state+8);sample.thread=GetCurrentThreadId();
    Need(Readable(base,sample.global,716),"Native script owner global unavailable");
    sample.head=PPC_LOAD_U32(sample.global+712);uint32_t previous=0;std::unordered_set<uint32_t> seen;
    for(auto node=sample.head;node;){
        Need(sample.owners.size()<65536 && !(node&3) && Readable(base,node,20) && seen.insert(node).second,"Native script owner list malformed or exceeds bound");
        NativeScriptOwner owner{node,PPC_LOAD_U32(node),PPC_LOAD_U32(node+4),PPC_LOAD_U32(node+8),PPC_LOAD_U32(node+12),PPC_LOAD_U32(node+16)};
        Need(Readable(base,owner.state,128) && PPC_LOAD_U32(owner.state+8)==sample.global && owner.previous==previous,"Native script owner links/VM mismatch");
        sample.owners.push_back(owner);previous=node;node=owner.next;
    }
    Need(PPC_LOAD_U32(state+8)==sample.global && PPC_LOAD_U32(sample.global+712)==sample.head,"Native script owners changed during inspection");
    out=std::move(sample);return true;
}catch(const std::exception& ex){error=ex.what();return false;}
bool RebindNativeScriptOwners(PPCContext& ctx,uint8_t* base,uint32_t state,const NativeScriptOwners& expected,
    std::span<const NativeScriptOwnerReplacement> replacements,std::string& error)try{
    Environment environment;GuestReadBatch inspection;error.clear();NativeScriptOwners current;
    Need(expected.thread==GetCurrentThreadId() && ReadNativeScriptOwners(base,state,current,error) && current==expected,"Native script holders changed or belong to another VM/owner thread");
    Need(!replacements.empty() && replacements.size()<=65536,"Native script replacement batch is empty or exceeds bounds");
    const auto storage=PPC_LOAD_U32(state+48),bottom=PPC_LOAD_U32(state+40),top=PPC_LOAD_U32(state+36);
    Need(storage<=bottom && bottom<=top && !(uint32_t(top-storage)%8),"Native script stack bounds invalid");
    std::unordered_map<uint32_t,uint32_t> states;states.reserve(current.owners.size());
    for(const auto& owner:current.owners)states.emplace(owner.address,owner.state);
    std::unordered_set<uint32_t> unique;std::vector<std::array<uint32_t,4>> prepared;prepared.reserve(replacements.size());
    for(const auto& replacement:replacements){
        const auto found=states.find(replacement.owner);
        Need(found!=states.end() && unique.insert(replacement.owner).second,"Native script owner missing or duplicated");
        const auto slot=replacement.rooted_slot;
        Need(slot>=bottom && (slot-storage)%8==0 && uint64_t(slot)+8<=top && Readable(base,slot,8),"Replacement is not a rooted value in this VM stack frame");
        prepared.push_back({replacement.owner,found->second,PPC_LOAD_U32(slot),PPC_LOAD_U32(slot+4)});
    }
    // Native assignment is a nonallocating intrusive-list splice. The source
    // triple is borrowed from a rooted TValue; preserve the destination thread
    // rather than assigning every native holder to the main lua_State.
    Need(ctx.r1.u32>=32768 && Readable(base,ctx.r1.u32-32768,32768),"Native script rebind owner stack unavailable");
    for(const auto& item:prepared)Need(uint64_t(item[0])+20<=ctx.r1.u32-32768 || item[0]>=ctx.r1.u32,"Native script owner overlaps borrowed PPC call stack");
    struct Scope {
        PPCContext& ctx;PPCContext saved;uint8_t* memory;std::array<uint8_t,32768> bytes;
        Scope(PPCContext& c,uint8_t* p):ctx(c),saved(c),memory(p){std::memcpy(bytes.data(),p,bytes.size());}
        ~Scope(){std::memcpy(memory,bytes.data(),bytes.size());ctx=saved;}
    }scope(ctx,base+ctx.r1.u32-32768);
    const auto stack=scope.saved.r1.u32-16384,source=stack+1024;
    for(const auto& item:prepared){
        PPC_STORE_U32(source,item[1]);PPC_STORE_U32(source+4,item[2]);PPC_STORE_U32(source+8,item[3]);
        ctx=scope.saved;ctx.r1.u64=stack;ctx.r3.u64=item[0];ctx.r4.u64=source;__imp__sub_826D26B8(ctx,base);
    }
    return true;
}catch(const std::exception& ex){error=ex.what();return false;}
}
