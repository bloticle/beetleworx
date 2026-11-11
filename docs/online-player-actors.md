# Native actors for larger online sessions

Status: development, not accepted sixteen-player gameplay. The current in-game route still uses the two-player Session adapter. The new Room transport has real sixteen-peer tests and feeds typed frames into the native owned-actor coordinator; it has not replaced the in-game startup and controller path. A larger roster alone does not create controllable actors.

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
arguments include character in r4 (0=Mickey, 1=Oswald), mode in r5, and a required Vector4 position pointer in r6. The character selector is not a player slot: repeated calls construct fresh entities without growing or replacing the two retail records. Values other than 0/1 are not additional player indices.

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
physical interfaces; this does not establish separate human input routes. Their native motion state was ground (0), movement enabled,
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

`BeginOwnedActorBasisTick` and `PrepareOwnedActorBasis` stage a normalized ground-projected view direction for each peer. The native controller-to-motion boundary (`827874B0`, return `827837E8`, saved `r27` equals the verified owned component) uses that immutable direction only for the current actor and tick. It restores the original native stack argument afterward. Captured transition bases, alternate references, disabled actors and stale world/generation/tick state reject. Native movement, smoothing, permissions and physics continue through the original code.

`ReplayOwnedActorMovement` now delivers the exact native Movement action through the original Mickey handler `82A3C930`, including its pause, per-entity, special-mode, task/FSM and lock gates. It requires a staged basis and claims input once per peer/tick separately from the motion-basis claim. Every admitted tick must deliver a native movement action, including neutral. Missing peer input must stop the world barrier, rather than advancing with a previous input cache. Other actions remain unsupported; this API is not wired to network gameplay or sixteen actors yet.

## Native movement reference lifetime

Live inspection confirmed Mickey's Basic controller at full-controller+16 has vtable `8211DE70`, with virtual108=`82A342E8`. This derived setter first assigns the reference through native NiPointer, then retargets a retail player camera. Extra actors must not steal that camera. `online_actor_reference.cpp` validates the actual derived layout and uses the original base reference operation `827DDD88` for the identical native ownership effect without the retail camera retarget. It rejects self-reference cycles, foreign pre-existing references, arbitrary non-camera entities and stale actor/world identities. Each component retains/releases its own reference, including when several actors share a real camera entity; the staged peer basis controls ordinary movement direction.

Factory failure rollback occurs strictly before entering native admission. It holds the fresh entity with native NiPointer, invokes native owner RemoveAt `82747128`, then releases the hold. It never invokes component initialization/deactivation for an actor that was not admitted. `827CAE68` only propagates time-mode flags; admission `82742F70` first invokes entity virtual368, then virtual36 to activate components. The normal destroy queue `82744E90` invokes deactivation and is unsuitable for fresh unadmitted rollback. No guessed entity-initialized byte is used.

## Current verification

The same executable's fixtures cover original native action delivery, transition reference projection, controller update, property iteration, motion direction/smoothing, tick ownership, stale handles, native permission gates and exception restoration. Their captured motion sinks stop before contacts; they are not a complete physics/playthrough test.

Two private, input-free live checks ran in `tcy_autotopia_visit1.sectiona_first`:

- Forced registration rejection removed the fresh actor from its native owner (57 to 56 entries); its handle disappeared and the original players stayed at health 5. No actor was admitted.
- Successful registration/admission retained a real movement reference. The extra Mickey had one scene root and animation graph, remained grounded at health 5 for the five-second hold, released the reference, and completed native removal with its handle gone. Both original players stayed at health 5 and their handles remained unchanged.

Evidence: private capture `private-gpu-profile-20261111-071443-2d3eedbd`, `game.log` for forced rollback and `game-reference-fixed.log` for successful lifetime. Successful live executable SHA256: `8A260B821342604CAC67F18DFF102DF23EBDE698E91F2EF3174FD3489D357703`. No movement/camera input or screenshots were sent. These checks establish one idle owned actor's lifecycle, not online movement or sixteen-player synchronization.


## Room frame integration

`online_room.cpp` now supports sixteen actual TCP peers with host-assigned identities, authoritative roster/start, and identical contiguous input frames. `online_actor_frames.cpp` preflights all owned actors before native action delivery, validates logical spawn incarnations separately from guest handles, and requires explicit native interpreted movement/view data. The real loopback-to-native fixture receives Room frames and executes the original action and controller callbacks for two owned actors, including different view directions and neutral release; its controlled continuation sink stops before physical contacts.

The adapter is not yet connected to normal lobby startup/world stepping. Native local action production, shared checkpoint installation for all guests, actor spawning/respawn, additional actions and Oswald, complete world synchronization and Internet relay remain required. The intended native delivery point is after synchronized join and before original world update `829ED310`; all-peer preflight must happen before committing that world tick.

## Owned death notification boundary

The original Mickey event handler `82A38FC8` sends event61 `ReloadCheckpoint` into global fade `82A010E8` at return `82A39504` with hardcoded player0. `online_actor_death.cpp` can intercept this exact call for an explicitly registered extra Mickey, after the original native event/FSM handling. It validates the live registry, controller, event-listener vtable, event string and fade singleton. Retail actors, FallRescue, unregistered actors and other callers retain the original behavior.

The route is **unarmed by default**. A native lifecycle consumer must explicitly arm it with a verified actor and consumer epoch. Interception produces bounded, deduplicated immutable tickets instead of starting player zero's global fade. The consumer can inspect them only outside native event traversal; it must complete native actor removal or world teardown before explicitly confirming ownership. Disarming retains pending tickets. Stale world/handle/controller identities are reported as retired and never retargeted to a replacement actor.

Regression fixtures execute the original Mickey event/FSM forwarding and native fade fallback, and check deduplication, stale ownership, exact acknowledgement, retail/FallRescue fallback and context preservation. This is not yet an active online death/respawn consumer: normal gameplay does not arm the route, and synchronized removal, respawn, actor epochs and world barriers remain required.

## Native jump action prerequisite

A passive live snapshot in `tcy_autotopia_visit1.sectiona_first` confirmed Mickey's named `Jump` task is present in the native dispatch collection, has vtable `820BC838`, and owns the same NiEntity as the selected Mickey. Its actual start/stop/action methods are `8287ECF8`, `8287D048`, and `8287EA38`. Evidence: `private-gpu-profile-20261111-084759-851208bd/native-movement-tasks.json`; no gameplay input was sent.

Original Mickey input forwards Jump through the base task/FSM path. The task resolves its own owner's Basic controller, motion and character-physics components before reaching jump stimulus `82884018`. The scoped movement lookup alone therefore cannot establish jump ownership: the named task, dispatch membership, task owner and typed motion/physics owners must be revalidated for each action. Original release handling clears the task's active flag and notifies the native task system; that flag is not proof of a physical jump or shortened jump height.

The development room adapter now carries explicit native Jump edges. A native Jump dispatch means a press; forwarding a held Pad button as a fresh Jump every tick would incorrectly repeat presses. The normal in-game Session route has not switched to this adapter.

`online_actor_jump.cpp` now provides scoped native press/release delivery for registered extra Mickey actors. Commands carry the expected native generation, exact tick and consecutive per-generation edge sequence. Duplicate/held presses, release without press, stale ownership, sequence gaps and reversed ticks reject. Each dispatch revalidates the named task, dispatch membership, typed motion/physics/gravity ownership and any animation owner. It requires the current tick's Movement delivery before controller update, and leaves that motion basis available. Original task-disable counts and native ability/FSM gates remain authoritative.

The same-executable fixture executes original `82A3C930` through the task dispatcher, native Jump start and physics stimulus `82884018`, stopping before body integration. Two actors produce separate receiver traces; native release clears only its task state. Fifteen scoped checks cover edges, generations, ownership, native disable behavior, raw caller/FP preservation, exceptions, atomic batch validation and retail-record isolation. The notification path remains original; a private null display-name fixture avoids requiring a live string-pool runtime. These checks do not establish body trajectories, animation correctness or live online button support. A further seventeen-check actual TCP fixture transports two distinct per-actor streams into original native Jump dispatch, observing two versus one physics stimuli, held ticks without repeat and released task flags. The independent local producer is tested separately. Normal lobby/runtime adoption remains required.
