# Native death-reward authority work

This remains private experimental work. Public demo enemy synchronization is
incomplete; this change does not establish campaign-wide combat authority or
native prediction/rollback.

## Reproduced cause

The three-native-world roster recorded in `online-spawner-lifetime-work.md`
failed at tick 360 because a ticket parent had different host/guest GUIDs.
The host issued it at 322; the guest issued it at 321. The original factory
chain is DeathReward event callback 82AA86E8 -> distribution 82AA8160 ->
kind selection 82AA7EF8 -> ticket creation 82AA7B48 -> registry instantiate
82743AD8 -> template clone 827B9960. Replaying a generic template clone omits
the original property selection, transform, initialization and parent setup.
Suppressing only distribution is also insufficient: the outer callback sets
component byte +73, consuming its one-shot reward even if distribution is skipped.

## Implementation

Room wire 27 adds at most 32 bounded death-reward commands per authority update.
Each has a portable owner GUID, native event reason, before/after component
words +76..+88, CRT random states, and ordered prototype/clone GUIDs with clone
flags and random states. No component pointer, native handle, callable address,
or raw native event object travels over the network. Null/duplicate owners,
malformed payloads and clone collisions across command types are rejected.

The adapter binds a real DeathRewardComponent and its native registry owner.
Only non-human callbacks in an agreed private full-authority update are owned.
Guests suppress the entire original void callback, preserving its one-shot flag.
At the host tick, replay resolves the local component/handle, reconstructs only
the audited scalar event fields on a backed-up borrowed stack, and invokes the
original callback. Original distribution and ticket construction remain intact.
Readback checks pre-state, handled flag, all clone inputs/results, final component
state and exact end RNG. No GUID rename or blind counter overwrite is used.
The caller PPC context, host FP/error state and borrowed stack are restored.

Original callback reads are event type +4 (u16, 16), target handle +8, and
scalar reason +16. Distribution has exactly one event-register dereference,
+16. The local original-function audit is
`analysis/native-death-reward-original-audit.json`, tied to image SHA-256
7bf582d9dc1525b20cf20d20917c23c697144c976c727d353226b7c0a68a663c.

The live birth-owner inventory now includes typed DeathReward components
(canonical registration index 117), so mapped early teardown can wait for the
host reward callback. Commands execute before missing collection births and
confirmed retirements at completion. This does not replicate every death Lua
continuation or every pickup path.

## Verification

Final candidate executable SHA-256:
`C888D4E90609B722BC4E838A7D46A3C91622A6078F58CEDE03F001C5D899078B`.
Same-executable regression exits 0: 121 death-reward codec checks and 9108 Room
transport checks. The full maximum authority batch includes 32 maximum reward
commands alongside the existing maximum damage, spawns, impacts, launches and
inventories. Real TCP reconstructs the complete batch exactly. Evidence:
`analysis/native-death-reward-test-result.json` and its referenced log.

Three-native-world roster
`analysis/private-native-room-roster-20261115-153637-e5905d15` captures owner
00151A87 at host tick 322, reason 2, with three original clones: scrapmetal_nif,
5xdistribution, and eticket_e_parentable. Both guests suppress the early 321
factory birth, but the subsequent original callback replay fails its combined
clone/handled/RNG check at 322 and creates none of those clones. This is NOT a
successful native reward replay. The final build separates those failure
reasons for the next capture; it has not repeated that live run. The typed native
binding, original distribution eligibility and event timing need further work.
The original 360 failure is not accepted as repaired by stopping earlier.

All three owned sessions close normally, exit 0, with normal user files
unchanged. Evidence: that roster's `death-reward-result.json`. These adapters
remain private/default-off in demo.15. Public online still uses the existing
host corrections; no claim of complete enemy sync accompanies this work.
