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
tag +76, and the type/raw/effective arrays +80/+92/+104.

The `BWDE` version-1 codec is bounded to 1,024 bytes, sixteen stimuli and a
255-byte tag. It rejects invalid identities, booleans, unsupported stimulus
kinds, nonfinite floats, embedded tag NULs, truncation and trailing bytes.
Capture and decode leave their destination unchanged on failure. Capture is
read-only and restores host FP/error state. Nonempty auxiliary arrays at +64
are explicitly unsupported until their semantics are mapped; their words are
not assumed portable.

The existing private correction probe captures a bounded diagnostic window
around its requested native damage tick, encodes/decodes each record and logs
GUIDs and sizes. Normal public gameplay does not perform this diagnostic.
These records are **not yet sent or replayed**. No new gameplay release is
claimed for capture/codec work alone.

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

## Next integration

1. Deliver bounded, ordered host event batches on the authenticated Room
   connection, with exact world, roster and completed-tick ownership. A final
   authority result must certify the complete event batch; neither a partial
   batch nor receipt alone may acknowledge native completion.
2. Resolve GUIDs freshly on the guest and reconstruct original native event
   storage with native allocation/refcount ownership. Replay exact adjusted
   amounts and metadata once before the guest update, without duplicate local
   effects or leaking the native replay authorization scope.
3. Admit all affected NPC/prop events before extending local suppression beyond
   human players. Handle missing/new targets, spawn generations, destruction,
   drops, scripts and pending unload safely. General lifecycle replication and
   host-owned enemy decisions remain necessary for complete campaign co-op.
