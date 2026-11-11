# EM1 compatibility engineering status

A bounded native courtyard preview runs with EM2 Mickey, original EM1 architecture,
static collision, materials and two native gear entities. **Full EM1 level gameplay
and campaign progression remain incomplete**; original damage/tutorial callbacks
are not dispatched yet. The deliverable is native, on-demand support for original
EM1 and EM2 assets in the same executable. Original ROMs remain read-only and
normal EM2 saves/settings/content remain separate.

## Current courtyard preview

In this development checkout, launch the canonical executable through:

```powershell
./scripts/start-private-gpu-profile.ps1 -Scale 1 -OriginalLighting -NoGpuProfile -FrameCap 60 -Em1SourceRvz 'D:\Roms\Wii\Disney Epic Mickey (USA) (En,Fr,Es).rvz' -Em1EnterCourtyard
```

Courtyard entry now selects the corrected EM1 material archive and enables vertex
alpha, dual UVs and cutouts automatically. This includes the grass fix: UV fetches
must precede the color fetch that overwrites their vertex-index register. A missing
archive fails before profile creation instead of falling back to incorrect materials.
`-Em1LegacyMaterials` explicitly selects the old fallback for comparison. Explicit
partial candidate switches and UV probes remain developer diagnostics.

The normal build now runs `scripts/prepare-em1-materials.ps1`: the canonical
executable validates 54 GPU draws and emits the pixel programs, then the native
FXLO adapter adds four EM1 effects to the staged shader archive. Original EM2
effect entries remain byte-identical. The build also checks vertex-fetch scheduling
before installing the archive. Release packaging already includes this staged
archive, so end users do not need Python or an analysis-folder shader package.
Default courtyard entry reads that installed archive. Explicit partial material
and UV diagnostics still use their separate development archives.

The runtime reads EM1 scene/model/texture data directly from RVZ. The private
profile copies saves/settings and does not modify the ROM or normal campaign.
An in-game EM1 campaign selector and complete progression are still unfinished.
No separate game executable is used.

## Native source path

`runtime/em1_compat` builds static target `em1_compat`; its public include directory
is `runtime`. The main runtime links that target. No extra runtime executable,
extractor, Python, Java, or shared codec DLL is required by these adapters.

- `em1_rvz`: bounded read-only RVZ/Wii source, SHA-1 verification, FST lookup,
  direct range reads, source identity and an eight-group per-source LRU.
- `em1_pack_source`: reads a PAK directory on demand, then only the selected
  resource's ranges. Keeps original source/archive identity in its origin key.
- `em1_inflate`: native Zlib streams with verified checksum, exact output size,
  no trailing data, and a bounded allocator. Original bytes are never replaced.
- `em1_level_source`: `ReadLevelPlan(source, "DBC_Courtyard_1st", ...)` reads the
  original `.level` and six `.part` descriptors directly from RVZ. It preserves
  authored fields, shared scenes, interior/destination links, audio and flags.
  `ReadAdaptedScene` then reads the original scene PAK and applies the native
  layout adapter in memory. It does not yet create the EM2 registry or entities.
- `em1_scene_adapter`: validated structural EM1-to-EM2 scene adaptation,
  retaining original UUID separately and preserving semantic fields.
- `em1_nif_textures`: native opaque NIF-container and Wii texture adapter.
  Preserves unrelated block payloads; implements tiled I4/I8/IA4/IA8/RGB565/
  RGB5A3/RGBA8/C8/CMPR with exact Wii interpolation and transparent-pixel RGB.
  Returns a count of still-unconverted Wii display-list blocks.

Developer validation compares **1,337 original resources** in courtyard, level
registry and courtyard-spawn archives against independent Python SHA-256 oracles.
All **102 scene adapters** in those archives match the independent output hashes.
Native courtyard and player-spawn requests resolve through original RVZ data.
Malformed checksum/truncation/trailing-data/length requests and unsafe/missing
paths reject. Resource outputs remain unchanged when their request fails.
Evidence is private under `analysis/em1-compatibility/resource-goldens`.

Native NIF validation matches all **354** courtyard NIF/KF containers and **214**
texture conversions against the independent oracle. All 354 truncated-footer
cases reject without replacing the previous output. The resulting corpus still
contains **506** Wii display-list stream blocks: this is not a mesh converter.
The native level-descriptor audit accepts **165/176** authored levels, including
legacy bare GSA lists, `//Soundbanks list`, inline section annotations and shared-
only scenes. Eleven reject because their named entry partition is absent from
retail levels.pak; no substitute partitions or geometry are fabricated.

Decoder-only official Zstd 1.5.8 and Zlib 1.3.2 sources retain original licenses,
notices and file hashes in their `PROVENANCE.json`. Zlib's source archive hash was
verified against the [upstream release page](https://zlib.net/). No encoder, CLI,
gzip/file-writing interface, emulator tree or installer is included.

## Verified engine integration boundaries

These addresses are specific to the current EM2 Xbox 360 executable; they are
static observations, not installed hooks or proof of native EM1 gameplay.

| Address | Observed role and constraint |
| --- | --- |
| `0x8274DDD0` | Pack manager request/queue; origin must follow asynchronous requests, not only thread-local state. |
| `0x82749E50` | Resource lookup and decompression. Metadata `+16 & 2` selects compression; `+4 >> 5` is stored length, `+8` decoded length, `+24` source data. Callers distinguish borrowed versus allocated buffers using metadata. |
| `0x82797DB8` | Always requests `levels/master.lvlc`, then parses at `0x82797910`. There is no authored-text fallback in this initializer. EM1 instead has 571 text descriptors/resources in levels.pak. |
| `0x82797800` | Retained SingleGsa path constructs a 112-byte zone (`0x82797010`) and 36-byte state (`0x827960B0`), stores manager `+152/+148`, then registers via `0x82794CC8`. Useful first-scene path; not campaign streaming support. |
| `0x82936B90` | Main SceneLoader stage reads a binary resource and starts deferred scene parsing. |
| `0x827A9458` | Scene decoder receives buffer in r4, length in r5 and immediate/deferred flag in r7. Stores data at manager `+112`, cursor `+116`, strings `+120`, active byte `+124`. |
| `0x827A8F48` | Continues deferred parsing; SceneLoader uses a 200-item budget. Adapted bytes must survive completion/cancel, not merely the initial decoder call. |

`0x82936250` loads palette scenes through the same decoder with immediate parsing.
`0x828624B0` is a UI-manager resource callback registered by `0x82862688`; do not
mistake it for the general scene/NIF loader just because it returns resource bytes.

Next integration requires a separate read-only `em1:` VFS/source namespace and
origin propagation through pack, resource and scene tasks. Replacing a buffer
pointer without correcting its ownership/size is unsafe. Existing EM2 lookups
and caches must not accidentally resolve an EM1 resource with the same name.
Do not activate a scene before mesh/physics/script dependency adapters exist.

## Independent research oracles

- `scripts/em1_rvz.py`: read-only, bounded RVZ/Wii partition and FST reader,
  using Python 3.14's built-in Zstd. Validates header and partition-table SHA-1,
  declared lengths, group spans, Zstd output, packed runs, and file paths.
  Supports the user's Zstd 128 KiB RVZ, including RVZ padding reconstruction.
  It reads selected files directly without dumping the entire disc.
- `scripts/em1_scene_upgrade.py`: EM1 scene to retail EM2 structural conversion.
  Preserves IDs, property payloads, strings, transforms and references; retains
  the removed scene UUID in the manifest. Unknown properties fail explicitly.
- `scripts/em1_prepare_import.py`: reproducible isolated RVZ-to-scene staging,
  refusing an existing destination and recording source/output SHA-256 hashes.
  Its manifest explicitly sets `engine_ready=false` and lists remaining work.
- `scripts/em1_scene_validate.rs`: independent development validation using the
  existing epic-mickey-lib-rs parser. All 69 first-castle scenes parse as EM2 and
  compare equal to original semantic data after excluding version/scene UUID.

The requested disc is SEME4Q. Its game partition contains 3374 FST files.
`packfiles/dbc_1st_zonec.pak` read directly from the RVZ is 12,268,960 bytes and
byte-identical to the existing local EM1 extraction. SHA-256:

`0203f75600d18815370e28251f93db5b157d3e97101d0c537ad558ed0251aeb5`

The first-castle archive contains 69 scenes, 12 Lua files, 338 Wii NIFs, 184 Wii
Havok assets, 6 HKW files, 16 KF animations, 16 KFM files and 37 BSQ files.
A second local projector archive adds 6 scenes which also pass the Python
semantic check; only the 69 first-castle scenes have independent Rust validation.

## Texture conversion and Lua recovery

`scripts/em1_nif.py` roundtrips all 354 first-castle NIF/KF containers byte for
byte while retaining opaque blocks. `scripts/em1_texture_upgrade.py` converts
all 214 cooked Wii texture blocks into portable RGBA8 NiPixelData, preserves
mipmaps/palettes/sRGB and updates only the affected texture links' persistence
flags. Five golden-pixel tests cover channels, tile/subblock order, truncation,
and Wii-specific CMPR interpolation and colored transparent pixels. Native EM2
loading remains unverified. Baseline RGBA deliberately increases storage; later
compression must preserve appearance and should follow native validation.
Run `python scripts/em1_texture_upgrade_test.py` for these tests. Add `--textures`
to the staging command below to include candidates in a separate directory.

Existing local unluac was compiled as a developer tool and recovered source for
all 12 first-castle scripts without errors. This proves source recovery, not
behavioral equivalence. Output is ignored under `analysis/em1-compatibility/lua-recovery`.
EM2's existing game console already invokes the actual Kore source compiler
(`sub_8271A360`, compiler settings at global_State+680). The next safe test is
compile-only loading with full stack/context restoration; do not execute
recovered top-level scripts until globals/APIs and scene dependencies are ready.
The 21 KiB projector script exceeds the current console's 16 KiB slab, so an
import-specific heap-backed bounded source buffer is required.

A lexical survey finds 160 call names: 101 candidate native matches and 31
candidate EM2-script helper matches. Unmatched names include ordinary builtins
and local functions as well as actual scene/scoring API gaps. No compatibility
stubs were substituted for missing gameplay behavior.

Texture format reference: [Dolphin's primary decoder](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/TextureDecoder_Generic.cpp).

## Compatibility gaps established by source evidence

| Area | Evidence | Required implementation |
| --- | --- | --- |
| Scene container | EM1 GUID/header/entity layout differs from EM2 retail | Implemented structural adapter; engine loading still untested |
| NIF | Both use Gamebryo 20.6.5.0 and big-endian payloads; EM1 user versions9â€“15 versus sampled EM2 versions15â€“17 | Decode Wii mesh/texture renderer payloads and emit supported native assets, preserve paint/thinner nodes, materials and animation links |
| Havok | EM1 Havok 7.0.0 packfile v7/layout 04-00-01-01; EM2 Havok 2011.2 packfile v9/layout 04-00-00-01 | Actual schema/layout conversion or reconstruction in EM2; cannot fix with a version-string edit |
| Havok runtime | EM2 image contains `Packfile versioning support is not linked` diagnostic and no Havok 7 registry version string | Do not assume automatic legacy upgrade; validate any supported patch path before use |
| Components | 66 component names in sampled EM1 scenes; 62 names present in EM2 image; AmmoRechargeComponent, InventoryComponent, SeedPointComponent and TrackingComponent absent | Inventory factory registrations/properties and implement behavior adapters; matching strings alone prove neither registration nor semantic compatibility |
| Lua | EM1 standard Lua 5.1 header, little-endian bytecode and 8-byte numbers; EM2 sampled Kore Lua header has format 0x0c, big-endian and 4-byte numbers | Decode/decompile and recompile through supported EM2 compiler, then adapt changed engine APIs/state machines; copying bytecode is invalid |
| Campaign | Scene loading also depends on global scripts, level registry, BSQ, quests, checkpoints, inventory and streamed dependencies | Isolated EM1 campaign namespace/save data and explicit dependency closure |

A bounded class-name survey finds 49 first-castle Havok assets with physics data,
including convex hulls, boxes, MOPP/static meshes and compound lists; many of the
remaining 184 HKX assets are spline animations or behavior graphs. This is not a
validated collision decoder.

**Local library trap:** epic-mickey-lib-rs advertises an HKX compiler, but its
`cpp/havok_ffi.cpp` XML compilation function merely returns XML text bytes, and
its C++ collision extractor returns an empty list. Neither implements Havok
conversion. Its separate Rust collision reader exists, but its shape layouts
and triangulation require independent validation before gameplay use.

## Reproduce the completed milestone

Run from the recomp repository with Python 3.14, choosing a new output folder:

```powershell
python scripts/em1_prepare_import.py "D:/Roms/Wii/Disney Epic Mickey (USA) (En,Fr,Es).rvz" analysis/em1-compatibility/new-import
```

The output includes converted scenes under `scenes/` plus
`em1_import_manifest.json`. It is not installed in the game's content directory.
No distributed game assets are added to source control. These are developer
tools; the native source adapters above read original RVZ data directly. End-user
menu mounting and actual engine deserializer integration remain work.

Independent validator build (uses the already built local Rust library):

```powershell
rustc --edition=2021 scripts/em1_scene_validate.rs --extern epic_mickey_lib_rs=D:/Repos/epic-mickey-lib-rs/target/debug/libepic_mickey_lib_rs.rlib -L dependency=D:/Repos/epic-mickey-lib-rs/target/debug/deps -o analysis/em1-compatibility/em1_scene_validate.exe
```

Run it with an original scene binary and the corresponding converted scene.
The executable is a development validator only, not another game executable.
Evidence is under ignored `analysis/em1-compatibility/`: RVZ source header,
first-castle source proof, independent scene results, component candidates,
asset headers, NIF type inventory and staged first-castle manifest.

## Next acceptance milestones

1. Convert a static first-castle mesh and its textures, preserving coordinate
   scale, materials and paint/thinner grouping; validate against both source
   structure and actual EM2 rendering.
2. Convert/rebuild that area's real collision and load it with EM2's physics
   API. Verify triangle indices, transforms, material/filter data and contact.
3. Add an isolated EM1 level registration, spawn the EM2 Mickey controller in
   the converted world, and verify traversal and level streaming. This is the
   first playable milestone, not full campaign support.
4. Adapt native component differences and source scripts, then verify paint,
   thinner, interactions, enemies, cutscenes, quests, saving and transitions.
5. Integrate safe native RVZ import into the existing executable and campaign
   selection UI once actual playable compatibility is established.

RVZ format reference: Dolphin's primary [WIA/RVZ format specification](https://github.com/dolphin-emu/dolphin/blob/master/docs/WiaAndRvz.md).


## Read-only runtime source mount and native parser fixture

The game now accepts an optional `em1_source_rvz` configuration value (empty by
 default). After the existing runtime/filesystem setup, `main.cpp` opens that
 original RVZ with the bounded native reader and mounts its original files at
 `em1:` / `\Device\Em1Disc`. The `game:` device is unchanged. An invalid optional
 image logs an error without activating EM1 scenes. No extraction, converter
 executable, or new runtime dependency is required. Root has built this path;
 a configured game launch still needs verification. The standalone SDK VFS
 fixture passed full/partial/EOF reads, denied writes, duplicate mount handling,
 guarded destination handling, and source timestamp preservation.

`runtime/em1_parser_probe.{h,cpp}` is an **unlinked developer fixture**, not an
 automatic hook or level launcher. It accepts a private zeroed guest allocation
 from a caller on an initialized game/loading thread. It adapts a synthetic empty
 EM1 scene, constructs a separate native parser, checks deferred input retention,
 finishes one instance, and cancels a second by destruction. Both instances own
 their own input storage throughout destruction. The caller retains the outer
 allocation; the native destructor frees only its members. It never points at
 the active parser/world, resets another parser's busy flag, or registers an
 imported level. The complete PPC context and host MXCSR are restored. A 32 KiB
 writable guest-stack check precedes the calls, with 2 KiB reserved below the
 interrupted frame so the native finalizer's 12,208-byte frame cannot overwrite
 the caller's ABI home area. Unexpected guest exceptions propagate rather than
 pretending that a partially constructed object can be safely destroyed.

Audited native paths:

- Constructor `0x827A8C40` initializes object-local containers and its fixed-string
  member. Its allocations are normal game heap allocations; it does not install
  the object into the active-world/registry globals. `0x827A7E20` reserves a local
  free-list pool of 3,000 entries. Destruction clears/frees these containers.
- Begin `0x827A9458` stores input at `self+112`, cursor at `+116`, string base at
  `+120`, and active state at `+124`. `r7=0` retains the input for later pumping.
  The dependency vector and source-name reference passed by this fixture are
  independently allocated zero values, matching the image's null fixed string.
- Pump `0x827A8F48` takes a **microsecond** budget in `r5`, not an entity count.
  Zero means unlimited; the fixture uses that only with zero entities/references.
  Its live-world calls/global entity counter updates are reached through nonempty
  object-local work lists; all those lists are empty in this fixture. Finalizer
  `0x827A84A8` walks only the isolated object's empty entity container.
- Destructor `0x827A8908` clears this object's active flag, releases its own entity
  references/containers/name, and calls base destructor `0x827A9980`. It does not
  free the outer object allocation, allowing caller-owned test scratch. Existing
  allocator bookkeeping is the expected shared side effect; no world pointers
  or active parser registration are replaced.

This fixture compiles cleanly against the actual SDK/generated ABI headers, but
has not been linked or run. Its eventual integration belongs
 in an explicit developer regression path after engine initialization, not in
 default startup or online lockstep. Passing it would prove the bounded empty
 parser ABI/lifetime path, **not** EM1 gameplay, mesh compatibility, or campaign
 streaming. Real imported scenes still require origin-aware pack ownership and
 the mesh/Havok/Lua/component work described above.


### Native Wii topology decoding

`em1_gx_mesh` decodes bounded static GX command buffers directly in C++. It
 independently validates 8-bit and 16-bit attribute-index interpretations and
 accepts exactly one. Complete position/normal/color/UV tuples determine welded
 vertices; output retains each original attribute index for byte-exact copying
 and later baked-light/morph mapping. Triangles, alternating-winding strips,
 quads, and fans become ordinary triangle indices. Skin/matrix interpretation
 is explicitly the NIF caller's responsibility and is not guessed.

The developer oracle includes the authorized BlueThinner `wii_mesh.rs` directly
 without modifying it. The native decoder matches **505 courtyard meshes,
 63,224 vertices and 51,727 triangles**, including **12 meshes with secondary UV
 channels**. Comparison covers triangle indices, original position/color
 indices, normals, colors, and every UV channel. Both implementations reject
 `DBC_toonrampF.nif_wii#166` because both index widths appear valid. Synthetic
 checks cover color seams, winding, both widths, malformed streams, and leaving
 prior output unchanged on failure.

This decoder is not yet linked into the game. NIF stream rewriting, modifier
 remapping, and actual native rendering remain separate required steps. The
 successful comparison does not make Wii models playable in the EM2 engine.


### Static Wii NIF vertex/index adaptation

The unlinked native `em1_nif_meshes` adapter now replaces supported Wii display
 lists with regular Gamebryo triangle-index and vertex streams. It preserves
 block IDs, opaque materials/transforms and unknown prefix words. Each vertex
 attribute keeps its original binary component format, bytes, clone/access flags
 and streamability. Separate output streams avoid merging attributes that have
 different clone/mutation semantics. Original position/color/all-UV index maps
 remain attached to the result for later baked-light and paint adaptation.
 Unsupported skin, morph, instancing, shared display lists and ambiguous index
 layouts are rejected rather than approximated. Original bytes are immutable.

The developer validator reads the actual original RVZ through the native PAK
 source. **353 of 354 courtyard NIF/KF containers passed**, with **460 converted
 meshes**. An independent Rust/reference reparse of the resulting NIFs confirms
 **62,348 vertices and 51,132 triangles**, including **12 meshes with secondary
 UVs**, with identical positions, normals, colors, UVs and topology. Existing
 opaque block payloads, IDs and strings are checked independently by the native
 fixture; malformed/truncated inputs leave the previous result unchanged.

The rejected `DBC_toonrampF.nif_wii` contains the previously identified ambiguous
 draw at block166. Its other45 otherwise decodable meshes are intentionally not
 emitted in a partial file. This is one rejected asset, not45 new decoder
 failures. Native Gamebryo loading/rendering, baked-light/paint updates, embedded
 skins, Havok and Lua remain unverified/incomplete. This result does not establish
 a playable EM1 level.

`em1_nif_container.h` is an isolated opaque block/table writer shared by the new
 mesh adapter. The existing texture adapter still uses its earlier equivalent
 local reader pending a coordinated refactor and rerun of the354 texture golden
 comparisons. No canonical-game CMake change or game launch was made by this
 validator work.

## Original gear damage bridge audit

`audit-em1-native-damage-routing.py` validates 21 interpreted retail instructions
against the extracted Xbox image and exports the relevant disassembly. Health
constructs event15 at `0x827D1E50` and delivers it through immediate dispatcher
`0x827340E0`; the trigger queue at `0x827347D0` is not this damage path. The event
contains the damaged entity handle at offset8 and another handle at offset12.

InputOutput listener `0x82755D50` filters the authored connection by the event's
first handle before calling connection executor `0x82755710`. The executor uses
input name at connection+20, parameters+24 and delay+32. The caller removes a
connection with flags bit0 after execution returns, including when native Lua
lookup did not find the callback. Both original DoorGear links use that one-shot
flag. Each gear also has separate impact and machine-switch audio connections.

A bridge should intercept only an eligible original EM1 connection after native
filtering, preserve authored delay/one-shot semantics, and leave native audio
connections intact. Do not also dispatch at the global event bus, retain transient
native pointers across Lua yields, or partially execute DoorGear before its engine
dependencies exist. This is a static ABI audit; no damage bridge or live player-hit
validation is claimed by it.

## Native cutscene input binding

The original Lua VM now exposes the no-argument `RestrictCutSceneInput()` and
`UnrestrictCutSceneInput()` forms through scoped engine callbacks. They use the
retail restriction helpers, with per-controller ownership counts retained by the
scene's script state. Extra arguments are currently rejected. Access is limited
to the active, ready EM1 script owner during simulation dispatch.

Nested calls balance only EM1-owned counts. Script failure cancels pending scene
callbacks and attempts to release acquired restrictions; level transition and
native-state release also perform cleanup. A native counter reset to zero revokes
old ownership without decrementing below zero. Controller identity changes or
ambiguous count drift are reported instead of touching an unrelated controller.

The private rotation/unload probe verifies nested 1/2/1/0 counts, explicit cleanup,
a new outstanding restriction, and native reset during real level unload. All27
Rust tests pass, including scope/argument/failure/yield cases. This does not yet
enable the complete DoorGear tutorial callback.

## Authored groups and tutorial actors

The native scene adapter retains 238 authored memberships across 139 groups,
resolved through complete prefab inheritance and restricted to placed entities.
Independent Python decoding and live native loading agree on fingerprint
`C6A096FD0D2AED17`; the 117 existing event connections retain their earlier hash.
Membership records contain original scene IDs, not yet live engine handles.

The four gear-tutorial steam groups contain eight actors. Names alone are not a
valid resolver: Steam_Jet_01 actors01/02 belong to group02, actors03/04 to group01;
four Steam_Blast actors likewise belong to two Steam_BlastGear groups. These use
original Common_Steam_02 and Steam_Jet_01 NIFs. The former includes custom
NonUniformParticleSystem/Simulator/QuadGenerator classes. Resource blocks are
inventoried under tutorial-actor-resources. Native loading/publication now passes
for both assets (see below); actor admission and group iteration remain pending. The entrance
door and portcullis additionally need AnimationComponentGB/KFM/KF and their
original dynamic collision bodies, not a static-model substitution.


## Native steam resource compatibility

The opt-in `em1_validate_particle_assets` probe loads the original Common_Steam_02
and Steam_Jet_01 closures through the native NiStream and typed resource manager,
then releases them and verifies the manager record count returns to baseline.
The private launcher exposes `-Em1ValidateParticleAssets`, requiring EM1 courtyard
entry. It does not attach actors or start emitters.

Two real source issues were fixed: LightRay_01B is a user-version9 texture container
with the validated NiSourceTexture layout, and the jet references SmokePuffs_08B
and Indirect_Smoke from original `packfiles/_Dynamic.pak`. Model closure now accepts
9/14/15 containers while retaining structural checks and importing only regenerated
NiPixelData. Missing local textures fall back only to that original shared archive;
local decode/I/O failures do not trigger fallback. Cache identity and provenance
use the actual source archive. Errors now include the texture path.

Live probe `100842-cdcf5e3d` passes both original assets: blast has two textures and
53,438 converted bytes; jet has four textures and101,713 bytes. This proves native
factory loading and resource ownership, including the blast's custom NonUniform
classes. It does not prove simulation, material rendering, actor admission or
visible particle emission. Full DoorGear execution remains disabled.


The particle probe now additionally projects original placed actor98677 (blast)
and98431 (jet), resolving their full prefab inheritance and retaining the four
original components, source identity, flags and property records. Only the NIF
path is rebound to the owned native resource. Native construction has a separate
Particle prototype path rather than applying trigger-specific class assumptions.

Because scene-begin is inside loading, particle construction is deferred to the
normal ready gameplay tick. Private run101250-7957eb6b constructs both actors with
transform/scenegraph/I/O/particle-controller component vtables820904D0/82084A68/
82075F98/8209BB90, then destroys them and releases resources to the original manager
count. Courtyard gear admission and real level unload also pass. This remains a
construction probe: actors are not admitted, emitters are not started, and visible
particle rendering remains unverified. Evidence: native-steam-construction-result.json.


## Original steam actor world admission

The opt-in `em1_verify_particle_activation` / private launcher
`-Em1VerifyParticleActivation` admits all eight original tutorial steam actors,
selected from the four exact authored groups. It retains two shared original model
resources and eight separately owned native entities. Each actor passes native
component-class validation and uses the normal fresh GUID/group insertion/full
startup path; a valid native scene root is required. Failed admission requests
normal deferred entity removal before releasing resources. Successful actors stay
owned until native level unload; resources are released only after every entity.

Private run101505-7b1a82cb passed all eight admissions with distinct handles118�125
and separate roots, followed by native particle and gear owner release during real
level unload. This is still opt-in: visible emission, material correctness and
original group/emitter Lua dispatch have not been verified. Evidence:
`native-steam-activation-result.json`. Retail StartEmitters/StopEmitters wrapper
instructions were also saved in `native-emitter-wrapper-disassembly.json` for the
next bridge audit; wrapper addresses are not callable as entity-pointer functions.


The eight admitted particles now retain authored IDs and register opaque original
Lua entity identities. Both direction lookups participate in the same active-world
checks as gears, reject pending destruction, and are revoked before level unload
or failed-admission cleanup. This enables future group dispatch without guessing
handles from actor names. Original Lua group/start-stop bindings remain pending.

`SetNativeParticleEmission` uses the retail component lookup82761C50, checks the
native controller class, calls controller828013F0, then applies the retail reflected
enabled-property setter. It reads that property back to verify the requested state.
The opt-in activation probe now stops all eight actors, starts them after30 ticks,
and stops them after another30 ticks. Gear unload observation is extended to allow
these transitions before teardown. Private101845-d4397b10 passed all24 property
checks (Off/On/Off for each handle), all8 group identity mappings, identity revocation,
and particle/gear ownership cleanup. Normal files and RVZ were unchanged.

Read-only `scripts/audit-em1-native-emitter-control.py` independently asserts19
retail instructions establishing the component lookup and start/stop call sequence.
Evidence: native-emitter-control-audit.json and native-steam-emission-result.json.
Controller/property success does not prove particle counts, visible output or
material correctness, and this remains opt-in until those are verified.


## Original Lua emitter bindings

`StartEmitters(entity)` and `StopEmitters(entity)` now resolve registered opaque
entity identities and dispatch through a scoped C ABI callback during simulation.
They require exactly one current entity; stale/foreign identities, numeric handles,
extra arguments, missing host callbacks and out-of-dispatch calls fail explicitly.
Native dispatch additionally requires the active EM1 script owner and an owned,
admitted particle actor whose authored identity has not been revoked.

The private probe now queues the start/stop phases through the original Lua
scheduler rather than calling native controls directly. Live102143-857902da passed
all16 native operations for8 actors, matching enabled-property readback, then revoked
identities and released particle/gear owners on level unload. All28 Rust tests pass,
including emitter argument/identity/failure/scope and coroutine-yield coverage.
Evidence: original-lua-emitter-result.json. Original ForEachEntityInGroup is still
pending; complete DoorGear execution and visible particle output are not proven.


## Original Lua group iteration

`ForEachEntityInGroup(callback, group)` now resolves retained authored memberships
in source order, deduplicates native handles and snapshots registered identities.
Known members without admitted owners cause resolution to fail before any callback;
a group with no authored members yields an empty iteration. The iterator currently
requires exactly a function and a nonempty group name (at most256 bytes).

Iteration stays in Lua so callbacks can yield. Each snapshot identity is checked
again before its callback, including after coroutine resumption. Native access is
scoped to simulation dispatch. Host callback arguments now support kind6: a named
global function captured at enqueue time, without source evaluation or later name
lookup. Empty/invalid names and non-function globals are rejected.

The private probe queues the original callback-first form for four steam groups.
Live102539-a86ff958 resolves each group to its two authored actors for both start
and stop, applies all16 native operations, then revokes identities and unloads the
particle/gear owners. All30 Rust tests pass, including empty/invalid groups,
resolution failure without partial callbacks, yielding iteration, stale handle
reuse and function argument snapshots. Evidence: original-lua-group-result.json.
This proves the group/emitter bridge, not visible steam, particle simulation output,
or the remaining full DoorGear audio/animation/door/checkpoint behavior.


## Native particle population evidence

The opt-in steam probe traverses two representative native graphs (one jet and
one blast), with native type checks, bounded child lists, cycle checks and mapped
memory checks. It observes four NiPSParticleSystem objects in the jet and one
standard plus one NonUniformParticleSystem in the blast. The correct native
particle base type is833FA8D0; RTTI names are reached through its standard RTTI
pointer. Earlier exploratory captures used a different retail controller type
identifier; those were diagnostic inventories, not population proof.

Read-only `scripts/audit-em1-particle-population.py` verifies12 retail instructions:
allocation size, capacity initialization at+23C, live count at+240 as used by
particle removal and clone-array copies, and native child traversal/type metadata.
The final probe reports these fields directly instead of retaining raw state dumps.

Live103245-5a33e10c observes all six systems at zero while stopped. After original
Lua group start, the jet populations are0/3/0/14 (capacities10/8/50/20); blast
populations are10 standard and8 custom (capacities25/10). Every count is in bounds.
This proves actual particle creation in four sampled systems, including the custom
EM1 class. Two jet layers remain zero at that sample; their authored timing and
expected activity still need investigation. It does not prove all eight actors,
particle motion, visible rendering or material correctness. Native unload passed;
normal files/RVZ were unchanged. Evidence: native-particle-population-result.json
and native-particle-population-audit.json.


## Particle lifecycle and authored timing

The opt-in probe observes all24 native systems across8 actors at baseline,
30/120 ticks after group start and120/240 ticks after stop. Private103518-996658bc
finds22 systems populated at sampled times. The previously empty spray and large
puff layers have their first authored enable near2.99167 seconds and positive rate
at3 seconds; they populate in the longer run. Two small-puff instances are zero
at the sampled times, which does not exclude their short authored bursts.

Crucially, the full capture splits by actor: systems on handles121/123/124/125
are zero after stopping, while those on118/119/120/122 retain populations. This
rules out a universal failure to age particles. Visibility-dependent simulator
updates are a hypothesis requiring correlation with culling/render execution.
Source lifetimes are0.2�0.5 seconds with limited variance; their field layout was
cross-checked with bluethinner/src/blotworx/brush_particles.rs::Emitter::read_shape.
No forced simulation update or lifetime alteration was introduced.

Native unload passed and normal files/RVZ were unchanged. Evidence:
native-particle-lifecycle-result.json, original-jet-emitter-curves.json and
original-steam-lifetimes.json. Curve decoder: scripts/audit-em1-jet-emitter-curves.py.
Particle motion, visibility/material correctness and full tutorial remain unproven.

### Native particle scheduling trace (2026-11-03)

The owned-only diagnostic now counts the retail update-context gate (830BB538),
time commit (830BA7F0), finalization (830BA808), and particle removal (830BA310).
Tracking is opt-in, capped at128 owned objects, and cleared before world transition
and cleanup. Hooks preserve PPC context, MXCSR and LastError and call original
functions unchanged. They do not force updates or alter authored lifetimes.

Both five-phase captures show360 enabled updates/time commits and12 seconds of
simulation time advancement for every system. The12 retained systems have zero
finalizations/removals; all12 draining systems have positive counts. Thus the
update-context gate is not responsible for the split. Next trace the simulator's
submission/finalization and native visibility policy; off-screen suspension is
still a hypothesis. This is scheduling evidence, not proof of visible particles.

Canonical SHA047A53283797A329EBFEE3BC80B9AC20CC652BAAFF0685063F39858D104BA86F.
Private104344-182f2e0f passed native unload and closed, preserving normal files and
RVZ, without gameplay input or screenshots. Reproducible checks:
scripts/audit-em1-particle-update.py (16 retail instructions),
scripts/audit-em1-particle-update-capture.py (24 systems/five samples/time/counts).
Evidence: native-particle-update-result.json and native-particle-finalization-result.json.

### Particle retention explained by native render-channel policy (2026-11-03)

Native simulator submission830C13A8 exits before dispatch when particle+2DC bit4
is set. Native update830BB8C0 checks five cached channels through82552F68 and sets
that bit if none is accepted. Those channels are the low bits of the per-object
scene+2EF24 flags;8256F948 sets/clears them according to accepted render collection.
The bit therefore expresses original render-dependent suspension. This does not
independently establish correctness of imported particle bounds or final pixels.

The owned-only probe now counts submission attempts and the flag at entry. In
private104816-5b078d74, each of24 systems receives360 attempts. All360 attempts for
each retaining system are flagged; all12 draining systems receive unflagged
attempts and have positive finalizations/removals. Existing time counters remain
uniform. Keep the original suspension policy; no lifetime/update workaround added.

BuildC64FC355B7D1AD5FAB95A366A63B781EE801D191E7BA5905AB76CA4BC1DE888A passed
native unload and private-session guards and was closed.23 static instruction
assertions and all five-phase capture assertions pass. Evidence:
native-particle-submission-audit.json and native-particle-submission-result.json.
Next verify material/texture submission and remaining original tutorial interactions.

### Original particle material binding audit (2026-11-03)

scripts/audit-em1-particle-materials.py reads all six original particle material
tables and texture/sampler/alpha metadata, then compares exact staged FXLO names.
Four systems use FX_alphaBlend_singleChannel, one uses
FX_DiffuseAlpha_VertexColorAlpha, and the distortion layer uses
SFX_accumulatedIndirect_Positive. None occurs as an exact FXLO catalog string.
The first five have Gen3-prefixed counterparts; the distortion layer does not.
EM2 does ship Gen3_FX_DistortionAccumulate and
Gen3_FX_AccumulatedDistortionBufferRender, but these are not verified substitutes.

The current preview adapter handles NiMesh only, leaving NiPS/NonUniform
materials unchanged. Before changing them, verify native name resolution/aliases
and equivalent blending/depth behavior. Distortion also needs its native render
target and resolve path. Evidence particle-material-bindings.json includes asset
and archive hashes, technique candidates, six texture bindings and properties.
This audit changes no runtime files and does not prove rendered pixel correctness.

### Native particle material mapping candidate (2026-11-03)

Baseline105536-2ec5eb09 directly observed null native resolver results for all six
original techniques. Thus name absence is a live failure, not just a catalog gap.
The native loader forwards the authored name; null results are not attached.
The material array ABI is object+F8/count+100,2C-byte entries/material at+4;
material names are stored at material+C.13 instructions independently verified.

AdaptPreviewParticleMaterials maps only FX_alphaBlend_singleChannel and
FX_DiffuseAlpha_VertexColorAlpha to their Gen3 equivalents in NiPS/NonUniform
blocks. All other bytes and properties remain unchanged. The default-off cvar
em1_particle_material_candidate guards this change and alters the resource origin
key with mapping version/count. Private launcher flag -Em1ParticleMaterialCandidate
requires -Em1VerifyParticleActivation. No distortion alias is guessed.

Candidate105838-8575804f selected native SA variants on20 instances (16 mask/4
RGBA) through all five lifecycle samples. Four unmapped distortion instances
selected Gen3_DefaultShaderTextured. Both runs unloaded and closed successfully,
with normal files/RVZ unchanged and no gameplay input. Build SHA
8D60BDFA6E8603782844F88C77A2169A3F04B34FCACF007F6C2BC32B80AD8E8D.
Evidence: native-particle-material-lookup-audit.json and
native-particle-material-candidate-result.json; checker:
scripts/audit-em1-particle-material-capture.py.

Native selection is now proven for the mapped systems. Pixel/blend/depth fidelity
and distortion accumulation/resolve remain unverified; keep this candidate opt-in.

### Particle shading source audit (2026-11-03)

- Demo PID25680 was verified live/responding and left untouched. No rebuild, restart, gameplay input or screenshot this turn.
- Added audit-em1-particle-shading.py. Reads original three Wii R3MT templates and extracts native FXLO pixel programs. SDK ucode fields verify both ordinary/SA single-channel programs fetch texture red, then multiply (1,1,1,red) by interpolated colour. Wii template uses texture alpha instead.
- Independently checked portable SmokePuffs_08A and Cone_Spray NiPixelData: R=A for every texel across14 mip levels (7 each). Therefore the channel choice is harmless for those fixtures; Smoke-B remains unverified. Source templates require alpha>0, LEQUAL/no depth writes and SrcAlpha/InvSrcAlpha for non-distortion; distortion uses additive One/One. Diffuse-alpha Wii TEV doubles/clamps RGB in its second stage; native vertex/pixel composition still needs verification.
- Script passes and writes particle-shading-audit.json with source/texture hashes, complete TEV/PE attributes, native program offsets/hashes/constants and per-mip comparisons. No GPU equivalence or effective native draw-state claim. Candidate stays opt-in; broad goal active.
- Next: obtain/check Smoke-B pixels, verify diffuse vertex-colour scaling/constant binding and effective native blend/depth state, then GPU comparison. The user's demo remains open; do not interrupt it merely to run diagnostics.


### Diffuse particle brightness investigation (2026-11-03)

- Verified demo PID25680 remained live/responding; no runtime build/restart/input. Continued offline shader comparison.
- Expanded audit-em1-particle-shading.py to assert both ordinary/SA diffuse pixel multiply sequences and vertex colour exports. The pixel path is texture * Constant_Color_1 * interpolated colour; both vertex shaders export fetched colour unchanged. All intervening ALUs are checked for writes to that register, including rejection of indirect destinations. Original Wii template has an extra Scale2/clamp RGB stage.
- Added authored NiColorExtraData to particle-material-bindings.json via audit-em1-particle-materials.py. The original blast's Constant_Color_1 is (1,1,1,1). Runtime parameter conversion or generated particle vertex data could still compensate; do not claim observed brightness error or double the shader until those inputs are checked.
- Both audits and diff check pass. Native bytecode and original templates remain unchanged, candidate remains default-off. Next inspect effective runtime constants/generated colour before a GPU brightness comparison; Smoke-B channel equality and native blend/depth verification remain open. Broad goal active.

