EM1 Lua recovery and native bytecode-layout preparation.

The native-layout prototype preserves the original Lua 5.1 instructions,
register metadata, nested functions, strings and exact f64 constants. It changes
endianness and string-length width and strips debug/source metadata. Fixture tests
load the result into the vendored native Lua 5.1 VM without executing game scripts,
dump it again, and compare its complete normalized executable with the original.
This route does not use decompilation and does not produce HavokScript bytecode.
The `em1_lua_prepare_chunk`, `em1_lua_chunk_view` and `em1_lua_chunk_release`
C interface exposes this path with separate owned binary results. The input is
limited to 1 MiB, output to 2 MiB, and the validation VM to 64 MiB. Preparation
loads and dumps the converted closure without calling it and requires exact
normalized executable equality. No source is decompiled. Engine bindings,
coroutine integration remain outstanding. The game statically links the library
and prepares original courtyard scripts on an import worker directly from RVZ,
retaining their owned bytecode in the scene entry until unload. The
`em1_lua_world_create/view/release` API initializes these original top-level
chunks in import order and retains their functions and globals in a native VM
owned by that scene entry. No native chunk is sent to the HKS loader.
Initialization uses an isolated VM with math/string/table support, a shared
100,000-instruction limit, a 64-MiB VM memory limit and no
active engine callbacks, filesystem loaders, dynamic compilation or protected-call loop.
Gameplay callback bodies are not dispatched. The native `send` feature allows
ownership to transfer from the import worker to the scene owner and be released
on unload; callers must serialize access and release. Initialization failure
discards the entire staged VM. Tests cover actual shared courtyard initialization,
input-buffer independence, thread ownership transfer, transactional failure,
hash mismatch, missing engine APIs and instruction-budget termination.

The first native engine binding is `GetPlayer()`. After initialization it returns
nil until the engine thread supplies a validated current player through
`em1_lua_world_set_player`. The courtyard runtime uses its live player-manager
lookup, not the saved bootstrap actor. The supplied identity is the engine's
entity reference at player-manager slot +20, matching retail GetPlayer's
82737940 -> 82769FB8 path, rather than the resolved entity's memory address.
The VM returns a cached opaque userdata
while that actor remains current, clearing it on unavailable samples and level
transitions. Observed replacement/invalidation advances a generation, including
reuse of a reference after a missing sample. Future entity-consuming bindings
must resolve that native reference again and revalidate ownership before use.
No guest address is exposed as a Lua number and no player mutation or event
dispatch is enabled by this binding. Startup counts describe original script
definitions and exclude the added engine binding.

The scene also owns a bounded callback scheduler. `em1_lua_world_start` queues a
named zero-argument function; `world_tick` advances it with effective simulation
seconds and `world_cancel` drops queued and suspended work. Lua `wait` and `Wait`
yield through a private original coroutine.yield closure, so Lua 5.1 never has to
yield across a Rust callback. Positive waits resume at the first eligible tick;
wait(0) resumes on a later positive tick; zero delta pauses everything. Each tick
allows at most 32 resumes sharing a 100k-instruction budget, with 256 tasks max.
An errored coroutine is dropped while other tasks remain available. The runtime
ticks after each actual native simulation update using clock 0x833FCA54, including
fixed-step batches, and cancels on an EM1 level transition. It does not tick from
render frames. No original gameplay events are enqueued yet: actor/event wiring,
arbitrary actor argument adapters and remaining engine bindings are still required. Tests cover
30/60/120/144-Hz timing, pause, cancellation, zero waits, capacity/fairness bounds,
infinite-loop termination, later-task recovery and the public C lifecycle.

`em1_lua_world_start_args` now accepts up to 16 ordered arguments: nil, boolean,
finite f64, binary string, or a snapshot of the currently bound player identity.
Strings are copied into the VM during the call (64 KiB each, 256 KiB total).
Trailing nils are retained. These arguments are consumed only on the coroutine's
first resume; later resumes do not accidentally supply them as wait results.
Arbitrary actor references and automatic native event admission remain pending.
The original DBC_1st_SetSide bytecode is tested through this API with nil/string
parameters and its authored DBC_1st_side global checked afterward. Callback
failures now include the queued function name for diagnosis.

The import worker also reads original `Apprentice/ApprenticeGlobalState.bsq`
directly from RVZ. The v30 parser retains every name, ID, signed initial value and
flag in the native world, with transactional load and borrowed definition views.
The original database has 5731 names, ID limit 3950, 183 nonzero defaults and 125
true flags. Zero IDs repeat and must not be deduplicated. Limits are 4 MiB input,
65536 entries and 65533-byte names; malformed padding, truncation, invalid UTF-8
and duplicate/empty names are rejected. Authored IDs stay intact; a separate
native_id preserves nonzero IDs and assigns repeated zero IDs by lexical name
above the declared/observed maximum. Duplicate nonzero IDs and exhausted native
ID space are rejected. The actual EM2 serializer requires this uniqueness.
Production simulation owns a native store per EM1 scene; singleton publication and
persistent campaign saves remain pending.
The inspected EM2 retail GetGlobal returns nil for absent names; SetGlobal only
updates an existing entry. A shadow map or assumed-zero default would not fix
that missing import. Reference format work: bluethinner src/em1_bsq.rs, database
branch; original bytes are never modified.

`GetGlobal` and `SetGlobal` are installed before original definitions load, so
scripts may cache their function references. `em1_lua_world_tick_native` supplies
synchronous native callbacks for one scheduler tick. Access is revoked on return,
wait, script error, or Rust unwind; cached functions resolve the current scope on
each call. Calling outside native dispatch raises an error rather than returning
an invented default. Callback status 0 means success, 1 maps to Lua nil, and 2
raises a script error. SetGlobal returns the input native integer on success;
the backend stores signed16 values as the original engine does. Names must be
1..999 bytes without embedded NUL, and input values must fit signed32. The
optional no-counter flag is forwarded. No shadow value store exists in Rust.
Callbacks must not unwind or reenter world APIs. Context and name pointers are
borrowed only during dispatch. All world access remains externally serialized.
The default-off runtime validation invokes these functions through the scheduler
against an actual native staging store and then tests original save/reset/load.
Normal simulation now supplies the scene-owned native store through this API.
Level teardown transfers it to an in-memory retained owner. A later scene with
the same original database SHA-256 adopts that native map; a different schema
cannot consume it. One unloaded store is retained, and replacement releases the
older native map. File saves, new-game/save-slot identity and automatic quest
event admission remain pending.

The original source-recovery API below retains its strict acceptance rules.

The library consumes original RVZ/PAK Lua bytes, validates a bounded standard
Lua 5.1 chunk, calls unluac 1.4.3 directly, compiles its generated source without
execution using vendored Lua 5.1, and requires byte-for-byte equality of normalized
executable chunks. Only source names, line data and debug arrays are discarded;
instructions, register metadata, f64 constants and nested functions are retained.
An automatic failure or mismatch returns no source. There is no reviewed-source
table, file lookup, Java process, command-line fallback or gameplay execution.

Limits: 1 MiB bytecode, 4 MiB generated source, 131072 instructions total,
4096 prototypes, nesting below 96, 64 MiB compile-only Lua VM memory. Input
validation happens before decompilation. unluac has no interruptible step/time
budget; these are structural bounds, not a promise of a hard execution deadline.
Call it on an import worker. Rust panics are caught at the ABI; invalid host
pointers and process-wide allocation failure cannot be recovered by this API.

All result/source/error buffers belong to Rust and must be released exactly once
through em1_lua_release. Views borrow those bytes and are not NUL terminated.
The input span is borrowed only during em1_lua_recover. Errors are capped at 4096
UTF-8 bytes. SHA-256 uses the Windows BCrypt platform provider.

This does NOT prove EM2 gameplay compatibility. A subsequent native compile-only
probe must call original EM2 8271A360 at a protected live VM boundary, restore VM
stack offsets against any relocated stack and discard the closure/error without
pcall. Original Lua API semantics, numeric f64-to-HKS-f32 differences, entity
construction and coroutine scheduling still need separate compatibility work.

Developer validation: cargo test --offline -- --nocapture. An optional
EM1_LUA_ORIGINAL_FIXTURES directory supplies original raw Lua bytecode for a
per-file pass/refusal inventory; those files are not bundled. No game executable
or reviewed game-script sources are needed or produced. The runtime CMake build
uses Cargo offline with the locked dependencies to build the embedded static
library; no extra runtime executable or DLL is installed.
