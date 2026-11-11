# Beetleworx

Beetleworx aims to be the ultimate upgrade of the MouseTrap engine for Epic Mickey and Epic Mickey 2. See the [public demo README](docs/BEETLEWORX_README.md) for setup and the list of unfinished features. The notes below document development and testing.

Experimental Windows x64 port of the Xbox 360 build using
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp) for static PowerPC translation
and the locally installed [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) for
Xbox services, graphics, audio, input, and memory management.

The normal build now prepares PC shader bytecode from all 2064 programs in the
original archive, plus validated code and variants from the existing native
shader cache. It prepares 1x and the saved render scale using the renderer's
actual vendor, binding, gamma and MSAA properties. The existing beetleworx.exe
performs preparation without a window or guest execution; no separate game
executable is used. A packed library carries bytecode and texture/sampler
bindings into normal startup. Known native driver pipelines finish preloading
before gameplay; exact cache misses still use the native translator and save
new entries in the background. Future dynamic variants, different profiles and
previously unseen driver pipelines are not guaranteed to be prepared. This
targets compilation stalls, not the GPU cost of drawing frames. Build with
`scripts/build.ps1`; `-SkipShaderPreparation` skips only the preparation step.
The build also prepares the EM1 compatibility material effects using a 54-draw
headless GPU validation in the same executable. It adds four effects to the
staged shader archive and checks their vertex-fetch order. Original EM2 effects
remain unchanged; the normal release package carries the prepared archive.
This supports the bounded EM1 courtyard preview, not full EM1 campaign gameplay.

Normal gameplay also suppresses informational GPU shader dumps before formatting
and file output; GPU warnings and errors retain their existing handling. Use
`--em2_verbose_gpu_log=true` or an explicit debug/trace log level for full GPU
logging. Driver-blob caching stays opt-in: the recorded pipeline workload did
not show a benefit on this machine, despite successful blob reuse.

The build also compiles 19 PC renderer HLSL variants for HDR presentation,
scene resolves, depth resampling, render-scale migration, geometry, AO and
indirect lighting. Their bytecode is embedded in beetleworx.exe; these passes
retrieve it directly without loading or calling the HLSL compiler during
initialization. HLSL edits rebuild the embedded library automatically, including
the single-sample and multisample variants. The full headless GPU diagnostic
recompiles every variant as a reference and compares its bytecode exactly.
Driver pipeline creation and unseen Xbox shader variants remain separate work.

CPU translation is implemented and the game has rendered playable 3D scenes.
Advanced Graphics and Control Bindings are integrated into the in-game Options
menus. Higher FPS selections currently use 30 Hz gameplay with interpolated
scene and skeletal rendering. The user reports smoother motion but delayed or
floaty camera/controls. Responsive high-FPS gameplay, loading performance and
broader gameplay stability remain unfinished. Interpolation now restores authoritative
scene poses after interrupted or nested C++ rendering; headless memory tests pass.
This cleanup fix does not resolve the normal camera-latency report. Scene interpolation
also reuses endpoint analysis adaptively when successive ticks show repeated
render samples. Complete matrices and FP flags match the reference; component
benchmarks improve reused calculations about1.2x-2.0x, with single-sample costs
close to the original. Overall game FPS impact remains unmeasured. Native Delta diagnostics now also log
camera-controller cadence/delta totals alongside physics timing, without adding
camera updates. Actual gameplay data is still needed to resolve the camera issue.
Moving-scene rendering also caches stable history-node references and samples
interpolation alpha once per render. Detachment removes cached references;
expiry/reset rebuild the list. `--em2_cache_scene_render_entries=false` selects
the map-lookup reference path. Full apply/restore CPU fixtures retain transform
bits and FP flags and cover rehash, detach, empty history and interrupted renders.
The latest512/4096-node fixtures use about18-41% less CPU time for this component;
this is not a gameplay FPS measurement or a fix for interpolation camera delay.
`--em2_benchmark_scene_render_entries=true --em2_test_pc_settings=true` runs the
synthetic comparison in the same executable without launching gameplay.
See `STATUS.md` for verified results.

Advanced Graphics -> **Scene lighting (experimental)** offers Original,
Material AO, and AO + diffuse bounce. It applies without restarting: the GPU worker
captures render storage and shader programs, builds a native-linear HDR/material
cache, restores its contents and publishes the replacement graph. Disabling
returns to the matching renderer configuration. AO toggles keep the display
presenter; HDR output and render-scale changes preserve the selected lighting
mode and its linear surface policy.
Queued, applied and failure status appear in the menu. Failed requests restore
the saved choice unless a newer selection superseded them. The default is Original.

This path reconstructs scene geometry from the matched original depth-fog draw,
computes geometry-aware AO, and attenuates a captured material lighting residual.
It extracts the SH-dependent residual of the pinned full Mickey and Oswald
normal materials, plus the baked-light-dependent residual of the standard
`Gen3_StaticEnv` and `Gen3_StaticEnv_DL` environment materials (without/with
dynamic lights). Both retain the authored minimum-lighting floor. Other environment and character
variants remain unsupported. The environment directional-lightmap texture holds
weights; baked colour enters through vertex outputs. Its reference evaluation
zeros that baked colour while retaining directional weights, dynamic lights,
textures and coverage. The result includes baked specular interactions and is
not a physically pure indirect-light buffer.

Original texture reads are shared and only the final nonlinear combine repeats.
For the character kernel, 203 native arithmetic instructions and four texture
fetches replace321/eight in its full dual-evaluation reference. The environment
kernel uses four original texture fetches; its controlled translated disassembly
contains1710 instruction lines versus2533 in the shared-texture dual reference.
These counts use different counting methods and are not comparable across
kernels. Neither comparison establishes a gameplay FPS improvement. Supported fallback draws clear or attenuate
the existing residual, including alpha and multiplicative blending; unsupported
coverage or allocation limits decline the private capture.

A development-only surface-capture fixture now extracts the pinned base
environment shader's sampled diffuse RGB and effective specular exponent into
private FP16 MRT6 before exposure/gamma. A combined probe also writes the
lighting-dependent residual to MRT5 in the same draw, sharing all four native
texture fetches. Independent UV/texel/constant and black-baked reference checks
preserve original colour and HDR/coverage outputs. The original base environment
vertex shader also creates a native pipeline with the combined pixel shader.
The native backend also has an immutable opt-in surface mode: it builds the
combined pipeline, owns/binds MRT6 alongside HDR4/material5, shares the material
memory budget and keeps resources until drained cache shutdown. Supported
non-surface material/auxiliary pipelines export RGB0/alpha-1 into MRT6 only on
covered pixels, retaining surrounding captured environment attributes. Their
original primary/HDR/ambient outputs and discard/scissor behavior are retained;
the private invalid marker ignores primary blend factors and colour masks.
Supported native RGBA8 ownership copies now mirror raw surface attributes with
the same native rectangles, descriptor allocation and FP16 transfer shader as
the HDR/ambient copies. Unavailable pixels and high specular exponents survive
1x/4xMSAA tile-boundary transfers. Missing sources, invalid destinations,
unsupported conversions or copy failures retain conservative invalidation;
partial/full clears retain that validity rule. A one-byte-short transfer budget
check rejects without copy commands, then recovers after release and full clear.
The mode survives renderer graph capture and resolution rebuilds. Ordinary
gameplay settings currently leave it disabled. Native copies now resolve raw
surface attributes into a separate FP16 image, with exact metadata/texture
lookup, memory invalidation and the shared material budget. RGB follows the
colour resolve's channel permutation, while brightness exponent bias never
scales material attributes. Negative-alpha or nonfinite MSAA samples make that
pixel unavailable instead of averaging invalid data into a valid material.
Native-copy GPU tests check all seven sample selectors and preserve high-range
specular exponents. GPU fallback coverage tests additionally check 20 blend/mask
cases and 5120 surface components against independent CPU expectations. Broader
material capture, wider transfer-format coverage, colour-space validation and
live indirect/reflection consumers remain unfinished.

A development diffuse-indirect pass now estimates one screen-space light bounce
at half resolution using world-space surface facing, emitter area/distance and
visible-surfel segment tests. Transport is energy bounded and uses captured
receiver diffuse values; it requires an explicitly established linear diffuse
encoding. An optional composition input adds that radiance alongside material
AO, with geometry/material edge rejection and original alpha preservation.
Seven controlled scenes verify coloured bounce, coplanar/dark/backface zero
contribution, blocker suppression, unavailable receivers and material seams:
1344 indirect and5376 composition components pass independent CPU references,
with1792 original HDR components unchanged. Managed frame bindings now retain
GI work before recording and through disable until native submission retirement;
controlled tests retain seven frames. The exact pinned native depth-fog shader
preserves existing material attributes with an unchanged pixel program and a
zero write mask on the surface target. Unknown draws remain conservative.
An optional linear material-surface mode now accepts the pinned world shader's
diffuse fetch1 only when all post-swizzle RGB signs request Gamma decoding.
Native PWL and sRGB decoding match independent CPU references; unsigned or
mixed signs write unavailable pixels. Original colour/HDR/ambient are retained,
and graph snapshots/rebuilds preserve this immutable policy. The diffuse-bounce
option enables this capture and selects matched surface resolves for the live
indirect pass. Missing or stale surface inputs select AO before GI commands
are recorded; unknown receiver materials contribute no bounce. Native resolve
tests verify positive additive lighting beyond AO alone, unchanged alpha,
missing-surface fallback, and raw/gamma guest publication. Ordinary-level
appearance, performance and broader receiver-material coverage remain unverified.
Screen-space visibility is incomplete and camera-view radiance can contain
specular/fog contributions; this is not a complete view-independent GI solution.

Canonical FP16 scene lighting stays separate from PWL/sRGB guest read views.
Composition requires matching camera, frame, pixel grid and image revisions.
SDR presentation clamps/quantizes the modified scene before the original native
brightness/gamma pass; HDR presentation uses the canonical floating-point image.
Resources remain owned through native submission fences. Controlled native GPU
checks cover original material output, auxiliary targets and resolves, AO
publication, SDR/HDR mailbox pixels, graph switching and render-data retention.
Both staged options archives are decompiled to verify the menu option and status
callback. Displayed in-level appearance, broader material coverage, physical HDR
output and performance still need gameplay validation. No automated gameplay
input is used by these checks.

Internal render resolution (1x/2x/3x) now has an experimental live path in
Advanced Graphics. It captures packed EDRAM, auxiliary depth/stencil, resolved
textures and shader programs, restores replacement target/texture/pipeline caches,
then transfers active state and native shader-cache ownership. The menu reports
queued/applied/failure status; failures restore the saved selection unless a newer
request replaced it. Busy changes can be selected again to retry.

The actual offscreen worker path passes1x->2x->3x->1x, with native resolves,
recapture, active shader remapping, controlled shadow reload and persistence
handoff checks. CPU fixtures test the real GFx status writer and saved-selection
rollback. Partial scaled-only resolves preserve untouched CPU baseline bytes when
returning to1x. Depth/MSAA tests retain full float depth and stencil precision.
Before publication, a GPU guard now compares every owned packed word before and
after native original-format transfer. One-bit corruption rejects; unowned gaps
are excluded. A known nonzero target survives the full1x->2x->3x->1x controller
cycle. This does not audit every extra float32 auxiliary-depth precision bit.
The live controller also releases capture payloads after their completed GPU
reads and releases both packed audit copies before converting/committing resolved
textures. This removes180MiB of temporary allocation overlap at3x in that phase;
it does not establish an exact overall peak-VRAM or FPS improvement.
No title is launched by these diagnostics. Actual level switching, forced memory
exhaustion/device loss and auxiliary precision failure auditing remain open.

Shadow sampling, bloom, texture-cache budget and lifetime, anisotropic filtering,
native lighting controls and display/window settings apply without restarting.
Variable refresh/tearing recreates the presentation surface. Player/environment
shadow dimensions use an experimental live replacement path that transfers the
engine's limited binding slots before retiring old targets; actual in-level
creation and switching still need validation. Menu appearance and presentation
changes also need gameplay QA. Verified evidence and historical details are in
STATUS.md; these results do not prove every graphics option stable in gameplay.

The environment-shadow 4096 selection currently hits the native allocator's
2048 clamp; its menu label now states that limitation. True 4096 support remains
unfinished.
The reported level-loading allocation failure has a tested startup memory-budget
mitigation, but the original level-loading reproduction still needs validation.
Its revised 320 MiB graphics budget fits alongside the native CPU pools in the
shared 512 MiB physical space; an isolated test verifies all startup allocations
concurrently through the real SDK allocator. A tested native command-ring/fence
drain is also ready for resource recreation. Live shadow resizing still needs
CPU resource synchronization and safe replacement of dependent views.
New shadow target creation reads current menu dimensions even when the native
INI cache holds older values. Startup reserves graphics headroom independently
of the initial quality. Replacing already-active targets remains unfinished.

Advanced Graphics → **Shadow sampling** offers Original (4 taps), experimental
8 taps and experimental 16 taps, applied before the next affected draw. These add independent
native depth comparisons per shadow map and average the visibility results.
Original remains the default. Offscreen GPU rendering checks now verify exact
control-shader parity, unchanged flat lighting and finer edge coverage, including
varying kernel rotations. Real-game appearance and gameplay cost still need
validation.

Shadow sampling also offers **Adaptive (20 reads, exp.)**, applied live.
It performs a four-sample blocker search and sixteen filtered depth comparisons
per map. Filter width depends on blocker separation, using the game's projected
depth and authored bias. GPU checks verify the radius calculation, tighter
contact versus separated-blocker transition coverage, and unchanged flat
lighting. This is an experimental projected-depth approximation; live shadow
appearance, bias/leakage and gameplay cost remain unverified. It increases
sampling work, and Original remains the default.

Advanced Graphics → **Bloom** offers Off (experimental), Original sampling and
Smooth sampling, applied live. Smooth uses linear sampling in the
native Gaussian and composite programs, including final bloom enlargement.
GPU controls verify smoother enlarged ramps and bit-identical pixel-aligned
native-size output; live bloom appearance still needs acceptance testing. Off replaces only the matched native Gaussian
pixel programs with zero output and no texture reads; threshold/composite passes
remain. GPU output/cache checks pass; full in-game Off appearance is unverified.
Native-composite GPU tests also confirm zero bloom preserves the weighted scene
in either input slot, including HDR and negative colors.

Advanced Graphics includes **Offline 3D view width (FOV)**: Original, 110%,
125%, 150% or 175% of the authored view width. It changes the native camera's
projection input on the next normal 3D camera update and saves the selection.
It preserves native zoom proportions and bypasses special/scripted camera modes
and synchronized online sessions. Original remains the default. Native getter,
settings and compiled-menu checks pass; live framing, aiming and collision
acceptance are still pending.

Advanced Graphics → **Offline 3D camera response** offers Original smoothing,
Faster smoothing and No input smoothing, applied immediately and saved.
Faster doubles the two native input stages' response rate; No input smoothing
removes their acceleration/release tails and input blend. Native limits,
follow and collision remain. Original is the default, and special/scripted
cameras and online aiming keep original response. This addresses native input
filtering; it does not add render-rate camera updates or remove interpolation
latency. Live feel still needs verification.

Advanced Graphics → **Offline mouse camera → Raw movement (experimental)**
adds foreground Windows raw mouse movement to native yaw/pitch after the stick
filters. It defaults to Off. Relative movement changes angles directly, without
a timestep multiplier or a stick release tail; native pitch limits remain.
The Keyboard / mouse profile's horizontal/vertical strength and inversion apply.
It follows the selected single-player character, or player one in local co-op.
Capture is gated by camera availability, pause, menu and focus state, and stale
motion is discarded. Native-helper and input-model tests pass; OS capture and
live feel remain unverified. This covers normal offline 3D cameras, without
render-rate camera updates, separate paint/2D cursor control or online mouse
transmission. Simulation and physics are unchanged.

Character-switch recovery now handles a missing Oswald without waiting for him
before running the native factory, preserves scene-selected 2D AI during
recovery, and avoids creating a companion in native disabled-companion states.
Pending recovery displays "Returning to Mickey...". Missing manager/profile
globals preserve pending recovery, and replacement managers use current records
without following retired-manager pointers. Sixteen outer-frame lifecycle
scenarios pass with an injected native backend; live campaign transitions still
need acceptance testing.

The latest CPU changes localize native RTTI traversal and reuse skeletal
interpolation endpoint analysis between renders of one captured pose. Both are
enabled by default. Complete type-check benchmarks took up to 26% less time;
synthetic skin replay with two or more renders per capture took about 20–52%
less time, including snapshot/preparation costs. Those are component timings,
not measured gameplay FPS. The animation cache applies to compatibility timing;
it does not accelerate the native-delta simulation path. It skips allocation at
a 30-FPS cap and during multi-tick catch-up. Disable individually with
`--em2_local_type_checks=false` and `--em2_cache_pose_interpolation=false`.
The same EXE's headless tests accept `--em2_benchmark_type_checks=true` and
`--em2_benchmark_pose_interpolation=true` for reproducible comparisons.

Material binding also prefers virtual-address reads while preserving physical
heap translation. It is enabled in either timing mode and can be disabled with
`--em2_predicted_material_reads=false`. Complete binding/state checks pass in
75,168 cases. Longer production-dispatch benchmarks measured roughly 1–5% less
CPU time in small-binding cases; these remain routine measurements, not FPS.
`--em2_benchmark_material_reads=true` and
`--em2_benchmark_material_sparse_reads=true` add broad and longer focused
comparisons to the sole EXE's headless regression mode.

## Local build

Prerequisites: Visual Studio C++ tools, CMake, Python 3.11+, 7-Zip, and ReXGlue SDK
0.1. The supplied scripts use `D:/Repos/rexglue-sdk` by default and install a
checksum-verified portable LLVM 23.1.4 under `tools/llvm23`.

```powershell
./scripts/bootstrap.ps1
./scripts/recompile.ps1 -GameDirectory 'D:\Modding\Builds\EM2X360'
./scripts/test.ps1
./scripts/build.ps1 -GameDirectory 'D:\Modding\Builds\EM2X360' -Jobs 2
./scripts/run.ps1 -GameDirectory 'D:\Modding\Builds\EM2X360'
```

The native executable is `build/runtime23/beetleworx.exe`. It reads the original game
directory; save data is redirected to `build/runtime23/user_data`. The build scripts
leave the source game directory unchanged. An alternate SDK can be selected with `-RexSdk`.

Release builds retain ordinary per-source optimization by default. Experimental
[ThinLTO](https://clang.llvm.org/docs/ThinLTO.html) is available with
`./scripts/build.ps1 -ThinLTO` across translated code and native overrides.
Its first build recompiles all
sources and performs optimization at link time. Later links reuse
`build/runtime23/thinlto-cache`, with a 2-GiB cache limit. Two backend jobs are
used by default; CMake's `EM2_THIN_LTO_JOBS` controls that limit. A normal
`./scripts/build.ps1` restores per-source optimization; `-NoOptimize` selects
an unoptimized build. All configurations write the same game
executable. The headless regression suite includes cross-file weak/strong
dispatch checks; whole-game performance still requires gameplay measurements.
The reviewed matrix reference/replacement and tuned upload, skinning and
material kernels retain ordinary compilation: unrestricted LTO changed matrix
NaN results and regressed some measured kernel cases. Exact-bit regression
requirements remain in place. Controlled comparisons show mixed performance;
no net gameplay FPS improvement has been established, so ThinLTO stays opt-in.

`--em2_profile_render=true` enables detailed render profiling, including the
ordinary native path used when interpolation is bypassed. Reports separate
native and interpolated render calls, their phases, and nested helper timings.
Inclusive helper times overlap; the self column subtracts measured children.
The remaining time includes unhooked work, waits and profiling overhead. These
are thread-local wall times, not GPU timings or total worker-thread CPU use.
Modes identify the render path, not whether a scene is gameplay or a menu.
For a manual capture, use:

```powershell
./scripts/run.ps1 -ExtraArguments @('--em2_profile_render=true') -LogFile analysis/render-profile.log
```

Profiling defaults off. Reports arrive about every five seconds while rendering
continues; this flag does not automate any gameplay input.

Render binding-cache checks in `8266E0A0` compare value words directly in
original byte order and construct only the final guest register state. All
native short-circuit reads are retained; empty requests avoid the active helper.
`--em2_native_binding_cache=false` delegates to the retained original routine.
Resolved cache-record addresses are reused only inside one conservative guest
mapping window; boundary cases retain per-field mapping. The CPU suite checks
259,206 production-entry contexts/MXCSR states plus control/instruction probes,
including guard pages, unaligned/E0 mappings and wrapped indices. Reuse improves
value-comparison cases about 5..9% over the control without views; early cases
measure near parity with a small version-mismatch tradeoff. No global MOVBE
change was made. Current gameplay FPS is unmeasured.

Native skinning now resolves the palette's SDK physical-view offset once per
workload and rejects mapping-gap/wrapping parameter blocks before output writes.
Large packed jobs (128+ vertices) decode palette row pairs with AVX2; small jobs
retain narrow loads. `--em2_wide_skin_palette=false` restores narrow palette
decoding while retaining the address fix. On-demand caching remains a diagnostic
candidate because it regresses spread-out bone usage. Kernel timings do not
establish gameplay FPS gains.

Material binding now resolves its host CPU implementation during startup,
avoiding a local-static initialization check on each dispatch. Its entry has
no stack saves or ordinary calls. A broader guest-word-reader experiment passed
56,376 complete material context/memory/callback comparisons, but showed no
consistent whole-routine speedup across 80 cases; those readers remain
headless diagnostic candidates and global guest macros remain unchanged.
Dispatcher timings are near parity; no general gameplay FPS gain is established.

Known GPU float/bool/loop and fetch constant writes now inline the native store
and buffer/cache invalidation rules. The stores retain volatile semantics;
other registers still use the original SDK writer. Native metadata is checked at build
time and in the renderer diagnostic. `--em2_inline_gpu_constant_writes=false`
restores the previous shader-constant bulk reader and original SDK writer;
fetch packets then use the original scalar handler. The default
path makes one outlined write call per changed vector, avoiding bookkeeping on
unchanged shader vectors. Fetch words always write/invalidate, including equal
values; 16+ word fetch packets use bulk reads without value deduplication.
Controlled packet timings show substantial CPU savings for
changing data; aggregate gameplay FPS has not been measured.

Set the flag **at startup** to also select a profiled D3D12 processor. Its separate
graphics-worker reports time draw/copy/swap dispatch, including native waits,
and count bulk constant packets. Nested copy work is subtracted from parent
self time. These are CPU wall times, not GPU execution timestamps, actual GPU
draw counts, or total worker CPU use. Inclusive times overlap; root dispatch
time excludes the unmeasured packet parser and time between calls. The two
threads have independent reporting windows. The ordinary processor retains
native draw/copy/swap vtable entries, without per-call profiling flag checks.
The same startup flag now also brackets native deferred-command replay with GPU
timestamp queries. Five-second reports show measured GPU interval total/mean/max,
busy-slot skips and invalid results. The 64-pair pool never waits for profiling;
readback and reuse require completed native submissions. Resources outlive native
shutdown's fence wait. These intervals exclude work before/after deferred replay,
total frame time and whole-device utilization. Query/resolve overhead exists
when profiling is enabled; leave it off for normal performance comparisons.
`--em2_test_shader_storage=true` verifies real GPU timestamps, native delegate
execution and pool reuse/exhaustion without loading gameplay. API semantics:
[Microsoft timing](https://learn.microsoft.com/en-us/windows/win32/direct3d12/timing),
[queries](https://learn.microsoft.com/en-us/windows/win32/direct3d12/queries).


Options → Control Bindings provides separate keyboard/mouse-button and controller
profiles for both players, with seven pages of buttons, triggers and stick directions.
Press A/Enter to capture, X to clear, or use Reset this profile. Keyboard/mouse
profiles also capture Wheel up/down/left/right: scroll in the capture prompt.
High-resolution detents accumulate, and scroll actions use one simulation-tick
pulses; multiple detents in a tick coalesce. Wheel bindings persist and participate
in conflict swapping/reset. Mouse-movement camera aiming remains unimplemented.
 Escape cancels
capture; Back+Start cancels controller capture. F8 restores all profiles while
the binding menu is open. Settings persist in `user_data/controls.ini`.

LB+RB remains usable when conflict-swapped to another action, including camera
directions. A recognized chord consumes both shoulder components until both are
released, avoiding stray camera/sketch actions on release. Fresh individual
shoulder presses resume immediately; chord state is separate for each player.

Default keyboard controls include WASD movement, IJKL aim/camera, Space to
jump/confirm, Backspace for co-op/cancel, Escape for Start/pause, and mouse buttons
for paint/thinner. Experimental offline mouse-camera movement can be enabled
in Advanced Graphics as described above. Enter remains an
additional confirm input. The game retains its original controller prompts.

Single-player character switching uses **F6** or **LB + RB** by default. Rebind
"Swap character" on the binding pages. Both characters must be
available in ordinary gameplay; the Mickey-only prologue, cutscenes and local
co-op reject switching. The camera and controls move to Oswald; press again to
return to Mickey and restore Oswald's AI. Mickey waits while Oswald is selected.
The HUD follows the selected character, including Oswald's health and electricity,
with one centered reticle. Movement, jumping, spin and the electric remote were
tested in Mean Street, including repeated switches and restored Mickey control.
Broader scene/death/respawn behavior still needs validation.

The camera page in Control Bindings adjusts horizontal/vertical aim strength and
axis inversion independently for each keyboard/controller profile. Changes
apply immediately and persist in `user_data/controls.ini`; profile reset and
F8 recovery restore the camera settings too. Strength scales the useful range
after the native deadzone, so lowering sensitivity keeps small stick movements
usable. Default 100% retains exact raw/native input. The adapter observes the
game's configured deadzone. The first keyboard profile's strength and inversion
also control the experimental raw mouse camera, when enabled.

Controller profiles also have a final stick-tuning page with independent
horizontal/vertical deadzones (Original, 0%, 5%, 10%, 15%, 20%, 25%, 30%) and
Linear, Precision or Responsive curves. Precision softens small movements;
Responsive strengthens them. These apply immediately and save per player.
Original deadzones and Linear are the defaults; X resets an individual value,
profile reset and F8 recovery restore them all. Live layout and feel remain
unverified.

Local co-op uses player two's **Start / Pause / join** binding during gameplay.
Player 2 keyboard bindings start unassigned; configure its controls under
Control Bindings first. Native joining now activates Oswald's action controllers;
independent movement and jumping were verified in Mean Street with two virtual
keyboard controllers. Online co-op is still unfinished.

Once the prepared online connection is handed to the game, synchronized startup
uses normal simulation speed even if an offline slow-motion or pause preset is
configured. That offline value is preserved and restored when ownership ends.
Offline movement/animation overrides, console impulses/speed changes, solo
switching and time/step tools are inactive from adoption through loading and
co-op joining, including a failed connection waiting for shutdown. The existing
world-time tool panel explains the lock. Real-loopback policy tests pass;
full native campaign synchronization is still unverified.


An experimental direct-connect Host/Join screen is available in the same
executable with `beetleworx.exe --em2_online_menu=true`. The host enters a reachable
IPv4 or unscoped IPv6 address; its current native save folder is selected
automatically. Browse can select another `user_data`, `42560816`, `00000001`, or
`SAVEGAME` folder, or paste the full `save.dat` path in the field. Host checks for
a nonempty campaign save before hashing files or opening a connection. A profile
without a campaign cannot be continued: first reach an offline save point.
The client pastes the resulting invitation. File checks, save transfer
and isolated session-save installation run before the guest starts. Both players
keep their normal saves. The host's selected files are read into an immutable
bundle; ordinary open writers cause a clear retry error, and read leases prevent
ordinary writes/deletes to existing files during copying. Source inventory and
timestamps are checked again afterward. This is not a filesystem snapshot for
arbitrary memory-mapped writers; close another game using the selected save.
Both players must use the same build/content. The host can select 0–12 ticks of input buffering
(0–400 ms at 30 simulation ticks per second); two ticks remains the default.
Lower buffering reduces added input delay; higher buffering lets inputs arrive
earlier to reduce network stalls. Invitations and LAN discovery carry the chosen
value to the client. It applies only to that online session, not offline timing.
After the connection menu hands off a ready session, a temporary polling thread
services TCP messages, heartbeats and router lease updates during runtime and
renderer initialization. The main guest entry stops and joins that thread before
claiming transport ownership. This does not load a world or consume input ticks;
the existing world-startup deadline still applies. Real TCP fixtures cover a
6.25-second initialization interval, handoff and peer loss before native startup.
The developer-only `em2_online_qa_prelaunch` option connects localhost QA peers
before runtime creation and adopts the same retained-session path. Run
`scripts/test-retained-two-world-startup.ps1` to fingerprint separate copied
campaigns and content views, observe two instances of the canonical executable,
and close only their verified owned windows. It sends no hardware gameplay input or
screenshots. `-VerifyMemoryIsolation` records named sections from the two verified
private processes and checks their pagefile-backed guest mappings are distinct.
`-RequireNativeContinue` makes the test fail unless both original Resume handlers
accept Continue. Cleanup waits for an owned native window if startup has not yet
created one. Retained connections and rendered frames alone do not establish
that native co-op or campaign synchronization succeeded.
`-RequireNativeGameplay` additionally requires two consecutive matching native
state hashes after both worlds attach; it writes `gameplay-observations.json`.
`read-paired-gameplay.ps1 -PairRoot <run>` can compare an existing run without
controlling either game. Eight parser fixtures reject pre-attachment, missing,
nonconsecutive, mismatched and conflicting evidence. This short gate does not
prove campaign transitions or Internet connectivity. With online observation
tracing enabled, two agreed ticks record bounded CRT call traces. The window
starts at zero by default; `-RandomTraceStartTick N` selects a later window.
A lazy four-tick history (4096 events per tick) dumps recent unprinted events
when the native session fails, so a later mismatch retains its exact callers
without logging every successful frame. Session attach/detach clears the history.
`python scripts/compare-native-random.py <run>` identifies differing calls and
rejects incomplete or truncated captures.

Binary installation fingerprints use bounded sequential Windows reads rather
than buffered stream reads. Content/checkpoint hashes remain unchanged. A full
7.47 GB verification used 30,667 read operations after the change; the older
reader had already used over 1.77 million operations before finishing. Measured
scan times improved in the local runs, but cache warmth was not controlled and
this is not a gameplay FPS measurement.
Online target-selection scheduling uses a fixed work charge instead of the
wall-clock elapsed return at native callsite `829507A4`: each selector callback
costs 1/16 of the authored frame budget. Retail iteration, per-observer caps,
update intervals, callbacks and RNG remain in place. Outside committed shared ticks, the manager uses its retail disabled path and
direct registration selectors use their retail zero-budget path, restoring flags
and budget afterward. Both resume within synchronized simulation ticks. Offline operation and the other elapsed-timer callers are unchanged.
Canonical native join cooldown elapsed/reset calls (`8276EFC8`/`8276F070`)
use agreed ticks. Their retail helpers still execute and retain native timer-memory
effects; other timer callers remain unchanged. Native joining itself still runs
through the original factory, profile checks and controller activation.
During online sessions, native `NiPSEmitter` emission runs inside the existing
presentation RNG scope (enabled by default). Original emission, derived callbacks,
particle allocation and rendering continue. Local particle acceptance differences
therefore do not advance gameplay CRT state. Offline emitters retain their original
RNG path. Retail expired-particle fixtures compare complete caller/64KiB memory/FP
output across 0/1/3/17 emissions; live full-spawn/campaign acceptance is separate.
The session nonce and checkpoint define the initial native CRT RNG stream. It
initializes once at the first agreed native rand, without changing the generator;
a first explicit native srand remains authoritative. Co-op join dispatch now
runs on committed Start through the original factory, while presentation-driven
duplicate join attempts are suppressed during startup. Final paired validation
still diverges inside activation: this path is not verified full online play.
`-TraceRandomOwners` enables deeper owner/stack diagnostics in the paired harness;
they are off by default. Lua's wall-clock random seeds are not yet synchronized
across different machines, and campaign-wide native determinism remains unproven.

Once the native title menu reports a valid
save, online startup requests Continue automatically through the original
native handler. While the native profile chooser is idle, the original Menu /
JoinPad handler selects the session's local controller. The native code retains
its assigned-controller, sign-in, profile and save checks. This request is
single-flight and preserves the caller context/FP state; it does not claim that
Continue has happened. Continue remains available as a manual fallback. Use
`--em2_online_auto_continue=false` to retain manual profile selection and Continue.
Paired native runs have accepted Continue and loaded the transferred checkpoint.
This establishes startup progression, not complete campaign synchronization. The
accepted online Continue, automatic or manual, parks both native controller ports before world
preparation, so held physical movement/camera input cannot enter that path.
The input timeline now attaches when the loaded world permits co-op joining,
before Oswald's asynchronous controller activation. Startup samples are neutral;
the client lane supplies P2 Start through the actual shared frames. Movement
resumes after native co-op readiness, with the negotiated input delay. Both
roles and delays zero/two are covered by real TCP/native-Xam fixtures. Full
native-world acceptance remains a separate gate.
Co-op controller activation, join preparation and readiness checks run between
completed fixed simulation updates, including during the synchronized join. Presentation-only frames only poll the
connection. If attachment occurs during a batch, the remaining pre-world ticks
are discarded and the next frame obtains a fresh network tick budget. Scheduler
fixtures cover 30/60/120/240 Hz presentation and multi-tick stalls; this does not
prove that asynchronous native loading reaches readiness identically on both
machines. The manual callback retains the original native return/context/state
behavior and tracks only its live registered menu owner. Automatic queries after
an accepted manual request cannot trigger a duplicate request. Live manual
Continue and full two-world startup remain acceptance items. The
screen shows the listening TCP port for direct connection/port forwarding.
The optional automatic Internet setup checkbox tries PCP first, then NAT-PMP.
If UDP setup fails or times out, it tries UPnP through a background Windows COM
worker. It uses the selected local IPv4 gateway and retains the temporary TCP
lease after session handoff. PCP uses a nonce; UPnP uses a random ownership tag
and verifies the client/port/tag before renewal or best-effort cleanup.
UPnP requests 120 seconds, skips occupied external ports, rejects permanent-only
routers, and publishes only a verified public IPv4 endpoint. Windows SSDP/UPnP
services and compatible router settings are required. UDP negotiation has a
30-second deadline; UPnP setup/renewal has its own 30-second deadline. Cancellation
returns immediately; a late successful Add is cleaned up by the worker.
These paths pass isolated tests; real-router and ISP compatibility remain unverified.

Cancel exits preparation without starting gameplay. This startup UI has compiled
but has not yet been manually exercised; full two-player gameplay and campaign
determinism remain unverified. Internet relay and gameplay reconnection
are not implemented. This is not yet complete online multiplayer.

Per-tick verification includes canonical native level, state and interior names,
player modes/positions, current/max life and ammo, and the simulation-thread gameplay
CRT stream (gameplay digest schema 4). Scoped presentation/particle RNG remains
separate; no gameplay digest fields are removed. World names use the original native
getter layout. Player registry traversal is bounded and checks the original
hash/equality callbacks; component/resource reads reject missing, wrapping or
unavailable memory. World identity and root ownership are checked again before
publishing a sample. This reduces teardown-time invalid reads; it does not pin
engine objects against concurrent destruction or constitute a full world hash.
Missing observations still halt synchronized play until resynchronization;
Level-transition recovery and complete campaign synchronization remain unfinished.

Once the input gate is attached, native level-load calls made inside an agreed
simulation tick rendezvous before entering the loader and again when it returns.
Peers must match the exact destination, tick, call sequence and return result.
The protocol supports up to eight nested calls and stops on a mismatch,
disconnect or two-minute rendezvous timeout. Earlier ticks may finish while a
future load request is pending; the loading tick cannot publish its observation
until both load boundaries agree. Existing world-state verification remains in
effect after the loader returns. This boundary synchronization is tested over
real TCP with synthetic tick owners; actual campaign transitions and delayed
actor creation after loading still require validation. Loads outside an agreed
tick stop synchronized play rather than running independently.
Title shutdown cancels pending rendezvous waits without waiting for the peer
deadline.
Each boundary services its outgoing reply even if the peer arrived first and
local agreement was immediate, before native finalization resumes.

The native loading-screen callback also services the retained online transport
on its established main guest thread, at most once per 50 ms. This keeps
heartbeats and pending messages flowing while loading bypasses normal frame
updates. Worker threads cannot claim or poll that transport. Loading polls do
not attach player controls, consume input frames or declare a world ready.
This covers pumped loading stages; a single blocking operation that never
invokes the callback can still exceed the peer timeout.

Save transfer uses a two-minute inactivity deadline: validated incoming save data
and new outgoing chunks renew it. Clients also acknowledge newly received bytes
so host-side Windows buffering cannot conceal delivery progress. Reports are
coalesced per 32 KiB plus the final tail; duplicate byte counts do not renew the
deadline. Both copies need this build (transport protocol 5).
Continuing transfers may take longer than two
minutes; heartbeats alone cannot keep a stalled transfer or installation alive.
The separate runtime-startup deadline remains two minutes.

Connection errors now explain refused/unreachable hosts, occupied ports and Windows
network restrictions. Handshake errors distinguish build, content, save, invitation
and timing mismatches with recovery instructions. Technical socket codes remain in
the game log. Real loopback tests cover each mismatch and retry after a busy port
or refused connection; WAN gameplay remains unverified.

Online startup, sustained input/controller/verification waits and failures appear
in a noninteractive in-game banner independently of the performance display.
Brief waits stay quiet; failures retain restart guidance. Live appearance still
needs acceptance testing.

Committed online input ticks also select the fractional core-clock interval
phase, independent of local loading history. Native-clock regression checks
cover matching per-tick outputs across different local phases and counter
anchors. Initial world-clock ages and full campaign determinism remain unverified.

The connection screen lists detected local IPv4 and usable IPv6 addresses in an editable dropdown
and provides Browse for the host save folder. Hosts may select a fixed TCP port
for port forwarding, or leave `0` to allocate a port automatically. The displayed
local address is suitable only when the peer can reach that interface; a detected
LAN address is not automatic Internet/NAT traversal.

Hosts can opt into **Try automatic Internet connection (compatible router)**.
Choose this PC's local IPv4 address; compatible routers can supply an Internet
invitation automatically. The Copy button identifies LAN versus Internet codes,
and LAN discovery keeps the local invitation. The host session retains and
renews the temporary TCP mapping after the connection screen closes; stopping
the session requests best-effort cleanup. It supports
[PCP](https://www.rfc-editor.org/rfc/rfc6887),
[NAT-PMP](https://www.rfc-editor.org/rfc/rfc6886), and
[UPnP IGD](https://upnp.org/specs/gw/UPnP-gw-WANIPConnection-v1-Service.pdf)
through the [Windows control-point API](https://learn.microsoft.com/en-us/windows/win32/api/upnp/nf-upnp-iupnpservice-invokeaction).
Automatic setup is Off by default.
In the native Online Multiplayer menu, enable **Auto Internet port** before
selecting **Host game**. The lobby displays **Copy Internet invite** when the
router supplies a public endpoint. **Copy LAN invite** is available while the
router request is pending or if setup fails. The setting applies to hosts
started from the lobby during the current run.

The native lobby makes the invitation available while the host checks its game
files. The guest can begin checking their files during that time. Both copies
must complete verification before save transfer starts. After the guest
installs the shared save, the host selects **Start online game**. The guest
enters the shared game automatically after acknowledging the host's start.
Both players need the same Beetleworx build.
Private/shared upstream addresses are rejected as Internet endpoints. Windows
firewall permission may still be needed. Loopback protocol/ownership tests pass;
actual adapter/gateway selection, router compatibility, UI layout and Internet
reachability remain unverified. A relay or VPN may be necessary on unsupported
networks.

On the experimental connection screen, **Find LAN games** searches for waiting
IPv4 hosts on local networks; select one and press **Join selected LAN game**.
The host must finish preparing its save first. Discovery uses UDP port41037,
with normal TCP compatibility checks and isolated save transfer after joining.
Loopback discovery and the discovered-invitation boot flow pass tests; actual
cross-machine LAN discovery and the visible window still need acceptance.

IPv6 invitations use an 81-character code; existing 62-character IPv4 codes keep
their original format. IPv6 accepts global/ULA and loopback addresses; scoped
link-local addresses and mapped IPv4 addresses are excluded. The selected
address family determines the listener. Real loopback tests cover both families,
including save installation and retained session handoff; cross-machine Internet
and full campaign multiplayer acceptance remain outstanding.

The same executable runs headless checks with `--em2_test_pc_settings=true`; these
cover PC settings, control mapping/capture, and the experimental simulation math.
See [the upgrade ledger](docs/pc-upgrade-work.md) for the remaining requested work.
Authored host/game call declarations explicitly permit C++ exceptions across
the C-linkage boundary, so recovery catches and cleanup remain active under
`/EHsc`. A native callback regression verifies catch and cleanup through the
original integration-header method. This does not prove recovery from hardware
faults or the reported graphics-upgrade level-entry crash.
Skin replay also unwinds its temporary source pointer, guest context, floating-point
state and trace scope after C++ job failures. Saved job inputs are rewound only
after a confirmed drain; failed drains retain live buffers and propagate failure.
Injected-failure regressions cover this logic; a real native worker fault and
automatic continuation after one remain unverified.



For offline timing investigation, Advanced Graphics → Gameplay timing →
Native delta (experimental) uses
the original variable-delta world update above 30 FPS, including native input,
camera and animation updates every frame. It bypasses fixed-tick interpolation;
the native camera filter delta-time corrections still apply. This experimental
path defaults off and the choice persists in pc-settings.ini. It applies on the
next frame, without a restart. Physics, scripted timers and campaign behavior
have not been validated at higher update rates. The debug flag
`--em2_native_variable_simulation=true` also forces this mode. Online sessions and an explicit
`--em2_fixed_simulation=true` retain fixed timing regardless of this flag.
This mode has a confirmed manual report of jittery movement and incorrect
physics; it remains experimental. It automatically records five-second native
world-dispatch and integration-interval summaries. `--em2_trace_physics=true`
also enables these diagnostics in other timing modes. Dispatches are grouped by
world and thread; integration batches include body-entry counts and native step-start ranges. World-dispatch
summaries now distinguish successful returns from integration/collision/advance
retries and unknown result codes. These counts are not whole-world tick frequency. Diagnostics preserve native intervals and
floating-point state and do not correct the gameplay issue.

Native CPU skinning also reuses a blended matrix across consecutive vertices
with bit-identical bone indices and weights in jobs of at least128 vertices.
Eight consecutive misses stop probing; small jobs keep their existing path.
`em2_reuse_skin_blends=false` disables it. Full-buffer/MXCSR and guard-page
comparisons pass. Measured repeated-key kernels are1.13-1.50x faster; changing-key
controls include about2% slower cases. These timings do not measure whole-game FPS.

CPU optimizations also include SIMD nonvolatile-register save/restore and
batched inactive scene-flag processing. Both are enabled by default, retain
the original paths for fallback, and are checked against original translated
register/memory results in the same executable's regression mode. Debug flags
`--em2_native_register_bank=false` and `--em2_native_visibility_scan=false`
disable them individually. Optional `--em2_benchmark_register_bank=true` and
`--em2_benchmark_visibility_scan=true` add controlled routine benchmarks to
`--em2_test_pc_settings=true`; these measurements are not whole-game FPS results.

Native matrix multiplication now packs sixteen output word stores into four
vector stores, including byte-unaligned destinations. It retains the native
matrix calculations, register results and fallback for MMIO/address boundaries.
`--em2_native_matrix_stores=false` disables this optimization;
`--em2_benchmark_matrix_math=true` adds controlled whole-routine comparisons to
the same executable's headless regression mode.

Shader-parameter binding skips unchanged vertex/pixel stages independently,
retaining native register results and final dirty-mask clearing. Supported CPUs
also use specialized bit-scan/shift instructions; other CPUs retain the generic
path. `--em2_native_material_binding=false` restores the original routine.
`--em2_benchmark_material_binding=true` compares complete binding calls in the
headless regression mode, including mixed clean/changed stages. These are
routine measurements; gameplay FPS and the frequency of clean stages remain
unmeasured.

The D3D12 command processor also avoids redundant float, bool and loop constant
writes. Exact register bits are compared, including NaNs and negative zero;
fetch registers retain their native invalidation behavior. This keeps current
constant buffers valid across identical writes and reduces CPU work parsing
repeated shader constants. `--em2_deduplicate_gpu_constants=false` restores
native writes. The build bridges only the verified installed SDK archive;
another SDK requires a factory/ABI review before this bridge can be rebuilt.
The existing `--em2_test_shader_storage=true` diagnostic now also checks the
actual derived backend through 56,320 Type-0 parser comparisons, 3,120 direct
ring comparisons, 23,744 fetch comparisons and 85,120 Type-3 comparisons. Add
`--em2_benchmark_gpu_constants=true` for controlled complete-packet CPU timings.
These do not establish a gameplay FPS improvement or GPU upload-time result.

Large Type-0 and immediate Type-3 SET_CONSTANT/SET_CONSTANT2/SET_SHADER_CONSTANTS
packets use a bulk ring-buffer read and exact-bit SIMD decoding/comparison.
This covers 16..2048 float/bool/loop or fetch-only words on the project D3D12
processor. Fetch words always retain invalidation, including repeated values.
Changed shader words retain native register effects. Unsupported/mixed ranges,
control registers, short payloads and special Type-0 flags use the SDK handler.
Ring wrapping and byte-unaligned payloads/metadata are covered by the diagnostic;
85,120 additional native-reference comparisons cover the three Type-3 forms.
Type-3 occurrence in actual gameplay and overall FPS impact remain unmeasured. `--em2_batch_gpu_constant_reads=false` restores scalar packet reads
independently of the existing deduplication flag. Both optimizations default on.
The diagnostic intentionally logs SDK overflow errors for truncated-packet
negative cases, then verifies the same result and reader state in both modes.

Long scalar/vector shader-constant and 24-byte fetch-record uploads also use
direct prefix copies while retaining native packet encoding and final register
state. Fetch masks with no run of eight records delegate to the original routine.
Overlap, MMIO, guest wrap and potentially aliased physical views retain native
copy order. This is enabled by
default; `--em2_native_shader_upload=false` disables it. In headless regressions,
`--em2_benchmark_shader_upload=true` compares complete upload-routine timings.

Native CPU skinning uses packed 12-byte SIMD transfers for positions, normals,
morphs and tangents on jobs of at least 16 vertices. Smaller jobs and address
boundary cases retain scalar transfers. `--em2_packed_skin_streams=false`
disables this transfer optimization; `--em2_benchmark_skinning=true` in the
headless regression mode compares complete kernels against the previous SIMD
path. Kernel speedups do not establish a whole-game FPS gain.

Read-only archive I/O has an experimental `--em2_mapped_pack_reads=true`
startup option, off by default. It lazily maps `.pak` files after repeated small
reads and preserves native bulk, EOF and non-pack paths. Only the actual read-only
game mount is eligible; saves and write-capable handles retain native I/O. A mapped
pack stays locked against writes until closed. Mapped-copy faults recover through
a guarded native fallback. Warm repeated-read benchmarks improve, but short
workloads lose time to setup, so actual loading/streaming/FPS benefit is unproven.
`--em2_test_pc_settings=true --em2_benchmark_pack_reads=true` runs controlled
open/read/close comparisons in the same EXE without gameplay. CPU/GPU diagnostics
now isolate crash reports under `diagnostic-crashes`, preserving the ordinary
crash log. See `analysis/pack-reads-results.json` for measured costs and limits.

The renderer now initializes the SDK's persistent shader/pipeline storage at
startup. Its files live beside the EXE in `cache/renderer-v1/rex-<SDK hash>`;
CPU-only rebuilds retain them, and an SDK change selects a fresh namespace.
The SDK layer contains microcode and pipeline descriptions and still translates
shaders during preload. An optional `compiled-pso-v1` layer stores actual
driver-compiled graphics pipeline blobs. Its key includes the adapter, serialized
root signature, shader bytes and canonical pipeline state. Bounded background
writes drain on shutdown. Corrupt or rejected records fall back to native
creation and can be repaired. Real GPU tests verify disk reuse in fresh renderer
instances and a separate process. Gameplay loading-time and FPS gains remain
unmeasured. Warm synthetic measurements found the full cache slower than native
creation, so this layer defaults OFF. `--em2_persistent_pso_cache=true` opts in;
`--em2_persistent_shader_cache=false` disables both.
An exclusive writer lease prevents simultaneous instances from sharing writes;
a busy or unwritable cache falls back to uncached rendering.
`--em2_test_shader_storage=true` tests real D3D12 pipeline creation, persistence,
preloading into two native cache instances, shader identities and valid cached
translations, lease contention and reopening without launching the guest game.
It uses synthetic shaders and an isolated test directory. Both default bindless
resources and `--d3d12_bindless=false` have been checked. The diagnostic makes
pipeline creation synchronous; normal gameplay keeps its existing threading.
`--em2_test_shader_storage_path=<directory>` optionally shares an isolated test
cache across diagnostic processes; it does not start gameplay.

## Implementation

- `config/em2.toml`: verified register helper addresses and metadata skip.
- `scripts/recover-switches.py`: recovers reordered inline absolute tables,
  scheduled byte-offset tables, and halfword-offset tables.
- `patches/xenonrecomp.patch`: bounded function discovery, switch-aware control
  flow, native lowering additions, and half-float pack/unpack support.
- `patches/recompiler_extra.inl`: additional instruction emitters.
- `runtime/ppc_context.h`: uses the runtime's context layout and C import linkage.
- `runtime/entrypoints.cpp`: keeps guest execution and APC callbacks on the same
  thread context for compatibility with ReXGlue 0.1.
- Native jump-buffer handling and physical-alias accesses use the SDK's helpers.
- `scripts/compact-declarations.py`: gives each translation unit only the
  declarations it uses, without changing its instruction bodies.
- `src/analysis_tests.cpp` and `runtime/lowering_tests.cpp`: regressions for
  bounded discovery, conditional counter returns, and execution of generated
  instruction fixtures.
  Runtime instruction, settings, controls and simulation checks execute inside
  `beetleworx.exe` through `scripts/test.ps1`.

Game executables, loaded images, translated code, logs, and build outputs remain
local under ignored directories. Nothing has been uploaded or published.

## Upstream references

XenonRecomp is pinned to `ddd128bcca99fe8bfbb99bea583c972351fa6ace` as a Git
submodule. Local modifications are reproducible with `scripts/apply-patches.ps1`.
ReXGlue runtime sources and licenses remain in the installed SDK. The runtime
host starts from that SDK's generated application template. SIMD conversion uses
the SDK's SIMDe dependency. See upstream license files before redistribution.








Live shadow resizing now reserves retirement bookkeeping before native creation
or sampler-ownership transfer. Host allocation failure at this stage reports a
memory limit and preserves active targets. Staging uses a fixed three-target
array; all new/reused combinations and failure quarantine pass allocator-injection
checks. This does not establish that the reported level-entry crash is fixed;
full native creation and repeated in-level changes still need manual validation.
Evidence: `analysis/shadow-publication-results.json`.

Native-variable camera timing remains experimental. Known normal-camera pitch
springs now advance their native error/velocity state by elapsed time, including
recenter velocity handoff and sign changes. Reference-rate calls preserve native
behavior; unsupported spectra and scripted/online paths retain the original
implementation. The correction is enabled by default; use
`--em2_camera_spring_timing=false` for comparison. Native parent trajectories at
60/120/240Hz and uneven intervals match authored30Hz samples within0.000000179
radians in controlled fixtures. Live camera feel and full character/contact physics
remain unverified. See `analysis/camera-spring-correction-results.json`.

Native pitch manual/strong input and recenter-velocity thresholds now scale with
their separate native elapsed-time clocks. Absolute settled-angle tolerance is
unchanged. Complete native fixtures preserve angular-speed acceptance at4..1000Hz,
including local co-op, boundaries and native reference-rate parity. Use
`--em2_camera_pitch_threshold_timing=false` for comparison. Live feel and full
high-rate character physics remain unverified. Evidence: `analysis/pitch-threshold-results.json`.

A fresh live-shadow crash after rapid2048/1024/2048 changes is preserved in
`analysis/live-shadow-human-20261030-082251-results.json`. Native CPU allocator
corruption is under investigation; live-shadow/level stability remains unverified.
The session's updated saved settings were preserved.

Live shadow changes now restore the original shared-source CPU cache invalidation
before detaching retired maps. Native destructor ordering and repeated transfer
fixtures pass, along with the full runtime suite. The rapid live-shadow crash
still needs in-game confirmation; this is a verified lifecycle correction, not
a claim of resolved level stability. Evidence: `analysis/shadow-native-cache-results.json`.

Rapid settings selections now supersede queued UI/GPU work by request revision.
Stale failures cannot roll back a newer selection of the same value, and old
render-scale completions cannot overwrite the latest status. CPU and offscreen
GPU diagnostics pass; live menu latency is unmeasured. A new shadow creation
crash remains under investigation: `analysis/live-setting-versions-results.json`.


## Engine console controls

Press the backtick key (`) to open the console. It opens on **Gameplay**, with
the retail level-state browser, native developer actions and a multiline Lua
editor. **Tools** contains world pause/step/speed, character velocity, movement
tuning, graphics/camera choices, combat effects, and native entity/animation tools.
The **Commands** tab groups actions by system and provides labeled argument
fields for every command. Example values are filled in initially; edit them and
review **Command preview** before pressing **Run**. **Choose a value** lists named
graphics choices, movement switches, native animation identifiers and backend
variables where available. Quoting is automatic. Optional fields can be blank;
required fields are marked with an asterisk. **Copy to command line** lets you
continue editing the generated command. **Engine variables** exposes registered
backend variables, descriptions, defaults and live-edit capability.

Use `engine shadow`, `engine camera`, or `engine animation` to search across
commands, graphics, movement tuning and backend variables together. Results
include current settings and distinguish live variables from startup-only ones.
The input field shows syntax help while you type.

Gameplay examples (enter a normal offline level before using level/developer
actions):

```text
levels meanstreet
level.load meanstreet_north.visit_1
level.current
level.restart
debug fly
debug teleport_camera
debug god_on
debug god_off
debug refill
debug hud
give tickets 500
unlock abilities
unlock costumes
unlock pins
lua return LevelManager_GetCurrentLevelName(), GetAmmo("Paint")
lua SetAmmo("Paint", GetAmmoMax("Paint"))
lua.api SetHealth
game.get GodModeOnPlayer
game.set GodModeOnPlayer True
```

`level.load` validates exact destination IDs against 205 state records recovered
from the original `levels/master.lvlc`, then calls `CleanLoadLevel` in the game VM.
It uses the native clean-transition path; it does not change startup files.
`debug` lists all developer actions, including native HUD/debug displays,
cutscene-input release, inkwells and buddy movement/follow helpers. Player actions
use the original `GetPlayer()` target; use Lua and `GetPlayerOrAI(PLAYER_TWO)` when
you need an explicit second-player target. Resource and unlock changes can be
preserved by normal game autosaves.

`lua` takes the remainder as source, preserving Lua's own quotes. A single outer
quoted source argument is also accepted for generated command forms. The
Gameplay editor accepts up to 16 KiB of multiline source. Source is compiled by
EM2's retail Kore/Lua-compatible compiler and executed with its original protected
call function in the live VM's globals. Return values and `print` are copied to
Output. Syntax/runtime VM errors are reported; arbitrary engine calls can still
violate native assumptions. Code runs at the next successful protected VM boundary
on that VM's current thread; no Lua-state pointer is retained across level unloads.
Resume or step if paused simulation is preventing that boundary.
`lua.file experiments.lua` runs a reusable script from `user_data/scripts` in
that same live VM. Absolute paths are accepted, including quoted paths containing
spaces. Files use the same 16 KiB source limit and protected execution as inline
Lua; a UTF-8 BOM is accepted. Edit the file and rerun the command to experiment
without restarting. The Gameplay tab also has a file field and an example picker
for level inspection, resources, god-mode state, fly camera and buddy movement;
choosing an example loads the editor without executing it.

`lua.cancel` (or **Cancel queued Lua**) clears waiting requests. Starting an
online session also cancels waiting requests; it never carries an offline Lua
mutation across synchronized-session ownership.

`lua.globals [search]` searches the actual loaded VM globals, including Lua
helpers absent from the recovered export index. `lua.inspect math` or
`lua.inspect <global.table.path> [search]` shows raw table entries, types and
primitive values. These commands execute through the same protected VM boundary
as snippets; they do not invoke table metamethods. Output contains at most 100
matches and scans at most 4096 entries, with explicit counts/limit feedback.
The Gameplay tab also has a **Browse loaded globals** button. These are read-only
inspections; table entries may change as levels and scripts load.

`lua.api` searches 939 recovered native export names and includes original
parameter text for 825 of them. The editor's API search shows those declarations
and drafts a call. `NiFixedString` arguments are Lua strings; `int`/`float` are
numbers; `ScriptEntityHandle` arguments can come from `GetPlayer()` or
`GetPlayerOrAI(PLAYER_TWO)`. Script-defined helpers add functions beyond this index.
Lua function names and level destinations also support Tab completion.

`game.get`/`game.set` address the game's native configuration store through
`GetConfigVariableString`/`SetConfigVariableString`, independently of PC backend
`get`/`set` variables. Writes return success and the resulting string value.
Some native systems cache configuration values; this does not force those
systems to reinitialize. These edits do not rewrite ConfigFiles.ini.

Gameplay actions require a world that has reached playable control once.
Temporarily disabled movement during a later cutscene keeps that world available;
world identity replacement/unload or a native level-load request invalidates
pending gameplay requests. Lua/developer mutations are disabled during retained
online sessions. Level changes and developer gameplay effects remain manual
acceptance items; no automated gameplay input is used for validation.

`--em2_verify_game_console_vm=true` runs non-gameplay probes at real protected VM
boundaries: arithmetic/strings/booleans/nil, native function visibility, syntax and
runtime errors, `print`, and a temporary private configuration key's string/bool
read-write-restore cycle, live global discovery, and raw local table inspection.
It checks stack top/base/call-index and complete caller
context/MXCSR. The same executable is used throughout.

In Tools, expand **Native entity and animation graph**, then **Refresh loaded
graph** during offline gameplay. Event and variable lists come from that actor's
actual graph. Select an event to send it, or a variable to hold a float/integer
override. Variable types are not known: speed/blend values generally use floats,
while flags/state counters use integers. Release overrides to return those values
to game control. Refresh after changing characters or levels. Raw graph experiments
are not saved. Native actions report their result on the console's status line.
**Snapshot values** displays both float and integer interpretations of the raw
graph words at the last refresh; these are not inferred variable types.

**Native motion: launch either character** applies EM2's original external-motion
function to the selected target. It replaces the preceding external impulse;
positive Y launches upward. This also works with an explicit `entity.push` actor,
independently of which character you control. It does not teleport the actor.

Commands can use names rather than setting indices or combat type IDs:

```text
help animation
engine shadow
graphics bloom "Smooth sampling"
movement dash_cooldown 0
movement dash_cooldown reset
target oswald
entity.push 0 15 0 0.9 oswald
combat spin 5 8
```

Run one command per line. `graphics <setting>` lists readable choices, and
`movement` lists all movement parameters. Graphics and movement changes save
automatically. Native entity, animation and combat commands follow `target`;
`player.speed` and `player.push` operate on the controlled character. Tab completes
commands and quoted choices; Up/Down recalls history. Game Options pages remain
in the native game UI. Live gameplay outcomes still need manual verification.


Four profiled integer video routines use local temporary registers. Three cache
their leaf cores; the symbol decoder accelerates only long loop prefixes and
executes its final iteration, short paths and guest fill call in original code.
Arithmetic and guest memory accesses remain original. This is automatic;
`--em2_native_video_blocks=false` disables acceleration. The same executable runs
focused checks with `--em2_test_video_blocks=true`, plus
`--em2_benchmark_video_blocks=true` for alternating-order benchmarks against
the retained original routines. The symbol decoder64-symbol case measured
874.0ns versus1331.5ns (1.52x); cached/repeated cases were near parity. These
are routine measurements, not gameplay FPS. `--em2_verify_video_blocks=true`
compares original context, memory and MXCSR on16 real cases per kernel;
symbol cases specifically select long runs.64 actual intro checks passed.
A mismatch restores original results and disables acceleration; normal play
does not duplicate decoding. Evidence: `analysis/video-symbol-validation.json`.
