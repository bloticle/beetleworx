# Native spawn authority development

The private authority adapter now includes targeted native Shooter burst replay
and guest-only projectile eligibility. Generic world births, projectile motion
and lifecycle authority remain incomplete. This work does not claim a playable
server-authoritative world or a new demo release.

## Confirmed failing object

The failing target is `dropwingbubbleprojectile`, template GUID
`00069771/00000000/00000000/00000000`. It is not an unspecified missing NPC.
Its native component inventory includes dynamic rigid-body physics,
ProjectileComponent, LifetimeComponent, TemplateSpawnerComponent,
PaintThinnerCounterComponent, script, FX and transformation components.

In the complete operation capture, the host creates the projectile at tick
2573 and the guest lacks its GUID when Damage arrives at tick 2879. The host
snapshot observes the moving rigid body between those points. The operation
record resolves both its spawner owner and anchor to native BEntity GUIDs,
captures the template ID and three scalar arguments, and verifies an unchanged
native CRT owner while reading the pre/post random states.
Evidence:
`analysis/private-native-room-pair-20261115-094047-c8788978/missing-damage-identity.json`.

A separate clone capture observes an earlier eligibility difference at tick
5179: the guest creates a pickup-spawner item, `ammo_thinner` and its parentable
form, while the host creates a bubble-burst effect. At tick 6001 the host then
creates a Dropwing projectile the guest does not create; Damage fails at tick
6007. Thus replacing GUIDs alone cannot repair all of this divergence.
Evidence:
`analysis/private-native-room-pair-20261115-093008-ea27442f/missing-damage-identity.json`.

Both runs are recorded as failed world integrations. A normal process exit
from the ownership finisher is not a gameplay pass.

## Native factory paths

The bounded PPC frame walk resolves constructor/clone callers without changing
native execution. The observed paths include:

- `827CC8A0`: generic entity constructor; registers the BEntity interface at
  result +8. The registration return site `827CC988` alone does not identify the
  asset or higher-level spawn operation.
- `827B9960`: instantiate a native entity-template description and its native
  components. Inputs include the template description and two flags; the
  result is the entity root, with BEntity at +8. The template's +0 string and
  four-word ID at +4 identify its source without transmitting its pointer.
- `82743AD8`: common world/template instantiation path, with observed clone
  return site `82743B9C`.
- `827B3298`: the spawner operation that creates the failing bubble projectile.
  Its observed parent return site is `827B3F14`, inside `827B3C60`. The live
  capture validates an owning entity through component +48, an entity anchor
  in r4, a template description in r5, scalar r6/r7/r8 inputs, and a returned
  BEntity interface. The scalar meanings are not guessed from their values.

The existing private random trace for `827B3298` now also records those typed
observations when the private world-Damage probe is enabled. It only reads
native state; it does not seed/replay randomness or change spawn eligibility.
Likewise the clone observer does not instantiate extra objects or substitute
host/guest entities by matching local addresses.

Native spawning also performs parenting, component initialization, transforms,
callbacks and spawner bookkeeping. Replaying only a constructor or copying a
new GUID is insufficient. A future spawn command needs a validated native
operation, source-template identity, entity references, required state and
the correct simulation phase; missing local births must actually be created,
and independently generated guest-only gameplay births must be controlled.

## Full spawner boundary

The audited parent `827B3C60` takes the component in r3, a signed selection
argument in r4, the anchor entity in r5, a flag in r6, and an optional local
output collection in r7. The collection pointer cannot be transmitted.
It calls `827B3298` at return sites `827B3D8C` and `827B3F14`, potentially
several times, and returns the last spawned BEntity or null. Its zero-result
paths can also change component state through a virtual method.

Before returning normally, it increments component +104, queries the configured
limit through a property callback, and can dispatch native event type 24 when
the count reaches that limit. Its random selection reads component +140/+144.
Consequently a low-level SpawnOne replay alone does not reproduce this parent
operation. Both zero-birth and multiple-birth calls matter; observing just
successful clones misses these transitions.

The private observer now brackets this original parent call, recording portable
owner/anchor identities, scalar inputs, collection presence, +104/+140/+144
before/after, returned identity, and CRT states. The observer preserves the
original PPC context around inspection and always calls the original once.
It does not suppress guest calls or replay a host batch. The comparison tool
`scripts/compare-native-spawn-batches.py` preserves call order and separates
random-state differences from differences in spawn inputs/results/bookkeeping.
Incomplete trailing operations are retained separately.

The observer build passes a bounded two-window run through agreed tick 3000.
All 12 completed parent batches match across both peers, including scalar
inputs, native child identities, counters/selection fields and CRT states.
One observed projectile changes +104 from 3 to 4, +140 from 1 to 2 and +144
from 0 to 1; the low-level constructor does not describe those parent effects.
Neither peer reports a missing Damage identity in the bounded prefix. This
run does not reproduce or resolve the divergent births from the failed runs
above. Both owned processes close normally with normal user files unchanged.
Evidence:
`analysis/private-native-room-pair-20261115-095829-6b8cba86/spawn-batch-smoke-result.json`.
Observer executable SHA256:
`AEFAB965B26DB60C2BA636D3751D6CCB9AC197CBABC65B26049E42D46C962236`.

## Portable native batch capture and transport

`online_spawn_batch.h/.cpp` now define the audited TemplateSpawner parent
operation as bounded portable data. The format retains owner and anchor GUIDs
or stable lobby-player references, the signed selection argument, flag and
collection presence, nesting depth, component +104/+140/+144 before/after,
optional native CRT states, the returned entity ID, and ordered native child
template IDs, generated IDs and scalar arguments. Local pointers, native
handles, collection storage and callable addresses are excluded.

The private `em2_probe_room_spawn_batches` adapter captures completed native
parents and their immediate SpawnOne children. Nested parents retain their
own depth and child lists. It binds human references through the current
native lobby roster rather than keeping respawn-generated human GUIDs.
Storage retains at most 65 children to reject operations exceeding the wire
limit of 64 without unbounded diagnostic allocation. Unsupported ownership,
results or native flags fail capture rather than being silently normalized.

Room wire version 20 carries at most 32 batches per completed authority tick.
Validation rejects duplicate generated IDs, references to absent lobby peers,
oversized child/batch counts, ambiguous references and malformed payloads.
The full maximum payload fits the existing 512 KiB authority packet budget;
the existing publication preflight retains its backpressure behavior.

This adapter compares the guest's completed native batches against the
received host batch and stops at a divergence. It does not instantiate
host-only entities, suppress independent guest births, restore CRT/counters,
or implement full gameplay rollback. Common-world factory births outside
TemplateSpawner are not covered by this command type. Public/demo launches
leave the capture adapter disabled. Do not enable it as a multiplayer fix.
The isolated pair launcher exposes it as `-SpawnBatches`, requiring host
correction mode.

The same-executable suite passes 160 spawn-codec and 9,083 Room checks. The
Room fixture sends maximum-size spawn batches alongside maximum damage/AI
and character data over real TCP, checking fragmentation, exact equality,
ordering and pressure/rejection behavior. These are codec/transport checks,
not proof of native replay.
Evidence:
`analysis/native-spawn-transport-test-63cd51ac3433493ca94b2a88b1af7a1a/rollback.log`.

The live wire-20 run verifies six nonempty host batch messages on the guest at
ticks 451, 458, 872, 934, 1198 and 1462. At tick 1720 the new adapter stops on
a guest-only `dropwingbubbleprojectile` birth (ID
`00004486/00007792/00000AC8/00002ECA`), while the host completes that tick
without a parent batch. The eight earlier full observer batches, including two
pre-admission operations, match exactly. This is an intentionally failed
integration, not completion of spawn authority; it catches creation-eligibility
divergence before waiting for a later missing-Damage target. Both owned
processes close normally and leave normal user files unchanged. The host
finisher's requested gameplay-stop marker is absent, so its observation times
out before the normal owned close; that observation is not reported as passed.
Evidence:
`analysis/private-native-room-pair-20261115-100911-0ff20939/spawn-transport-integration-result.json`.
Executable SHA256:
`6E09FAE0EF2CB462748CEA0FD6C2ED304C65D657A645B673B839C352DB156F43`.

The guest-only call returns to `828B71C4` inside `828B7148`. The original
caller immediately tests r3 for zero and takes its normal null-return epilogue
at `828B7238` before any spawned-entity dereference. This audited boundary is
a candidate for eligibility suppression; the capture-only adapter does not
skip it. Low-level clone suppression remains unsafe at other callers that
dereference the result unconditionally.

## Guest-only projectile eligibility

The separate, default-off private flag `em2_probe_room_spawn_eligibility`
now excludes guest-only operations at the audited `828B71C4` call site.
It requires the existing capture flag, a host-corrected admitted guest, the
matching pending authority tick, no transition/load, depth zero and no output
collection. It resolves the owner to its native GUID or stable lobby identity.
If that owner has an unconsumed host operation, original execution remains
enabled, including zero-birth operations. If no such operation exists, the
wrapper returns null without entering the native parent. The caller follows
its original checked null-result path, and no constructor/RNG/counter effects
from the excluded parent occur. The adapter does not fake a consumed command.

Eligibility first requires the completed local batch sequence to be an exact
host prefix; a prior mismatch halts instead of being hidden by suppressing
subsequent calls. Other call sites and nested operations retain original
execution until their native behavior is audited. The isolated launcher
exposes this as `-SpawnEligibility`, requiring `-SpawnBatches`.

The same-executable suite passes 168 codec/eligibility and 9,083 Room checks.
New decision checks cover absent owners, outstanding and exhausted commands,
different owners, zero-birth calls, stable human references and invalid/excess
completed prefixes. Evidence:
`analysis/native-spawn-eligibility-test-d60dd09f575544fa917d93742c3c864c/rollback.log`.

Reusing the failed capture run's lobby nonce reproduces its matching early
batches and exact guest-only projectile at tick 1720, with owner
`000054B5/0000342B/00006C33/000068CF` and counter/selection/used fields 3/1/0.
The new adapter excludes that operation, returns safely through the real native
caller, and completes the tick. At tick 1728, the host creates a projectile for
the same owner while the guest makes no parent call; capture correctly halts.
Thus eligibility suppression is exercised in actual native gameplay, while
host-only birth replay remains unimplemented. This is a failed full-world
integration with a passed targeted suppression case, not a playable authority
claim. Both owned processes close normally and leave normal user files alone.
Evidence:
`analysis/private-native-room-pair-20261115-101639-c9dce970/spawn-eligibility-integration-result.json`.
Executable SHA256:
`1A2D1E1882C295C1F07656BC076DF12B5CA1EC43FBD975AC526CE0CBC28AE516`.

## Additional projectile initialization outside the parent

The `828B7148` caller does more than obtain a new entity: after the parent
returns it configures the projectile's native source/instigator references,
collision relationships through `8275D948`, and an FX owner through
`827D87D8`. Its burst caller `828B92A8` invokes it at `828B936C` and records
successful native entity handles in the shooter's component +80/+84 array via
`82747B60`. A null result takes that caller's existing failure path instead.

Consequently replaying `827B3C60` alone and assigning a host GUID would still
omit native projectile setup and burst ownership. Host-only replay must cover
these outer effects or an audited complete firing operation; constructor-only
replay must not be presented as a functional host-owned projectile.

## Original native Shooter preparation replay

The new, default-off `em2_probe_room_spawn_replay` flag requires capture,
eligibility, an admitted host-corrected guest and a pending authority command
for the exact current tick. Spawn-batch format 2 and Room wire 21 carry a
Shooter-origin boolean, not a remotely supplied function pointer. The adapter
resolves the current owner through its GUID or lobby peer, decodes native
component RTTI leaves without calling getters, and requires uniquely bound
`ShooterComponent` and `TemplateSpawnerComponent` objects on that owner.
The Shooter's +72 link must point to that spawner, and its pending-shot list
must be empty before replaying a fresh burst.

Missing host births invoke original `828B92A8` on a bounded borrowed native
stack at the completed guest boundary. Its original `828B7148` path performs
source/instigator, collision relationship and FX setup; the burst's original
array append records returned handles. No Havok object, native pointer or
allocator state is copied from the host. Matching local firing calls also
consume host selection/flag and random inputs at their existing parent entry.
The initialized native CRT owner is validated, seeded through original srand,
and read back; the wrapper preserves the caller's full PPC context and host
FP/error state around that seed operation. Native constructors generate IDs
normally from those inputs; this adapter does not replace generated GUIDs.

Before execution, the local parent counters/selection fields must already equal
the host pre-state. Counters are not blindly restored. Every captured replay
must equal the complete host parent payload, including native child template
IDs, generated IDs, arguments, RNG and bookkeeping. The outer firing result is
then checked against registered local entities and the real pending-shot handle
array. Unsupported paths, replaced ownership, missing classes, nonempty shot
lists, no-progress calls, callbacks leaving the owned world, or payload
mismatches halt the private adapter. Other factories and nested operations are
not treated as this firing RPC. Launcher switch: `-SpawnReplay`.

The first live replay test reuses the earlier nonce, suppresses the extra
projectile at 1720, then creates the missing host projectile at 1728 with ID
`000001C6/00002A78/00001DCF/00006F74`. Exact parent data and one pending-shot
handle pass native readback. Both peers subsequently sample the moving entity.
Its positions differ, however, so birth identity/setup do not prove motion sync.
The run fails at host tick 1985 when a real collision produces 522 child keys,
exceeding the old limit of 512. Both windows close normally with user files
unchanged. Evidence:
`analysis/private-native-room-pair-20261115-103026-4d330a6d/native-shooter-replay-integration-result.json`.
Executable SHA256:
`2B9F0964484AC928C909794CE22D86ABF71D114E29DD9B7BB4A8D838EB7F38ED`.

The collision-key limit is now 1024, with an 8192-byte Damage envelope, a 1 MiB
authority envelope and a bounded 2 MiB per-connection queue. The compile-time
budget covers maximum damage, character, AI and spawn batches together;
publication still preflights all queues before appending. The native fixture
now captures the observed 522-key case and the full 1024-key boundary without
dropping or reordering keys. Native capture/codec and Room checks pass 18, 156
and 9,083 checks; the spawn-codec/eligibility checks pass 170.
Evidence:
`analysis/native-shooter-dense-damage-test-ec5f0ef021b94b7aabe0ca5e3f2042d7/`.

The updated live run repeats native burst replay at 1728, then passes the old
failure at 1985: 15 Damage events with 1166 total collision keys are rebuilt
and delivered on the guest with exact native recapture. At 1990 the host's
next parent pre-state is count 4, choice/occupied field 1, mask 0; the guest's
pre-state check fails, so replay halts instead of assigning those fields.
Impact/lifecycle behavior and projectile motion remain unsynchronized; the
available observations do not prove that registry deletion alone would repair
the slot difference. The parent captures through the preceding burst match
across peers. Both owned windows close normally with user files unchanged.
This is a failed full integration with passed birth and dense-Damage cases.
Evidence:
`analysis/private-native-room-pair-20261115-103808-5a3b27b5/native-shooter-dense-damage-integration-result.json`.

## Host-owned impact and retirement

The default-off private flag `em2_probe_room_projectile_impacts` adds impact
authority for projectile IDs captured from Shooter preparation. It requires
world Damage, spawn capture, eligibility and replay in the isolated launcher
(`-ProjectileImpacts`). Other projectiles and unaudited callers retain original
behavior. Tracking is bounded and scoped to the native load generation.

The fixed native operation `8293D8A0` takes a Projectile component, requests
retirement, performs area effects and creates impact FX. Native lookup
`828BA0C0` confirms that this component's entity interface is at +20, while
its owner is at +60; treating it as a generic component at +8/+48 is wrong.
The hook verifies that original typed binding and only suppresses local guest
calls at audited void-return callers. No local counter or pending-shot array
is cleared by the impact adapter.

Room wire 22 transports bounded impact records: projectile GUID, exact host
position bits, native RNG inputs/end state, callback/queue mode and a bounded
ordered list of FX clone template IDs, generated IDs, flags and RNG states.
Host requests remain native. The guest resolves its own live entity, sets the
impact position through the original setter, and calls original `8293D8A0` on
a preserved borrowed stack. Damage from local area queries is suppressed;
the exact host Damage payload was delivered separately. Native FX clone
inputs are validated and seeded at the original clone boundary, with exact
generated-ID and post-state checks. The original retirement queue is read
back. The host operation's end RNG is committed through original srand because
queries on already-painted geometry can consume different local random draws.
Native pointers, collision objects and callback addresses are never sent.

The codec passes 98 truncation, identity, float, clone and malformed-data checks.
The Room suite passes 9,087 checks, including maximum impact records alongside
maximum damage, AI, character and spawn data over fragmented real TCP. The
existing 1 MiB authority and 2 MiB queue budgets still cover the full maximum.
Evidence:
`analysis/native-projectile-impact-test-73464b6ff2e248808a10521fa4fdf92e/rollback.log`.

The live run matches four host/guest impacts at ticks 1471, 1708, 1985 and 2229.
Each checks one canonical FX clone and a queued native retirement. Native
gameplay passes the previous 1990 mismatch and is observed through tick 2200.
It stops at 2252 because the guest still has unlaunched projectiles when the
host requests another preparation. Both owned windows close normally and leave
normal user files unchanged. This remains a failed full integration with
passed impact cases. Evidence:
`analysis/private-native-room-pair-20261115-110057-92b2979c/native-impact-integration-result.json`.
Live executable SHA256:
`C5135C2BF78440B5CBF7D9D6A2C117CB0B410D95BFF6F08FEA810F9A4859E7BF`.

## Pending launch is a separate native operation

Earlier logs/results called Shooter +80/+84 an active-shot list. The source
audit now establishes that it holds pending launches. Original `828B92A8`
prepares entities and appends their handles; that is not complete firing.
Original `828B8DD0` iterates these handles, resolves each local entity, invokes
`828B81D8` with its ordinal, and then clears +84 itself. Its success result is
used by native AI/script callers. Historical raw logs and JSON keys retain
their old label; their readback proves pending registration, not launch.

The new source corrects diagnostic wording. The launch processor has not yet
been replicated, and it must not be called blindly at preparation time: host
windup/animation can intentionally separate preparation and launch. Correct
launch timing and inputs, continuing projectile motion, general world births,
same-tick causal ordering and broader multiplayer transitions remain required.

## GUID mutation fence fixed

Cloning assigns a new GUID after the entity is registered. Original
`82517E50` writes the four words at BEntity +4/+8/+12/+16. Previously the native
identity revision observed insertion/removal/teardown but not this setter,
allowing an older identity observation to retain the same revision after a
native GUID change.

The wrapper now fences that audited setter exactly like registry mutation.
Known EM1 preview callers also use the fenced entry instead of bypassing it.
The original native setter still performs all writes; identities are not
renamed to work around the missing-spawn failure. This is not coverage of
arbitrary component/property writes.

The same-executable regression compares the real original setter against the
wrapped call, requiring byte-identical guest memory and complete PPC-context
effects while the revision advances. The lifetime fixture also checks all
five forwarding hooks, nested mutation, exceptions and cross-thread fencing.
Damage/codec and Room fixtures pass 16, 156 and 9,078 checks respectively.
Evidence: `analysis/native-guid-fence-test-0b9fe52f74074dbb9970c88dfb2799c7/`.

A separate two-window smoke passes through tick 2000, including original
death/respawn at tick 180 and suppression of guest-only fatal damage at 360.
It verifies the fence change in that bounded run, not general spawn authority.
Evidence:
`analysis/private-native-room-pair-20261115-095016-ffd77b90/guid-fence-smoke-result.json`.
Executable SHA256:
`E93EDFC0E060C095E878509E6C334C5971E1C0DD0F0FB96616D88B0D447BB2F7`.
