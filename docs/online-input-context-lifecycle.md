# Native input-context lifecycle: confirmed online blocker

The passing extra-player recovery captures cover one damaged human at a time.
They do not establish recovery when both retail humans die while an extra
human survives. The new isolated `DamagePlayer 3` fixture means both retail
players, not room peer 3. It calls original `Damage`/`GetHealth` through Lua at
tick 700, leaves the extra human untouched, and schedules fresh extra-human
movement, reversal and jump inputs after that tick. No reload is requested.

## Confirmed evidence

The baseline capture `analysis/native-lobby-20261114-094959-27112349` reached
native health `0/5/0/5` for the two retail characters at tick 700. At tick 701,
extra-camera binding validation stopped the session. It treated unavailable
analog look actions as an invalid binding after having admitted them earlier.

The context capture `analysis/native-lobby-20261114-095457-d1a0f8a6` confirmed
the native active context was `CutScene`, the selected rotation family was
`RotationFixed`, and the extra player's original health property remained 5.
This is a native control-mode transition, not evidence that the extra player
was killed or that an Internet packet was missing.

## Camera handling change

Additional normal cameras now explicitly recognize the combination of a valid
owned camera/controller, the currently selected native `CutScene` context,
and the original selector's `RotationFixed` family. In that exact mode they
continue the original native camera update without dispatching analog OnLook.
Other contexts retain normal binding validation. Missing bindings alone never
imply permission to ignore a validation failure. The transition is logged;
normal gameplay look bindings are rediscovered and validated when needed.

The follow-up capture `analysis/native-lobby-20261114-095838-ae6aadc1`, candidate
SHA-256 `27D175BB3B6AF5BD3BB0F520BB344AD6FD94D32799DEAC0CED70581E39EF0EA7`,
recognized the fixed camera context at tick 701 and passed that camera path.
The session then stopped at the button completion revalidation in the same
update. Its original game input definitions had changed to CutScene, so the
captured gameplay button receiver became stale. All three processes exited
normally, but the gameplay capture is rejected. This is not an end-to-end
cutscene, survivor, party-death or recovery pass.

## Required next work

Native input-context transitions need an explicit synchronized lifecycle for
source capture, delayed commands, actor receiver ownership and rearming when
normal control returns. Gameplay snapshots cannot simply be reused while
native contexts/definitions change, and stale-definition checks must not be
globally disabled to hide this failure. Completing a native frame that changes
its input context must be distinguished from admitting another frame against
old definitions. Surviving additional humans also need correct party-death
policy; it is not established whether the original two-player death sequence
will reload the level while they remain alive, because validation currently
stops before that outcome.

`verify-native-retail-pair-survivor.py` requires a retained world, unchanged
survivor health, actual movement/reversal/jump/landing, matching histories in
three real worlds, retail recovery, completed input programs and normal exits.
It rejects these captures. A health reading, a camera-mode log or suppressing
an exception is insufficient to pass it.

```powershell
.\scripts\test-native-room-menu-roster.ps1 -Players 3 -ScriptedInput -CaptureShutdownHang -DamagePlayer 3 -PipelineCreationThreads 2 -CheckpointReloadTick 700 -ObserveSeconds 65 -StartupTimeoutSeconds 480
python .\scripts\verify-native-retail-pair-survivor.py <capture-directory>
```

This source work does not create or replace a public binary release.

## Completed-frame validation separated from next-frame admission

`FinishRetailButtonTick` now validates the delivered frame header, owner thread,
active transaction, fault state, native tick and unchanged retail actor/movement
leases. It no longer reruns definition admission after the native world update.
Those definitions were already validated in complete preflight and before every
actual native action callback. A native callback exception or partial delivery
still faults the transaction. Subsequent frames still require current producer
and receiver definitions; completing a previous frame does not rearm anything.

The focused canonical-executable online suite passed (exit 0), including the
extended original-callback fixture with 31 retail button receiver checks. That
fixture changes a button definition after the native update, proves the
already-delivered neutral frame completes, rejects next-frame preflight on an
independent receiver that observes the changed definition, rejects resurrection
when the bytes are restored, and requires explicit fresh receiver capture.
Existing duplicate delivery, malformed schemas, quarantine and native callback
exception checks remain. This does not implement in-place global delivery
rearming after gameplay starts; the existing lifecycle restriction remains.

In the real three-world capture
`analysis/native-lobby-20261114-101148-c9edaeda`, tick 701 completed with the
survivor's native health still 5 after entering CutScene. The next source
production failed because it retained the old gameplay Movement/Jump binding.
The capture remains rejected as playable survivor/cutscene evidence. This
isolates the next required work to context-aware source/receiver lifecycle,
including delayed frames and control-return rearming, rather than completion
of the already delivered native update. No new binary release was created.
