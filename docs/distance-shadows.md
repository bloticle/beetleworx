# Distance-based shadows: implementation and remaining acceptance

The native point/spot shadow resolution policy is implemented behind
`em2_distance_shadows` (default **Off**). The native menu now exposes
Distance shadow resolution with Fixed, Quality, Balanced and Performance
presets; broader release acceptance remains separate. It does not create missing shadow casters or convert the three native
textures into cascaded sun shadows.

The implementation follows the actual light-selection boundary:

- `827A58A8` asks the light selector for up to three lights, then calls
  `829BA908` at return address `827A59D4`.
- The selector at `830B7E90` considers authored modes and calls `830B8C40` to
  score local lights against the supplied cameras. It is real selection code,
  not a missing external function or an unconditional stub.
- The camera arguments are an explicitly counted array (`r28` and `r26` at
  preparation entry), not a primary-camera pointer or NUL-terminated list. A
  live check caught the initial incorrect interpretation; the conservative
  camera validation retained full quality. The reader and fixtures now use
  the actual counted contract. Capture `private-gpu-profile-20261110-035002-fe66f530`
  confirmed count 1 and native `NiCamera` vtable `8203E610` after the correction.
- Native generator views retain their effect at `+80` and actual map target at
  `+84`. `ShadowMapID` selects the target independently of selection-array
  order. Distance LOD follows this binding before choosing a target size.
- Point/spot maps use the nearest supplied camera position. Player target zero
  and directional lights retain their configured quality. Unknown cameras,
  bindings or nonfinite data fall back to full quality.

The Balanced preset uses reduction boundaries of 25 and 60 world
units, restoration boundaries of 20 and 50, and a one-second reduction cooldown.
Approaching a light restores detail immediately. The configured map size remains
the upper bound and 128 pixels is the lower bound. Native transactional resizing,
view refresh, GPU draining and resource retirement apply the changes. Disabling
the policy restores configured dimensions. These thresholds are experimental;
player-facing presets are described below; broader visual quality validation remains open.

Tests cover transition boundaries, hysteresis, cooldowns, teleports, invalid
distances and clocks, per-map limits, selected-order versus target-order mapping,
the nearest additional camera, directional-light preservation, foreign bindings
and disabling. These establish policy and binding behavior with controlled native
memory fixtures, not shadow rendering quality or a game FPS increase.

Live read-only evidence explains why resizing alone cannot complete the feature:

| Capture | Native scene | Evidence |
| --- | --- | --- |
| `private-gpu-profile-20261110-032423-f98b70ab` | Imported full GV | Three allocated targets; no bound depth-shadow sources/views |
| `private-gpu-profile-20261110-033456-36226015` | Retail Autotopia Section C | Preparation enabled; selector supplies no shadow lights at the saved view; four native spotlights, none retaining a shadow generator |
| `private-gpu-profile-20261110-033815-902b34b4` | Retail Mean Street North visit 1 | Native preparation enabled; no selected shadow lights; two constructed inactive spotlights with zero diffuse color |

All captures used the existing executable, isolated copies of saves/settings and
engine-driven loading. No gameplay input, camera input or screenshots were used.
These are specific scene/view observations, not a claim that every game scene
lacks shadow casting. Character grounding projectors use a separate native path
and do not establish active depth-map shadow casting.

`scripts/inspect-native-light-shadows.py` validates the captured executable hash
and native registry/selector layout, then records light instances, generator
ownership and selector records using read-only process memory. It never writes
or suspends the game. `em2_trace_shadow_preparation` records bounded observations
at the real native selection boundary. Both diagnostics distinguish allocated
textures from actual active views.

Remaining work includes active-caster test scenes, GPU/visual validation of all
LOD transitions, near/far caster coverage and bias, menu configuration, unload/
reload and co-op views, EM1 lighting/shadow compatibility, and true geometry-based
sun shadows where authored local-light maps are insufficient. Distance policy
tests or texture-size changes alone do not satisfy these acceptance requirements.

### Native cast probe and binding correction (November 9)

A private-only `em2_probe_shadow_mode=2` diagnostic temporarily supplies an
existing active authored spotlight to the original native preparation path. It
restores the input array on exit/unwind and leaves normal selection unchanged
when disabled. It is not a release feature or a new sun-shadow implementation.

Capture `private-gpu-profile-20261110-040124-6680078e` established a real native
`NiShadowGenerator` retaining the authored spotlight, bound to environment map
one at 1024 by 1024. The process remained active until the protected close script
finished it, with normal user files unchanged. A bound generator alone does not
establish visible shadow pixels or useful caster coverage.

The live layout exposed an additional policy bug: the first selected light can
occupy native view one, independently of selection order. The distance reader now
scans native views and admits only effects present in the selected-light set,
then follows each view's retained target. Regression coverage includes mismatched
selection/view indices and restoration for a retained but unselected effect.

Capture `private-gpu-profile-20261110-040613-c7af6040`, with distance policy and
GPU instrumentation enabled, crashed immediately after binding. Its matching map
symbolizes the failure to `8256E5E0`, called by `8256C920` in native task execution:
a null object was supplied to a method that accesses its property slots. There
was no live resize acceptance in this failed run. The precise cause remains
under investigation; it is not evidence that distance shadows are finished.

Both read-only inspectors now resolve the guest mapping from the matching native
Runtime accessor and validate the guest image. A fixed 4 GiB host-base assumption
was incorrect for the first capture, which reserved its guest mapping at 8 GiB.
The object at native target offset 20 was initially reported as auxiliary.
Its actual RTTI getter now identifies it as `NiDepthTexture` (see below), with
the known 56-byte layout of table `821DE978`; this identification does not rely
on an adjacent RTTI string. The render click is at target offset 24;
its child retains the shared source at 36 and native camera at 44. Older JSON
field names `material`, `binding_map` and `native_task` were provisional labels;
the recorded raw words remain authoritative.

The corrected distance/view-index build passed the complete in-executable
regression suite (exit 0; `shadow-distance-native-view-regressions-result.json`).
Its no-GPU-profiling live capture `private-gpu-profile-20261110-040928-787ecba1`
reproduced the same null-property-state crash before any resize log. This rules
out GPU instrumentation as a necessary trigger, but does not yet identify the
initialization error. Both failed captures preserved normal user files. The
private probe and distance feature remain disabled by default.

### Retired-view source lifetime

The bounded child-source trace in capture
`private-gpu-profile-20261110-041424-49653e9f` identified the crash precisely.
After the environment map changed from 1024 to 256, the old child view
`A22A7440` (type `8203EA6C`) was still executing in the native render-click
queue, but commit had cleared its shared source at offset 36 and index at 40.
This is a retirement/lifetime bug in live resizing, not proof of a missing stock
constructor dependency. Earlier captures' truncated logs did not establish that
resizing had never started.

Commit now transfers target-slot ownership while retaining the old child's
source reference and native view index. The child can finish queued work. An
identity-checked retirement record releases that reference only when the native
child destructor actually runs. Reusing a retired target removes its retirement
record, reattaches its existing source and preserves balanced references.
Ordinary destructors still execute their original cache invalidation; transferred
old children do not invalidate replacement materials. Retirement bookkeeping is
reserved before the transaction mutates native bindings.

The crash observer preserves the original failed call. No generic null-pointer
return, skipped draw, or disabled culling path is used to hide the error.

### Live transition and cleanup correction

Three independent transition errors were corrected:

- A queued old child lost its shared source too early. Retirement now preserves
  source/index until actual native child destruction, including target reuse.
- During the one-frame rebind, the policy treated its own retained old target
  as foreign and reversed the resize. Tracking now retains its logical slot.
- The old generated patch bypassed a black-light branch rather than the earlier
  unchanged-selection return. The new patch bypasses the correct return only
  when a pending active source needs refresh; black-light behavior is unchanged.

This made the generator actually bind the smaller target. Restoring full quality
then exposed a separate native double free. A 52-byte texture header was freed
immediately at `8267CC74`, and then again by native deferred cleanup at
`82681788`; the second free read a recycled allocator header. Capture
`private-gpu-profile-20261110-043827-0b5c014c` records both calls and the changed
allocation offset before the fault. This was not solved by skipping rendering.

`native_texture_retirement.cpp` checks the exact pending resource queue under
its original `+520` critical section. Only when that queue already owns the
exact header does the native texture-data destructor omit its immediate free.
The original queue, GPU-busy check, backing cleanup and final header free remain
in place. Unqueued/unknown allocations use ordinary cleanup. Both native
texture-data destructor bodies are extracted with fixed source hashes, so an
upstream body change requires review. Temporary global allocation/free observers
used to diagnose this were removed after capturing the evidence.

Capture `private-gpu-profile-20261110-044809-8bc0f1dc` completed the private
full/reduced/full/reduced cycle and restored its original policy. Logs show exact
queue matches, native retirement completion and valid deferred header cleanup.
Live inspection after the cycle confirmed sizes 512/256/1024, generator one bound
to the reduced environment target, and cached dimension 256. The process was
closed through the protected script, with normal user files unchanged.
The private timed cycle allows startup/loading frames; its FPS samples are not
quality-matched performance comparisons. The first dense-frame-only attempt did
not complete because LOD lowered draw counts below that diagnostic's threshold.

These results establish active native allocation, rebinding and lifetime
transitions in this scene. They do not establish visible shadow-pixel coverage,
all-scene quality, a complete directional/sun-shadow implementation, or EM1/co-op
acceptance. The end-user distance option therefore remains disabled by default
and has no release menu exposure yet.

Final cleaned build verification: the complete in-executable regression suite
passed (`native-shadow-release-cleanup-regressions-result.json`, exit 0).
Capture `private-gpu-profile-20261110-045310-15d0723f` then completed the same
cycle without the temporary general-free observers. The exact binary hash is
`56D8C4FF39C436DB924E845E83AB1B629E6F13C3117B1F54D3D3452715AF85E8`.
`native-shadow-release-cleanup-live-result.json` records transitions
256/1024/256, final active binding/cached dimension 256, no crash log and no input
or screenshots. Protected closure verified normal user files unchanged.


### Opaque caster investigation (2026-11-10)

The earlier resource cycles did not prove that geometry reached the map. A
read-only capture in Autotopia found the active click's rendered-object count
and last-rendered frame both zero. Its shadow camera had 14 cached candidates,
but the native mesh predicate rejected all ten meshes. Their authored no-shadow
bit (`0x400`) prevented the engine from deriving its cast bit (`0x800`).

An isolated `-OpaqueShadowCasters` probe now temporarily clears that authored
bit only for known, enabled opaque meshes, calls the original eligibility
recompute, and restores only the owned shadow bits after native validation and
rendering. Blended/cutout meshes, unsupported material modes and unknown layouts
retain their original behavior. The original culling validator, material
renderer and shadow generator remain in use. This is a private experiment,
not an enabled release setting or a universal material policy.

The full regression suite passed for binary
`D0A487237F118512F9A3DA99D4333A5D4F2C7BBDD755ACF07093EB1F286739FA`,
including actual native recompute/predicate calls, exclusions and flag
restoration during exception unwinding. Private capture
`private-gpu-profile-20261110-051646-720e3cf4` reports one rendered mesh in the
active environment shadow click and last-rendered frame 2639; the unchanged
baseline reported zero. Evidence is
`analysis/opaque-native-shadow-caster-live-resources.json`. This establishes
native caster submission, not nonempty GPU shadow pixels or visual quality.
The normal save/settings hashes were unchanged after protected closure.

GPU diagnostic reports can now include all groups, including unmeasured ones,
with `-GpuReportGroups 4096`. Exact prepared-DXBC hashes can be mapped back to
original effect aliases using `index-prepared-shader-identities.py` and
`summarize-gpu-work-profile.py --game-identities`. Identical depth programs have
multiple effect aliases; their presence alone does not prove a shadow draw.


The first resize cycle with the opaque probe exposed a further defect:
`8256E7E0`, previously described as material-cache invalidation, is full source
registry teardown. The live registry dropped from populated to zero and stayed
empty after resolution changes. Consequently a crash-free resize cycle alone
was insufficient acceptance. The replacement now retains registered objects
and their counters, rebinds the native camera, and dirties only that shadow
camera's visibility result. Full teardown remains in the ordinary native
source-destruction path. New regression and live-cycle results are recorded
separately from the older empty-map lifetime captures.


The corrected binary
`C2EAAB8D6A783F36F4C3798E9E10D6086CC673D4D2AFDE9724CEF697E0CE91A9`
passed the complete regression suite (exit 0). Capture
`private-gpu-profile-20261110-052135-901f87ee` then retained 1089/1116/1123/1123
registered objects through map widths 256/1024/256/1024. Each inspected active
map reported one rendered mesh and an advancing rendered frame, including after
restoration. Protected closure verified unchanged normal user files; no crash
log, gameplay input or screenshots. See
`analysis/shadow-camera-registry-preservation-live-result.json` and its four
read-only snapshots. GPU-pixel/visual quality and broader gameplay acceptance
remain open. The private forced-light/opaque probe remains disabled normally.


### Actual GPU contents (2026-11-10)

The original numeric probe inspected the R32 color attachment, not the separate
depth texture. In the earlier Section C fixture, both forced authored lights
produced only `1.0`: 1,048,576 finite R32_FLOAT texels at 1024, with no variation.
Rendered-object counts alone did not establish working shadows. Mode 2 collects one opaque mesh; mode 3 collects thirteen.
The latter's original material selector does find
`Gen3_SpotLight_ShadowWrite`. A missing selected shader therefore does not
explain the empty map. The draw/pass state, camera/projection and resolve path
still require investigation before enabling this feature for players.

`-ShadowPixels` enables a one-shot numeric GPU probe in an isolated profile at
1x internal resolution. It follows the actual target/click/generator and native
texture header, validates dimensions/format/pitch/endian/address bounds, waits
two seconds, then schedules an outside-packet graphics-worker callback. The
callback uses the native resource checkpoint, requires complete GPU-resolved
coverage in the tracked ledger, copies through native shared-memory barriers,
and waits for the GPU fence before reading values. It rejects descriptor
changes across capture and retains readback resources if GPU completion is
unknown. No CPU upload is requested to fill missing data. It logs extrema,
finite/zero/one counts and variation; it produces no image and sends no input.
The launch script requires 1x scaling; bounded transition readback now supports
stable replacements during the resize-cycle diagnostic.

Evidence: captures `private-gpu-profile-20261110-052912-6ad839c0` and
`private-gpu-profile-20261110-053740-e48aa979` have explicit GPU-pixel log rows;
`analysis/native-shadow-selected-shader-name.json` identifies the selected
native shader. Protected closure kept normal user files unchanged. The
intermediate shader-selection observer was removed from the final runtime to
avoid a normal draw-path hook.

A private alpha-tested candidate was also removed after it failed to admit
additional casters. Executing original `825874E8` now verifies that either
blending or alpha testing blocks its derived cast bit. In `82587070` the blend
branch returns one; the earlier interpretation that it returned zero was
incorrect. Supporting alpha-correct shadows needs a real rendering path,
not an unconditional cast-bit override.


Final cleaned build
`F8AD426FECDB37110A1F72B14DAFE431D5453B9ECAFD3F1979CA348946FD8BE2`
passed the full regression suite (exit 0) and repeated the mode-3 numeric
readback after removing temporary general shader-selection hooks. The exact
final result is `analysis/native-shadow-pixel-readback-final-live-result.json`:
13 collected meshes, 1024 square map, all 1,048,576 texels exactly 1.0, no
nonfinite values or depth variation. No crash log or normal user-file changes.
This does not establish working shadows. Subsequent indexed-geometry
auditing found the forced-light fixture has no sampled in-frustum triangles;
the all-clear result alone therefore does not establish a rendering failure. Matching symbols and the protected private capture are retained.


### Draw/resolve isolation and corrected fixture diagnosis

The earlier cleanup build
`D742A2354626343043A519BC0214C6364C327D1B155A310AA1FF3D9C66855BBC`
passed the full regression suite (exit 0). The experimental package plan remains
29 files / 9,710 prepared shader records, excluding personal saves, full game
archives and a separate launcher.

Earlier private comparisons restored original render bindings, changed RTV/ROV
and selected 30/60 FPS. Their all-clear map results are retained in
`analysis/native-shadow-render-path-isolation-result.json`, but they do not
exclude renderer faults: the forced authored spotlight is an invalid positive
control for producing shadow pixels.

Capture `private-gpu-profile-20261110-063850-3a0c79b1` records actual position
fetch 95, packed transform constants, index buffers and native clip controls.
All 16,725 triangles in the initial 13 collected draws are trivially outside
that light camera's raw clip volume. Later buffer allocations give 25 first-
per-position-buffer samples / 32,906 triangle references, all rejected, with no
unknown indices. These are CPU FP32 reconstructions before backend system
conversion, not hardware pipeline statistics, exhaustive frame coverage or a
visual acceptance result. See
`analysis/native-shadow-invalid-caster-fixture-result.json` for every sample
and the limitations. Whole-buffer bounds crossing a clip plane do not establish
that an individual triangle can rasterize.

The trace confirms depth-only native draws (pitch 1040 for a 1024 viewport,
GreaterEqual comparison, depth clear zero) and a separate color resolve to the
inspected R32_FLOAT texture. The texture's role as the depth resource actually
sampled by shadow lighting has not been established. It would be premature to
call the color resolve or absence of a pixel shader a renderer bug.

An experimental color-output override and depth-comparison bypass both left the
map clear and have been removed completely, including their launch switches.
Disassembly of the authored `Gen3_SpotLight_ShadowWrite.fxlo#PS0` variant shows a
constant silhouette output, not a depth writer; enabling it cannot be described
as a verified depth-map fix. Prepared bytecode can have multiple effect aliases,
so compiled shader identity alone is insufficient to identify a pass.

Private `-TraceShadowGpuState` logging and numeric `-ShadowPixels` readback remain
opt-in, isolated diagnostics; they do not alter draw state. No gameplay/camera
input or screenshots were used, and protected closures verified unchanged
normal user data. The following Section A captures establish populated map contents through
resizing. Tracing the resource lighting actually samples and validating visible
receiver shading remain necessary before exposing a finished player option.

### Populated map resize cycle

The Continue-path redirect now permits an isolated retail-level test with normal
player initialization: `-StartLevel tcy_autotopia_visit1.sectiona_first`. It
uses the native loader after the title screen, sends no input and cannot be
combined with the direct, pre-initialization `-BootLevel` path. Private launches
now preserve a symbol-map copy and hash automatically.

Unlike the earlier Section C fixture, Section A produces written pixels with
the private authored-mode-3/opaque-caster probes. Capture
`private-gpu-profile-20261110-070547-bda9819b` reads 739,132 zeros and 309,444
ones from the original 1024-square R32 texture. Capture
`private-gpu-profile-20261110-070802-718a9386`, with the distance policy enabled,
reads 51,122 zeros and 14,414 ones at 256 square. These are binary coverage
values, not proof of a continuous depth map or correct receiver shading.

Build `66097506A13CAC5D224B0F216DA9609EBE8B2AC46B37D3E23900D91B6222DDD5`
then completes a populated-map replacement cycle in
`private-gpu-profile-20261110-071114-4b099786`:

| Native side | Zero-valued texels | One-valued texels | Nonfinite |
| --- | ---: | ---: | ---: |
| 256 | 51,122 | 14,414 | 0 |
| 1024 | 819,749 | 228,827 | 0 |
| 256 | 51,122 | 14,414 | 0 |
| 1024 | 819,749 | 228,827 | 0 |

Both repeated sizes have identical aggregate counts. The actual headers/clicks
are replaced, the original distance-policy setting is restored, and protected
closure verifies unchanged normal user data. This proves populated texture
contents survive the tested resize/rebind/retirement cycle. It does not prove
pixel-by-pixel image equivalence, all caster coverage or an FPS improvement.
Authoritative numeric evidence and limitations are in
`analysis/native-shadow-populated-resize-cycle-result.json`.

For this diagnostic, `-ShadowPixels -BenchmarkDistanceShadows` now enables
bounded transition readback: one native slot, an exact stable header/fetch/click
identity, two seconds of settling, and at most twelve attempts. Native resource
checkpoints, complete GPU resolve coverage, header revalidation, fenced readback
and unknown-completion resource retention remain required. A replacement gets
a new capture; an unchanged descriptor is not repeatedly copied. It produces
no screenshots. Readback stalls make this a lifecycle test, not an FPS benchmark.

The opt-in GPU trace additionally records analyzed shader texture bindings,
including format aliases whose base matches the last validated probe address.
The bounded captures have not established a direct receiver binding for this
target. Absence from that trace does not prove the texture is unused: it is not
an exhaustive resource-consumption graph or proof of executed pixel fetches.
Visible receiver quality, actual depth-resource semantics, alpha-correct
casters, normal light selection, EM1 and co-op acceptance remain open.

### Actual depth texture and receiver identified

The second target object is now positively identified as `NiDepthTexture`.
Its table `821DE978` points to RTTI getter `830AFB10`, which returns
`833FA49C`; that RTTI object's name pointer resolves to `NiDepthTexture`.
`analysis/shadow-native-auxiliary-texture-storage-corrected.json` records the
actual methods, RTTI object and allocation chain. The similarly named earlier
uncorrected capture used a mistyped RTTI address and is superseded.

The render target owns separate objects:

| Target field | Native object | Storage |
| --- | --- | --- |
| +16 | Rendered color texture (`82043108`) | Native data `82055A1C`, header +24, R32_FLOAT |
| +20 | NiDepthTexture (`821DE978`) | Native data `82055A3C`, header +28, D24FS8 |
| +24 | Shadow render click | Owns the native render invocation |

For the full-size mode-3 fixture, the color attachment occupies physical
`0E471000`, while the depth texture occupies `0E871000`. The committed depth
resolve explicitly reports format 23, source select 4 and `is_depth=true`.
The native pixel shader references fetch 3 at that exact depth address. Its
microcode hash `862341C148EA0A55` matches the project's `adaptive16` PCF variant
byte-for-byte (2,619 words, SHA-256
`2f5ec308999f7b50dc7b059c6c706800314f4cef5d0bad11bb2c3bb0ddaff98e`).
This identifies the actual bound receiver program rather than guessing from a
compiled DXBC alias. See
`analysis/native-shadow-actual-depth-receiver-identities.json`.

`-ShadowDepthPixels` now selects the depth object for the same fenced readback
path and implies `-ShadowPixels`. The exact native header must describe tiled
D24FS8 with 8-in-32 endian conversion. Readback byte-swaps each packed word,
separates stencil and converts its high 24 bits with the installed backend's
`Float20e4To32`. Reported values are guest depths, not the host DSV's half-range
representation. The existing ledger proves complete current 4-byte-texel
coverage; descriptor/type validation and the typed resolve establish the depth
interpretation. No stale CPU upload or substitute texture is used.

Capture `private-gpu-profile-20261110-073039-40b106ef` verifies the actual depth
texture through replacements:

| Side | Clear (zero) depths | Maximum guest depth | Nonfinite |
| --- | ---: | ---: | ---: |
| 256 | 14,414 | 0.811903477 | 0 |
| 1024 | 228,827 | 0.811900616 | 0 |
| 256 | 14,414 | 0.811903477 | 0 |
| 1024 | 228,827 | 0.811900616 | 0 |

Repeated-size aggregate results match. Direct adaptive-PCF depth bindings are
observed for both resolutions and replacement addresses. The policy restores
its original value, protected closure completes, and normal user files remain
unchanged. The exact result is
`analysis/native-shadow-real-depth-resize-cycle-result.json`.

Final build `B58044A0B71C97D648C69B2DED630B856963434DF593076D3D1ABFF74E78F409`
repeats a full-size depth capture: 309,444 clear texels, maximum guest depth
0.855491638, no nonfinite values and the matching typed resolve/PCF binding.
It passes the full regression suite and package dry run.
`analysis/native-shadow-real-depth-final-live-result.json` records that build.

These tests establish real depth generation, bindings and replacement survival.
Visible receiving pixels, normal light selection and broad gameplay acceptance
remain unproven. In particular, the private Continue redirect retains a Section C
checkpoint's player spawn while loading Section A. The read-only save inspection
finds both preservation flags set and spawn positions from the old section;
live camera snapshots reach negative heights. This fixture is useful for
render-resource tests, not evidence of correct destination spawning or controls.
The captures also inherit experimental Native delta timing (`gameplay_timing=1`).
Negative camera heights therefore do not isolate a spawn defect: a 30 Hz control
and normal `CleanLoadLevel` entry are needed to separate timing from destination
spawn handling before claiming gameplay or visual acceptance.

### Native entry corrects the void fixture

The old low-level Continue redirect was independently reproduced at original
30 Hz timing. It still placed the actors using the Section C checkpoint while
loading Section A. It has been removed. `-StartLevel` now restores the checkpoint
world normally, waits for ready native actors, then queues the same validated
`level.load` / `CleanLoadLevel` command as the game menu. It preserves normal
level-transition and destination-spawn initialization instead of replacing only
the loader argument and startup string.

Build `10ED66F10A094392C01F433ADABB5AA698B92EF50AC7C75FD1894BEFB45D9C86`
enters Section A through that path. Camera snapshots remain at normal positive
heights rather than the old fall/respawn positions, native light selection
activates without a forced light, and both native motion objects report grounded
state with movement enabled. Full regressions and the package dry run pass.
`analysis/native-start-level-entry-live-result.json` records this 30 Hz control.
No input was sent and normal saves/settings were unchanged; the corrected private
test was left open for the user. This fixes the test-entry defect, not all
experimental high-FPS physics or imported EM1 level behavior.

### Native menu presets and persistence

The authored Scaleform settings pages now include **Distance shadow resolution**
in Settings & Mods > Rendering & Lighting. Fixed is the default. The setting
uses native option ID 1028 and persists as `distance_shadow_resolution`; existing
28-option files migrate with Fixed unless the player chooses otherwise.

| Preset | Half-size beyond | Quarter-size beyond | Restore full below | Restore half below |
| --- | ---: | ---: | ---: | ---: |
| Quality | 50 | 100 | 40 | 85 |
| Balanced | 25 | 60 | 20 | 50 |
| Performance | 15 | 35 | 12 | 28 |

Distances are native world units from the nearest active camera to the local
light. The existing 128-pixel floor and selected maximum apply; player and
directional maps retain configured quality. Preset changes restore detail
immediately where needed; further reductions wait one second to avoid repeated
allocations during rapid setting changes. Fixed restores configured dimensions
through the same native resize path. No restart or console command is required.

Native callback/persistence tests exercise all four choices, startup reload,
invalid-choice rejection and stale/current rollback. Policy tests cover ordering
across 601 distances, every preset's hysteresis/cooldown and actual native target
binding when quality changes. The compiled game GFX was exported again with
JPEXS to verify option 1028 and all four choices; this is not merely an unused
source menu row. The title and pause menu archives and their changed-only
distribution patches were rebuilt together with the executable.

The corrected native level entry, with Balanced selected and without forced
lights or opaque-caster probes, reports player/color-depth size 512, distant
local-light/color-depth size 256 and nearby local-light/color-depth size 1024.
The near map contains real finite D24FS8 depths (maximum 0.312009811) and is
referenced by the adaptive PCF shader. This verifies native allocation and depth
binding for the normal-selection case. It is not an FPS measurement or visual
acceptance of every scene, imported level, transparency mode or co-op view.
Evidence: `analysis/distance-shadow-balanced-live-resources.json` and
`analysis/distance-shadow-balanced-live-lights.json`.
