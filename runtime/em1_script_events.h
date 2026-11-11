#pragma once
#include "em1_script_source.h"
#include <span>
#include <array>
#include <memory>
#include <string_view>
struct PPCContext;
namespace em2::em1 {
struct RegisteredBlueprintLibrary;
enum class ScriptEventArgumentKind {Nil,Boolean,Number,String,Entity};
struct ScriptEventArgument {
  ScriptEventArgumentKind kind=ScriptEventArgumentKind::Nil;
  double number=0;
  std::string text;
  uint32_t handle=0;
};
enum class ScriptEventRoute {NotOwned,NativeFallback,Queued,Rejected};
// Simulation/loading thread only. Begin AFTER native world invalidation and
// before its original load. Loading admits original OnLoad/OnEnable callbacks
// but does not tick their scheduler. Commit only after native load succeeds;
// End on rollback/transition BEFORE releasing actors/blueprints/script state.
bool BeginRegisteredScriptEvents(uint8_t* base,std::string_view state,
    std::shared_ptr<RegisteredBlueprintLibrary>,std::shared_ptr<LevelOriginalScripts>,std::string& error);
bool CommitRegisteredScriptEvents(uint8_t*,std::string& error);
bool UpdateRegisteredScriptEvents(uint8_t*,std::string& error);
bool EndRegisteredScriptEvents(std::string& error);
// Active phase only: callers MUST immediately call Update, which validates
// explicit native world lifecycle/ownership and reports/retires stale worlds.
// Player/camera identity changes do not invalidate a scene's script lifecycle.
bool RegisteredScriptEventsReady();
// Services wired for complete registered worlds. Animation-variable bit512 is
// added only after installing a checked target catalog. No callback executes
// during admission; preview-only services remain excluded.
inline constexpr uint32_t kRegisteredScriptEngineCapabilities=2|4|8|32|64|128|256|1024|2048|4096|8192|16384|32768|65536|131072;
// Validate the retained VM, native lifecycle/registry and loading-thread stack
// before a shared engine service acts; preserves PPC/FP/OS error context.
bool ValidateRegisteredScriptEngineScope(PPCContext&,uint8_t*,const NativeScriptState&,std::string& error);
// Original zero-argument reset / three-argument defaults, primary camera only.
uint32_t ControlRegisteredScriptCamera(PPCContext&,uint8_t*,const NativeScriptState&,
    uint32_t operation,float a,float b,float c,std::string& error);
//0=fade out,1=fade in,2=fade in without HUD. Uses the native fade controller;
// negative duration retains its native configured-duration behavior.
uint32_t FadeRegisteredScriptScreen(PPCContext&,uint8_t*,const NativeScriptState&,
    uint32_t operation,float seconds,std::string& error);
// Queries the real native IO group list in its native order. Non-nil members
// must retain source/primary-role identities; zero preserves a native nil entry.
// Failure preserves output.
uint32_t FindRegisteredScriptGroup(PPCContext&,uint8_t*,const NativeScriptState&,
    std::string_view name,std::vector<uint32_t>& handles,std::string& error);
// Scoped engine bindings. Validate against this retained VM, the current native
// registry and an already admitted live identity; never reenter the Lua C ABI.
// Name lookup returns0=found,1=absent,2=foreign/invalid. Destroy returns0 only
// after the original deferred-destroy path admits it;1 is the original managed
// player no-op (retain its live token);2 is foreign/stale/invalid admission.
uint32_t FindRegisteredScriptEntity(PPCContext&,uint8_t*,const NativeScriptState&,
    std::string_view name,uint32_t& handle,std::string& error);
// Resolves within the actual native owner prefab. Returned identities must
// already belong to the retained source/primary registry; never global fallback.
uint32_t FindRegisteredScriptRelativeEntity(PPCContext&,uint8_t*,const NativeScriptState&,
    uint32_t owner,std::string_view name,uint32_t& handle,std::string& error);
// Actual native prefab-owner identity for source-owned persistent Lua data.
//0=found,1=absent (zero GUID),2=invalid (output unchanged). No HKS table access.
uint32_t GetRegisteredScriptPrefabOwner(PPCContext&,uint8_t*,const NativeScriptState&,
    uint32_t handle,std::array<uint32_t,4>& guid,std::string& error,uint32_t* owner_handle=nullptr);
struct RegisteredPrefabData {
  std::array<uint32_t,4> owner{};
  uint32_t handle=0;
  std::vector<std::string> defaults,overrides;
};
// Copies the actual inherited source arrays through the original native prefab
// property interface. No HKS table references or guest pointers escape.
uint32_t ReadRegisteredScriptPrefabData(PPCContext&,uint8_t*,const NativeScriptState&,
    uint32_t handle,RegisteredPrefabData&,std::string& error);

uint32_t DestroyRegisteredScriptEntity(PPCContext&,uint8_t*,const NativeScriptState&,
    uint32_t handle,std::string& error);
// Original event name, recovered from verified source executable, resolves to
// the actual Xbox registry ID before using the audited native AnimEvent path.
uint32_t AnimateRegisteredScriptEntity(PPCContext&,uint8_t*,const NativeScriptState&,
    uint32_t handle,std::string_view original_name,std::string& error);
uint32_t SetRegisteredScriptAnimationVariable(PPCContext&,uint8_t*,const NativeScriptState&,
    uint32_t handle,std::string_view original_name,int32_t value,std::string& error);
uint32_t GamebryoRegisteredScriptEntity(PPCContext&,uint8_t*,const NativeScriptState&,
    uint32_t handle,uint32_t operation,std::string_view sequence,std::string& error);
// Native current transform and full teleport lifecycle; output remains unchanged
// on rejection. Zero references retain original zero-vector/no-op semantics.
uint32_t ReadRegisteredScriptPosition(PPCContext&,uint8_t*,const NativeScriptState&,
    uint32_t handle,std::array<float,4>& position,std::string& error);
uint32_t TeleportRegisteredScriptEntity(PPCContext&,uint8_t*,const NativeScriptState&,
    uint32_t handle,uint32_t destination,uint32_t reset,uint32_t orientation,std::string& error);
// Original parent component attach/detach, with native hierarchy and transform
// notifications. Parent0 detaches; a missing child/component is a native no-op.
uint32_t ParentRegisteredScriptEntity(PPCContext&,uint8_t*,const NativeScriptState&,
    uint32_t handle,uint32_t parent,std::string& error);
//0Enable,1Disable,2Hide,3Show,4SetAppCulled,5ClearAppCulled.
// Uses the original native component/child
// traversal, player refusal and mesh/physics notifications, never raw flags.
uint32_t ControlRegisteredScriptEntity(PPCContext&,uint8_t*,const NativeScriptState&,
    uint32_t handle,uint32_t operation,std::string& error);
// Original component-name lookup and disabled-state virtual dispatch. No
// component creation or raw flag writes; absent components retain native no-op.
uint32_t ControlRegisteredScriptComponent(PPCContext&,uint8_t*,const NativeScriptState&,
    uint32_t handle,std::string_view component,uint32_t disabled,std::string& error);
// Native particle controller + reflected Enabled property. Returns1 for the
// original absent-controller no-op,2 for invalid/stale/unsupported state.
uint32_t SetRegisteredScriptParticleEmission(PPCContext&,uint8_t*,const NativeScriptState&,
    uint32_t handle,uint32_t enabled,std::string& error);
// Call ONLY after the original native IO connection/target eligibility filter,
// before its HKS-only function-existence gate. Looks up functions only in this
// source's retained VM, resolves/revalidates live native handles, and prepends
// the target receiver to copied typed parameters. Does not parse Lua expressions
// or retain guest pointers. The native connection owner controls one-shot state:
// Queued admits consumption; Rejected must keep an authored one-shot for retry.
// NativeFallback/NotOwned leave ordinary EM2/native/audio execution unchanged.
ScriptEventRoute QueueRegisteredScriptEvent(PPCContext&,uint8_t*,uint32_t target,
    std::string_view callback,std::span<const ScriptEventArgument> parameters,double delay,std::string& error);
bool TestRegisteredScriptEventOwnership();
}
