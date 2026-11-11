#include "transfer_benchmark.h"
#include "direct_color_resolve.h"
#include "pc_settings.h"
#include <array>
#include "stencil_zero_proof.h"
#include "stencil_transfer.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <chrono>
#include <immintrin.h>
REXCVAR_DECLARE(bool,em2_distance_shadows);
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(std::string,em2_user_data_directory);
REXCVAR_DEFINE_BOOL(em2_benchmark_distance_shadows,false,"Debug","Private only: alternate distance-shadow resolution to verify live rebind/retirement and restoration; no gameplay input");
REXCVAR_DECLARE(bool,em2_direct_color_resolve);
REXCVAR_DEFINE_BOOL(em2_benchmark_direct_color_resolve,false,"Debug","Compare guarded direct color resolve at unchanged graphics settings");
REXCVAR_DECLARE(bool,execute_unclipped_draw_vs_on_cpu);
REXCVAR_DEFINE_BOOL(em2_benchmark_draw_extent,false,"Debug","Compare native CPU estimation of unclipped draw extents at unchanged graphics settings");
REXCVAR_DECLARE(bool,em2_tight_transfer_scissors);
REXCVAR_DECLARE(bool,em2_skip_proven_zero_stencil);
REXCVAR_DECLARE(bool,em2_stencil_plane_copy);
REXCVAR_DECLARE(bool,em2_skip_stencil_scratch_clear);
REXCVAR_DECLARE(bool,em2_stencil_plane_barriers);
REXCVAR_DECLARE(bool,em2_coalesce_stencil_copies);
REXCVAR_DECLARE(bool,em2_bounded_transfer_division);
REXCVAR_DECLARE(bool,em2_fuse_depth_stencil_transfers);
REXCVAR_DEFINE_BOOL(em2_benchmark_fused_depth_stencil,false,"Debug","Compare combined depth/stencil transfer draws at unchanged rendering quality");
REXCVAR_DEFINE_BOOL(em2_benchmark_bounded_division,false,"Debug","Compare exact bounded tile-address division Off/On with unchanged resolution, lighting, HDR and sample counts");
REXCVAR_DEFINE_BOOL(em2_benchmark_stencil_copies,false,"Debug","Compare exact-union stencil copy coalescing Off/On at unchanged graphics settings");
REXCVAR_DEFINE_BOOL(em2_benchmark_stencil_barriers,false,"Debug","Alternate stencil-only barriers Off/On; four 15-second windows after initial settling, restore original flag");
REXCVAR_DEFINE_BOOL(em2_benchmark_stencil_clear,false,"Debug","Alternate native stencil scratch clear omission Off/On; four 15-second windows, restore original flag");
REXCVAR_DEFINE_BOOL(em2_benchmark_zero_stencil,false,"Debug","Alternate proven-zero skipping Off/On; four 15-second windows, no gameplay input, restore original flag");
REXCVAR_DEFINE_BOOL(em2_benchmark_transfer_scissors,false,"Debug","Alternate transfer clipping Off/On; four 15-second windows, no gameplay input, restore original flag");
REXCVAR_DEFINE_BOOL(em2_benchmark_stencil_value,false,"Debug","Alternate native R8 stencil transfers Off/On; four 15-second windows, no gameplay input, restore original flag");
namespace {
enum class Target {Clip,Zero,Value,Clear,Plane,Merge,Division,Fusion,Extent,Direct,Distance};
bool Active(Target target){switch(target){case Target::Distance:return REXCVAR_GET(em2_benchmark_distance_shadows);case Target::Clip:return REXCVAR_GET(em2_benchmark_transfer_scissors);case Target::Zero:return REXCVAR_GET(em2_benchmark_zero_stencil);case Target::Value:return REXCVAR_GET(em2_benchmark_stencil_value);case Target::Clear:return REXCVAR_GET(em2_benchmark_stencil_clear);case Target::Plane:return REXCVAR_GET(em2_benchmark_stencil_barriers);case Target::Merge:return REXCVAR_GET(em2_benchmark_stencil_copies);case Target::Division:return REXCVAR_GET(em2_benchmark_bounded_division);case Target::Direct:return REXCVAR_GET(em2_benchmark_direct_color_resolve);case Target::Extent:return REXCVAR_GET(em2_benchmark_draw_extent);case Target::Fusion:return REXCVAR_GET(em2_benchmark_fused_depth_stencil);}return false;}
bool Option(Target target){switch(target){case Target::Distance:return REXCVAR_GET(em2_distance_shadows);case Target::Clip:return REXCVAR_GET(em2_tight_transfer_scissors);case Target::Zero:return REXCVAR_GET(em2_skip_proven_zero_stencil);case Target::Value:return REXCVAR_GET(em2_stencil_plane_copy);case Target::Clear:return REXCVAR_GET(em2_skip_stencil_scratch_clear);case Target::Plane:return REXCVAR_GET(em2_stencil_plane_barriers);case Target::Merge:return REXCVAR_GET(em2_coalesce_stencil_copies);case Target::Division:return REXCVAR_GET(em2_bounded_transfer_division);case Target::Direct:return REXCVAR_GET(em2_direct_color_resolve);case Target::Extent:return REXCVAR_GET(execute_unclipped_draw_vs_on_cpu);case Target::Fusion:return REXCVAR_GET(em2_fuse_depth_stencil_transfers);}return false;}
void SetOption(Target target,bool value){switch(target){case Target::Distance:REXCVAR_SET(em2_distance_shadows,value);break;case Target::Clip:REXCVAR_SET(em2_tight_transfer_scissors,value);break;case Target::Zero:REXCVAR_SET(em2_skip_proven_zero_stencil,value);break;case Target::Value:REXCVAR_SET(em2_stencil_plane_copy,value);break;case Target::Clear:REXCVAR_SET(em2_skip_stencil_scratch_clear,value);break;case Target::Plane:REXCVAR_SET(em2_stencil_plane_barriers,value);break;case Target::Merge:REXCVAR_SET(em2_coalesce_stencil_copies,value);break;case Target::Division:REXCVAR_SET(em2_bounded_transfer_division,value);break;case Target::Direct:REXCVAR_SET(em2_direct_color_resolve,value);break;case Target::Extent:REXCVAR_SET(execute_unclipped_draw_vs_on_cpu,value);break;case Target::Fusion:REXCVAR_SET(em2_fuse_depth_stencil_transfers,value);break;}}
void Finish(Target target){switch(target){case Target::Distance:REXCVAR_SET(em2_benchmark_distance_shadows,false);break;case Target::Clip:REXCVAR_SET(em2_benchmark_transfer_scissors,false);break;case Target::Zero:REXCVAR_SET(em2_benchmark_zero_stencil,false);break;case Target::Value:REXCVAR_SET(em2_benchmark_stencil_value,false);break;case Target::Clear:REXCVAR_SET(em2_benchmark_stencil_clear,false);break;case Target::Plane:REXCVAR_SET(em2_benchmark_stencil_barriers,false);break;case Target::Merge:REXCVAR_SET(em2_benchmark_stencil_copies,false);break;case Target::Division:REXCVAR_SET(em2_benchmark_bounded_division,false);break;case Target::Direct:REXCVAR_SET(em2_benchmark_direct_color_resolve,false);break;case Target::Extent:REXCVAR_SET(em2_benchmark_draw_extent,false);break;case Target::Fusion:REXCVAR_SET(em2_benchmark_fused_depth_stencil,false);break;}}
const char* Name(Target target){return target==Target::Distance?"Distance-shadow-lifetime":target==Target::Direct?"Direct-color-resolve":target==Target::Extent?"Draw-extent":target==Target::Fusion?"Fused-depth-stencil":target==Target::Division?"Bounded-division":target==Target::Merge?"Stencil-merge":target==Target::Plane?"Stencil-plane":target==Target::Clear?"Stencil-clear":target==Target::Value?"Stencil-value":target==Target::Zero?"Zero-stencil":"Transfer clipping";}
}
thread_local uint32_t em2::transfer_benchmark_draws=0;
void em2::TransferBenchmarkAtSwap(){
 using Clock=std::chrono::steady_clock;
 struct State {bool running=false,original=false,warming=true;Target target=Target::Clip;unsigned phase=0;uint64_t frames=0,draws=0,skipped_start=0,value_start=0,clear_start=0,plane_start=0,copy_start=0,saved_start=0,fused_start=0,direct_start=0;Clock::time_point start{};bool settings_captured=false;std::array<int,kPCOptionCount> settings{};};static thread_local State state;
 const auto draws=transfer_benchmark_draws;transfer_benchmark_draws=0;
 if(state.running && !Active(state.target)){SetOption(state.target,state.original);state={};}
 if(!Active(Target::Clip) && !Active(Target::Zero) && !Active(Target::Value) && !Active(Target::Clear) && !Active(Target::Plane) && !Active(Target::Merge) && !Active(Target::Division) && !Active(Target::Fusion) && !Active(Target::Extent) && !Active(Target::Direct) && !Active(Target::Distance))return;
 struct Fp{unsigned csr=_mm_getcsr();~Fp(){_mm_setcsr(csr);}}fp;const auto now=Clock::now();
 if(Active(Target::Distance) && (!REXCVAR_GET(em2_private_profile) || REXCVAR_GET(em2_user_data_directory).empty())){Finish(Target::Distance);REXLOG_WARN("Distance-shadow lifetime diagnostic requires an isolated private profile");return;}
 if(!state.running){state.target=Active(Target::Distance)?Target::Distance:Active(Target::Direct)?Target::Direct:Active(Target::Extent)?Target::Extent:Active(Target::Fusion)?Target::Fusion:Active(Target::Division)?Target::Division:Active(Target::Merge)?Target::Merge:Active(Target::Plane)?Target::Plane:Active(Target::Clear)?Target::Clear:Active(Target::Value)?Target::Value:Active(Target::Zero)?Target::Zero:Target::Clip;if((state.target==Target::Clear || state.target==Target::Plane || state.target==Target::Merge || state.target==Target::Fusion) && !Option(Target::Value)){Finish(state.target);REXLOG_WARN("{} A/B requires native R8 transfers enabled; benchmark not started",Name(state.target));state={};return;}state.running=true;state.original=Option(state.target);state.start=now;SetOption(state.target,false);REXLOG_INFO("{} A/B starting: {}; original flag will be restored; {}",Name(state.target),state.target==Target::Distance?"timed native resource lifecycle; startup/loading frames allowed":"waiting for dense 3D frames, >=400 accepted draws",state.target==Target::Distance?"only distance-shadow policy changes, no gameplay input; lifetime validation, not a quality-matched speed comparison":"no input or quality settings changed");}
 if(state.settings_captured){for(int i=0;i<kPCOptionCount;++i)if(state.settings[i]!=PCSetting(i)){SetOption(state.target,state.original);Finish(state.target);REXLOG_WARN("{} A/B aborted: graphics setting {} changed during comparison; results are invalid",Name(state.target),i);state={};return;}}
 // Loading/movie/menu samples are not valid measurements of the slow scene.
 // The lifetime cycle intentionally changes culling work. A reduced shadow
 // pass can fall below the performance benchmark's dense-frame threshold and
 // otherwise prevent restoration forever. Startup/loading frames are allowed
 // for this private lifecycle diagnostic; its FPS samples are not comparisons.
 if(draws<400 && state.target!=Target::Distance){state.start=now;state.warming=true;state.frames=state.draws=0;return;}
 const auto elapsed=std::chrono::duration<double>(now-state.start).count();
 if(state.warming){if(elapsed>=(state.phase==0?15:1)){if(!state.settings_captured){for(int i=0;i<kPCOptionCount;++i)state.settings[i]=PCSetting(i);state.settings_captured=true;}state.start=now;state.warming=false;state.frames=state.draws=0;state.skipped_start=StencilZeroSkippedDraws();state.value_start=StencilValueTransferMeshes();state.clear_start=StencilScratchClearOmissions();state.plane_start=StencilPlaneBarrierTransfers();state.copy_start=StencilCopyCommands();state.saved_start=StencilCopyCommandsSaved();state.fused_start=FusedDepthStencilTransferMeshes();state.direct_start=DirectColorResolveDispatches();REXLOG_INFO("{} A/B phase {} enabled {} begin; GPU-work profiler should be disabled for this comparison",Name(state.target),state.phase,state.phase&1);}return;}
 ++state.frames;state.draws+=draws;if(elapsed<15)return;
 REXLOG_INFO("{} A/B phase {} enabled {} complete: {:.3f}s, {} swaps, {:.3f} swaps/s, {:.1f} accepted draws/swap, {:.1f} stencil bit draws skipped/swap, {:.1f} R8 value meshes/swap, {:.1f} scratch clears skipped/swap, {:.1f} stencil-only transitions/swap, {:.1f} copy commands/swap, {:.1f} copy commands saved/swap (live scene, not deterministic replay)",Name(state.target),state.phase,state.phase&1,elapsed,state.frames,state.frames/elapsed,double(state.draws)/state.frames,double(StencilZeroSkippedDraws()-state.skipped_start)/state.frames,double(StencilValueTransferMeshes()-state.value_start)/state.frames,double(StencilScratchClearOmissions()-state.clear_start)/state.frames,double(StencilPlaneBarrierTransfers()-state.plane_start)/state.frames,double(StencilCopyCommands()-state.copy_start)/state.frames,double(StencilCopyCommandsSaved()-state.saved_start)/state.frames);
 if(state.target==Target::Fusion)REXLOG_INFO("Fused-depth-stencil A/B phase {} combined meshes/swap: {:.3f}",state.phase,double(FusedDepthStencilTransferMeshes()-state.fused_start)/state.frames);
 if(state.target==Target::Direct)REXLOG_INFO("Direct-color-resolve A/B phase {} direct dispatches/swap: {:.3f}",state.phase,double(DirectColorResolveDispatches()-state.direct_start)/state.frames);
 if(++state.phase==4){SetOption(state.target,state.original);Finish(state.target);REXLOG_INFO("{} A/B complete; original option {} restored",Name(state.target),state.original);state={};return;}
 SetOption(state.target,bool(state.phase&1));state.warming=true;state.start=now;state.frames=state.draws=0;
}
