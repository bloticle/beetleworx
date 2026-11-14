# Extra-player recovery validation

Health restoration alone does not establish a working respawn. The isolated
extra-player damage fixture dispatches the original native Damage event and
reads the original health property. It does not write health or force a reload.

`verify-native-extra-respawn.py` requires continuous valid native physics,
ignored movement/jump while dead, fresh movement/reversal/jump and landing after
recovery, matching health and trajectories across real game processes, untouched
neutral retail players, and normal shutdown. It also runs the existing native
menu and early extra-player input checks. Missing physics samples fail the test.

## Confirmed failure

Capture `analysis/native-lobby-20261114-092538-bb7793e5` ran three native worlds.
Extra Mickey lost health at tick 700 and restored health at tick 972, but its
physics observation became unavailable at tick 941 and remained unavailable.
The new recovery verifier rejects this capture. The older early-input verifier
passes because its motion checks precede death; that result cannot be used as
post-respawn evidence.

Read-only route diagnostics in capture
`analysis/native-lobby-20261114-093004-b35f6f4b` confirmed position components
`FFC00000/FFC00000/FFC00000` (NaN) from tick 941 onward. Health restoration did
not repair that position. The same route retained its original entity/motion
ownership while the body's secondary native type changed. This diagnosed capture precedes the fix described below. One client
also required forced closure in this diagnostic capture, so it is rejected as
a shutdown pass.

## Reproduction

```powershell
.\scripts\test-native-room-menu-roster.ps1 -Players 3 -ScriptedInput -CaptureShutdownHang -DamageExtraPeer 3 -PipelineCreationThreads 2 -CheckpointReloadTick 700 -ObserveSeconds 85 -StartupTimeoutSeconds 480
python .\scripts\verify-native-extra-respawn.py <capture-directory>
```

`-ExtraOswald` selects Oswald for the additional player through the normal lobby
appearance request before Ready. These are isolated local diagnostics, not
proof of sixteen-player or full-campaign recovery. No binary release is implied.

## Respawn index and supported placement fix

The original human respawn entry (`82A37518`) asks PlayerManager for its retail
index (`8276CC90`, caller `82A37578`). Extra humans have no retail slot, so the
lookup returns -1. The caller passes this unchecked index into checkpoint
matrix lookup (`82769518`), then builds a pose from missing transform data.
The first invalid pose was traced to caller `82A375E0` at tick 941.

For that specific validated owned-controller query, the recomp now resolves
the actor's native Mickey/Oswald profile. The original checkpoint transform
and character recovery tasks remain in use. Other lookup callers and retail
actors follow the original implementation.

The corresponding checkpoint transform query uses the original collision
placement helper to separate the extra player from the retail player. It
prefers an existing supported point within four horizontal units and one
vertical unit of the authored checkpoint, then searches supported points in
four rings. Unsupported or distant current positions are not reused. Authored
rotation is retained. If all separation queries fail, it logs a warning and
retains the original native checkpoint; tight checkpoint layouts and large
simultaneous respawn groups still need coverage.

The index-only candidate kept finite physics but overlapped retail Mickey;
its reversal check failed. The first radial-placement candidate stepped off a
nearby ledge during the scripted return/jump sequence and died again, so it
also failed. Neither is accepted as a complete recovery test.

The supported-position candidate passed extra Mickey recovery in three actual
worlds: `analysis/native-lobby-20261114-094307-0e856a31`, executable SHA-256
`C62F10B820258D24FBC366525187603B56FE5959F1EE8C270BCDBAF88B81A795`.
Health recovered at tick 972 in every world; dead movement/jump inputs were
ignored; fresh movement, reversal, a 1.52-unit jump and landing passed. Health
and full trajectories matched, the two neutral retail players remained
unchanged, and all three processes exited normally. The shutdown verifier
also passed with 1,502 matching native updates. This remains one-level
coverage, not a full campaign or sixteen-player claim.

The same executable also passed extra Oswald recovery in three native worlds:
`analysis/native-lobby-20261114-094543-fa60a387`. The original respawn used
profile 1, health recovered at tick 972, and fresh controls produced a
2.24-unit jump and landing. All three trajectories and health histories
matched. The shutdown verifier passed with 1,462 matching native updates and
normal exits in every process. Normal user saves/settings/crash files were
unchanged in both accepted captures. Whole-party death, crowded checkpoint
placement, falls in arbitrary campaign scenes and full-game recovery remain
outside this validation.
