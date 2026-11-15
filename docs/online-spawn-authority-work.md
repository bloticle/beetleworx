# Native spawn authority development

The current private authority adapter still stops when a host Damage target
does not exist on the guest. Spawn replication is not implemented yet. This
work identifies the failing native path and closes an identity-fence gap;
it does not claim a playable server-authoritative world or a new demo release.

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
a candidate for eligibility suppression; the current adapter does not yet
skip it. Low-level clone suppression remains unsafe at other callers that
dereference the result unconditionally.

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
