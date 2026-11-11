#include "online_uuid.h"
#include "allocation_trace.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <array>
#include <atomic>
#include <cstring>
#include <immintrin.h>
#include <map>
#include <mutex>
#include <windows.h>

REXCVAR_DEFINE_BOOL(em2_trace_online_uuid,false,"Debug","Trace native GUID creation callsites without changing random state or identity");
PPC_EXTERN_FUNC(__imp__sub_82533EB0);
namespace {
struct Record {
    uint32_t caller=0,output=0;
    std::array<uint32_t,4> id{};
    std::array<uint32_t,2> parents{};
    std::array<uint32_t,4> source{};
};
bool ReadRecord(uint8_t* base,uint32_t caller,uint32_t output,Record& out){
    if(!base || output<4096 || uint64_t(output)+16>0x100000000ull)return false;
    em2::GuestReadBatch inspection;
    if(!em2::GuestReadBatch::Readable(PPC_RAW_ADDR(output),16))return false;
    out.caller=caller;out.output=output;
    for(unsigned i=0;i<4;++i)out.id[i]=PPC_LOAD_U32(output+i*4);
    return true;
}
void ReadParents(uint8_t* base,uint32_t stack,Record& record){
    em2::GuestReadBatch inspection;
    for(auto& caller:record.parents){
        if(stack<4096 || (stack&15) || !em2::GuestReadBatch::Readable(PPC_RAW_ADDR(stack),4))return;
        const auto previous=PPC_LOAD_U32(stack);
        // The generated retail prologues save LR at oldSP-8 and store
        // oldSP in the new frame's backchain. Never scan guessed stack words.
        if(previous<=stack || uint64_t(previous)-stack>65536 || (previous&15) ||
           !em2::GuestReadBatch::Readable(PPC_RAW_ADDR(previous-8),8))return;
        const auto address=PPC_LOAD_U32(previous-8);
        if(address<0x82000000 || address>=0x83400000 || (address&3))return;
        caller=address;stack=previous;
    }
}
void TraceRecord(const Record& record){
    static std::mutex mutex;
    static uint64_t serial=0;
    static std::map<uint32_t,uint64_t> callsites;
    std::lock_guard lock(mutex);
    // Bound diagnostics even if a broken level keeps spawning entities. This
    // ordinal is provenance only and is never used as a gameplay identifier.
    if(serial>=65536)return;
    const auto ordinal=++callsites[record.caller];
    const auto level=em2::CurrentNativeLoadLevel();
    REXLOG_INFO("Native GUID create serial={} caller={:08X} ordinal={} thread={} output={:08X} id={:08X}{:08X}{:08X}{:08X} parents={:08X},{:08X} source={:08X}{:08X}{:08X}{:08X} level='{}'",
        ++serial,record.caller,ordinal,GetCurrentThreadId(),record.output,
        record.id[0],record.id[1],record.id[2],record.id[3],record.parents[0],record.parents[1],
        record.source[0],record.source[1],record.source[2],record.source[3],level.data());
}
using Sink=void(*)(const Record&);
void Invoke(PPCContext& ctx,uint8_t* base,PPCFunc* original,Sink sink){
    const auto caller=uint32_t(ctx.lr),output=ctx.r3.u32;
    original(ctx,base);
    // The CRT constructor's four random draws and every resulting guest
    // register/memory side effect have already happened. Diagnostics must not
    // disturb the returned context, guest FP status, or native error state.
    struct Restore {
        PPCContext& context;PPCContext saved;unsigned csr;DWORD error;
        explicit Restore(PPCContext& value):context(value),saved(value),csr(_mm_getcsr()),error(GetLastError()){}
        ~Restore(){context=saved;_mm_setcsr(csr);SetLastError(error);}
    } restore(ctx);
    // Diagnostic allocation/logging failure cannot change the game's result.
    // Keep the original call outside this catch so its behavior stays intact.
    try {
        Record record;
        if(ReadRecord(base,caller,output,record)){
            ReadParents(base,ctx.r1.u32,record);
            // 827A82F8 clones the serialized descriptor held in r29. Its
            // authored GUID at +4 is copied to remap-record+20 immediately
            // after this call; the new GUID goes to remap-record+4 before
            // component cloning. This source is stronger than allocator IDs.
            if(caller==0x827A8384 && uint64_t(ctx.r29.u32)+20<=0x100000000ull){
                Record source;if(ReadRecord(base,0,ctx.r29.u32+4,source))record.source=source.id;
            }
            sink(record);
        }
    }catch(...){}
}
thread_local Record test_record;
void TestSink(const Record& record){test_record=record;_mm_setcsr(0x1F80);SetLastError(998);}
void ThrowingSink(const Record&){_mm_setcsr(0x1F80);SetLastError(999);throw 1;}
void TestOriginal(PPCContext& ctx,uint8_t* base){
    const auto output=ctx.r3.u32;
    for(unsigned i=0;i<4;++i)PPC_STORE_U32(output+i*4,0x10203040u+i);
    ctx.r3.u64=1;ctx.r11.u64=0x9988776655443322ull;ctx.lr=0x82533F00;
    ctx.cr6.eq=1;
    _mm_setcsr(0x3F80);SetLastError(71);
}
}
PPC_FUNC_IMPL(sub_82533EB0){
    if(!REXCVAR_GET(em2_trace_online_uuid)){__imp__sub_82533EB0(ctx,base);return;}
    Invoke(ctx,base,__imp__sub_82533EB0,TraceRecord);
}
bool em2::net::TestOnlineUuidProvenance(){
    const auto saved_csr=_mm_getcsr();const auto saved_error=GetLastError();
    struct Restore{unsigned csr;DWORD error;~Restore(){_mm_setcsr(csr);SetLastError(error);}}restore{saved_csr,saved_error};
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,65536,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!base)return false;
    struct Free{uint8_t* memory;~Free(){VirtualFree(memory,0,MEM_RELEASE);}}free{base};
    PPCContext actual{},expected{};actual.r3.u32=8192;actual.lr=0x827A8384;actual.r1.u32=4096;actual.r29.u32=32768;expected=actual;
    PPC_STORE_U32(4096,12288);PPC_STORE_U32(12288-8,0x82507304);
    PPC_STORE_U32(12288,24576);PPC_STORE_U32(24576-8,0x827B9A18);
    for(unsigned i=0;i<4;++i)PPC_STORE_U32(32768+4+i*4,0x55667780u+i);
    TestOriginal(expected,base);
    Invoke(actual,base,TestOriginal,TestSink);
    bool ok=std::memcmp(&actual,&expected,sizeof(actual))==0 && _mm_getcsr()==0x3F80 && GetLastError()==71 &&
        test_record.caller==0x827A8384 && test_record.output==8192 &&
        test_record.parents==std::array<uint32_t,2>{0x82507304,0x827B9A18};
    for(unsigned i=0;i<4;++i)ok=ok && test_record.id[i]==0x10203040u+i && PPC_LOAD_U32(8192+i*4)==test_record.id[i] &&
        test_record.source[i]==0x55667780u+i && PPC_LOAD_U32(32768+4+i*4)==test_record.source[i];
    actual={};actual.r3.u32=8192;actual.lr=0x827A8384;expected=actual;
    TestOriginal(expected,base);Invoke(actual,base,TestOriginal,ThrowingSink);
    ok=ok && std::memcmp(&actual,&expected,sizeof(actual))==0 && _mm_getcsr()==0x3F80 && GetLastError()==71;
    Record invalid;
    ok=ok && !ReadRecord(base,1,0,invalid) && !ReadRecord(base,1,0xFFFFFFF8u,invalid) && !ReadRecord(nullptr,1,8192,invalid);
    PPC_STORE_U32(4096,4096);Record cycle;ReadParents(base,4096,cycle);
    ok=ok && cycle.parents==std::array<uint32_t,2>{};
    if(!ok)REXLOG_ERROR("Native GUID provenance altered constructor context/output or admitted an invalid read");
    return ok;
}
