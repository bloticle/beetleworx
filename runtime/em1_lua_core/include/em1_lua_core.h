#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct Em1LuaRecovery Em1LuaRecovery;
enum Em1LuaStatus {
  EM1_LUA_OK=0, EM1_LUA_INVALID=1, EM1_LUA_DECOMPILE=2,
  EM1_LUA_SYNTAX=3, EM1_LUA_DIFFERENT_EXECUTABLE=4,
  EM1_LUA_BUDGET=5, EM1_LUA_INTERNAL=6, EM1_LUA_REENTRANT=7,
  // Tick completed; individual failed Lua threads were retired, others survive.
  EM1_LUA_SCRIPT_FAILED=8
};
typedef struct Em1LuaRecoveryView {
  const uint8_t* source;
  size_t source_size;
  const uint8_t* error;
  size_t error_size;
  uint8_t original_sha256[32];
  uint8_t executable_sha256[32];
  uint32_t status;
} Em1LuaRecoveryView;
// Input must be a readable span for the duration of this call. Worker only.
// No filesystem, process launch, reviewed-source lookup or Lua execution.
// All failures return an owned diagnostic object; source is empty on failure.
Em1LuaRecovery* em1_lua_recover(const uint8_t* original, size_t original_size);
// Borrowed UTF-8 byte spans, NOT NUL terminated. Valid until release; do not free
// with the host allocator. Only status==OK authorizes consuming source.
uint32_t em1_lua_view(const Em1LuaRecovery*, Em1LuaRecoveryView* output);
// Exactly once per owned result, from this same library. NULL is allowed.
void em1_lua_release(Em1LuaRecovery*);
typedef struct Em1LuaChunk Em1LuaChunk;
typedef struct Em1LuaChunkView {
  const uint8_t* bytecode;
  size_t bytecode_size;
  const uint8_t* error;
  size_t error_size;
  uint8_t original_sha256[32];
  uint8_t executable_sha256[32];
  uint32_t status;
} Em1LuaChunkView;
// Worker-only lossless layout conversion and native Lua 5.1 load validation.
// Never executes the original script. Output is standard Lua 5.1 for this host,
// NOT HavokScript bytecode: never pass it to EM2's HKS loader.
// Input limit 1 MiB, output limit 2 MiB, validation VM limit 64 MiB.
Em1LuaChunk* em1_lua_prepare_chunk(const uint8_t* original,size_t original_size);
// Borrowed binary/error spans owned by the result; valid until chunk_release.
// On failure bytecode_size is zero. Only status==EM1_LUA_OK permits use.
uint32_t em1_lua_chunk_view(const Em1LuaChunk*,Em1LuaChunkView* output);
// Exactly once for each result; NULL is allowed. Do not use recovery_release.
void em1_lua_chunk_release(Em1LuaChunk*);
typedef struct Em1LuaWorld Em1LuaWorld;
typedef struct Em1SourceMapView {
  const uint8_t* full;size_t full_size;
  const uint8_t* sub;size_t sub_size;
  uint64_t revision;
} Em1SourceMapView;
// Original manager selection state, not a claim of native map-art loading.
// Borrowed bytes last until the next mutating world call; calls serialize.
// Same-world callback reentry rejects without changing the supplied output.
uint32_t em1_lua_world_map_view(const Em1LuaWorld*,Em1SourceMapView*);
uint32_t em1_lua_world_reset_map(Em1LuaWorld*);
typedef struct Em1LuaWorldInput {
  const uint8_t* bytecode;
  size_t bytecode_size;
  uint8_t executable_sha256[32];
} Em1LuaWorldInput;
typedef struct Em1LuaWorldView {
  uint32_t status,scripts,functions,globals;
  const uint8_t* error;
  size_t error_size;
} Em1LuaWorldView;
// Executes original top-level initializers in a fresh, restricted native VM.
// No engine callbacks, file access or coroutine dispatch. Entire initialization
// has a 100000-instruction/64-MiB VM limit; at most 512 scripts/64 MiB input.
// Hashes must match prepare_chunk output. Failure discards all partial state.
// Inputs are borrowed for this call. Result owns its persistent VM or error.
// Ownership may move between threads; callers must serialize use and release.
Em1LuaWorld* em1_lua_world_create(const Em1LuaWorldInput*,size_t count);
typedef struct Em1LuaWorldSource {
  Em1LuaWorldInput chunk;
  const uint8_t* path;
  size_t path_size;
  uint32_t startup,reserved;
} Em1LuaWorldSource;
// Registers owned original binary modules before executing startup=1 chunks
// in supplied order. Original Openfile executes imports synchronously, including
// repeated calls, with source-relative path normalization; it has no filesystem
// or dynamic-source access. Imports/hashes/paths validate before any startup.
// Original retail BuildConfig is FINAL. Only validated coroutine.yield is
// exposed (no create/resume/wrap escape from scheduling/instruction budgets).
// Input bytecode/paths are copied. Same restricted VM/budgets as create above.
Em1LuaWorld* em1_lua_world_create_sources(const Em1LuaWorldSource*,size_t count);
uint32_t em1_lua_world_view(const Em1LuaWorld*,Em1LuaWorldView*);
void em1_lua_world_release(Em1LuaWorld*);
typedef struct Em1GlobalSummary { uint32_t count,id_limit,nonzero,flagged; } Em1GlobalSummary;
typedef struct Em1GlobalDefinitionView {
  const uint8_t* name;
  size_t name_size;
  uint32_t id;
  int32_t initial;
  uint32_t flag;
  uint32_t native_id; // Nonzero, unique PC serialization ID; id retains original.
} Em1GlobalDefinitionView;
// Retain original v30 ApprenticeGlobalState definitions transactionally. This
// does not publish native engine entries or bind a shadow GetGlobal/SetGlobal.
// Input is borrowed only during load. Views expire on successful reload/release.
// A failed load keeps prior definitions intact. Calls must be serialized.
uint32_t em1_lua_world_load_globals(Em1LuaWorld*,const uint8_t*,size_t);
// Exact original database SHA-256. Writes32 bytes; invalid until definitions load.
uint32_t em1_lua_world_global_schema(const Em1LuaWorld*,uint8_t* out32);
uint32_t em1_lua_world_global_summary(const Em1LuaWorld*,Em1GlobalSummary*);
uint32_t em1_lua_world_global_definition(const Em1LuaWorld*,size_t,Em1GlobalDefinitionView*);
// Engine-thread snapshot of the live player's native entity reference (not a
// guest memory address); zero clears it. GetPlayer
// returns an opaque, stable userdata identity while this actor remains current,
// or nil when unavailable. No guest memory is accessed by this API. Serialize
// with all other world calls. Future entity consumers must revalidate identity.
uint32_t em1_lua_world_set_player(Em1LuaWorld*,uint32_t entity);
// Registered-world primary role. Caller must verify native manager slot,
// registry handle and live actor identity before admitting/binding that entity.
// GetPlayer returns the SAME userdata as typed entity arguments and engine
// consumers. Zero clears; unbound IDs fail closed. Revocation clears GetPlayer
// and cached references remain stale across handle/address reuse. The opaque
// legacy set_player API remains available for the isolated preview only.
uint32_t em1_lua_world_set_bound_player(Em1LuaWorld*,uint32_t identity);
// Queue a named zero-argument callback, then advance with effective simulation
// seconds, never display-frame wall time. wait/Wait suspend until the deadline;
// wait(0) resumes on a later positive simulation tick. Zero delta is paused.
// 256 tasks maximum, 32 resumes/100000 Lua instructions per tick. Script errors
// drop the failing task; other tasks remain queued. Cancel drops all tasks.
// Calls are serialized. Nonzero return reports failure; world_view.error gives
// the latest operation error (view.status still describes initialization).
uint32_t em1_lua_world_start(Em1LuaWorld*,const uint8_t* name,size_t length);
// Raw lookup only; invokes no callback/metatable and enqueues nothing. Requires
// a live initialized world and a valid UTF-8 callback name. Calls serialize.
uint32_t em1_lua_world_callback_exists(Em1LuaWorld*,const uint8_t*,size_t,uint32_t* exists);
// Conservative, non-executing admission before consuming a native one-shot.
// Resolves original nested global dependencies and rejects unresolved globals
// or unavailable scoped services. Capability bits: rotator1, cutscene2,
// particles4, authored groups8, runtime Openfile16, owned entity lookup/destroy32,
// semantic original-to-native animation64, verified Gamebryo reset/sequence128.
// Zero means globals/pure VM
// only. A nonzero result supplies the actual missing dependency in world_view.
uint32_t em1_lua_world_preflight_callback(Em1LuaWorld*,const uint8_t*,size_t,uint32_t capabilities);
typedef struct Em1LuaArgument {
  uint32_t kind,reserved;
  double number;
  const uint8_t* bytes;
  size_t size;
} Em1LuaArgument;
// Kinds: 0=nil, 1=boolean (number 0 or 1), 2=finite double,
// 3=binary string, 4=current-player identity snapshot (requires live player).
// reserved must be zero. At most 16 arguments, 64 KiB/string, 256 KiB total.
// Input bytes are copied during this call; argument order/trailing nils survive.
// Arguments are supplied only on first resume, never as wait return values.
// 5=registered native entity identity (number is an exact nonzero uint32).
// 6=global function snapshot (bytes is a nonempty UTF-8 name, at most256 bytes).
// Host must bind only an owned live entity and unbind before destruction/unload.
// Rebinding a revoked ID creates a new generation; cached tokens stay stale.
uint32_t em1_lua_world_bind_entity(Em1LuaWorld*,uint32_t identity);
uint32_t em1_lua_world_unbind_entity(Em1LuaWorld*,uint32_t identity);
uint32_t em1_lua_world_start_args(Em1LuaWorld*,const uint8_t* name,size_t length,
                                const Em1LuaArgument*,size_t count);
// Same owned argument/function snapshots, with an authored nonnegative finite
// delay in effective simulation seconds. Pauses never advance the deadline;
// zero delay still dispatches on a later positive tick. No borrowed event or
// connection pointers survive this call. Invalid delays enqueue nothing.
uint32_t em1_lua_world_start_args_after(Em1LuaWorld*,const uint8_t* name,size_t length,
                                     const Em1LuaArgument*,size_t count,double delay);
typedef struct Em1LuaEventInput {
  const uint8_t* name;size_t name_size;
  const Em1LuaArgument* args;size_t count;
  double delay;
} Em1LuaEventInput;
// Atomic native connection admission: all targets/typed arguments/delays are
// staged before committing any scheduler tasks. Failure preserves the previous
// queue, so an authored group one-shot can be retried without duplicate earlier
// targets. Maximum256 requests, with the same total scheduler task budget.
uint32_t em1_lua_world_start_batch(Em1LuaWorld*,const Em1LuaEventInput*,size_t count);
uint32_t em1_lua_world_tick(Em1LuaWorld*,double simulation_delta);
// Synchronous engine-thread callbacks: 0=found/success, 1=unknown name,
// 2=failure. Never throw/unwind across this ABI or reenter world APIs. Borrowed
// names/context are valid only during this tick. Access is revoked on return,
// error or suspended wait; cached Lua functions remain valid for later ticks.
typedef struct Em1GlobalCallbacks {
  uintptr_t context;
  uint32_t (*get)(uintptr_t,const uint8_t*,size_t,int32_t*);
  uint32_t (*set)(uintptr_t,const uint8_t*,size_t,int32_t,uint32_t no_counter);
} Em1GlobalCallbacks;
uint32_t em1_lua_world_tick_native(Em1LuaWorld*,double,const Em1GlobalCallbacks*);
typedef struct Em1EngineCallbacks {
  uintptr_t context;
  uint32_t (*set_cutscene_input)(uintptr_t,uint32_t restricted);
  uint32_t (*set_rotator_speed)(uintptr_t,uint32_t native_handle,float degrees_per_second);
  uint32_t (*set_particle_emission)(uintptr_t,uint32_t native_handle,uint32_t enabled);
  uint32_t (*resolve_group)(uintptr_t,const uint8_t* name,uint32_t length,uint32_t* handles,uint32_t capacity,uint32_t* count);
  uint32_t (*resolve_entity_name)(uintptr_t,const uint8_t* name,uint32_t length,uint32_t* handle);
  uint32_t (*destroy_entity)(uintptr_t,uint32_t handle);
  uint32_t (*animate_entity)(uintptr_t,uint32_t handle,const uint8_t* original_event_name,uint32_t length);
  // operation0=reset,1=verified entity/sequence form. Unknown/missing sequence
  // reports failure, never succeeds with an unrelated or empty animation.
  uint32_t (*gamebryo_animation)(uintptr_t,uint32_t handle,uint32_t operation,const uint8_t* sequence,uint32_t length);
  uint32_t (*control_entity)(uintptr_t,uint32_t handle,uint32_t operation);
  uint32_t (*set_animation_variable)(uintptr_t,uint32_t handle,const uint8_t* original_name,uint32_t length,int32_t value);
  uint32_t (*control_component)(uintptr_t,uint32_t handle,const uint8_t* name,uint32_t length,uint32_t disabled);
  uint32_t (*get_position)(uintptr_t,uint32_t handle,float* output_four);
  uint32_t (*teleport_entity)(uintptr_t,uint32_t handle,uint32_t destination,uint32_t reset,uint32_t orientation);
  uint32_t (*set_parent)(uintptr_t,uint32_t child,uint32_t parent);
  uint32_t (*resolve_relative_entity)(uintptr_t,uint32_t owner,const uint8_t* name,uint32_t length,uint32_t* handle);
  // Host-owned copied payload, valid until the next callback in this dispatch.
  // Payload: two BE32 row counts, each followed by BE32 length + raw row bytes.
  uint32_t (*get_prefab_data)(uintptr_t,uint32_t handle,uint32_t* owner_guid_and_handle_five,const uint8_t** data,size_t* size);
  uint32_t (*fade_screen)(uintptr_t,uint32_t operation,float seconds);
  uint32_t (*camera_control)(uintptr_t,uint32_t operation,float a,float b,float c);
} Em1EngineCallbacks;
// Calls on one world must be serialized across threads. Same-thread reentry
// returns REENTRANT before touching the world/output; release is ignored while
// busy and must be retried after the outer call. No nested operation succeeds.
// Same synchronous lifetime/no-reentry contract as globals. Entity references
// are validated against this VM's live registry immediately before dispatch.
uint32_t em1_lua_world_tick_engine(Em1LuaWorld*,double,const Em1GlobalCallbacks*,const Em1EngineCallbacks*);
uint32_t em1_lua_world_cancel(Em1LuaWorld*);
typedef struct Em1LuaSchedulerStats {
  uint64_t submitted,resumes,completed,failed,cancelled,pending;
} Em1LuaSchedulerStats;
// Read-only cumulative totals for this retained VM. Wait/delay/pause/yield and
// cancellation never count as completion; failed tasks count separately.
// Batch rollback changes no totals. pending is the current queued/waiting count.
uint32_t em1_lua_world_scheduler_stats(const Em1LuaWorld*,Em1LuaSchedulerStats*);
// Recover original EVENT_* constants from the mounted original executable.
// Accepts only the audited module SHA/registration shape, retains copied names,
// and installs original Wii ordinals. AnimEvent dispatch translates each ID by
// name through the actual Xbox registry. Never accepts unverified tables.
uint32_t em1_lua_world_load_animation_events(Em1LuaWorld*,const uint8_t*,size_t);
typedef struct Em1LuaAnimationVariableTarget {
  const uint8_t* name;
  size_t length;
  uint32_t id,reserved;
} Em1LuaAnimationVariableTarget;
// Caller supplies names/IDs recovered from the actual checked target image.
// Copies all records; only an identical repeated installation is allowed.
uint32_t em1_lua_world_install_native_animation_variables(Em1LuaWorld*,const Em1LuaAnimationVariableTarget*,size_t count);
// Clears source-owned prefab tables on an explicit native world generation.
uint32_t em1_lua_world_reset_prefab_data(Em1LuaWorld* world);
#ifdef __cplusplus
}
#endif
