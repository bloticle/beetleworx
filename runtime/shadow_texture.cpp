#include "ppc_config.h"
#include "shadow_resources.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <array>
#include <cstring>
#include <immintrin.h>
#include <vector>
#include <windows.h>

PPC_EXTERN_FUNC(__imp__sub_8267C260);
PPC_EXTERN_FUNC(EM2CheckedShadowTexture);
PPC_EXTERN_FUNC(EM2ShadowTextureAllocationFixture);
PPC_FUNC_IMPL(sub_8267C260) {
    if(em2::ShadowCreationActive())EM2CheckedShadowTexture(ctx,base);
    else __imp__sub_8267C260(ctx,base);
}
namespace {
struct AllocatorFixture {
    std::array<uint32_t,2> replies{};
    std::vector<std::array<uint32_t,3>> requests;
    std::vector<std::array<uint32_t,2>> frees;
};
thread_local AllocatorFixture* fixture=nullptr;
void Allocate(PPCContext& ctx,uint8_t* base) {
    const auto index=fixture->requests.size();
    fixture->requests.push_back({PPC_LOAD_U32(ctx.r4.u32),PPC_LOAD_U32(ctx.r5.u32),ctx.r7.u32});
    ctx.r3.u64=index<fixture->replies.size()?fixture->replies[index]:0;
}
void Free(PPCContext& ctx,uint8_t*) {fixture->frees.push_back({ctx.r4.u32,ctx.r5.u32});}
}
bool em2::TestShadowMetadataAllocationFailure() {
    const auto csr=_mm_getcsr();struct RestoreFP {unsigned csr;~RestoreFP(){_mm_setcsr(csr);}}fp{csr};
    auto check=[](bool ok,const char* text){if(!ok)REXLOG_ERROR("Shadow metadata failure test: {}",text);return ok;};
    const auto previous=fixture;struct RestoreFixture {AllocatorFixture* previous;~RestoreFixture(){fixture=previous;}}restore_fixture{previous};
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));
    if(!base)return false;
    struct Release {uint8_t* base;~Release(){VirtualFree(base,0,MEM_RELEASE);}}release{base};
    if(!VirtualAlloc(base,0x40000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0x833C0000,0x20000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+PPC_IMAGE_BASE+PPC_IMAGE_SIZE,0x10000,MEM_COMMIT,PAGE_READWRITE))return false;
    PPC_LOOKUP_FUNC(base,0x822F0200)=Allocate;PPC_LOOKUP_FUNC(base,0x822F0210)=Free;
    unsigned cases=0;
    for(bool header:{false,true})for(uint32_t invalid:{0u,8u,0xFFFFFFF8u,0xFFFFFFFCu}) {
        std::memset(base,0,0x40000);
        // Original allocation wrappers and TLS bucket lookup, with only the
        // final allocator interface scripted. No graphics device is invoked.
        PPC_STORE_U32(0x833D05FC,0x30000);PPC_STORE_U32(0x30000,0x31000);
        PPC_STORE_U32(0x31000,0x32000);PPC_STORE_U32(0x32004,0x822F0200);PPC_STORE_U32(0x32008,0x822F0210);
        PPC_STORE_U32(0x2100,0x2300);PPC_STORE_U32(0x2300+332,7);PPC_STORE_U32(0x833CDD8C,17);
        PPC_STORE_U32(0x6000,0x6500);PPC_STORE_U32(0x6500+20,0x12345678);
        AllocatorFixture allocation;allocation.replies=header?std::array<uint32_t,2>{0x8000,invalid}:std::array<uint32_t,2>{invalid,0};fixture=&allocation;
        PPCContext ctx{};ctx.r1.u64=0x10000;ctx.r13.u64=0x2000;ctx.r4.u64=0x6000;
        // The allocation-stage fixture bypasses only device format validation.
        // Its allocation, guards, cleanup and native epilogue are unchanged.
        ctx.r5.u64=0;ctx.lr=0x11223344;ctx.fpscr.InitHost();
        unsigned seed=25;
        for(auto* reg:std::array{&ctx.r25,&ctx.r26,&ctx.r27,&ctx.r28,&ctx.r29,&ctx.r30,&ctx.r31})reg->u64=0x10000000+seed++;
        const auto original=ctx;const auto before_csr=_mm_getcsr();
        ShadowCreationAuditScope scope({true,0x12340000,0,512,1024});
        EM2ShadowTextureAllocationFixture(ctx,base);
        const auto trace=CurrentShadowCreation();
        if(!check(ctx.r3.u32==0 && ctx.r1.u64==original.r1.u64 && ctx.lr==original.lr && _mm_getcsr()==before_csr,
            "guarded native body returns failure with caller stack/LR and MXCSR restored"))return false;
        if(!check(std::array{ctx.r25.u64,ctx.r26.u64,ctx.r27.u64,ctx.r28.u64,ctx.r29.u64,ctx.r30.u64,ctx.r31.u64}==
            std::array{original.r25.u64,original.r26.u64,original.r27.u64,original.r28.u64,original.r29.u64,original.r30.u64,original.r31.u64},
            "native failure epilogue restores nonvolatile registers"))return false;
        if(!check(trace.failed_allocation_size==(header?52u:40u) && trace.failed_allocation_address==invalid &&
            allocation.requests.size()==(header?2u:1u) && allocation.requests[0]==std::array<uint32_t,3>{40,16,1} &&
            PPC_LOAD_U32(0x833CDD8C)==17 && PPC_LOAD_U32(0x6500+20)==0x12345678,
            "failure is recorded, allocation stops, native object count and unpublished output remain intact"))return false;
        if(header) {
            if(!check(allocation.requests[1]==std::array<uint32_t,3>{52,8,5} &&
                allocation.frees==std::vector<std::array<uint32_t,2>>{{0,14},{0x8000,3}} &&
                PPC_LOAD_U32(0x8000)==0x82039314,
                "original native backing destructor balances its object and frees it exactly once without freeing the rejected header"))return false;
        }else if(!check(allocation.frees.empty(),"failed backing allocation creates no object to destroy"))return false;
        ++cases;
    }
    for(uint32_t bytes:{40u,52u}) {
        ShadowCreationAuditScope inactive({});
        if(!check(ValidateShadowMetadataAllocation(4096,bytes) && ValidateShadowMetadataAllocation(uint32_t(0x100000000ull-bytes),bytes) &&
            !ValidateShadowMetadataAllocation(uint32_t(0x100000001ull-bytes),bytes) && !ValidateShadowMetadataAllocation(0,bytes) &&
            !CurrentShadowCreation().failed_allocation_size,"span boundaries and disabled audit isolation"))return false;
    }
    REXLOG_INFO("Shadow metadata failure handling passed {} guarded native allocation-stage bodies (device format validation bypassed): null/null-page/wrapping backing and header results, original allocation wrappers/TLS lookup, original backing destructor, exact frees/object count, unpublished output and native stack/LR/nonvolatile/MXCSR restoration; valid full resource creation and allocator health remain live checks",cases);
    return true;
}
