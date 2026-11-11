#include "ppc_context.h"
#include "em1_native_effect_trace.h"
#include "em1_native_resources.h"
#include <rex/logging.h>
#include <immintrin.h>
#include <string>
#include <atomic>
#include <rex/cvar.h>
#include <windows.h>
#include <bit>
#include <array>
#include <set>
#include <mutex>
#include <map>
#include <cstdio>

REXCVAR_DECLARE(bool, em1_masked_material_candidate);
REXCVAR_DECLARE(bool, em1_cutout_material_candidate);
PPC_EXTERN_FUNC(__imp__sub_825874E8);
PPC_EXTERN_FUNC(sub_825874E8);

namespace {
std::atomic_bool mask_effect_loaded{false},blend_effect_loaded{false},dual_effect_loaded{false},cutout_effect_loaded{false};
std::mutex mesh_mutex;
std::set<uint32_t> imported_meshes;
std::map<uint32_t,std::pair<uint32_t,uint32_t>> last_classification;
unsigned transition_count=0;
std::atomic_uint64_t cull_application_calls{0};
std::atomic_uint64_t imported_cull_submissions{0};
bool ReadWord(uint8_t* base,uint32_t address,uint32_t& value){
  SIZE_T read=0;
  if(!address || !ReadProcessMemory(GetCurrentProcess(),base+address,&value,4,&read)||read!=4)return false;
  value=std::byteswap(value);return true;
}

}
PPC_EXTERN_FUNC(sub_825BB568);
PPC_EXTERN_FUNC(__imp__sub_825BB568);
// Read-only evidence of what the original NiStencilProperty loader receives.
PPC_FUNC(sub_825BB568) {
  const auto property=ctx.r3.u32;
  const bool observe=REXCVAR_GET(em1_cutout_material_candidate) && em2::em1::NativeModelPublicationActive();
  __imp__sub_825BB568(ctx,base);
  if(!observe)return;
  const auto csr=_mm_getcsr();const auto error=GetLastError();
  try {
    static std::atomic_uint count{0};uint32_t flags=0;
    if(count.fetch_add(1)<32 && ReadWord(base,property+28,flags))
      REXLOG_INFO("[em1] native stencil loaded property={:08X} flags={:04X} drawMode={}",property,flags>>16,(flags>>26)&3);
  }catch(...){}
  _mm_setcsr(csr);SetLastError(error);
}

PPC_EXTERN_FUNC(sub_82552E90);
PPC_EXTERN_FUNC(__imp__sub_82552E90);
PPC_FUNC(sub_82552E90) {
  const auto object=ctx.r3.u32,type=ctx.r4.u32;const auto caller=uint32_t(ctx.lr);
  __imp__sub_82552E90(ctx,base);
  if(type!=7 || !REXCVAR_GET(em1_cutout_material_candidate))return;
  const auto csr=_mm_getcsr();const auto error=GetLastError();
  try {
    std::lock_guard lock(mesh_mutex);
    static std::set<std::array<uint32_t,3>> observed;
    if(imported_meshes.contains(object) && observed.size()<128 && observed.insert({object,ctx.r3.u32,caller}).second) {
      uint32_t flags=0;ReadWord(base,ctx.r3.u32+28,flags);
      REXLOG_INFO("[em1] native stencil lookup mesh={:08X} property={:08X} flags={:04X} drawMode={} caller={:08X}",
          object,ctx.r3.u32,flags>>16,(flags>>26)&3,caller);
    }
  }catch(...){}
  _mm_setcsr(csr);SetLastError(error);
}
bool em2::em1::NativeEm1BlendEffectsLoaded(){return mask_effect_loaded.load() && blend_effect_loaded.load();}
PPC_EXTERN_FUNC(sub_8266E148);
PPC_EXTERN_FUNC(__imp__sub_8266E148);
PPC_FUNC(sub_8266E148) {
  const auto owner=ctx.r3.u32,record=ctx.r4.u32,caller=uint32_t(ctx.lr);
  __imp__sub_8266E148(ctx,base);
  if(!REXCVAR_GET(em1_cutout_material_candidate))return;
  const auto csr=_mm_getcsr();const auto error=GetLastError();
  try {
    uint32_t type=0,value=0;
    if(ReadWord(base,record,type) && type==3 && ReadWord(base,record+8,value)) {
      std::lock_guard lock(mesh_mutex);
      if(!imported_meshes.empty())++imported_cull_submissions;
      static std::set<std::array<uint32_t,4>> observed;
      if(observed.size()<64 && observed.insert({owner,caller,value,uint32_t(!imported_meshes.empty())}).second)
        REXLOG_INFO("[em1] native cull submission owner={:08X} record={:08X} value={} caller={:08X} applied={} imported={}",owner,record,value,caller,ctx.r3.u32,!imported_meshes.empty());
      const auto mesh=caller==0x829C4928 ? ctx.r31.u32 : caller==0x8324D6D8 ? ctx.r30.u32 : 0;
      static std::set<std::pair<uint32_t,uint32_t>> meshes;
      if(mesh && !imported_meshes.empty() && meshes.size()<256 && meshes.insert({mesh,caller}).second) {
        uint32_t name=0,state=0,stencil=0,bits=0;std::array<char,96> label{};SIZE_T read=0;
        // Retail NiObjectNET name accessor (8257DE98) returns object + 0xC.
        ReadWord(base,mesh+12,name);
        if(name)ReadProcessMemory(GetCurrentProcess(),base+name,label.data(),label.size()-1,&read);
        ReadWord(base,mesh+0xF0,state);ReadWord(base,state+0x24,stencil);ReadWord(base,stencil+28,bits);
        REXLOG_INFO("[em1] cull mesh identity mesh={:08X} name={} tracked={} caller={:08X} requested={} stencil={:08X} flags={:04X}",mesh,label.data(),imported_meshes.contains(mesh),caller,value,stencil,bits>>16);
        if((bits>>16)==0x4D80) {
          // NiMesh stream-ref array/count established by retail 825F7ED8.
          uint32_t refs=0,count=0;
          if(ReadWord(base,mesh+336,refs) && ReadWord(base,mesh+344,count) && count<=16)
            for(uint32_t slot=0;slot<count;++slot) {
              uint32_t reference=0,stream=0;
              if(!ReadWord(base,refs+slot*4,reference)||!ReadWord(base,reference,stream)||!stream)continue;
              std::string words;
              for(uint32_t offset=0;offset<64;offset+=4) {
                uint32_t word=0;if(!ReadWord(base,stream+offset,word))break;
                char text[10];std::snprintf(text,sizeof(text),"%08X/",word);words+=text;
              }
              REXLOG_INFO("[em1] cutout CPU stream mesh={:08X} slot={} ref={:08X} stream={:08X} words={}",mesh,slot,reference,stream,words);
            }
        }
      }
    }
  }catch(...){}
  _mm_setcsr(csr);SetLastError(error);
}
PPC_EXTERN_FUNC(sub_830FD7E8);
PPC_EXTERN_FUNC(__imp__sub_830FD7E8);
PPC_FUNC(sub_830FD7E8) {
  const auto owner=ctx.r3.u32,record=ctx.r4.u32,caller=uint32_t(ctx.lr);
  __imp__sub_830FD7E8(ctx,base);
  if(!REXCVAR_GET(em1_cutout_material_candidate))return;
  const auto csr=_mm_getcsr();const auto error=GetLastError();
  try {
    uint32_t type=0,enabled=0,value=0;
    if(ReadWord(base,record,type) && type==3 && ReadWord(base,record+4,enabled) && ReadWord(base,record+8,value)) {
      std::lock_guard lock(mesh_mutex);
      static std::set<std::array<uint32_t,4>> observed;
      if(observed.size()<128 && observed.insert({owner,record,caller,value}).second)
        REXLOG_INFO("[em1] native material cull record owner={:08X} record={:08X} enabled={} value={} caller={:08X} r29={:08X} r30={:08X} r31={:08X} imported={}",owner,record,enabled>>24,value,caller,ctx.r29.u32,ctx.r30.u32,ctx.r31.u32,!imported_meshes.empty());
    }
  }catch(...){}
  _mm_setcsr(csr);SetLastError(error);
}
PPC_EXTERN_FUNC(sub_82679778);
PPC_EXTERN_FUNC(__imp__sub_82679778);
// Native pass setup applies stencil-derived culling only behind this byte gate.
// Observe the original function without forcing the gate or changing state.
PPC_FUNC(sub_82679778) {
  const auto parameters=ctx.r4.u32;
  __imp__sub_82679778(ctx,base);
  if(!REXCVAR_GET(em1_cutout_material_candidate))return;
  ++cull_application_calls;
  const auto csr=_mm_getcsr();const auto error=GetLastError();
  try {
    uint32_t state=0,kind=0,stencil=0,bits=0,gate=0;
    if(ReadWord(base,parameters+16,state) && ReadWord(base,state,kind) &&
        kind==0x82040E98 && ReadWord(base,state+36,stencil) &&
        ReadWord(base,stencil+28,bits) && (bits>>16)==0x4D80 &&
        ReadWord(base,0x83396F18,gate)) {
      std::lock_guard lock(mesh_mutex);
      static std::set<std::array<uint32_t,3>> observed;
      if(observed.size()<64 && observed.insert({state,stencil,gate>>24}).second)
        REXLOG_INFO("[em1] native cull application state={:08X} stencil={:08X} flags={:04X} gate={}",state,stencil,bits>>16,gate>>24);
    }
  }catch(...){}
  _mm_setcsr(csr);SetLastError(error);
}
PPC_EXTERN_FUNC(sub_826A7490);
PPC_EXTERN_FUNC(__imp__sub_826A7490);
PPC_FUNC(sub_826A7490) {
  const auto value=ctx.r4.u32,caller=uint32_t(ctx.lr);
  __imp__sub_826A7490(ctx,base);
  if(!REXCVAR_GET(em1_cutout_material_candidate))return;
  const auto csr=_mm_getcsr();const auto error=GetLastError();
  try {
    std::lock_guard lock(mesh_mutex);static std::set<std::pair<uint32_t,uint32_t>> observed;
    if(observed.size()<64 && observed.insert({caller,value}).second)
      REXLOG_INFO("[em1] native cull device setter caller={:08X} value={:08X}",caller,value);
  }catch(...){}
  _mm_setcsr(csr);SetLastError(error);
}
PPC_EXTERN_FUNC(sub_8266DF08);
PPC_EXTERN_FUNC(__imp__sub_8266DF08);
PPC_FUNC(sub_8266DF08) {
  const auto value=ctx.r4.u32&255,caller=uint32_t(ctx.lr);
  __imp__sub_8266DF08(ctx,base);
  if(!REXCVAR_GET(em1_cutout_material_candidate))return;
  const auto csr=_mm_getcsr();const auto error=GetLastError();
  try {
    std::lock_guard lock(mesh_mutex);static std::set<std::pair<uint32_t,uint32_t>> observed;
    if(observed.size()<64 && observed.insert({caller,value}).second)
      REXLOG_INFO("[em1] native cull gate setter caller={:08X} value={}",caller,value);
  }catch(...){}
  _mm_setcsr(csr);SetLastError(error);
}
bool em2::em1::NativeEm1DualUvEffectLoaded(){return dual_effect_loaded.load();}
bool em2::em1::NativeEm1CutoutEffectLoaded(){return cutout_effect_loaded.load();}
void em2::em1::ObserveEm1FinalMaterialFlags(uint8_t* base){
  if(!REXCVAR_GET(em1_masked_material_candidate))return;
  const auto csr=_mm_getcsr();const auto error=GetLastError();
  try{
    std::lock_guard lock(mesh_mutex);std::map<uint32_t,unsigned> counts;unsigned unavailable=0;
    for(const auto mesh:imported_meshes){uint32_t state=0,flags=0;
      if(ReadWord(base,mesh+284,state)&&state&&ReadWord(base,state+16,flags))++counts[flags];else ++unavailable;
      if(REXCVAR_GET(em1_cutout_material_candidate)) {
        uint32_t vtable=0,update=0,node=0;
        ReadWord(base,mesh,vtable);ReadWord(base,vtable+128,update);
        ReadWord(base,mesh+0xD8,node);
        std::set<uint32_t> visited;
        while(node && visited.size()<32 && visited.insert(node).second) {
          uint32_t property=0,kind=0,bits=0,next=0;
          if(!ReadWord(base,node+8,property)||!ReadWord(base,property,kind))break;
          // Verified retail NiStencilProperty vtable; do not invoke guest code.
          if(kind==0x8203FFB0 && ReadWord(base,property+28,bits))
            REXLOG_INFO("[em1] settled own stencil mesh={:08X} property={:08X} flags={:04X} update={:08X}",mesh,property,bits>>16,update);
          if(!ReadWord(base,node,next))break;
          node=next;
        }
        // Search only the established NiMesh base extent, recognizing the
        // verified NiPropertyState vtable rather than guessing pointer fields.
        for(uint32_t offset=0x38;offset<=0x11C;offset+=4) {
          uint32_t pointer=0,kind=0,stencil=0,bits=0;
          if(ReadWord(base,mesh+offset,pointer) && pointer &&
              ReadWord(base,pointer,kind) && kind==0x82040E98 &&
              ReadWord(base,pointer+0x24,stencil) && stencil &&
              ReadWord(base,stencil+28,bits))
            REXLOG_INFO("[em1] settled cached stencil mesh={:08X} offset={:X} state={:08X} property={:08X} flags={:04X}",mesh,offset,pointer,stencil,bits>>16);
        }
      }
    }
    for(const auto& [flags,count]:counts)
      REXLOG_INFO("[em1] settled imported material flags={:08X} meshes={} eligibility-bit={} excluded-bit={}",flags,count,bool(flags&0x800),bool(flags&0x400));
    REXLOG_INFO("[em1] settled material observation imported={} unavailable={}",imported_meshes.size(),unavailable);
    if(REXCVAR_GET(em1_cutout_material_candidate))
      REXLOG_INFO("[em1] native stencil-derived cull application calls={} post-import state submissions={}",cull_application_calls.load(),imported_cull_submissions.load());
  }catch(...){}
  _mm_setcsr(csr);SetLastError(error);
}

// Read-only observation of the original mesh eligibility classifier. Never change
// its return value, material flags, or render state to manufacture eligibility.
PPC_FUNC(sub_825874E8) {
  const auto mesh=ctx.r3.u32;
  const auto caller=ctx.lr;
  __imp__sub_825874E8(ctx,base);
  if(!REXCVAR_GET(em1_masked_material_candidate))return;
  const auto csr=_mm_getcsr();const auto saved_error=GetLastError();
  try {
    std::lock_guard lock(mesh_mutex);
    const bool publishing=em2::em1::NativeModelPublicationActive();
    const bool first=publishing && imported_meshes.size()<256 && imported_meshes.insert(mesh).second;
    if(first || imported_meshes.contains(mesh)){
      auto word=[&](uint32_t address){uint32_t value=0;SIZE_T read=0;
        if(!ReadProcessMemory(GetCurrentProcess(),base+address,&value,4,&read)||read!=4)return uint32_t(0);
        return std::byteswap(value);};
      const auto state=word(mesh+284),flags=state?word(state+16):0;
      const auto previous=last_classification.find(mesh);
      const bool changed=previous==last_classification.end() || previous->second!=std::pair{state,flags};
      if(first || (changed && transition_count++<384))
        REXLOG_INFO("[em1] native mesh classifier mesh={:08X} result={} state={:08X} flags={:08X} caller={:08X} publishing={}",
            mesh,ctx.r3.u32,state,flags,uint32_t(caller),publishing);
      last_classification[mesh]={state,flags};
    }
  }catch(...){/* Diagnostics must not affect native classification. */}
  _mm_setcsr(csr);SetLastError(saved_error);
}

PPC_EXTERN_FUNC(__imp__sub_83096AB8);
PPC_EXTERN_FUNC(sub_83096AB8);
// Observe only the private EM1 effect candidates. Native loading, ownership,
// registration and shader creation remain entirely in the original function.
PPC_FUNC(sub_83096AB8) {
  std::string name;
  if(ctx.r3.u32) {
    const auto* source=reinterpret_cast<const char*>(base+ctx.r3.u32);
    for(unsigned i=0;i<260 && source[i];++i) name.push_back(source[i]);
  }
  auto key=name;for(auto& c:key)if(c>='A' && c<='Z')c+='a'-'A';
  const bool candidate=key.find("em1m_staticenv_blend.fxlo")!=std::string::npos ||
                       key.find("em1w_staticenv_blend.fxlo")!=std::string::npos ||
                       key.find("em1d_staticenv_blend.fxlo")!=std::string::npos ||
                       key.find("em1a_staticenv_blend.fxlo")!=std::string::npos;
  __imp__sub_83096AB8(ctx,base);
  if(key.find("em1m_staticenv_blend.fxlo")!=std::string::npos)
    mask_effect_loaded.store(ctx.r3.u32!=0);
  if(key.find("em1w_staticenv_blend.fxlo")!=std::string::npos)
    blend_effect_loaded.store(ctx.r3.u32!=0);
  if(key.find("em1d_staticenv_blend.fxlo")!=std::string::npos)
    dual_effect_loaded.store(ctx.r3.u32!=0);
  if(key.find("em1a_staticenv_blend.fxlo")!=std::string::npos)
    cutout_effect_loaded.store(ctx.r3.u32!=0);
  if(candidate) {
    const auto csr=_mm_getcsr();
    REXLOG_INFO("[em1] native effect loader {} returned {:08X}",name,ctx.r3.u32);
    _mm_setcsr(csr);
  }
}

