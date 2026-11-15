# Native particle retirement drain

This is private authority work, not a public enemy-sync or rollback completion
claim. The reward path is covered in `online-death-reward-work.md`.

## Reproduced failure

The three-native-world portable-parent run reaches verified tick 421, then stops
at 422 because dust FX 00006110/00001102/00007A4C/000011BF remains registered
on the guests after original retirement flush. Host removal is already complete.
Read-only captures bind its actual seven native components. Six readiness leaves
return constant true (82F1BF10); ParticleSystemController uses 828000A8, which
reads byte +80 from the component interface. It is false on both guests.

Original update 828012A8 sets that flag from original completion poll 82800178.
The poll checks the native particle systems and includes a 90-check timeout.
Local presentation timing and visual random streams do not guarantee the same
particle drain moment on every peer. A single host-tick registry-absence demand
therefore mistakes pending cosmetic cleanup for failed gameplay replication.
Original machine-code audit: `analysis/native-particle-retirement-original-audit.json`.

## Implementation

After the ordinary native request and zero-delta flush, only an already-queued
entity with the exact mapped visual-only seven-component shape can be reported
as pending cleanup. The accepted components are InputOutput, Lifetime, LOD,
ParticleSystemController, GameEvents, JPSSceneGraph and JPSTransformation.
Every component owner link, primary/interface vtable, uniqueness, native readiness
method and the exact leaf instructions must match. A human target, physics,
health, AI, script, spawner, pickup, damage or unknown component fails admission.
The particle readiness byte must be a valid boolean; initial deferral requires
it to be false. Other entities still require immediate confirmed native absence.

Pending cleanup retains its actual local entity address and handle alongside its
stable GUID; those addresses never travel in network state. Each active update
observes the native registry once for the pending set. Changed identity, queue,
component ownership or native shape stops the adapter. Only real absence removes
the pending entry and authoritative inventory ID. Ready-but-still-queued effects
can finish in the following original cleanup phase. The list is bounded to 1024
entries and 120 active updates per entry; passive/Game Over/cutscene intervals
do not spend that active-update budget. Fresh-world admission resets the list.

No native refcount writes, readiness override, force-all cleanup, fabricated
registry removal, particle counter adjustment or timer advance is used. Original
native updates and finalization own the drain and all callbacks. This does not
make the original cosmetic particle simulation deterministic or establish that
every FX layout is handled.

## Verification

Candidate SHA-256:
`070A3955A4B6ABDCD40F4E9D8E83D3727A983179E4400ACA21F58D7772857D84`.
Same-executable regression exits 0, including 18 bounded native shape/ownership/
readiness/mutation checks, 147 reward codec checks and 9108 Room transport checks.
The visual guard fixture validates actual mapped native-layout reads and memory/
host environment preservation. It does not simulate native particle completion.
Evidence: `analysis/native-particle-retirement-test-result.json` and its log.
Native roster
`analysis/private-native-room-roster-20261115-160325-e130fa41` reproduces the
pending dust FX on both guests at 422 and verifies its normal registry removal
at 424 (one intervening active-update observation). No forced cleanup or timer
advance occurs. All peers continue through verified tick 699 and enter Gulch at
700, then reset into fresh native admission at 701. This repairs the reproduced
particle retirement failure and exercises the pending-state world-reset boundary.

The run stops on a separate new-world bootstrap proof mismatch at 701. Logged
world names, retail positions, life/ammo, all three character positions/velocities
and random trace agree. The extra human GUID agrees; the two retail human GUIDs
differ between host and guests. That creation/comparison path needs further
investigation; no proof digest was ignored or replaced. Autotopia is NOT reached.
This is not a passed three-world route or full campaign synchronization.

Evidence: the roster's `particle-drain-result.json`, including observations,
character identities, errors and shutdown records. All three owned sessions
close normally, exit 0, with normal user saves/settings unchanged. Public demo.15
is unchanged; these adapters remain private/default-off.
