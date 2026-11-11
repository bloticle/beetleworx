#include "video_blocks.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <vector>

PPC_EXTERN_FUNC(__imp__sub_83343AC8);
PPC_EXTERN_FUNC(sub_83343AC8);
PPC_EXTERN_FUNC(__imp__sub_833441A8);
PPC_EXTERN_FUNC(sub_833441A8);
PPC_EXTERN_FUNC(__imp__sub_83344528);
PPC_EXTERN_FUNC(sub_83344528);
PPC_EXTERN_FUNC(__imp__sub_8333C6C0);
PPC_EXTERN_FUNC(sub_8333C6C0);
REXCVAR_DEFINE_BOOL(em2_native_video_blocks,true,"CPU","Localize integer video block registers, retaining original arithmetic and guest memory accesses");
REXCVAR_DEFINE_BOOL(em2_verify_video_blocks,false,"Debug","Compare the first 16 real blocks per localized video kernel against the original translation");
REXCVAR_DEFINE_BOOL(em2_benchmark_video_blocks,false,"Debug","Benchmark video block kernels in the existing headless regression mode");
namespace {
std::atomic_uint verification_failures{};thread_local bool expected_verification_rejection=false;
bool ReadableGuestRange(uint8_t* base,uint32_t address,size_t bytes){
    if(!bytes)return true;
    if(uint64_t(address)+bytes>0x100000000ull)return false;
    auto begin=reinterpret_cast<uintptr_t>(PPC_RAW_ADDR(address));
    if(reinterpret_cast<uintptr_t>(PPC_RAW_ADDR(uint32_t(address+bytes-1)))!=begin+bytes-1)return false;
    const auto end=begin+bytes;
    while(begin<end){MEMORY_BASIC_INFORMATION region{};if(!VirtualQuery(reinterpret_cast<void*>(begin),&region,sizeof region)||region.State!=MEM_COMMIT||(region.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
        if(!(region.Protect&(PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return false;
        const auto next=reinterpret_cast<uintptr_t>(region.BaseAddress)+region.RegionSize;if(next<=begin)return false;begin=next;
    }return true;
}
}

bool em2::VerifyVideoBlock(PPCContext& ctx,uint8_t* base,PPCFunc* original,PPCFunc* localized,unsigned kernel){
    static std::array<std::atomic_uint,4> checks{};
    static thread_local bool active=false;
    if(active||kernel>=checks.size()||checks[kernel].load(std::memory_order_relaxed)>=16)return false;
    const auto entry_csr=_mm_getcsr();
    uint32_t symbol_capacity=0,symbol_output=0,verified_symbols=0;
    if(kernel==3){
        if(!ReadableGuestRange(base,ctx.r3.u32,52)||!ReadableGuestRange(base,ctx.r4.u32,12)||!ReadableGuestRange(base,ctx.r5.u32+384,4)){_mm_setcsr(entry_csr);return false;}
        symbol_capacity=PPC_LOAD_U32(ctx.r3.u32+44);symbol_output=PPC_LOAD_U32(ctx.r3.u32+48);
        if(symbol_capacity>1024*1024||!ReadableGuestRange(base,symbol_output,symbol_capacity)){_mm_setcsr(entry_csr);return false;}
        // Spend the real-input budget on calls that enter the accelerated
        // prefix, not cached output or repeated/short runs. Decode only the
        // bounded count+repeat header; leave all guest state untouched.
        const auto width=PPC_LOAD_U32(ctx.r3.u32+40),bits=PPC_LOAD_U32(ctx.r4.u32+8);
        if(PPC_LOAD_U32(ctx.r3.u32)!=PPC_LOAD_U32(ctx.r3.u32+4)||width>32||bits>32){_mm_setcsr(entry_csr);return false;}
        const uint32_t mask=bits==32?UINT32_MAX:bits?((1u<<bits)-1):0;
        uint64_t encoded=PPC_LOAD_U32(ctx.r4.u32)&mask;
        if(bits<width+1){const auto stream=PPC_LOAD_U32(ctx.r4.u32+4),words=(width+1-bits+31)/32;
            if(!ReadableGuestRange(base,stream,words*4)){_mm_setcsr(entry_csr);return false;}
            encoded|=uint64_t(PPC_LOAD_U32(stream))<<bits;
            if(words==2)encoded|=uint64_t(PPC_LOAD_U32(stream+4))<<32;
        }
        const uint32_t count_mask=width==32?UINT32_MAX:width?((1u<<width)-1):0;
        verified_symbols=uint32_t(encoded)&count_mask;
        if(verified_symbols<=16||verified_symbols>symbol_capacity||verified_symbols>INT32_MAX||((encoded>>width)&1)){_mm_setcsr(entry_csr);return false;}
    }
    const auto check=checks[kernel].fetch_add(1,std::memory_order_relaxed);
    if(check>=16){_mm_setcsr(entry_csr);return false;}
    // Source-hashed transform kernels write their red zone and8x8 pixel rows.
    // The coefficient decoder also stores at SP+20, outputs128 bytes, and
    // updates the12-byte bit reader. Capture the union, including any overlap.
    struct Footprint{std::array<uint8_t,544> stack;std::array<std::array<uint8_t,8>,8> rows;std::array<uint8_t,128> coefficients;std::array<uint8_t,12> reader;std::array<uint8_t,52> symbol_header;std::array<uint8_t,4> symbol_state;std::vector<uint8_t> symbols;bool operator==(const Footprint&)const=default;};
    const auto before_context=ctx;const auto before_csr=entry_csr;
    const uint32_t stack=ctx.r1.u32-512,destination=ctx.r3.u32,stride=ctx.r4.u32;
    auto copy=[&](Footprint& footprint,bool restore){
        auto bytes=[&](uint32_t address,void* data,size_t size){if(!size)return;if(restore)std::memcpy(PPC_RAW_ADDR(address),data,size);else std::memcpy(data,PPC_RAW_ADDR(address),size);};
        bytes(stack,footprint.stack.data(),footprint.stack.size());
        if(kernel==3){footprint.symbols.resize(symbol_capacity);bytes(destination,footprint.symbol_header.data(),52);bytes(stride,footprint.reader.data(),12);bytes(before_context.r5.u32+384,footprint.symbol_state.data(),4);bytes(symbol_output,footprint.symbols.data(),symbol_capacity);}
        else if(kernel==2){bytes(destination,footprint.coefficients.data(),128);bytes(stride,footprint.reader.data(),12);}
        else for(unsigned row=0;row<8;++row)bytes(destination+row*stride,footprint.rows[row].data(),8);
    };
    Footprint before{},expected{},actual{};copy(before,false);_mm_setcsr(before_csr);original(ctx,base);const auto expected_context=ctx;const auto expected_csr=_mm_getcsr();copy(expected,false);
    copy(before,true);ctx=before_context;_mm_setcsr(before_csr);
    struct Active{bool& value;explicit Active(bool& v):value(v){value=true;}~Active(){value=false;}}guard{active};
    localized(ctx,base);copy(actual,false);
    const bool context_ok=!std::memcmp(&ctx,&expected_context,sizeof ctx),memory_ok=expected==actual,csr_ok=_mm_getcsr()==expected_csr;
    if(!context_ok||!memory_ok||!csr_ok){
        verification_failures.fetch_add(1,std::memory_order_relaxed);
        // Keep original results if diagnostic verification rejects a block.
        copy(expected,true);ctx=expected_context;_mm_setcsr(expected_csr);
        REXCVAR_SET(em2_native_video_blocks,false);
        if(expected_verification_rejection)REXLOG_INFO("Deliberate video verifier rejection: restored original context/memory/MXCSR and disabled optimization");
        else REXLOG_ERROR("Live video block verification failed: kernel {} check {}, context {}, memory {}, MXCSR {}; optimization disabled",kernel,check,context_ok,memory_ok,csr_ok);
    }else REXLOG_INFO("Live video block verification passed: kernel {} check {}, destination {:08X}, original context/memory/MXCSR match, accelerated symbols {}",kernel,check,destination,verified_symbols);
    return true;
}

bool em2::TestNativeVideoBlocks(){
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100010000ull,MEM_RESERVE,PAGE_NOACCESS));
    const bool saved=REXCVAR_GET(em2_native_video_blocks),saved_verify=REXCVAR_GET(em2_verify_video_blocks);const auto csr=_mm_getcsr();
    struct Release{uint8_t* base;bool saved,verify;unsigned csr;~Release(){if(base)VirtualFree(base,0,MEM_RELEASE);REXCVAR_SET(em2_native_video_blocks,saved);REXCVAR_SET(em2_verify_video_blocks,verify);_mm_setcsr(csr);}}release{base,saved,saved_verify,csr};
    REXCVAR_SET(em2_verify_video_blocks,false);
    struct Region{uint32_t guest;size_t bytes;};
    constexpr std::array regions{Region{0,0x80000},Region{0x822E0000,0x10000},Region{0xA0200000,0x10000},Region{0xE0400000,0x10000}};
    if(!base)return false;
    for(auto region:regions)if(!VirtualAlloc(PPC_RAW_ADDR(region.guest),region.bytes,MEM_COMMIT,PAGE_READWRITE))return false;
    constexpr size_t bytes=0xB0000;
    std::vector<uint8_t> initial(bytes),expected(bytes);
    uint32_t seed=0x531A27B9;
    auto random=[&](){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;};
    auto transfer=[&](std::vector<uint8_t>& data,bool store){size_t at=0;for(auto region:regions){auto* memory=PPC_RAW_ADDR(region.guest);if(store)std::memcpy(memory,data.data()+at,region.bytes);else std::memcpy(data.data()+at,memory,region.bytes);at+=region.bytes;}};
    auto equivalent=[&](){size_t at=0;for(auto region:regions){if(std::memcmp(PPC_RAW_ADDR(region.guest),expected.data()+at,region.bytes))return false;at+=region.bytes;}return true;};
    const std::array originals{__imp__sub_83343AC8,__imp__sub_833441A8,__imp__sub_83344528,__imp__sub_8333C6C0};
    const std::array localized{sub_83343AC8,sub_833441A8,sub_83344528,sub_8333C6C0};
    auto symbol_fixture=[&](PPCContext& ctx,unsigned trial,int benchmark){
        const bool bench=benchmark>=0;
        const uint32_t root=bench?0x11000:trial%5?0x11000:0xA0202000,reader=bench?0x60000:trial%7?0x60000:0xE0404000,codec=0x52000,stream=0x50000;
        const uint32_t output=bench?0x22000:trial%13==0?root+16:trial%17==0?0x6FE00:0x22000;
        const uint32_t width=bench?9:std::array<uint32_t,8>{0,1,4,8,9,16,31,32}[trial%8],bits=bench?32:(trial/11)%33;
        const uint32_t count=bench?(benchmark==2?256:64):std::array<uint32_t,11>{0,1,2,3,4,31,32,64,255,256,257}[trial%11];
        const bool repeat=bench?benchmark==2:((trial/(11*33))%2)!=0,cached=bench?benchmark==0:trial%19==0;
        ctx.r3.u64=root;ctx.r4.u64=reader;ctx.r5.u64=codec;ctx.r6.u64=stream+(bench?4096:std::array<uint32_t,5>{0,4,8,64,4096}[trial%5]);
        PPC_STORE_U32(root,output+(cached?1:0));PPC_STORE_U32(root+4,output);PPC_STORE_U32(root+32,1+trial%4);PPC_STORE_U32(root+36,0x54000);PPC_STORE_U32(root+40,width);PPC_STORE_U32(root+44,256);PPC_STORE_U32(root+48,output);
        for(unsigned i=0;i<16;++i)PPC_STORE_U8(root+16+i,uint8_t((i+trial)%16));
        for(unsigned state=0;state<16;++state){const auto n=1+state%4;PPC_STORE_U32(codec+256+state*4,n);PPC_STORE_U32(codec+320+state*4,0x56000+state*16);
            for(unsigned i=0;i<16;++i){PPC_STORE_U8(codec+state*16+i,uint8_t((state+i)%16));PPC_STORE_U8(0x56000+state*16+i,uint8_t(((1+i%n)<<4)|(i%16)));}}
        PPC_STORE_U32(codec+384,trial%16);
        for(unsigned i=0;i<16;++i)PPC_STORE_U8(0x54000+i,uint8_t(((1+i%(1+trial%4))<<4)|(i%16)));
        for(unsigned i=0;i<4096;++i)PPC_STORE_U8(stream+i,uint8_t(i*37+trial));
        const uint32_t mask=bits==32?UINT32_MAX:bits?((1u<<bits)-1):0;uint32_t buffered=0xA56CF981u&mask;
        const uint32_t count_mask=width==32?UINT32_MAX:width?((1u<<width)-1):0;
        const uint64_t encoded=uint64_t(count&count_mask)|(uint64_t(repeat)<<width);
        for(unsigned bit=0;bit<=width;++bit){const uint32_t value=uint32_t((encoded>>bit)&1);
            if(bit<bits)buffered=(buffered&~(1u<<bit))|(value<<bit);
            else{const uint32_t word=stream+((bit-bits)/32)*4,shift=(bit-bits)%32;PPC_STORE_U32(word,(PPC_LOAD_U32(word)&~(1u<<shift))|(value<<shift));}}
        PPC_STORE_U32(reader,buffered);PPC_STORE_U32(reader+4,stream);PPC_STORE_U32(reader+8,bits);
    };
    unsigned comparisons=0,guest_fill_calls=0;
    for(unsigned function=0;function<4;++function)for(unsigned trial=0,cases=function==3?1452:function==2?2112:384;trial<cases;++trial){
        for(auto& value:initial)value=uint8_t(random());transfer(initial,true);
        PPCContext start{};
        constexpr std::array registers{
            &PPCContext::r0,&PPCContext::r1,&PPCContext::r2,&PPCContext::r3,&PPCContext::r4,&PPCContext::r5,&PPCContext::r6,&PPCContext::r7,
            &PPCContext::r8,&PPCContext::r9,&PPCContext::r10,&PPCContext::r11,&PPCContext::r12,&PPCContext::r13,&PPCContext::r14,&PPCContext::r15,
            &PPCContext::r16,&PPCContext::r17,&PPCContext::r18,&PPCContext::r19,&PPCContext::r20,&PPCContext::r21,&PPCContext::r22,&PPCContext::r23,
            &PPCContext::r24,&PPCContext::r25,&PPCContext::r26,&PPCContext::r27,&PPCContext::r28,&PPCContext::r29,&PPCContext::r30,&PPCContext::r31};
        for(auto member:registers)(start.*member).u64=(uint64_t(random())<<32)|random();
        start.lr=(uint64_t(random())<<32)|random();start.ctr.u64=(uint64_t(random())<<32)|random();
        start.xer.ca=trial&1;start.xer.so=(trial>>1)&1;start.cr0.eq=(trial>>3)&1;start.cr0.gt=(trial>>4)&1;start.cr0.lt=(trial>>5)&1;start.cr6.eq=trial&1;start.cr6.gt=(trial>>1)&1;start.cr6.lt=(trial>>2)&1;
        start.r1.u64=0x70000+(trial%4)*16;
        const auto destination=trial%6==0?0xA0201000:trial%6==1?0xE0401000:0x20100;
        const auto input=trial%7==0?destination:trial%7==1?0x6FF00:0x40000;
        start.r3.u64=destination+(trial%4);start.r4.s64=std::array<int,6>{8,16,32,64,-8,-32}[trial%6];
        start.r5.u64=input+(trial%3)*2;start.r6.u64=function?trial%16:0x50000;
        start.r7.u64=trial%5==0?start.r3.u64:0x30100;
        start.r8.s64=trial%2?16:-16;
        // Zero, DC-only, sparse and dense signed coefficients; quantizers also
        // cover signed/high-bit values, not only plausible movie content.
        for(unsigned i=0;i<64;++i){const auto value=trial%4==0?0:trial%4==1?(i<8?random():0):trial%4==2?(i%9==0?random():0):random();PPC_STORE_U16(start.r5.u32+i*2,uint16_t(value));}
        if(function==2){
            const uint32_t header=trial%16,bits=(trial/16)%33,pattern=trial/(16*33),stream=0x50000+(trial%4)*4;
            start.r3.u64=trial%7==0?0x6FED0:destination+(trial%4)*4;
            start.r4.u64=trial%11==0?0x6FE00:trial%13==0?start.r3.u64:0x60000+(trial%4)*4;
            for(unsigned i=0;i<4096;++i)PPC_STORE_U8(stream+i,pattern==0?0:pattern==1?255:pattern==2?uint8_t(i*37):uint8_t(random()));
            const uint32_t mask=bits==32?UINT32_MAX:bits?((1u<<bits)-1):0;
            const uint32_t buffered=((random()&~15u)|header)&mask;
            if(bits<4){const uint32_t needed=4-bits,low_mask=(1u<<needed)-1;PPC_STORE_U32(stream,(PPC_LOAD_U32(stream)&~low_mask)|(header>>bits));}
            PPC_STORE_U32(start.r4.u32,buffered);PPC_STORE_U32(start.r4.u32+4,stream);PPC_STORE_U32(start.r4.u32+8,bits);
        }
        if(function==3)symbol_fixture(start,trial+1==cases?1:trial,trial+1==cases?1:-1);
        transfer(initial,false);
        const auto original=originals[function],optimized=localized[function];
        const unsigned case_csr=(csr&~0xE040u)|((trial%4)<<13)|((trial&4)?0x8000:0)|((trial&8)?0x40:0);
        auto reference=start;_mm_setcsr(case_csr);original(reference,base);const auto expected_csr=_mm_getcsr();transfer(expected,false);
        if(function==3&&reference.ctr.u64!=start.ctr.u64)++guest_fill_calls;
        for(unsigned mode=0;mode<(trial+1==cases?3u:2u);++mode){
            const bool enabled=mode!=0;transfer(initial,true);REXCVAR_SET(em2_native_video_blocks,enabled);REXCVAR_SET(em2_verify_video_blocks,mode==2);auto actual=start;_mm_setcsr(case_csr);optimized(actual,base);
            const bool memory_ok=equivalent();
            if(!memory_ok||std::memcmp(&reference,&actual,sizeof actual)||_mm_getcsr()!=expected_csr){REXLOG_ERROR("Video block mismatch: function {}, case {}, enabled {}, memory {}",function,trial,enabled,memory_ok);return false;}
            ++comparisons;
        }
    }
    REXCVAR_SET(em2_verify_video_blocks,false);_mm_setcsr(csr);
    if(verification_failures.load(std::memory_order_relaxed))return false;
    if(!guest_fill_calls)return false;
    REXLOG_INFO("Video symbol decoder exercised {} original guest fill calls alongside long-loop prefix acceleration",guest_fill_calls);
    REXLOG_INFO("Video block localization matched original complete guest context, MXCSR and all fixture/guard bytes in {} comparisons",comparisons);
    {
        // Exercise rejection independently from the localized implementations:
        // deliberately damage a result byte, GPR and host processor state.
        transfer(initial,true);PPCContext start{};start.r1.u64=0x70000;start.r3.u64=0x20100;start.r4.u64=16;start.r5.u64=0x40000;start.r6.u64=0x50000;start.lr=0x12345678;
        auto reference=start;__imp__sub_83343AC8(reference,base);transfer(expected,false);const auto expected_csr=_mm_getcsr();
        transfer(initial,true);auto actual=start;_mm_setcsr(csr);REXCVAR_SET(em2_native_video_blocks,true);
        const auto previous_failures=verification_failures.load(std::memory_order_relaxed);
        struct ExpectedRejection{ExpectedRejection(){expected_verification_rejection=true;}~ExpectedRejection(){expected_verification_rejection=false;}}scope;
        const bool checked=VerifyVideoBlock(actual,base,__imp__sub_83343AC8,+[](PPCContext& ctx,uint8_t* base){
            const auto destination=ctx.r3.u32;__imp__sub_83343AC8(ctx,base);ctx.r3.u64^=1;
            PPC_STORE_U8(destination,uint8_t(PPC_LOAD_U8(destination)^1));_mm_setcsr(_mm_getcsr()^0x8000);
        },0);
        const bool rejected=verification_failures.load(std::memory_order_relaxed)==previous_failures+1;
        if(!checked||!rejected||REXCVAR_GET(em2_native_video_blocks)||!equivalent()||std::memcmp(&reference,&actual,sizeof actual)||_mm_getcsr()!=expected_csr)return false;
        verification_failures.store(previous_failures,std::memory_order_relaxed);
    }
    if(REXCVAR_GET(em2_benchmark_video_blocks)){
        for(unsigned function=0;function<4;++function)for(unsigned pattern=0;pattern<3;++pattern){
            for(auto& value:initial)value=uint8_t(random());transfer(initial,true);
            for(unsigned i=0;i<64;++i)PPC_STORE_U16(0x40000+i*2,pattern==0?(i<8?32:0):pattern==1?(i%9==0?64:0):uint16_t(random()));
            if(function==2)for(unsigned i=0;i<4096;++i)PPC_STORE_U8(0x50000+i,pattern==0?0:pattern==1?255:uint8_t(random()));
            transfer(initial,false);std::array<double,2> elapsed{};std::array<uint64_t,2> checksums{};
            constexpr unsigned iterations=12000,trials=4;
            for(unsigned trial=0;trial<trials;++trial)for(unsigned phase=0;phase<2;++phase){
                const auto mode=(phase+trial)%2;transfer(initial,true);REXCVAR_SET(em2_native_video_blocks,mode!=0);PPCContext context{};context.r1.u64=0x70000;
                if(function==3)symbol_fixture(context,pattern,pattern);
                const auto kernel=mode?localized[function]:originals[function];
                const auto begin=std::chrono::steady_clock::now();
                for(unsigned i=0;i<iterations;++i){context.r3.u64=0x20100;context.r4.u64=16;context.r5.u64=0x40000;context.r6.u64=function?0:0x50000;context.r7.u64=0x30100;context.r8.u64=16;context.lr=0x12345678;
                    if(function==2){context.r4.u64=0x60000;const auto header=std::array<uint32_t,3>{0,8,15}[pattern];PPC_STORE_U32(0x60000,header);PPC_STORE_U32(0x60004,0x50000);PPC_STORE_U32(0x60008,32);}
                    if(function==3){context.r3.u64=0x11000;context.r4.u64=0x60000;context.r5.u64=0x52000;context.r6.u64=0x51000;
                        PPC_STORE_U32(0x11000,0x22000+(pattern==0?1:0));PPC_STORE_U32(0x11004,0x22000);PPC_STORE_U32(0x52180,pattern%16);
                        PPC_STORE_U32(0x60000,pattern==2?(256u|512u):64u);PPC_STORE_U32(0x60004,0x50000);PPC_STORE_U32(0x60008,32);}
                    kernel(context,base);}
                elapsed[mode]+=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-begin).count();
                checksums[mode]+=context.r3.u64+context.r5.u64+context.r6.u64+PPC_LOAD_U32(0x20100)+PPC_LOAD_U32(0x20164);
                if(function==2)checksums[mode]+=PPC_LOAD_U32(0x60000)+PPC_LOAD_U32(0x60004)+PPC_LOAD_U32(0x60008);
                if(function==3)checksums[mode]+=PPC_LOAD_U32(0x11000)+PPC_LOAD_U32(0x11004)+PPC_LOAD_U32(0x52180)+PPC_LOAD_U32(0x60000)+PPC_LOAD_U32(0x60004)+PPC_LOAD_U32(0x60008);
            }
            if(checksums[0]!=checksums[1])return false;
            REXLOG_INFO("Video block benchmark {} pattern {}: original {:.1f}, localized {:.1f} ns/call; {:.2f}x kernel speedup (not game FPS)",function,pattern,elapsed[0]/(iterations*trials),elapsed[1]/(iterations*trials),elapsed[0]/elapsed[1]);
        }
    }
    return true;
}
