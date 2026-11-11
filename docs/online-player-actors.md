# Native actors for larger online sessions

Status: investigation, not implemented multiplayer capacity. The current
transport and gameplay adapter still support two players. A larger roster alone
does not create controllable actors.

## Online companion policy

Online gameplay uses human-controlled roster actors; it must not keep an
additional AI Oswald companion. Offline single-player retains the native AI.
The two-player adapter converts the existing companion through the original
synchronized co-op join. It also re-arms that join if a subsequent level load
restores a single-player 2D or 3D AI record. Join preparation uses agreed neutral
inputs and P2 Start, then restores normal controls after native human activation.
Camera/cutscene changes alone do not request another player or replace actors.
Native multi-world online acceptance remains separate from the policy and TCP
regression fixtures; the adapter still supports two network players.

## Verified retail factory path

The read-only live capture `analysis/native-player-factory-live-20261110.json`
records build `A7D318524B3B387ECAE49D78BCE246FFED886F0CD34D456E5FBB99427BD1EACD`.
The actual PlayerManager vtable is `8210A3E8`, with factory method `8276A828`
at offset 72. That method selects these exact, live string constants:

| Character/controller | Guest string address | Prefab |
| --- | --- | --- |
| Mickey | `8207877C` | `Player` |
| Human Oswald | `82078784` | `PlayerTwo` |
| AI Oswald, 3D | `82078790` | `AI_Oswald_2` |
| AI Oswald, 2D | `8207879C` | `AI_Oswald_2D` |

Source: `generated/ppc/ppc_recomp.134.cpp`, function `8276A828`. Its factory
arguments include slot in r4, mode in r5, and a required Vector4 position
pointer in r6. It accepts slot 0 or 1, mode 0..2, and returns zero for other slots. Do not pass slot 2
and expect it to construct a third player.

The selected string goes through `82743D18` to resolve a blueprint using
`82742640`, then `82743AD8` to clone it with `827B9960`. The latter inserts
the entity through `82510C20`, applies the supplied transform, and optionally
admits it through `82742F70`. This is a general entity-creation route, separate
from storing a player in the manager's two records. Source:
`generated/ppc/ppc_recomp.127.cpp`.

The player replacement routine `8276CCF0` computes record address
`manager + slot * 320`, stores its handle at record+20, and applies native
initialization and admission around the factory. It does not grow an array.
Calling it for arbitrary extra slots risks accessing unrelated manager memory.
Source: `generated/ppc/ppc_recomp.135.cpp`.

The capture resolves the existing two handles through the native registry,
rather than treating them as pointers. Both actors have separate motion and
input components. Their native motion state was ground (0), movement enabled,
and desired velocity zero. This is a snapshot, not a movement/playthrough test.

The first private instantiation probe (`081657-9b3eab51`, build `A211002A`)
omitted r6. It crashed in `8275C0C8`, called from `82743AD8` through
`82743D18` and `8276A828`. The symbolized report is
`analysis/native-extra-player-factory-first-crash-symbols.txt`. The corrected
probe obtains a real Vector4 with the original `8275C160` position getter and
passes it explicitly. Do not infer that the factory has only slot/mode arguments
because its wrapper does not modify r6.

## Verified additional-instance lifecycle

Private capture `analysis/private-gpu-profile-20261110-082143-8c838da7` used
build `CF8346BB5E0DDCDF38C8015001AB376A7AD1BC03C48DE34F9CCABB6B878034A5`.
The one-shot `em2_probe_extra_native_players=14` test constructed fourteen
additional native character instances, alternating seven Mickeys and seven
human-prefab Oswalds. Every extra entity had a distinct handle and motion
component. All fourteen went through original initialization `827CAE68` and
world admission `82742F70`, then original deferred destruction `82744E90`.
The following update resolved zero surviving extra handles.

The two original manager handles stayed unchanged. A subsequent read-only
snapshot (`analysis/native-extra-player-admission-retail-after.json`) resolved
both original actors grounded, movement/input enabled, and stationary. The
protected capture closure verified normal user files unchanged. The test sent
no gameplay input and used no extra player executable.

This tests native construction/admission/destruction within an update. It does
not prove visible rendering across frames, independent controllers, additional
actors' physics/interaction behavior, level transitions or network replication.
Actual online capacity remains two.

Later live testing did render fourteen extra instances. The default-enabled
`runtime/mickey_ears.cpp` captures each native Mickey's complete dynamic ear
matrix pair and rebinds it for meshes belonging to that actor. It does not
disable dynamic ears or allocate replacement native ear processors. Build
`0FB2CB4F4D4E5A43806413EFC8BC26EAF9E2613EF98CFF3E182A6978D2A41F0D`
passed targeted and full regressions; private capture
`private-gpu-profile-20261110-092849-c8f0ab5c` observed successful native uniform
bindings for seven distinct current Mickey roots and cleanup of all extras.
Visual acceptance and confirmation of the final GPU constant uploads remain open.

Longer spawn tests still expose a checkpoint reload. Read-only health samples
showed both original players staying at 5 health while extra instances 6 and 7
dropped from 5 to 0. Reducing the spawn radius from 2.5 to 1.0 did not prevent
those same two copies dying. See
`analysis/extra-player-death-original-health-result.json` and
`analysis/extra-player-close-spawn-health-result.json`.

The subsequent read-only native health/contact traces identified the cause:
`827AC868` finds penetrating contacts against one motion-type-4 colliding body,
then invokes `82760348`, which writes zero Health through `827CA5F0`. First
fatal depths were approximately -0.506 and -0.522, beyond the live 0.2 tolerance.
The two original actors stayed at Health 5. The original ring therefore placed
extra actors inside collision; this was not the originals dying on admission.
Evidence: `analysis/native-extra-player-restart-cause.json`, private capture
`private-gpu-profile-20261110-095943-f05ea8fa`, matching build
`36E5520CECD94E544A1148F9A81EB86CBA9388267F172690544B658F7C3B03D2`.

`native_player_spawn.cpp` now supplies a native character-hull query for group
placement, including the native sweep API's initial-contact collector so a
vertical cast cannot silently ignore an existing wall overlap. It checks a
walkable supporting floor and the final occupied volume before accepting a
site; the original actor/body is not moved. Native-world validation is separate
from the CPU fixture. Individual online death/respawn routing remains unresolved;
this is not a working sixteen-player online implementation.

## Required player appearance

Every player must select a unique session color. The exact same RGBA value must
drive that actor's pants and the username label above their head. Character
selection is independent: multiple Mickeys or Oswalds are allowed with distinct
colors. The host must enforce uniqueness atomically on join, appearance changes
and reconnect handling; a client-side color picker alone is insufficient. A
rejected color change must preserve the previous character/color and roster
revision. Releasing a color must not recolor other players.

The roster model supplies sixteen distinct color values and rejects occupied
colors. Actual per-instance pants shading, overhead labels, network replication
and native lobby editing remain unimplemented integration work. Do not implement
appearance with a single shared character-material tint: it would reproduce the
same cross-instance sharing problem being investigated in Mickey's ears.

The preceding inactive-only attempt (`081855-7c0daf94`) crashed during deferred
component deactivation: `827CBA28` -> `825172F0` -> `831BF098` -> `831BFA78`.
That path removes components from the world update registry. Cloning without
admission had never registered them. The corrected test follows admission
before destruction rather than suppressing native unregister failures.

To repeat in an isolated profile, after closing any running game:

```powershell
pwsh -NoProfile -File scripts/start-private-gpu-profile.ps1 -Scale 1 -FrameCap 30 -GameplayTiming 0 -OriginalLighting -NoGpuProfile -OriginalRenderBindings -StartLevel tcy_autotopia_visit1.sectiona_first -ExtraPlayerInstances 14
```

The probe waits for four seconds of stable native player handles after level
entry. It runs only once, only with an explicit private-profile flag and user
directory, and rejects synchronized sessions. Its default count is zero. It
does not change either retail player slot, assign controllers, free guest
memory directly or edit native reference counts. Invalid/aliased factory
results are never added to its destruction list.

## Implementation requirements

Extra actors must use separate, owned handle records scoped to the world
registry. They must not overwrite the two retail records or inherit another
player's input route. Before adding network capacity, verify actual native
instantiation, activation, unique runtime identity, input isolation and clean
destruction/recreation across a level change. Shared action names such as
`Movement2` must not broadcast one player's input to every Oswald clone.

After actor ownership works, the two-lane `Session`/input gate, load barriers,
initialization sequences and desync checks require corresponding integration.
Sixteen distinct colors, rendering of name labels/pants, lobby admission and
Internet matchmaking remain additional requirements. None is proven by this
factory inspection.

## Reproducing the snapshot

```powershell
python scripts/inspect-native-player-factory.py <private-capture-directory> --output <new-evidence-file.json>
```

The script verifies process creation time, canonical executable path/hash,
guest mapping and registry identity. It bounds registry traversal, checks
manager/registry/handle stability, and refuses to overwrite evidence. It opens
the process for read/query only; it does not execute factory calls, create
actors, write process memory, suspend threads or send input. Changing motion
values can still occur during observation; stability does not imply an atomic
whole-frame snapshot.

## Independent actor input boundary

`online_actor_input.cpp` introduces explicit peer-to-native-actor ownership for extra Mickey actors, validated through the world registry, entity handle and typed Mickey controller. Registered extras are quarantined from ordinary human-input broadcasts; the two retail players retain the original route. Ownership expires when the registry, handle, entity or controller changes, and disabled extras remain quarantined until their native listeners have been removed.

The base movement lookup is scoped to its audited native call site (`8276B968`, return `82783EB0`) and the explicitly replayed listener. It never allocates a third retail player slot. Native callback fixtures send opposite movement samples to two extra controller caches through the original base handler and verify that retail record/health/view sentinels remain untouched. Those sentinels are not a physics or death/respawn test.

Production action replay currently rejects with `BasisRouteUnverified`. Native navigation consumes a camera-relative basis later than the input callback; substituting one of the two retail cameras would make additional actors move relative to somebody else's view. A typed per-peer basis alone cannot fix that consumer. This boundary is preparatory work, not working sixteen-player movement, and it is not wired to automatically spawn additional actors.

The next audited input dependency is the retained camera reference: native `8276C0C0` resolves a retail actor, looks up type `833DAE58` and invokes virtual `+108` with the camera entity returned by `82850FF0`; fallback `82761990` uses virtual `+120`. Setter `827DDD88` retains that reference at component `+80` through native `8308B3E8`. Because navigation consumes it after the action callback, a temporary callback-only camera override is insufficient. Per-extra-actor consumers and reference ownership still require verification; viewport normalization `8276BC90` is not the movement yaw basis.


Native callback fixtures now cover transition-reference projection through original `82783208` and ordinary action-to-world movement through original `827874B0` followed by `8278A790`. The latter verifies cardinal camera directions, opposing actors, native lateral sign, movement permission and smoothing; its virtual movement sink captures the vector before physics, so it is not a contacts/playthrough test. The ordinary controller-to-motion boundary is `827874B0` at return `827837E8`, with the typed component in saved `r27`. A future adapter must preserve owned retained-reference lifetime and reject stale tick/generation, captured transition bases, alternate references and disabled modes. A null retained reference causes native `827835A8` to skip movement before reaching that boundary; substituting only the final basis cannot fix uninitialized extra actors.
