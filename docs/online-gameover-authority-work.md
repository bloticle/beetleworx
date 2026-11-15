# Host-authoritative Game Over input phase

## Reproduction

Private route `analysis/private-native-room-pair-20261115-142154-71bdc942`
entered `ftl_fort01a.v1_fort_spatterfight` through the original transition at
700 and fresh admission at 701. Both native humans died during the opening
cutscene; this is not evidence of a completed enemy encounter.
The host stopped at 716 with `Native GameOver authoritative frame failed phase
admission`. The guest's later connection-close error was secondary.

A diagnostic repeat, pair
`analysis/private-native-room-pair-20261115-142514-059cd2e0`, confirmed:

- Guest terminal entry 713, host terminal entry 714.
- Both remained in input epoch 2 (cutscene), native mode 7, dead count 2,
  and the original registry.
- At frame 716, the host command was valid V5 cutscene intent and the guest
  command was valid V4 terminal intent, both actor epoch 1 / input epoch 2.
- Input delay was 2. The validator rejected the early guest terminal intent
  inside the host's delayed prefix. Merely widening that prefix locally would
  leave the guest and host with different terminal cutoff clocks.

## Change

Room wire 25 adds an optional host Game Over entry tick to completed authority
state. The host samples the original native terminal mode/death ownership at
the completed update and retains its initial entry tick. The guest pins its
terminal delayed-prefix boundary to that world-scoped host clock. A changed
host entry within the same world is rejected.

Inside an already certified native Game Over prefix, same-epoch terminal
no-action commands may overlap older input from another peer. Every command
still passes the normal intent validator, roster and actor-epoch checks.
The transport keeps each peer irreversibly terminal; no gameplay callbacks
are dispatched. Past the host cutoff every peer must be terminal. Old UI,
future epochs, hidden actions, malformed/truncated metadata, and future entry
ticks remain rejected. The wire version prevents old peers misreading the
additional field; no demo version/release is changed by this source work.

## Regression evidence

Canonical executable SHA-256:
`AA6CC018C8A0D4380D398367DDF63DF17EDF4C3986A376B3C6B77869C40BD5A9`.

The full online-input regression exits 0, including 35 Game Over checks and
9099 Room checks. It covers the exact mixed V5/V4 epoch-2 frame at 716,
post-cutoff rejection at 717, malformed terminal actions, exact authority-state
roundtrip, invalid presence/truncation, and future terminal entry rejection.
The host clock also roundtrips through real TCP state correction.
Evidence: `analysis/native-gameover-authority-input-result.json` and
`analysis/native-gameover-authority-37b4799cd53c4c5fba9fc325dae6dcf7/input.log`.
The separate rollback-chain transport log also reports 9099 checks, but its
external process observer did not retain an exit code; the input run supplies
the known exit-0 result.

## End-of-update health ordering

The first phase-clock candidate passed frame 716 and both peers ran the
original checkpoint request, but respawn did not complete. Pair
`analysis/private-native-room-pair-20261115-143233-3a4390a1` stopped at the
old-world proof drain. Host completed through tick 774; guest through 773.
The guest's native respawn request was one update early. Both closed normally
with unchanged normal user files; this is not a passed respawn test.

The trace identifies the earlier cause: at tick 713 the guest applied both
host zero-health values before its native update, although these were the
host's completed-update values. The host entered Game Over after 714 while
the guest entered after 713. Aligning only input-phase metadata cannot rewind
the original death countdown.

Full Damage-event authority now leaves any remaining direct-property zero
correction until the completed guest update. `CompletedPropertyOnly` calls the
original Health property setter without synthesizing a lethal Damage event.
Original Health processing then sees the zero on the following tick, preserving
the native ordering of direct paths such as FallRescue. Real captured Damage
events retain their replay path. The legacy health-only mode remains separate.

## Live health-boundary result

Final candidate executable SHA-256:
`24A35AC64FD47F9D29D0E19478C9A385C271D47F2EDFB449AE4DF141AF5E3D0D`.

Pair `analysis/private-native-room-pair-20261115-143717-88160e56` now has
matching terminal entry 714 on both peers. Both agree original checkpoint
loader Begin/End at tick 775 and fresh admission at 776. Original gameplay
updates continue through tick 1509 on both. This repairs the observed input
rejection and subsequent one-tick respawn/proof-barrier deadlock.

The continued run is NOT a completed three-destination combat test: at tick
1510 the guest rejects a Shooter spawn because native spawner pre-state differs
from the host. The host's later socket reset is secondary. That distinct
projectile/spawner lifecycle issue remains to be repaired. The destination
`ftl_gulch.main` scheduled at 1900 was not reached. No mismatch was ignored.

Both owned processes were closed normally, exit 0, with normal files unchanged.
Exact entry/load/admission events and later failures are retained in
`gameover-health-boundary-result.json` inside that pair directory.

The final build also passed the focused native Damage capture/codec tests
(18/156 checks) and the rollback-chain suite including 9099 Room checks, both
exit 0. Logs and process results are under
`analysis/native-gameover-health-d0bd19d9d74e45009206045d30c19499/`.
The earlier online-input suite covers the unchanged frame/codec changes.

This is targeted terminal-phase/death-order alignment, not native rollback,
general lifecycle restoration, or complete enemy sync. Source publication
includes no new public binary release.
