# Native damage event replication work

Demo 14 corrects recognized NPC motion/health and gives the host authority over
player Damage events. It does not own enemy AI, spawning or complete combat.
Reading health after an update also misses a target removed by that update.
The next replication path therefore needs the original host event before its
native listeners run, with stable identities and exact adjusted amounts.

## Implemented capture and codec

`native_damage_event.cpp` captures the retail Damage object, class `82076C70`,
type 14, before dispatch. It resolves target and optional instigator handles to
the BEntity four-word GUID. Native registry hashing is reproduced from
`827B58D0` / `8272F940`, including signed high-byte input; a focused fixture
compares it to the original compiled native function. The registry lifecycle
revision fences capture against concurrent insertion/removal or teardown.

The portable `DamageEvent` holds only GUIDs, two native booleans, eight vector
float words, stimulus kinds with separate raw and effective amount words, and
the native tag bytes. Local handles, pointers, capacities and lifecycle counters
are excluded. Adjusted amounts are preserved rather than recalculated locally.
Native event fields used are handles +8/+12, flags +16/+17, vectors +32/+48,
child shape keys +64, tag +76, and the type/raw/effective arrays +80/+92/+104.

The current `BWDE` version-3 codec is bounded to 1,024 bytes, sixteen stimuli,
96 child shape keys and a 255-byte tag. It rejects invalid identities, booleans, unsupported stimulus
kinds, nonfinite floats, embedded tag NULs, truncation and trailing bytes.
Capture and decode leave their destination unchanged on failure. Capture is
read-only and restores host FP/error state. The array at +64 holds collision child shape keys; collidable pointers are
resolved to entity GUIDs separately and are never transmitted.

The existing private correction probe captures a bounded diagnostic window
around its requested native damage tick, encodes/decodes each record and logs
GUIDs and sizes. Normal public gameplay does not perform this diagnostic.
The development build now sends and replays causal root events behind private
probe flags. These flags default off; this is not enabled in the published
demo. No new gameplay release is claimed for this integration alone.

## Evidence

The same game executable's `--em2_test_native_damage=true` mode passes twelve
native capture checks and 121 codec checks, including the original hash,
signed-zero/raw/effective bit preservation, maximum payload, every truncated
prefix, malformed values, missing handles, invalid entity pointers/vtables,
unavailable arrays/tags and unsupported auxiliary payloads.

Final focused-test executable SHA256:
`62DF63A04C8AC07DAC03DC82821A9E01D42F1BDACA228F6BE5C6BEFD980BDF02`.
Log: `analysis/native-damage-test-3dc4d9741c5f46f99f21a28fce7203df/game.log`.

The earlier native gameplay capture used SHA256
`4B90C2BFEE1495F492436D45D7960A71A04C7F860EB9ECA06ACAFA1338CD1AFC`,
before adding the extra entity-vtable/alignment rejection. Its result is
`analysis/private-native-room-pair-20261115-075332-90398633/portable-damage-result.json`:
28 captured objects on each peer round-tripped exactly, with no unsupported
records in the sampled window. Both sets contained the same ten target GUIDs.
This checks per-record preservation and target identity, not equality of full
event payloads across peers or any network delivery. The guest's local player
damage remained suppressed by the existing authority policy; capture is before
that guard. Other observed paint Damage broadcasts retained native dispatch.
Both processes exited normally and normal user files were unchanged.

## Development transport and native replay

Authority wire version 16 carries up to 128 ordered Damage records in the final
completed-tick state. Only authenticated authority frames may use the expanded
128 KiB frame bound; other messages retain the existing 8 KiB limit. The sender
preflights every peer queue and retries bounded backpressure without advancing
publication or rerunning the native update. Partial records never expose a
partial batch or certify guest completion.

The actual TCP transport fixture passes 9,071 checks, including a maximum batch
larger than 8 KiB, fragmented delivery, invalid records, queue pressure, exact
payload/order preservation and explicit post-update acknowledgement. Evidence:
`analysis/authority-damage-transport-test-577a878108d44306871e9e85e6ad4c5f/game.log`.

The guest resolves GUIDs to its own live handles and reconstructs native Damage
arrays and interned tags using the original allocator/refcount functions. It
recaptures the reconstructed object and requires exact portable equality before
dispatch through original listeners. It does not call AddStimulus (which can
draw random secondary damage) or CalculateDamage (which would apply local
resistance again). A scoped replay authorization permits the host event while
local duplicate Damage is suppressed within the private probe's scope.
Nested events caused by a captured root retain native dispatch but are not also
transmitted as roots, avoiding replay of the same causal children twice.

The player-only live pair replayed host tick 180's one-point Damage through
native guest listeners: health changed to four with zero numeric-health
corrections. It continued past tick 429 without a reported failure. Evidence:
`analysis/private-native-room-pair-20261115-081437-15196bc3/native-damage-replay-result.json`.

The broader world probe replayed **7,487 events in 915 batches**, last tick 955,
including native death/respawn, then stopped on an unsupported nonempty
auxiliary array. This is a **failed integration run**, not a full combat pass.
Both owned windows exited normally and normal user files were unchanged.
Evidence:
`analysis/private-native-room-pair-20261115-081841-bd3ccef4/world-damage-result.json`.

## Remaining for complete enemy synchronization

1. Validate child-shape replay in live gameplay and exercise larger collision
   assets against the bounded payload. The native producer mapping below is
   implemented, but does not itself prove live compatibility.
2. Handle targeted listeners, missing/new targets, spawn generations,
   destruction, drops, scripts and pending unload. A captured event alone does
   not establish entity lifecycle ownership.
3. Move enemy decisions and spawning under host authority. Enemy AI and scripts
   still run independently. Snapshot correction plus Damage replay does not
   establish full campaign synchronization, prediction or rollback.

## Child-shape mapping after the failed world probe

A second diagnostic run replayed 15,047 events in 1,755 batches through tick
1795, then failed at tick 1796 with caller `8295C4FC` and 43 auxiliary words.
This remains a failed run; its executable did not contain child-key support.
Evidence: `analysis/private-native-room-pair-20261115-082900-140d169e/world-damage-result.json`.

`8295C318` constructs Damage, copying its third argument's POD array to the
Damage object's +64 array using `8321D858`. Its collision-query caller groups native
collision rows by entity: the collidable pointer at row +32 goes through
`827D2B58` to resolve the owning entity, while row +36 is appended separately
through `827985A8`. This is the same child-shape-key field used by the native
sweep result in `native_player_spawn.cpp`, not the collidable address.
Independently, `828C07C8` appends the scalar key output of `82946628`; that
function returns the shape pointer separately in r3 and writes the child key
through its seventh argument. These paths distinguish asset child keys from
process-local body pointers.

The new capture retains ordered key bits, including the all-ones sentinel.
Replay appends them with original native POD-array allocator `827985A8` and
includes them in the mandatory recapture/equality check before dispatch.
Codec and transport maximum-size fixtures now include maximum child-key arrays.
Live validation of this revision is pending.

## Current focused checks

The child-key revision passes 15 native capture checks and 144 codec checks:
`analysis/native-damage-shape-test-9fd0f226035a436fa08f698ed6f17698/game.log`.
That focused capture/codec executable is
`5BD51B89E763402086180C02FEF9F537C3ED06802823B079D52E20B77DF52643`.

After adding key-count observation and separating the changed protocol as wire
15, the final executable
`82A9626F1275B6A118F288B4B9933D6B21D5CC165630E6D7E9A52FD611CC2B1B`
passes all 9,071 Room checks with maximum child-key arrays included in each
maximum-size authority batch:
`analysis/authority-shape-wire15-test-4a3c1df56ed643f08413883a648c8699/game.log`.
Neither focused result alone proves native gameplay synchronization.

## Nullable tag compatibility

The first child-key gameplay run accepted a 73-key array at host tick 1215,
then failed reading the event tag before publication. No nonempty child-key
batch was replayed in that failed run. Evidence:
`analysis/private-native-room-pair-20261115-083621-58811c5d/world-damage-result.json`.

Native string assignment `825048A0` copies its source's nullable reference;
reference increment/release operate separately. The portable record now retains
an explicit tag-presence flag, distinguishing a null reference from an interned
empty string. Capture never reads address zero for a null tag. Replay releases
the constructor reference before restoring null. Nonzero unreadable references
remain errors. Wire version 16 and BWDE version 3 distinguish this layout.
This revision still requires focused and live validation.

The nullable-tag revision passes 16 native capture and 149 portable codec
checks, including invalid presence flags and contradictory absent/nonempty
payload rejection. Replay additionally requires that a null reference matches
the engine's canonical empty reference at `8203D278`, which native retain and
release explicitly exempt. It does not assume every image can release null.
Executable: `097BE8E5CE75886FC9C88E10286DDE2BC81C963D3597BD3CA2E2576109FBFAD9`.
Evidence: `analysis/native-damage-null-tag-test-d38af929742d43aca9e2b7a008b8d1e5/game.log`.

## Live payload compatibility result

The nullable-tag build's actual TCP Room regression passes 9,071 checks:
`analysis/authority-null-tag-test-e3c7283a96bd432ea6883067ab5075f1/game.log`.

The two-window native gameplay run successfully replays a batch with 326 child
shape keys across 21 events at tick 917, through original guest listeners after
exact native payload recapture. It continues through tick 1271, then stops on a
host Damage target with no matching live guest GUID. Thus **the payload case
passes, but the overall world integration run fails**. Evidence:
`analysis/private-native-room-pair-20261115-084210-0d01f6a4/world-damage-result.json`.
Both owned processes close normally and normal user files are unchanged.

Entity lifecycle divergence is now the observed next blocker. Missing identities
are not guessed, retargeted or silently ignored. Failure diagnostics now log the
missing GUID and local snapshot revision to distinguish host-only creation from
guest-only destruction in the next capture. No new gameplay release is claimed;
normal public sessions still use the published correction policy, and the new
world-Damage replication remains behind private flags.
