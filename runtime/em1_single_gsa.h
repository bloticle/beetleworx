#pragma once
#include "em1_native_resources.h"
#include "em1_preview_scene.h"
#include "em1_script_source.h"
namespace em2::em1 {
// Explicit static-architecture exploration entry, not full campaign support.
// The caller owns player/camera bootstrap, collision and transition lifecycle.
struct SingleGsaEntry {
  ModelClosure closure;
  ArchitectureScene scene;
  std::vector<NativeScriptResource> scripts;
  NativeScriptState script_state;
  NativeRawSceneResource raw;
  NativeModelResource model;
  std::string scene_name,model_name,error;
  bool prepared=false,loading=false,entered=false,source_requested=false;
};
// Host-only original RVZ adaptation; no guest model allocation while old world lives.
bool PrepareSingleGsaEntry(std::shared_ptr<RvzSource>,SingleGsaEntry&,std::string& error);
// Call at an ordinary offline engine safepoint, with the actual app argument
// captured from LoadFile829E3178. This calls the native transition and load pump.
bool EnterSingleGsa(PPCContext&,uint8_t*,uint32_t app,SingleGsaEntry&,std::string& error);
// Before ORIGINAL82749E50: retain/verify the scene already published by the
// native PAK pipeline. Never bypass the original raw reader.
void ServeSingleGsaRawRead(PPCContext&,uint8_t*,uint32_t manager,uint32_t path,SingleGsaEntry&);
// After successful ORIGINAL 827A9458; retain original input/size across that call.
void AfterSingleGsaSceneBegin(PPCContext&,uint8_t*,uint32_t input,uint32_t size,bool success,SingleGsaEntry&);
// Only after native scene/entity unload and collision removal, never while active.
bool ReleaseSingleGsaOwners(PPCContext&,uint8_t*,SingleGsaEntry&,std::string& error);
}
