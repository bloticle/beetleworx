#include "em1_ai_compat.h"
#include "em1_level_entry.h"
#include "ppc_context.h"
#include <windows.h>
#include <immintrin.h>
#include <cstring>
namespace em2::em1 {
namespace {
bool Read(uint8_t* base,uint32_t address,void* value,size_t size){
  SIZE_T count=0;return base && address>=4096 && uint64_t(address)+size<=0x100000000ull &&
    ReadProcessMemory(GetCurrentProcess(),base+address,value,size,&count) && count==size;
}
}
bool TryLegacyBodylessAi(PPCContext& ctx,uint8_t* base){
  struct RestoreHost {DWORD error=GetLastError();unsigned csr=_mm_getcsr();~RestoreHost(){SetLastError(error);_mm_setcsr(csr);}} restore;
  // Original EM1 REL2DF9E0..2DFB14 and300038 skip navigation-agent
  // creation when the owning actor has no character controller. EM2's helper
  // unconditionally reads character radius first. Restore the original branch
  // only for source-owned AI initialization; no collider or agent is invented.
  if(uint32_t(ctx.lr)!=0x827F1CBC || ctx.r3.u32>UINT32_MAX-8)return false;
  const auto entity=ctx.r3.u32+8;
  if(!IsScopedRegisteredLevelActor(base,entity))return false;
  uint8_t type=0,slot=0;uint32_t types=0;
  if(!Read(base,0x833DC718,&type,1) || !Read(base,entity+88,&types,4))return false;
  types=_byteswap_ulong(types);
  if(uint64_t(types)+type>UINT32_MAX || !Read(base,types+type,&slot,1) || slot!=255)return false;
  ctx.r3.u64=0;return true;
}
bool TestLegacyBodylessAi(std::string& error){
  auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));
  if(!base){error="Could not reserve isolated legacy AI fixture";return false;}
  struct Release {uint8_t* base;~Release(){VirtualFree(base,0,MEM_RELEASE);}} release{base};
  if(!VirtualAlloc(base,0x10000,MEM_COMMIT,PAGE_READWRITE) || !VirtualAlloc(base+0x833D0000,0x10000,MEM_COMMIT,PAGE_READWRITE)){error="Could not commit legacy AI fixture";return false;}
  auto library=std::make_shared<RegisteredBlueprintLibrary>();library->guest_base=base;library->index[{1,0,0,0}]=0x6000;
  PPC_STORE_U32(0x4004,1);PPC_STORE_U32(0x4058,0x5000);PPC_STORE_U8(0x833DC718,5);PPC_STORE_U8(0x5005,255);
  PPCContext ctx{};ctx.r3.u64=0x3ff8;ctx.lr=0x827F1CBC;ctx.fpscr.InitHost();const auto before=ctx;
  if(TryLegacyBodylessAi(ctx,base)){error="Ordinary actor was intercepted without original scope";return false;}
  RegisteredLevelActorScope scope(library);const auto csr=_mm_getcsr();SetLastError(0x1234);
  auto expected=before;expected.r3.u64=0;
  if(!TryLegacyBodylessAi(ctx,base) || std::memcmp(&ctx,&expected,sizeof ctx) || _mm_getcsr()!=csr || GetLastError()!=0x1234){error="Original no-character AI branch changed caller/host state";return false;}
  ctx=before;PPC_STORE_U8(0x5005,1);
  if(TryLegacyBodylessAi(ctx,base) || std::memcmp(&ctx,&before,sizeof ctx)){error="Actor with character physics was intercepted";return false;}
  PPC_STORE_U8(0x5005,255);PPC_STORE_U32(0x4004,2);
  if(TryLegacyBodylessAi(ctx,base)){error="Foreign actor identity was intercepted";return false;}
  PPC_STORE_U32(0x4004,1);ctx.lr=0x1234;
  if(TryLegacyBodylessAi(ctx,base)){error="Noninitialization AI caller was intercepted";return false;}
  ctx=before;{RegisteredLevelActorScope nested(nullptr);if(TryLegacyBodylessAi(ctx,base)){error="Nested ordinary loader retained original scope";return false;}}
  if(!TryLegacyBodylessAi(ctx,base)){error="Nested source actor scope was not restored";return false;}
  error.clear();return true;
}
}
