#include "ppc_context.h"
#include "em1_runtime_validation.h"
#include "em1_model_validation.h"
#include "em1_native_preview.h"
#include "em1_single_gsa.h"
#include "em1_preview_runtime.h"
#include "em1_loader_diagnostics.h"
#include "em1_parser_probe.h"
#include "em1_level_entry.h"
#include <windows.h>
#include <immintrin.h>
#include <rex/logging.h>

PPC_EXTERN_FUNC(__imp__sub_827A9458);
PPC_EXTERN_FUNC(__imp__sub_827B96B0);
PPC_FUNC_IMPL(sub_827B96B0){
    em2::em1::ResolveNativeBlueprintParent(ctx,base);
    __imp__sub_827B96B0(ctx,base);
}

// The original scene begin establishes an initialized game-thread loader context.
// Validation is opt-in and uses separate parser objects without activating entities.
PPC_FUNC_IMPL(sub_827A9458) {
    const auto parser=ctx.r3.u32,input=ctx.r4.u32,size=ctx.r5.u32;
    const auto incoming_error=GetLastError();const auto incoming_csr=_mm_getcsr();
    const auto library=em2::em1::MatchRegisteredSceneBlueprints(base,input,size);
    em2::em1::SetRegisteredParserBlueprints(parser,library);
    SetLastError(incoming_error);_mm_setcsr(incoming_csr);
    {
      em2::em1::NativeBlueprintParentScope scope(library?&library->index:nullptr,library?&library->parent_resolutions:nullptr);
      __imp__sub_827A9458(ctx, base);
    }
    if(ctx.r3.u32!=1){const auto error=GetLastError();const auto csr=_mm_getcsr();em2::em1::SetRegisteredParserBlueprints(parser,{});SetLastError(error);_mm_setcsr(csr);}
    em2::em1::AfterSingleGsaSceneBegin(ctx,base,input,size,ctx.r3.u32==1,em2::em1::ActiveSingleGsaEntry());
    if (ctx.r3.u32 == 1) {
        em2::em1::MaybeValidateNativeAnimations(ctx, base);
        em2::em1::MaybeValidateNativeLevel(ctx, base);
        em2::em1::MaybeValidateNativeModel(ctx, base);
    }
}
PPC_EXTERN_FUNC(__imp__sub_827A8F48);
PPC_FUNC_IMPL(sub_827A8F48){
    const auto error=GetLastError();const auto csr=_mm_getcsr();
    const auto library=em2::em1::RegisteredParserBlueprints(ctx.r3.u32);
    em2::em1::NativeBlueprintParentScope scope(library?&library->index:nullptr,library?&library->parent_resolutions:nullptr);
    SetLastError(error);_mm_setcsr(csr);__imp__sub_827A8F48(ctx,base);
}
PPC_EXTERN_FUNC(__imp__sub_827A8908);
PPC_FUNC_IMPL(sub_827A8908){
    const auto parser=ctx.r3.u32;
    const auto incoming_error=GetLastError();const auto incoming_csr=_mm_getcsr();
    const auto library=em2::em1::RegisteredParserBlueprints(parser);
    SetLastError(incoming_error);_mm_setcsr(incoming_csr);
    {em2::em1::NativeBlueprintParentScope scope(library?&library->index:nullptr,library?&library->parent_resolutions:nullptr);__imp__sub_827A8908(ctx,base);}
    const auto error=GetLastError();const auto csr=_mm_getcsr();em2::em1::SetRegisteredParserBlueprints(parser,{});SetLastError(error);_mm_setcsr(csr);
}

#include "em1_native_resources.h"
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <cstring>
PPC_EXTERN_FUNC(__imp__sub_82555510);
PPC_EXTERN_FUNC(__imp__sub_827C8CB0);
PPC_FUNC_IMPL(sub_827C8CB0){
    if(em2::em1::NativeSceneConstructionActive()){
        const auto error=GetLastError();const auto csr=_mm_getcsr();uint32_t words[26]{};SIZE_T read=0;
        if(ReadProcessMemory(GetCurrentProcess(),base+ctx.r3.u32,words,sizeof(words),&read) && read==sizeof(words)){
            const auto slots=_byteswap_ulong(words[11]),capacity=_byteswap_ulong(words[12]),count=_byteswap_ulong(words[13]);uint32_t component=0;
            const bool empty=count==0;
            if(empty || (ReadProcessMemory(GetCurrentProcess(),base+slots+ctx.r4.u32*4,&component,4,&read) && read==4 && !component)){
                char name[256]{};const auto pointer=_byteswap_ulong(words[7]);
                if(pointer<4096 || !ReadProcessMemory(GetCurrentProcess(),base+pointer,name,sizeof(name)-1,&read))name[0]=0;
                REXLOG_ERROR("[em1] complete source construction null component: owner={:08X} slot={} count={} capacity={} owner_words={:08X},{:08X},{:08X},{:08X},{:08X},{:08X},{:08X},{:08X}",ctx.r3.u32,ctx.r4.u32,count,capacity,
                    _byteswap_ulong(words[0]),_byteswap_ulong(words[1]),_byteswap_ulong(words[2]),_byteswap_ulong(words[3]),_byteswap_ulong(words[4]),_byteswap_ulong(words[5]),_byteswap_ulong(words[6]),_byteswap_ulong(words[7]));
                REXLOG_ERROR("[em1] complete source construction owner name candidate='{}'",std::string(name,strnlen(name,sizeof(name))));
                if(auto logger=rex::GetLogger())logger->flush();
            }
        }
        _mm_setcsr(csr);SetLastError(error);
    }
    __imp__sub_827C8CB0(ctx,base); // Observe only. Never skip/drop a source component.
}
// Observe the genuine typed-resource loader without changing its input or result.
PPC_FUNC_IMPL(sub_82555510) {
    const bool trace=em2::em1::NativeModelPublicationActive() || em2::em1::RegisteredLevelEntryProbeRequested();
    const auto owner=ctx.r3.u32, length=ctx.r5.u32;
    __imp__sub_82555510(ctx,base);
    if(trace) {
        const auto error=GetLastError(); const auto csr=_mm_getcsr();
        const char* diagnostic=reinterpret_cast<const char*>(base+owner+800);
        REXLOG_INFO("[em1] typed NiStream result={} roots={} status={} bytes={} message='{}'",
            ctx.r3.u32,PPC_LOAD_U32(owner+360),PPC_LOAD_U32(owner+796),length,
            std::string(diagnostic,strnlen(diagnostic,260)));
        _mm_setcsr(csr);SetLastError(error);
    }
}

PPC_EXTERN_FUNC(__imp__sub_8274C588);
PPC_FUNC_IMPL(sub_8274C588){
    const bool trace=em2::em1::RegisteredLevelEntryProbeRequested();
    const auto kind=ctx.r5.u32,length=ctx.r7.u32;
    char name[241]{};
    if(trace){const auto error=GetLastError();const auto csr=_mm_getcsr();SIZE_T read=0;
      if(ctx.r4.u32<4096 || !ReadProcessMemory(GetCurrentProcess(),base+ctx.r4.u32,name,240,&read))name[0]=0;
      SetLastError(error);_mm_setcsr(csr);
    }
    __imp__sub_8274C588(ctx,base);
    if(trace){const auto error=GetLastError();const auto csr=_mm_getcsr();
      REXLOG_INFO("[em1] full-entry typed publication: name='{}' type={:08X} bytes={} result={:08X}",name,kind,length,ctx.r3.u32);
      SetLastError(error);_mm_setcsr(csr);
    }
}

PPC_EXTERN_FUNC(__imp__sub_82749E50);
PPC_FUNC_IMPL(sub_82749E50) {
    em2::em1::ObserveSingleGsaRawLookup(ctx,base,ctx.r3.u32,ctx.r4.u32);
    em2::em1::ServeSingleGsaRawRead(ctx,base,ctx.r3.u32,ctx.r4.u32,em2::em1::ActiveSingleGsaEntry());
    __imp__sub_82749E50(ctx,base);
}

PPC_EXTERN_FUNC(__imp__sub_830BB538);
PPC_EXTERN_FUNC(__imp__sub_830BA7F0);
// Observers only: retain the original native update/time-commit behavior.
PPC_FUNC_IMPL(sub_830BB538){
    em2::em1::TraceNativeParticleUpdate(ctx,base,0);
    __imp__sub_830BB538(ctx,base);
}
PPC_FUNC_IMPL(sub_830BA7F0){
    em2::em1::TraceNativeParticleUpdate(ctx,base,1);
    __imp__sub_830BA7F0(ctx,base);
}

PPC_EXTERN_FUNC(__imp__sub_830BA808);
PPC_EXTERN_FUNC(__imp__sub_830BA310);
PPC_FUNC_IMPL(sub_830BA808){
    em2::em1::TraceNativeParticleUpdate(ctx,base,2);
    __imp__sub_830BA808(ctx,base);
}
PPC_FUNC_IMPL(sub_830BA310){
    em2::em1::TraceNativeParticleUpdate(ctx,base,3);
    __imp__sub_830BA310(ctx,base);
}

PPC_EXTERN_FUNC(__imp__sub_830C13A8);
PPC_FUNC_IMPL(sub_830C13A8){
    em2::em1::TraceNativeParticleUpdate(ctx,base,4);
    __imp__sub_830C13A8(ctx,base);
}

PPC_EXTERN_FUNC(__imp__sub_825BEAE0);
// Observe exact native material resolution only during owned EM1 publication.
// Failed names are evidence; never fabricate a material or alter lookup inputs.
PPC_FUNC_IMPL(sub_825BEAE0){
    char name[128]{};bool observe=false;
    if(em2::em1::NativeModelPublicationActive()){
        const auto error=GetLastError();const auto csr=_mm_getcsr();
        uint32_t pointer=0;SIZE_T read=0;
        if(ReadProcessMemory(GetCurrentProcess(),base+ctx.r3.u32,&pointer,4,&read) && read==4){
            pointer=_byteswap_ulong(pointer);
            if(pointer && ReadProcessMemory(GetCurrentProcess(),base+pointer,name,sizeof(name),&read) &&
               read==sizeof(name) && std::memchr(name,0,sizeof(name))){
                observe=std::strcmp(name,"FX_alphaBlend_singleChannel")==0 ||
                    std::strcmp(name,"FX_DiffuseAlpha_VertexColorAlpha")==0 ||
                    std::strcmp(name,"SFX_accumulatedIndirect_Positive")==0 ||
                    std::strcmp(name,"Gen3_FX_alphaBlend_singleChannel")==0 ||
                    std::strcmp(name,"Gen3_FX_DiffuseAlpha_VertexColorAlpha")==0;
            }
        }
        _mm_setcsr(csr);SetLastError(error);
    }
    __imp__sub_825BEAE0(ctx,base);
    if(observe){
        const auto error=GetLastError();const auto csr=_mm_getcsr();
        REXLOG_INFO("[em1] original particle material lookup name='{}' result={:08X}",name,ctx.r3.u32);
        _mm_setcsr(csr);SetLastError(error);
    }
}

#include "em1_compiled_level.h"
#include "em1_vfs.h"
#include "em1_level_entry.h"
#include "em1_native_component.h"
#include <rex/cvar.h>
#include <rex/kernel/kernel_state.h>
REXCVAR_DEFINE_BOOL(em1_registry_roundtrip,false,"Debug",
    "Developer baseline: parse initial retail level registry from an identical owned buffer");
REXCVAR_DEFINE_STRING(em1_register_level,"","Debug","Original EM1 .level entry to register during initial native registry loading");
PPC_EXTERN_FUNC(__imp__sub_827CCE50);
PPC_EXTERN_FUNC(__imp__sub_832187A8);
PPC_FUNC_IMPL(sub_832187A8){
    const bool observe=em2::em1::RegisteredLevelEntryProbeRequested();
    if(observe){const auto error=GetLastError();const auto csr=_mm_getcsr();
        REXLOG_INFO("[em1] full-entry native NIF factory begin: resource={:08X} data={:08X} bytes={}",ctx.r3.u32,ctx.r5.u32,ctx.r6.u32);
        SetLastError(error);_mm_setcsr(csr);}
    __imp__sub_832187A8(ctx,base);
    if(observe){const auto error=GetLastError();const auto csr=_mm_getcsr();
        REXLOG_INFO("[em1] full-entry native NIF factory returned: result={:08X}",ctx.r3.u32);
        SetLastError(error);_mm_setcsr(csr);}
}
// Reserve new type slots before any entity allocates its fixed-size type cache.
PPC_FUNC_IMPL(sub_827CCE50){
    __imp__sub_827CCE50(ctx,base);
    const auto saved=ctx;const auto error=GetLastError();const auto csr=_mm_getcsr();
    if(!REXCVAR_GET(em1_register_level).empty() || em2::em1::CurrentLevelCatalog()){
        const auto capacity=PPC_LOAD_U8(0x833DBAF1);
        if(capacity<=248){PPC_STORE_U8(0x833DBAF1,capacity+7);REXLOG_INFO("[em1] reserved native component type capacity: {} -> {} before entity allocation",capacity,capacity+7);}
        else REXLOG_ERROR("[em1] native component type capacity cannot fit original EM1 adapters");
    }
    ctx=saved;SetLastError(error);_mm_setcsr(csr);
}
PPC_EXTERN_FUNC(__imp__sub_82797910);
namespace {
bool RegistryReadable(uint8_t* base,uint32_t address,size_t size){
    if(!base || address<4096 || !size || uint64_t(address)+size>0x100000000ull)return false;
    auto at=reinterpret_cast<uintptr_t>(base+address),end=at+size;
    while(at<end){
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info)) || info.State!=MEM_COMMIT ||
            (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
        const auto protection=info.Protect&255;
        if(protection!=PAGE_READONLY && protection!=PAGE_READWRITE && protection!=PAGE_WRITECOPY &&
            protection!=PAGE_EXECUTE_READ && protection!=PAGE_EXECUTE_READWRITE && protection!=PAGE_EXECUTE_WRITECOPY)return false;
        const auto next=reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;
        if(next<=at)return false;at=std::min(end,next);
    }
    return true;
}
}
PPC_FUNC_IMPL(sub_82797910){
    // Only the original initializer owns stack50/54 and their resource token.
    const auto original_level=REXCVAR_GET(em1_register_level);
    const bool source_catalog=bool(em2::em1::CurrentLevelCatalog());
    if((!REXCVAR_GET(em1_registry_roundtrip) && original_level.empty() && !source_catalog) || ctx.lr!=0x82797FB4 ||
       uint64_t(ctx.r1.u32)+0x58>0x100000000ull || !RegistryReadable(base,ctx.r1.u32+0x50,8)){
        __imp__sub_82797910(ctx,base);return;
    }
    const auto incoming_error=GetLastError();const auto incoming_csr=_mm_getcsr();
    const auto input=ctx.r4.u32,token=PPC_LOAD_U32(ctx.r1.u32+0x54);
    auto* kernel=rex::kernel::kernel_state();
    uint32_t replacement=0;size_t size=0,replacement_size=0;std::string error;
    if(PPC_LOAD_U32(ctx.r1.u32+0x50)==input && RegistryReadable(base,token,0x1c) && kernel && kernel->memory()){
        // Native82749E50 uses descriptor+8 as decompressed byte length.
        size=PPC_LOAD_U32(token+8);
        if(size<=32*1024*1024 && RegistryReadable(base,input,size)){
            em2::em1::CompiledLevel registry;std::vector<uint8_t> encoded;
            if(em2::em1::ReadCompiledLevel(std::span(base+input,size),registry,error) &&
               em2::em1::WriteCompiledLevel(registry,encoded,error) && encoded.size()==size &&
               std::equal(encoded.begin(),encoded.end(),base+input)){
                const auto retail_encoded=encoded;
                if(!original_level.empty()){
                  std::vector<uint8_t> extended;
                  if(!em2::em1::RegisterNativeAmmoFacade(ctx,base,error) || !em2::em1::RegisterNativeInventoryFacade(ctx,base,error) || !em2::em1::ExtendNativeLevelRegistry(ctx,base,encoded,original_level,extended,error) ||
                     !em2::em1::RegisterNativeNavigationFacade(ctx,base,em2::em1::RegisteredLevelNavigation(original_level),error) ||
                     !em2::em1::RegisterNativeLogicFacades(ctx,base,error) || !em2::em1::RegisterNativeSeedFacade(ctx,base,error) ||
                     !em2::em1::RegisterNativeTrackingFacade(ctx,base,error) ||
                     !em2::em1::RetainRegisteredLevelBlueprints(ctx,base,original_level,error)){
                    REXLOG_ERROR("[em1] original level registry extension failed: {}",error);
                    SetLastError(incoming_error);_mm_setcsr(incoming_csr);__imp__sub_82797910(ctx,base);return;
                  }
                  encoded=std::move(extended);
                }
                if(source_catalog){
                  if(original_level.empty()){
                    auto navigation=std::make_shared<const std::vector<em2::em1::NavigationDatabase>>();
                    if(!em2::em1::RegisterNativeAmmoFacade(ctx,base,error) || !em2::em1::RegisterNativeInventoryFacade(ctx,base,error) ||
                       !em2::em1::RegisterNativeNavigationFacade(ctx,base,std::move(navigation),error) ||
                       !em2::em1::RegisterNativeLogicFacades(ctx,base,error) || !em2::em1::RegisterNativeSeedFacade(ctx,base,error) ||
                       !em2::em1::RegisterNativeTrackingFacade(ctx,base,error)){
                      REXLOG_ERROR("[em1] debug level catalog component registration failed: {}",error);
                      SetLastError(incoming_error);_mm_setcsr(incoming_csr);__imp__sub_82797910(ctx,base);return;
                    }
                  }
                  std::vector<uint8_t> extended;
                  if(em2::em1::ExtendDeferredNativeLevelRegistry(base,retail_encoded,encoded,extended,error))encoded=std::move(extended);
                  else REXLOG_ERROR("[em1] complete debug level catalog failed: {}",error);
                }
                replacement_size=encoded.size();replacement=kernel->memory()->SystemHeapAlloc(uint32_t(replacement_size),32);
                if(replacement)std::memcpy(base+replacement,encoded.data(),replacement_size);
            }
        }
    }
    SetLastError(incoming_error);_mm_setcsr(incoming_csr);
    if(!replacement){__imp__sub_82797910(ctx,base);return;}
    // Do not change the initializer's original pointer or token stack slots.
    ctx.r4.u64=replacement;
    __imp__sub_82797910(ctx,base);
    const auto result_error=GetLastError();const auto result_csr=_mm_getcsr();
    kernel->memory()->SystemHeapFree(replacement);
    REXLOG_INFO("[em1] initial registry owned-buffer parsed original_bytes={} registered_bytes={} original_level='{}'; original resource cleanup retained",size,replacement_size,original_level);
    SetLastError(result_error);_mm_setcsr(result_csr);
}
