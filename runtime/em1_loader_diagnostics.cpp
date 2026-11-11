#include "ppc_context.h"
#include "em1_preview_runtime.h"
#include "em1_single_gsa.h"
#include "em1_loader_diagnostics.h"
#include "em1_level_entry.h"
#include "em1_ai_compat.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <bit>
#include <cstdint>
#include <string>

PPC_EXTERN_FUNC(__imp__sub_8289F5A0);
PPC_EXTERN_FUNC(__imp__sub_82877C98);
PPC_EXTERN_FUNC(__imp__sub_82732A18);
PPC_EXTERN_FUNC(__imp__sub_82788710);
REXCVAR_DEFINE_BOOL(em1_trace_loader_pending,false,"Debug",
    "Bounded read-only pending-loader observations during isolated EM1 entry");
REXCVAR_DEFINE_BOOL(em1_trace_camera_snapshot,false,"Debug",
    "Bounded EM1 readiness/standing camera identity, world pose and native frustum snapshots");

namespace {
struct Restore {
 PPCContext& context;PPCContext saved;DWORD error;unsigned csr;
 explicit Restore(PPCContext& c):context(c),saved(c),error(GetLastError()),csr(_mm_getcsr()){}
 ~Restore(){context=saved;SetLastError(error);_mm_setcsr(csr);}
};
bool Mapped(uint8_t* base,uint32_t address,size_t size) {
 if(!base || address<4096 || !size || uint64_t(address)+size>0x100000000ull)return false;
 auto at=reinterpret_cast<uintptr_t>(base+address);const auto end=at+size;
 while(at<end){MEMORY_BASIC_INFORMATION info{};
  if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info)) || info.State!=MEM_COMMIT ||
     (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
  const auto access=info.Protect&255;
  if(access!=PAGE_READONLY && access!=PAGE_READWRITE && access!=PAGE_WRITECOPY &&
     access!=PAGE_EXECUTE_READ && access!=PAGE_EXECUTE_READWRITE && access!=PAGE_EXECUTE_WRITECOPY)return false;
  const auto next=reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;
  if(next<=at)return false;at=std::min(end,next);
 }return true;
}
std::string Text(uint8_t* base,uint32_t at) {
 std::string text;for(unsigned i=0;i<160;++i){if(!Mapped(base,at+i,1))return "<unmapped>";
  const auto c=base[at+i];if(!c)return text;if(c<32 || c>126)return "<nontext>";text+=char(c);}
 return text+"<bounded>";
}
void Observe(PPCContext& ctx,uint8_t* base,uint32_t loader,bool pending) {
 Restore restore(ctx);
 if(!REXCVAR_GET(em1_trace_loader_pending))return;
 const auto& entry=em2::em1::ActiveSingleGsaEntry();
 struct Rate {bool loading=false;uint32_t loader=0,count=0;ULONGLONG next=0;};
 static thread_local Rate rate;
 if(!entry.loading && !em2::em1::RegisteredLevelEntryProbeRequested()){rate={};return;}
 if(!rate.loading || rate.loader!=loader)rate={true,loader,0,0};
 const auto now=GetTickCount64();if(rate.count>=90 || now<rate.next)return;
 ++rate.count;rate.next=now+1000;
 auto Word=[&](uint32_t at){return Mapped(base,at,4)?PPC_LOAD_U32(at):UINT32_MAX;};
 auto Byte=[&](uint32_t at){return Mapped(base,at,1)?uint32_t(base[at]):UINT32_MAX;};
 if(!Mapped(base,loader,968)){REXLOG_INFO("[em1] loader pending probe: loader={:08X} unmapped pending={}",loader,pending);return;}
 const auto inner=Word(loader+8),active=Word(loader+960),secondary=Word(loader+964);
 const auto manager=Word(0x833da818),packstate=manager!=UINT32_MAX?Word(manager+532):UINT32_MAX;
 REXLOG_INFO("[em1] loader pending #{} owner={:08X} result={} inner={:08X} flags156/181={}/{} queued688/completed692={}/{} queued952/completed956={}/{} active960={:08X} secondary964={:08X} phase92={} owner376={:08X} packstate={:08X}/{} source_requested={}",
  rate.count,loader,pending,inner,Byte(inner+156),Byte(inner+181),Word(loader+688),Word(loader+692),Word(loader+952),Word(loader+956),active,secondary,Word(loader+92),Word(loader+376),packstate,Word(packstate+296),entry.source_requested);
 for(const auto request:{active,secondary})if(request && request!=UINT32_MAX && Mapped(base,request,32))
  REXLOG_INFO("[em1] loader request={:08X} kind4={} state8={} status12={} payload20={:08X} prerequisite24='{}' resource28='{}'",
   request,Word(request+4),Word(request+8),Word(request+12),Word(request+20),Text(base,Word(request+24)),Text(base,Word(request+28)));
 const auto phase=Word(loader+92),step=phase<16?Word(loader+380+phase*4):UINT32_MAX;
 const auto table=step && step!=UINT32_MAX?Word(step):UINT32_MAX;
 REXLOG_INFO("[em1] loader phase={} step={:08X} vtable={:08X} update={:08X}",phase,step,table,table!=UINT32_MAX?Word(table+4):UINT32_MAX);
 const auto scene=Word(0x833db67c);
 REXLOG_INFO("[em1] loader global scene_manager={:08X} decode124={} phase_callback={:08X}",scene,Byte(scene+124),step!=UINT32_MAX?Word(step+4):UINT32_MAX);
 if(inner && inner!=UINT32_MAX && Mapped(base,inner,280))
  REXLOG_INFO("[em1] inner loader current148={:08X} scene152={:08X} active160={:08X} stage164={} flags156/176/178/180/181={}/{}/{}/{}/{} pending276={}",
   Word(inner+148),Word(inner+152),Word(inner+160),Word(inner+164),Byte(inner+156),Byte(inner+176),Byte(inner+178),Byte(inner+180),Byte(inner+181),Word(inner+276));
 if(em2::em1::RegisteredLevelEntryProbeRequested() && inner && inner!=UINT32_MAX && Mapped(base,inner,280)){
  const auto job=Word(inner+160),table=Word(inner);
  REXLOG_INFO("[em1] full-entry inner vtable={:08X} slots0/4/8={:08X}/{:08X}/{:08X} job={:08X} words={:08X},{:08X},{:08X},{:08X},{:08X},{:08X},{:08X},{:08X}",
    table,Word(table),Word(table+4),Word(table+8),job,Word(job),Word(job+4),Word(job+8),Word(job+12),Word(job+16),Word(job+20),Word(job+24),Word(job+28));
 }
}
} // namespace

void em2::em1::ObserveSingleGsaRawLookup(PPCContext& ctx,uint8_t* base,uint32_t manager,uint32_t path) {
 Restore restore(ctx);
 try {
  if(!REXCVAR_GET(em1_trace_loader_pending))return;
  const auto& entry=ActiveSingleGsaEntry();
  static thread_local uint32_t count=0;static thread_local ULONGLONG next=0;
  if(!entry.loading && !RegisteredLevelEntryProbeRequested()){count=0;next=0;return;}
  const auto now=GetTickCount64();if(count>=30 || now<next)return;++count;next=now+1000;
  const auto first=Mapped(base,path,4)?PPC_LOAD_U32(path):UINT32_MAX;
  REXLOG_INFO("[em1] raw lookup #{} manager={:08X} path_ptr={:08X} actual='{}' first_word={:08X} indirect='{}' expected='{}'",
   count,manager,path,Text(base,path),first,Text(base,first),entry.scene_name);
 }catch(...){/* An opt-in observation cannot change a native lookup. */}
}

void em2::em1::ObserveArchitectureCameraSnapshot(PPCContext& ctx,uint8_t* base,uint32_t controller,std::string_view phase) {
 Restore restore(ctx);
 try {
  static unsigned count=0;
  if(!REXCVAR_GET(em1_trace_camera_snapshot) || count>=16 || !ActiveSingleGsaEntry().entered)return;
  ++count;
  auto Word=[&](uint32_t at){return Mapped(base,at,4)?PPC_LOAD_U32(at):UINT32_MAX;};
  auto Float=[&](uint32_t at){return std::bit_cast<float>(Word(at));};
  const auto registry=Word(0x833df47c),core=Word(0x833da7e4);
  const auto slot=Word(controller+16),raw=slot<2?Word(registry+4+slot*4):0;
  const auto registered=raw && raw!=UINT32_MAX && Mapped(base,raw,104)?raw+8:0;
  uint32_t controlled=0;
  if(Mapped(base,controller,580) && core && core!=UINT32_MAX && Mapped(base,core,32) &&
      ctx.r1.u32>=32768 && Mapped(base,ctx.r1.u32-32768,32768)){
   ctx.r1.u64=uint64_t(restore.saved.r1.u32)-2048;ctx.r3.u32=core;ctx.r4.u32=controller+20;
   __imp__sub_82732A18(ctx,base);controlled=ctx.r3.u32;
  }
  REXLOG_INFO("[em1] camera snapshot #{} '{}' controller={:08X} vtable={:08X} slot={} active/default={:08X}/{:08X} camera_handle={:08X} resolved={:08X} registry={:08X} registered_entity={:08X} same={}",
   count,std::string(phase.substr(0,96)),controller,Word(controller),slot,Word(controller+24),Word(controller+392),Word(controller+20),controlled,registry,registered,controlled && controlled==registered);
  REXLOG_INFO("[em1] camera cached pose: position={}/{}/{} fov={} transition28/32/36={:08X}/{:08X}/{:08X}",
   Float(controller+96),Float(controller+100),Float(controller+104),Float(controller+112),Word(controller+28),Word(controller+32),Word(controller+36));
  // Known, audited ScriptEntity->JPSSceneGraphComponent->NiAVObject path.
  // A mapped arbitrary component word is NOT sufficient evidence of ownership.
  auto Graph=[&](uint32_t entity){
   if(!entity || !Mapped(base,entity,96) || !Mapped(base,0x833db0ec,1))return uint32_t(0);
   const auto types=Word(entity+88),components=Word(entity+36);const auto type=base[0x833db0ec];
   if(!Mapped(base,types,uint32_t(type)+1))return uint32_t(0);const auto index=base[types+type];
   if(index==255 || !Mapped(base,components+uint32_t(index)*4,4))return uint32_t(0);
   const auto face=Word(components+uint32_t(index)*4);if(face<8 || !Mapped(base,face-8,152))return uint32_t(0);
   return Word(face-8+88);
  };
  const auto controlled_root=Graph(controlled),registered_root=Graph(registered);
  REXLOG_INFO("[em1] camera graph roots: controlled={:08X} vtable={:08X} registered={:08X} vtable={:08X}",
   controlled_root,Word(controlled_root),registered_root,Word(registered_root));
  auto Camera=[&](uint32_t object,const char* owner){
   if(!object || !Mapped(base,object,368) || Word(object)!=0x8203e610){
    REXLOG_INFO("[em1] {} camera graph root is not an audited direct NiCamera; frustum unresolved",owner);return;}
   REXLOG_INFO("[em1] {} native NiCamera={:08X} world_position={}/{}/{} frustum L/R/T/B={}/{}/{}/{} near/far={}/{} ortho={} minNear/maxRatio={}/{}",
    owner,object,Float(object+192),Float(object+196),Float(object+200),Float(object+304),Float(object+308),Float(object+312),Float(object+316),Float(object+320),Float(object+324),base[object+328],Float(object+332),Float(object+336));
  };
  Camera(registered_root,"registered");if(controlled_root!=registered_root)Camera(controlled_root,"controlled");
 }catch(...){/* Diagnostics may not replace gameplay behavior. */}
}

PPC_FUNC_IMPL(sub_8289F5A0) {
 const auto loader=ctx.r3.u32;
 // Original behavior and exceptional exits remain outside diagnostic handling.
 __imp__sub_8289F5A0(ctx,base);
 try {Observe(ctx,base,loader,(ctx.r3.u32&255)!=0);}catch(...){/* Observation cannot fail the original load. */}
}

PPC_FUNC_IMPL(sub_82788710){
 try{
  Restore restore(ctx);
  if(em2::em1::RegisteredLevelEntryProbeRequested()){
   const auto owner=ctx.r4.u32;
   if(Mapped(base,owner,128) && Mapped(base,0x833DDF1C,1)){
    const auto types=PPC_LOAD_U32(owner+88);const auto type=base[0x833DDF1C];
    const auto index=Mapped(base,types+type,1)?base[types+type]:255;
    if(index==255){
     REXLOG_INFO("[em1] movement missing InAirState: controller={:08X} owner={:08X} type={} owner_name24='{}' owner_name56='{}' words4/8/12/16/20/24/28/32={:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}",
      ctx.r3.u32,owner,type,Text(base,PPC_LOAD_U32(owner+24)),Text(base,PPC_LOAD_U32(owner+56)),
      PPC_LOAD_U32(owner+4),PPC_LOAD_U32(owner+8),PPC_LOAD_U32(owner+12),PPC_LOAD_U32(owner+16),PPC_LOAD_U32(owner+20),PPC_LOAD_U32(owner+24),PPC_LOAD_U32(owner+28),PPC_LOAD_U32(owner+32));
     const auto components=PPC_LOAD_U32(owner+36),count=PPC_LOAD_U32(owner+40);
     if(count<=256 && Mapped(base,components,count*4))for(uint32_t i=0;i<count;++i){
      const auto face=PPC_LOAD_U32(components+i*4);if(face>=8 && Mapped(base,face-8,64))
       REXLOG_INFO("[em1] movement owner component {}: object={:08X} class='{}' definition={:08X}",i,face-8,Text(base,PPC_LOAD_U32(face-8+56)),PPC_LOAD_U32(face-8+60));
     }
    }
   }
  }
 }catch(...){/* Observe the failure without changing native controller behavior. */}
 __imp__sub_82788710(ctx,base);
}


// Observe only the failing source-owned AI prerequisite. Never fabricate a
// physics body, skip AI initialization or change the native getter's result.
PPC_FUNC_IMPL(sub_82877C98){
 if(em2::em1::TryLegacyBodylessAi(ctx,base)){
  REXLOG_INFO("[em1] original bodyless AI navigation branch: source actor has no character controller; no walking agent created");return;
 }
 if(em2::em1::RegisteredLevelEntryProbeRequested())try{
  Restore restore(ctx);const auto owner=ctx.r3.u32+8;
  if(Mapped(base,owner,96) && Mapped(base,0x833DC718,1)){
   const auto types=PPC_LOAD_U32(owner+88);const auto type=base[0x833DC718];
   const auto slot=Mapped(base,types+type,1)?base[types+type]:255;
   const auto components=PPC_LOAD_U32(owner+36),count=PPC_LOAD_U32(owner+40);
   const auto physics=slot!=255 && Mapped(base,components+slot*4,4)?PPC_LOAD_U32(components+slot*4):0;
   if(!physics){
    REXLOG_ERROR("[em1] AI startup lacks character physics: object={:08X} entity={:08X} name20='{}' name24='{}' guid={:08X}/{:08X}/{:08X}/{:08X} type={} slot={} components={}",ctx.r3.u32,owner,Text(base,PPC_LOAD_U32(owner+20)),Text(base,PPC_LOAD_U32(owner+24)),PPC_LOAD_U32(owner+4),PPC_LOAD_U32(owner+8),PPC_LOAD_U32(owner+12),PPC_LOAD_U32(owner+16),type,slot,count);
    if(count<=256 && Mapped(base,components,count*4))for(uint32_t i=0;i<count;++i){
     const auto face=PPC_LOAD_U32(components+i*4);if(face>=8 && Mapped(base,face-8,64))
      REXLOG_INFO("[em1] AI owner component {}: object={:08X} class='{}' definition={:08X}",i,face-8,Text(base,PPC_LOAD_U32(face-8+56)),PPC_LOAD_U32(face-8+60));
    }
   }
  }
 }catch(...){/* Diagnostic failure must leave the original call untouched. */}
 __imp__sub_82877C98(ctx,base);
}
