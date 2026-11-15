# Host spawner lifetime authority

## Reproduction with three actual native worlds

`start-private-room-roster.ps1` now has a coherent optional `-HostAuthority`
preset matching the two-peer test path: host correction, Damage, AI decisions,
spawn eligibility/replay, confirmed retirements, projectile impacts and launches,
with eight-update credit. Optional level routes are forwarded to every peer.
The default launcher path remains unchanged. Its owned-process error cleanup
also uses the intended two-second creation-time tolerance, rather than two
milliseconds, while retaining executable/path/PID/unique-root checks.

Three-player pair `analysis/private-native-room-roster-20261115-150605-f3976da7`
stopped at 322 before the first scheduled level change. The authored spawner
00151A87/00000000/00000000/00000000 was removed locally at 321, but the host
used it to create break dust and barrel-break FX at 322 before retiring it.
The missing-birth replay could no longer resolve its TemplateSpawner component.

A trace repeat, roster
`analysis/private-native-room-roster-20261115-151037-19b1ed59`, identifies
original HealthComponent request return site 827CFC44 on both guests at 321
and on the host at 322. The native request is `82744E90`; the Health caller
discards its return before loading r3 for its next operation. This is early
native lifetime divergence, not a missing player or an Internet relay timeout.

## Change

Room wire 26 appends a sorted, bounded inventory of up to 1024 live native
spawner GUIDs to completed host authority state. It rejects null, duplicate,
unordered, oversized and simultaneously retired/live IDs. Maximum combined
Damage/birth/impact/launch/retirement/inventory state stays within the existing
1 MiB authority envelope and 2 MiB queue bound.

The host filters native registry component-mask index 40 and validates the
actual TemplateSpawner component type before publishing its ID. Human actors
are excluded. The local authority inventory tracks these owners, plus captured
birth owners, so their confirmed absence produces an ordinary retirement command.
No native addresses travel in this inventory.

On the guest, mapped Health and GUID DestroyEntity request sites can defer
teardown of a verified non-human spawner while the host still lists it alive,
or while a host retirement still depends on an unconsumed birth. Both registry
and world/tick ownership must match. Other destruction callers remain native.
The adapter neither writes reference counts nor forces arbitrary object pointers.

Retirement now waits for pending owner, anchor AND child dependencies. Missing
birth replay runs first; the existing original teardown/queue/zero-delta flush
then retires the owner and verifies registry absence. Human references cannot
alias non-human dependencies. The native request trace is private-only and
preserves the caller's host FP/error environment.

The initial Health-only candidate repaired 322 but exposed an early GUID-based
DestroyEntity request for a dynamic ticket spawner (827452A8). That mapped path
was added to the same lifetime rule. This does not claim that all native
teardown callbacks or script continuations are now replicated.

## Verification and remaining failure

Final candidate SHA-256:
`F824D1E05B11F884D699FE2EA0889FF14827D02181A7BED28D345089EC533AF9`.
The same executable's regression exits 0 with 186 spawn/dependency checks and
9104 Room checks, including the maximum inventory alongside all other maximum
authority payloads over real TCP. Malformed inventories cannot consume the
publication sequence. Evidence:
`analysis/native-spawner-guid-lifetime-test-result.json` and its referenced log.

Roster `analysis/private-native-room-roster-20261115-151926-f00e1872` has three
actual game processes and mixed native human characters. Both guests now defer
00151A87's early Health teardown at 321, reproduce both host children at 322
with complete parent/ordered output checks, then perform the original retirement
request and verify registry absence. This repairs the original missing-owner
failure without renaming entities or ignoring the mismatch.

The continued run still fails at 360. The host's native ticket factory created
`eticket_e_parentable` (template 000636D0) at 322 with GUID
00007720/00007B2F/0000602F/00007656. The guest independently created that template
at 321 with GUID 00003064/0000436A/0000585B/0000546A. This factory operation is
outside the captured TemplateSpawner/impact paths. Its later host collection
birth cannot resolve the absent canonical owner. A separate matching ticket
spawner is retained by the lifetime guard through 360, but this does not repair
the unmatched factory birth. Pickup creation/initialization and earlier death
callbacks remain the next causal replication work; GUIDs are not blindly changed.

All three owned windows close normally, exit 0, with normal user files unchanged.
The roster's `spawner-lifetime-result.json` retains the repaired operations,
contrasting factory births, subsequent failure and shutdown evidence. The
scheduled Gulch/Autotopia route was NOT reached. This is not complete enemy,
quest, pickup or campaign synchronization, native rollback, or sixteen-player
campaign acceptance. The adapters remain private/default-off and no public
binary release accompanies this source update.
