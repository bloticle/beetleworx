# Native entity retirement authority (private)

## Root cause

A selector-only candidate allowed guest calls with no remaining host command.
The resulting trace exposed a lifecycle mismatch: Floyd GUID
`00001B1C/00006BC0/00003AA0/00005D8C` was removed from the host registry at
2581, while the guest still contained it in its last snapshot at 4260. Both
had constructed the same GUID at 880. Suppressing those selectors alone would
leave a ghost NPC. That broad suppression candidate was rejected.

Evidence:
`analysis/private-native-room-pair-20261115-122544-8990f3a3/npc-retirement-root-cause.json`.
The later AI owner/order failure in that run is recorded separately; it is not
proof that every earlier AI mismatch had the same cause.

## Protocol and native replay

Room wire 24 adds up to 256 retired entity GUIDs per completed authority update.
They must be nonzero, unique, and disjoint from current character samples.
Bounds include the full maximum list alongside maximum actors, Damage, AI,
spawn, impact and launch data. No local pointer or callable address travels.

Default-off `em2_probe_room_entity_retirements` tracks admitted world characters,
AI owners and captured collection children. The host compares that inventory
against its complete native entity registry after the original update. Only a
confirmed absence produces a retirement; body activation or a rotating pose
batch is not used as evidence of deletion. Old world scope is discarded on
native rebootstrap. Private launchers expose `-EntityRetirements` with replay
and host-correction prerequisites.

The guest requires an existing authority identity, resolves its own registry
entity and protects the admitted human actors. It invokes original `82744E90`
(teardown/queue) and `827437B0` (deferred cleanup), retaining the lifetime fence.
An extra completed-boundary cleanup supplies r4=false and dt=0: it honors native
readiness and does not advance timers a second time. Exact registry absence is
required afterward; an entity already removed locally is a separate no-op case.
Caller PPC context, borrowed stack, host FP state and last-error are restored.
A queued human, invalid registry, changed world or incomplete cleanup fails.

A selector without a matching host decision is skipped only for an explicitly
retired, authority-tracked entity in the current packet. Other unknown or
out-of-order selectors still fail. The retirement passes bracket missing births
so a same-tick birth can be reconstructed before its removal. Original NPC
teardown callbacks and collision-helper cleanup remain native.

## Limits

This is confirmed-absence replication, not a capture of original request order,
destruction RNG, every factory or a complete causal event journal. Early guest
retirement is not yet prevented. Mid-update spawn/retirement dependencies,
quest/script callbacks, all destruction causes and longer campaigns remain
unverified. The native helper currently rejects a cleanup that cannot confirm
absence immediately rather than pretending it completed. The feature is not
enabled in the published demo.
## Verification (15 November 2026)

The isolated pair `analysis/private-native-room-pair-20261115-124321-536cba3c`
ran the private adapters with collection-drift injection, full entity traces,
AI decisions, Damage, native collection replay, projectile impacts and launch
replay. It entered `meanstreet_north.visit_2` by a clean native load at tick 700.
The last complete observations before requested normal shutdown were host 8858
and guest 8856. This was not an authored campaign transition or combat sweep.

Seven retirements required an original guest teardown request, at ticks 2753,
3094, 3835, 4307, 5379, 5576 and 7601; each confirmed local registry absence.
At 2753, Annette GUID `000036F8/00005CD0/00003E80/00005DBB` has matching host
and guest native remove records in the same tick. Already-absent no-op commands
are excluded from that seven. No native Room stop, AI owner/order mismatch,
spawn replay failure or retirement replay failure was found in the selected
log diagnostics. Filesystem/material lookup warnings and shutdown renderer
messages still exist; this is not an assertion that every subsystem is clean.

Both owned processes exited normally with code 0 and the launcher verified
normal user files unchanged. Raw evidence and selected diagnostics are in
`native-retirement-result.json` inside that pair directory (local test artifacts,
not distributed game data).

The live executable SHA-256 was
`FCDE88D6715DD17435841F5C56B099686D566FE1E555323B3EBBB768F3AA09D0`.
The final build changes only the retirement diagnostic to say “registry absence
verified,” which also describes already-absent no-ops accurately. The final
canonical executable again passed all 9095 Online Room checks, including the
maximum retirement payload over fragmented real TCP and malformed/null/duplicate/
current-character-overlap rejection. The test log is
`analysis/native-retirement-final-0a2f19b5fdba44a8bd6bb030b3f29663/rollback.log`.
These transport checks do not exercise native teardown or prove rollback.
