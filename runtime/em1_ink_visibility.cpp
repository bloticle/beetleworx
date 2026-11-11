#include "em1_ink_restoration.h"
#include "native_engine.h"
#include "pc_settings.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <atomic>
#include <chrono>
#include <immintrin.h>
#include <windows.h>

PPC_EXTERN_FUNC(__imp__sub_829FC0D0); // ScriptEntity interface -> native player controller
PPC_EXTERN_FUNC(__imp__sub_827B6618); // ChildComponent.GetChildCount
PPC_EXTERN_FUNC(__imp__sub_82A34C18); // PlayerSetDripsVisible native implementation
REXCVAR_DEFINE_BOOL(em2_restore_ink_drips,false,"Gameplay","Restore Mickey's original EM1 ink ribbons from the mounted Wii image");
REXCVAR_DEFINE_BOOL(em1_qa_ink_cycle,false,"Debug","Private profile only: cycle the real ink menu option On/Off/On after native children load");
REXCVAR_DECLARE(bool,em2_private_profile);

namespace {
std::atomic<bool> mounted{false}, authored_visible{true};
std::atomic<uint32_t> tracked_controller{0};
std::atomic<uint64_t> last_retail_request{0};
struct Restore {
  PPCContext& context;
  PPCContext original;
  unsigned mxcsr;
  DWORD last_error;
  explicit Restore(PPCContext& c):context(c),original(c),mxcsr(_mm_getcsr()),last_error(GetLastError()){}
  ~Restore(){context=original;_mm_setcsr(mxcsr);SetLastError(last_error);}
};
}
void em2::em1::SetOriginalInkPakMounted(bool value){mounted.store(value,std::memory_order_release);}
bool em2::em1::OriginalInkPakMounted(){return mounted.load(std::memory_order_acquire);}

void em2::em1::UpdateOriginalInkDrips(PPCContext& ctx,uint8_t* base,const NativeActor& mickey){
  // Called by the engine thread with its freshly resolved slot-zero ScriptEntity.
  static uint64_t identity=0;
  static uint32_t actor=0;
  static uint32_t observed_count=UINT32_MAX;
  static bool applied=false,setting=false;
  using Clock=std::chrono::steady_clock;static Clock::time_point qa_started{};static uint32_t qa_actor=0;
  if(!mounted.load(std::memory_order_acquire) || !mickey.ready || !mickey.entity){
    identity=0;actor=0;applied=false;tracked_controller.store(0);return;
  }
  if(REXCVAR_GET(em1_qa_ink_cycle) && REXCVAR_GET(em2_private_profile) && qa_started!=Clock::time_point{} && qa_actor==mickey.entity) {
    const auto elapsed=std::chrono::duration_cast<std::chrono::seconds>(Clock::now()-qa_started).count();
    const int wanted=elapsed<3 || elapsed>=6?1:0;
    if(PCSetting(kPCOriginalInkDrips)!=wanted)ChangePCSetting(kPCOriginalInkDrips,wanted);
  }
  const bool enabled=REXCVAR_GET(em2_restore_ink_drips);
  const bool changed=identity!=mickey.identity || actor!=mickey.entity;
  if(applied && !changed && setting==enabled)return;
  Restore restore(ctx);
  ctx.r3.u64=mickey.entity;__imp__sub_829FC0D0(ctx,base);
  const auto controller=ctx.r3.u32;if(!controller)return;
  if(changed){
    identity=mickey.identity;actor=mickey.entity;applied=false;
    const auto previous=last_retail_request.load();
    authored_visible.store(uint32_t(previous>>32)==controller?bool(uint32_t(previous)):true);
    tracked_controller.store(controller);observed_count=UINT32_MAX;
  }
  // The stock implementation walks Child Entities. During respawn the player
  // may be ready before its child factory has populated this list.
  const auto types=PPC_LOAD_U32(mickey.entity+88),components=PPC_LOAD_U32(mickey.entity+36);
  if(!types || !components)return;
  constexpr uint32_t child_type=0x833E0000u-18288;
  const auto slot=PPC_LOAD_U8(types+PPC_LOAD_U8(child_type));
  if(slot==255)return;
  const auto face=PPC_LOAD_U32(components+uint32_t(slot)*4);
  if(face<8)return;
  ctx.r3.u64=face-8;__imp__sub_827B6618(ctx,base);
  const auto count=ctx.r3.u32;
  if(count!=observed_count && REXCVAR_GET(em2_private_profile)){
    REXLOG_INFO("[em1] Original ink player {:08X} child roster {} (expected at least 19)",mickey.entity,count);
    observed_count=count;
  }
  if(count<19)return;
  if(REXCVAR_GET(em1_qa_ink_cycle) && REXCVAR_GET(em2_private_profile) && qa_actor!=mickey.entity){qa_actor=mickey.entity;qa_started=Clock::now();}
  ctx.r3.u64=controller;ctx.r4.u64=enabled && authored_visible.load();ctx.r5.u64=0;
  __imp__sub_82A34C18(ctx,base);
  setting=enabled;applied=true;
  REXLOG_INFO("[em1] Original Mickey ink setting applied to native player {:08X}, {} children, enabled={}",mickey.entity,count,enabled);
}

PPC_FUNC_IMPL(sub_82A34C18){
  // The retail game requests hiding during death/cinematics and showing after
  // respawn. Preserve its intent independently of the player's cosmetic setting.
  if(mounted.load(std::memory_order_acquire) && ctx.r3.u32){
    const bool visible=ctx.r4.u8!=0;
    last_retail_request.store((uint64_t(ctx.r3.u32)<<32)|uint32_t(visible));
    if(ctx.r3.u32==tracked_controller.load()){
      authored_visible.store(visible);
      if(!REXCVAR_GET(em2_restore_ink_drips))ctx.r4.u64=0;
    }
  }
  __imp__sub_82A34C18(ctx,base);
}
