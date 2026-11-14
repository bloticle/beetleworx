# Native online party-death participant counts

The original death-state entry increments the native dead-player counter at
`833E36CC`, then compares it with `82769260`. That helper literally returns 2.
The later revive check compares the same counter with the configured retail
player count through `82769F28`. Both queries therefore treat two dead retail
characters as the whole party even when an admitted additional human is alive.

This was traced in three native game processes. The original entry selected
control state 7 through caller `82B2C8C4`, which switched native input into
`CutScene` with `RotationFixed`. The extra human's original health property
remained 5. Extending only the first query delayed the failure until the revive
check at tick 941; both comparisons require the same participant count.

## Implementation

Only query callers `82B2C8A0` (death entry) and `82B2C494` (revive decision)
use the admitted online human count. The gameplay owner requires its current
Running Room, frozen identity and roster, original world/load incarnation,
exact committed native update, delivered input frame, ready native actor
roster and valid enabled per-peer actor leases. Extra actor cardinality must
match the locked roster. The count comes from actual admitted humans, rather
than lobby capacity. Outside these queries or without an admitted online
update, both helpers retain their original behavior.

The original native counter, death state, checkpoint selection, revive timers
and health changes remain responsible for recovery. PlayerManager's two-entry
arrays are not expanded by these scalar count hooks. An extra player's health
is not changed to prevent a party death. Native cutscene or binding validation
is not globally disabled.

## Verified survivor case

The canonical candidate SHA-256 is
`1907D5BE30B6805A5DCB1558B863014C0C845879DAA236900E53E513D93D0BFD`.
It passed the focused online input suite, including native callback exception,
quarantine, ownership, binding, original interpreter and transport fixtures.
That suite is not a full native campaign or Internet-play test.

Three native worlds with an extra Mickey passed capture
`analysis/native-lobby-20261114-102353-c21b6db0`. Both retail humans received
original fatal Damage at tick 700. The two death-entry queries reported three
participants with native dead counts 1 and 2; both revive queries reported
three participants at tick 941. The extra human retained health 5 and could
move, reverse, jump 1.52 units and land while both retail characters were dead.
Both retail humans recovered health at tick 972 without a world reload. All
three health/trajectory histories matched, all SDK programs completed, and
all processes exited normally. The independent shutdown verifier passed with
1,486 matching native updates. Normal user files were unchanged.

The same executable passed an extra Oswald survivor in
`analysis/native-lobby-20261114-102728-ae28cd4a`. All three worlds admitted
native action profile 1, observed unchanged survivor health 5, measured a
2.24-unit jump and landing while both retail humans were dead, and recovered
both retail humans at tick 972. Count-query and health/trajectory histories
matched. The shutdown verifier passed with 1,471 matching native updates;
all three processes exited normally and normal user files were unchanged.

## Reproduction and remaining scope

```powershell
.\scripts\test-native-room-menu-roster.ps1 -Players 3 -ScriptedInput -CaptureShutdownHang -DamagePlayer 3 -PipelineCreationThreads 2 -CheckpointReloadTick 700 -ObserveSeconds 65 -StartupTimeoutSeconds 480
python .\scripts\verify-native-retail-pair-survivor.py <capture-directory>
python .\scripts\verify-native-guest-shutdown.py <capture-directory>
```

`-ExtraOswald` selects an Oswald survivor through the normal lobby appearance
request before Ready. The verifier checks the actual admitted native action
profile as well as physical health and movement, both count query sites,
retained world ownership, completed SDK programs, matching independent worlds
and normal exits. Earlier failed captures remain rejected.

Death of every human, large simultaneous death groups, crowded checkpoints,
arbitrary falls, full-campaign behavior and sixteen native clients remain
unverified by this survivor test. Legitimate cutscene and input-context
transitions still need a synchronized source/receiver lifecycle; see
[the input-context investigation](online-input-context-lifecycle.md). This
fix removes the incorrect party-death trigger for the verified survivor case,
not that broader remaining work. No binary release is created by this change.


## Whole-party death diagnostic

The private harness also accepts `-DamagePlayer 4` with exactly three players.
It delivers original fatal Damage to both retail humans and extra peer 3 at
the same scheduled boundary, without forcing a reload. The extra-player
observer records the current native load generation and retires its target
on a legitimate load change instead of following a replacement actor.

The compiled diagnostic was exercised in
`analysis/native-lobby-20261114-103227-929d1603`. At tick 700, all three
native death entries were counted and the original control-state getter
reported GameOver state 7 with native dead count 3 and extra-player health 0.
The capture then failed because the live local source could no longer
produce original native Movement/Jump after the native input-context change.
This is a reproduced failure, not a successful checkpoint recovery test.
Normal user files were unchanged. The synchronized game-over input lifecycle
and recovery remain unfinished; no binary release accompanies this source
publication.
