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

Room wire 28 carries at most 32 bounded death-reward commands per authority update.
Payload version 2 has a portable owner GUID, native event reason, before/after
scalar words +76/+84/+88, stable GUIDs for the local +80 parent pointer, CRT
random states, and ordered prototype/clone GUIDs with clone flags and random states. No component pointer, native handle, callable address,
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

Initial prototype executable SHA-256:
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


## Borrowed-stack event lifetime correction

The diagnostic repeat in roster
`analysis/private-native-room-roster-20261115-154328-e3f8fc40` narrows the failure
to the ordered clone list: the original callback does set its handled flag but
creates no matching drops. All three sessions close normally with normal user
files unchanged.

A machine-code audit finds a concrete adapter bug. The old adapter staged its
20-byte event at original SP -128, then called native srand while still using
that original SP. Srand allocates 96 bytes; its PTD helper saves r31 another 16
bytes below that, exactly original SP -112: the staged event reason at +16.
The upper word of srand's zero-extended seed replaces the intended reason 2
with zero. Restoring the PPC register context does not undo native stack stores.
Audit: `analysis/native-death-reward-stack-audit.json` checks the original
instructions at 82B6DDA4, 82B6FFF0 and 82B6FFF4 against the source image.

The adapter now establishes the lower borrowed frame before seeding and stages
the event after that native call. Subsequent original callback frames grow below
that lower frame, away from the event. The final stack backup still restores
all borrowed bytes. Same-executable regression exits 0 with unchanged codec and
transport coverage; evidence is `analysis/native-death-reward-stack-test-result.json`.
Candidate SHA-256:
`6D8C249BA3998EA9F811E9CCCF7C366439042FFDF5FBD503A93FB280307BCA49`.
The following capture verifies original clone execution but exposes a separate
parent-pointer serialization error, described below. It is not a full route pass.


## Portable parent binding

The stack-corrected roster
`analysis/private-native-room-roster-20261115-154604-03f707da` reproduces all
three ordered host clone GUIDs at 322 on both guests, passes the handled flag
and exact end-RNG checks, then fails post-state equality. Auditing original
finalization 82AA69F0 establishes that component +80 holds the BEntity pointer
returned by original registry instantiate, not a scalar counter. The earlier
payload incorrectly treated that word as portable state. That private prototype
could neither compare parent bindings across processes nor truthfully exclude
all native addresses from its payload.

Payload version 2 replaces that word with a registry-resolved stable parent GUID.
Only +76/+84/+88 remain scalars. The after-parent must bind one of this operation's
verified clones, while null is supported for zero-birth operations. The native
adapter reads the actual local pointer, resolves its entity GUID and compares
that identity; it never installs a remote pointer or skips the parent check.
Room wire 28 rejects the earlier payload layout. Native acceptance is pending.


The portable-parent candidate is
`601950DD6F3DC6A6684F4C9A6447786C95E22790DC1EB70E784C372E70C9C1D4`.
Same-executable regression exits 0 with 147 reward codec checks and 9108 Room
transport checks, including maximum reward parent identities in the real-TCP
maximum authority batch. Evidence:
`analysis/native-death-reward-parent-test-result.json`. Original pointer-layout
audit: `analysis/native-death-reward-parent-audit.json`. Native capture results for this candidate follow.


## Native result: reward repaired, particle cleanup remains

Roster `analysis/private-native-room-roster-20261115-155109-dd9f0270` runs
three actual native game worlds with mixed human characters and synthetic SDK
inputs, using the portable-parent candidate above. Both guests successfully
replay owner 00151A87's original reward at tick 322: all three ordered clone
GUIDs, one-shot flag, scalar state, portable parent and exact end RNG agree.
The canonical ticket parent is now present at 360. The entire host collection
birth batch matches on both guests, and both tick-360 parent removals verify
registry absence. The separate ticket owner 00005D56/00006BC4/00007D06/00004B51
survives its early guest removal requests, reproduces its complete host birth
batch at 361, then retires through original native cleanup with registry absence
verified. This repairs the specific 360 mismatch described in the spawner notes.

The run continues through verified tick 421, then stops at 422: dust FX
00006110/00001102/00007A4C/000011BF remains registered after original zero-delta
retirement flush. Read-only captures on both stopped guests show seven component
types. Six readiness methods are original constant-true 82F1BF10. The remaining
ParticleSystemControllerComponent uses original 828000A8, a direct read of byte
+80 from its component interface; that byte is zero on both guests. Its actual
completion/update path still needs tracing. No readiness bypass, force-all queue
flush, direct registry removal or arbitrary timer advance was introduced.

Evidence: the roster's `portable-parent-result.json`, each guest's
`retirement-components.json`, and extracted original readiness functions.
All three windows close normally, exit 0, with normal user saves/settings
unchanged. The scheduled Gulch/Autotopia route is NOT reached. Full enemy,
quest, effect and campaign synchronization remain incomplete; this adapter stays
private/default-off. Demo.15 remains the earlier public build.
