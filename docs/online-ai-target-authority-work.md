# Native AI target authority development

This is a private development adapter, disabled in normal/demo sessions.
It gives the host ownership of one audited AI decision: the native brain's
target-selection call. Enemy behavior graphs, attacks, scripts and spawning
are not yet fully host-owned. This is not a campaign co-op completion claim.

## Native scope

`827F3230` performs target selection and calls `827F2FA8` to assign/clear the
target. The observed caller is `827EC3E8`, inside the normal brain update.
That caller does not read a return value from the selector. It continues its
own update after the selector returns.

The selector's direct brain write is the scalar ordinal at +184. The source
initializes the ordinal to zero and increments it while traversing the
selector inventory at +204/+208. On the final path, it writes +184 immediately
before calling the target setter at `827F361C`. The earlier clear-target path
calls the setter at `827F3350` without writing the ordinal. Early returns may
call neither. The adapter distinguishes all three outputs rather than issuing
an extra target-change callback every frame indiscriminately.

The owning BEntity interface is `*(brain + 60) + 8`. Its full four-word native
GUID identifies a non-human owner. Admitted human owners are excluded through
the current native roster, including extra humans. Player targets use scoped
lobby peers; world targets use GUIDs. Local pointers and handles are resolved
afresh on the guest and are not transmitted.

## Ordered host commands

Room wire version 19 adds up to 128 ordered AI decision records to the final
completed-update authority result. Each record contains owner GUID, optional
world/player target, whether the native setter ran, and whether the scalar
selector ordinal was written. Invalid booleans, ambiguous target forms,
out-of-roster peers, invalid owners and oversized ordinals/batches are rejected.
Existing world/roster/tick fencing and publication backpressure still apply.

The host reserves each command in selector-entry order and records its result
after the original selector returns. Unsupported multiple setter calls or
unknown native setter sites stop the private adapter rather than inventing a
portable interpretation.

At the guest's corresponding native selector call, the adapter consumes the
matching host command and skips independent target selection. It applies the
scalar ordinal only when the host wrote it, bounded against the guest's actual
inventory, then invokes the original native target setter when requested.
The resulting native target handle must match the freshly resolved local
target. Other brain/character updates continue normally. Completion requires
consuming the entire ordered command batch before acknowledgement; missing,
extra or reordered owner calls stop the adapter.

`em2_probe_room_ai_targets` enables this only with `em2_private_profile` and
host correction mode. `em2_trace_room_ai_decisions` is a separate observation
flag. `em2_probe_room_ai_target_drift` is a host-only QA fixture: during ticks
120-179 it requests native player 1 as the NPC target. It is disabled normally.
The guest does not execute that fixture; it receives the resulting command.

The private launcher exposes `-AiTargetAuthority`, `-AiTargetDrift` and
`-TraceAiDecisions`. These are development switches, not end-user menu labels.

## Evidence and limits

The observation-only pair captured 5,549 selector calls on each peer through
tick 2000, with matching owner sets and setter sites. All sampled native
requests in that idle scene were null. That observation alone therefore did
not prove delivery of a meaningful non-null target.
Evidence: `analysis/private-native-room-pair-20261115-090359-d43c474d/ai-target-observer-result.json`.

The authority build passes 9,078 Room checks, including real TCP delivery with
maximum AI batches, malformed flag rejection and invalid owner/target/ordinal
rejection. These are transport checks, not a native AI playthrough.
Evidence: `analysis/native-ai-target-authority-test-c44ff9fabe7442868a13077c88a2645a/game.log`.
Executable SHA256:
`0A9467A36C7C523E4DE0DD753A98E36C27212B36FB6EAFB1F02862021EF9ED63`.

The live native pair applies the host-only non-null target fixture through the
guest's original setter, with readback and independent selection skipped.
All 1,714 ordered host and guest owner/call entries match from gameplay
admission at tick 39 through tick 747, including 120 non-null fixture commands.
However, the overall world test **fails** at tick 748 on an unrelated missing
Damage target. The test result explicitly distinguishes the target-authority
fixture from the failed world integration.
Evidence: `analysis/private-native-room-pair-20261115-091338-333018ae/ai-target-authority-result.json`.

The lifecycle journal shows that missing object was inserted on the host at
tick 742 and removed at 749. Its native GUID changed between insertion and
removal. The guest records no inserts in the inspected tick-650..750 interval.
The 30-tick snapshots alone missed this transient object. Local lifetime
addresses relate those local events only; they are not cross-peer identities.
Evidence: `missing-damage-identity.json` in the same pair directory.

General spawn/destruction replication, stable identities for runtime-created
objects, complete behavior/ability authority, and interrupted old-world
updates still require work. The final native selector-ordinal path also needs
an active encounter test; the non-null fixture exercised the audited setter
through the earlier clear-target branch. No new gameplay release is claimed
for this private target-selection adapter.
