#include "online_room_init_trace.h"
#include "guest_read_batch.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <string>
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(std::string,em2_room_probe_role);
REXCVAR_DEFINE_BOOL(em2_trace_room_initialization,false,"Debug","Private Room native module registration and dependency-order observations");
PPC_EXTERN_FUNC(__imp__sub_826566D8);
PPC_EXTERN_FUNC(__imp__sub_82656FB0);
PPC_EXTERN_FUNC(__imp__sub_826570B8);
namespace {
struct Preserve {
    PPCContext& ctx;std::array<uint8_t,sizeof(PPCContext)> bytes;unsigned csr=_mm_getcsr();DWORD error=GetLastError();
    explicit Preserve(PPCContext& value):ctx(value){std::memcpy(bytes.data(),&ctx,sizeof(ctx));}
    ~Preserve(){std::memcpy(&ctx,bytes.data(),sizeof(ctx));_mm_setcsr(csr);SetLastError(error);}
};
thread_local std::array<unsigned,4> counts{};
bool Readable(uint32_t address,size_t size,uint8_t* base){
    return base && address>=4096 && size && uint64_t(address)+size<=0x100000000ull && em2::GuestReadBatch::Readable(base+address,size);
}
uint32_t Word(uint32_t address,uint8_t* base){return Readable(address,4,base)?PPC_LOAD_U32(address):0;}
unsigned Byte(uint32_t address,uint8_t* base){return Readable(address,1,base)?PPC_LOAD_U8(address):0;}
std::string Name(uint32_t address,uint8_t* base){
    if(!address)return "<null>";
    std::string value;
    for(unsigned i=0;i<160;++i){
        if(uint64_t(address)+i>UINT32_MAX || !Readable(address+i,1,base))return value+"<unreadable>";
        const auto c=PPC_LOAD_U8(address+i);if(!c)return value;
        if(c<32 || c>=127)return value+"<nontext>";value+=char(c);
    }
    return value+"<bounded>";
}
template<class Action>void Observe(PPCContext& ctx,unsigned kind,Action action) noexcept{
    Preserve preserve(ctx);
    try{
        if(!REXCVAR_GET(em2_trace_room_initialization) || !REXCVAR_GET(em2_private_profile) || REXCVAR_GET(em2_room_probe_role).empty() || counts[kind]>=96)return;
        ++counts[kind];action();
    }catch(...){/* Diagnostics never change native return/throw behavior. */}
}
void Summary(const char* phase,uint8_t* base){
    const auto count=Byte(0x833D03B8,base),initialized=Byte(0x833D03B9,base);
    std::string order;
    for(unsigned i=0;i<std::min(count,32u);++i){if(i)order+=',';order+=std::to_string(Byte(0x833D0098+i,base));}
    REXLOG_INFO("Private Room native init {}: thread {} count {} initialized {} ready {} allocator-owned {} registry {:08X} name {:08X} order [{}]{}",
        phase,GetCurrentThreadId(),count,initialized,Byte(0x833D03BA,base),Byte(0x833D03BB,base),Word(0x833DBAF4,base),Word(0x833E2AF4,base),order,count>32?" bounded":"");
}
void Modules(uint8_t* base){
    const auto count=Byte(0x833D03B8,base);
    for(unsigned i=0;i<std::min(count,32u);++i){
        const uint32_t record=0x833D00B8+i*24;
        const auto name=Word(record,base),dependencies=Word(record+4,base);
        REXLOG_INFO("Private Room native init module {}: name {:08X} '{}' deps {:08X} '{}' init {:08X} term {:08X} state {:08X} flags {:02X}/{:02X}",
            i,name,Name(name,base),dependencies,Name(dependencies,base),Word(record+8,base),Word(record+12,base),Word(record+16,base),Byte(record+20,base),Byte(record+22,base));
    }
}
}
PPC_FUNC_IMPL(sub_826566D8){
    const std::array<uint32_t,4> arguments{ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32};const auto caller=uint32_t(ctx.lr);
    Observe(ctx,0,[&]{
        REXLOG_INFO("Private Room native init register BEFORE: caller {:08X} name {:08X} '{}' init {:08X} term {:08X} deps {:08X} '{}'",
            caller,arguments[0],Name(arguments[0],base),arguments[1],arguments[2],arguments[3],Name(arguments[3],base));Summary("register-before",base);
    });
    __imp__sub_826566D8(ctx,base);
    Observe(ctx,0,[&]{Summary("register-after",base);});
}
PPC_FUNC_IMPL(sub_82656FB0){
    Observe(ctx,1,[&]{Summary("sort-before",base);Modules(base);});
    __imp__sub_82656FB0(ctx,base);
    Observe(ctx,1,[&]{REXLOG_INFO("Private Room native init sort ORIGINAL result {}",ctx.r3.u32);Summary("sort-after",base);});
}
PPC_FUNC_IMPL(sub_826570B8){
    Observe(ctx,2,[&]{Summary("dispatch-before",base);});
    __imp__sub_826570B8(ctx,base);
    Observe(ctx,2,[&]{Summary("dispatch-after",base);});
}
void em2::net::TraceNativeRoomComponentRegistry(PPCContext& ctx,uint8_t* base,const char* phase) noexcept{
    Observe(ctx,3,[&]{Summary(phase,base);const auto registry=Word(0x833DBAF4,base);
        REXLOG_INFO("Private Room native component registry {}: registry {:08X} readable {} bucket-count {} name {:08X}",
            phase,registry,Readable(registry,36,base),registry?Word(registry+20,base):0,Word(0x833E2AF4,base));});
}
