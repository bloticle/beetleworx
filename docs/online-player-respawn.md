# Native player death and recovery checks

Checkpoint reload tests alone do not prove recovery after gameplay damage.
The private scheduler fixture can now call the original `Damage` Lua export at
an agreed tick, with native `GetHealth` readings immediately before and after.
It does not request a reload, write health directly, or force a respawn.
The synthetic SDK source supplies fresh movement, reversal and jump input
following the native recovery period. These actions remain private diagnostics;
normal sessions do not enable them.

## Verified Mickey case

Three independent native worlds ran the same fatal-damage scenario. Mickey's
native health changed from 5 to 0 at tick 700 and recovered to 3 at tick 972.
The original game revived him in the existing world, without a level load.
All three worlds then observed movement, reversal, a 1.52-unit jump and landing
from the recovered character's fresh SDK input. The worlds matched for 1,895
consecutive updates, all processes exited normally, and normal user saves,
settings and crash files were unchanged.

Local capture: `analysis/native-lobby-20261114-083905-10ac577e`.
Candidate executable SHA-256:
`BC032C97355952DF46BD6B7DB77DB8EFAFC87BE6733CB50B58D13E7797E834F3`.
This verifies existing native recovery in one scenario; it is not a new respawn
implementation. An earlier capture was rejected because its harness wrongly
expected a level reload. That failed record remains failed.

## Reproduction

With the canonical executable built and private test content prepared:

```powershell
.\scripts\test-native-room-menu-roster.ps1 -Players 3 -ScriptedInput -CaptureShutdownHang -DamagePlayer 1 -PipelineCreationThreads 2 -CheckpointReloadTick 700 -ObserveSeconds 85 -StartupTimeoutSeconds 480
python .\scripts\verify-native-player-respawn.py <capture-directory>
```

Use `DamagePlayer 2` for the retail Oswald case. `PipelineCreationThreads` limits
only this capture's concurrent shader workers; omission preserves the SDK's
normal worker setting. It does not replace any native game world or player
with a transport-only participant. Larger runs remain subject to both memory
guards.

The verifier requires native health loss and recovery at matching ticks, fresh
SDK actions, actual position/jump/landing observations in every world, a single
retained world, completed programs, matching world digests and normal process
shutdown. It rejects the earlier incomplete capture. It does not accept a Lua
request or a returned health value alone as evidence of working controls.

## Confirmed shutdown failure and remaining coverage

The separate Oswald run reached fatal damage, native health restoration and
its follow-up SDK program, but the host hung on close. Its overall result is
rejected, not a passing respawn test:
`analysis/native-lobby-20261114-084202-75353bc4`.

That host dump confirms an abandoned console logger mutex. The pinned MSVC
mutex owner was guest main thread 20776, with ownership count 1; that thread
was absent from the dump's live thread list. UI thread 6472 was waiting in the
console sink after `KernelState::TerminateTitle`. The matching executable's
disassembly calls `CloseNativeRoomDiagnostics` and `TerminateTitle` immediately
before this blocked shutdown log. The stack scan provides address candidates,
not a full unwind; the separate mutex memory and thread-list evidence establish
the missing owner. Stopping arbitrary guest threads can abandon host locks, so
suppressing this log alone is not a complete shutdown solution. The subsequent [cooperative shutdown implementation](native-guest-shutdown.md)
addresses this confirmed failure; the failed original capture remains rejected.

Extra-human death/replacement, whole-party death and checkpoint restoration,
arbitrary campaign scenes, sixteen native clients, Internet conditions, and
rollback/state correction remain unverified by these tests. The confirmed shutdown failure is addressed in the newer cooperative-exit
source; see its separate native validation above. This work does not update
the public demo.
