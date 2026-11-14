# Native online movie skipping

Any connected human can request a state-2 movie skip through the original
Menu → Accept action. Controller A and the existing UI mapping for keyboard
Enter/Space supply the same Pad A intent. The adapter runs the original action
interpreter and original movie handler, retaining native delay and unskippable
rules. It does not call a movie-close export or alter sequence completion flags.

## Input and handler ownership

The live Menu inventory has one Accept action: kind 2, condition/curve 0,
wildcard controller user 7, key 01400009, one binding, no repeat interval.
`NativeCutsceneSkipSource::ArmMovieAccept` evaluates only this action against
private copies of the device, button history and action record. Existing
cinematic prompt/hold/release interpretation remains separate.

The existing typed V6 UI command carries only the held A level. The receiver
combines that intent after every human certifies UI availability. It validates
state 2, object table 8215B2B0, handler 82B1DF70, the current movie entry,
Menu subscription and live Accept record. Dispatch goes through original
profile callback 82770F58. Character controls remain neutral. Duplicate native
delivery is rejected; partial callbacks cannot be retried.

The existing UI entry fence requires release before a carried gameplay press
can become a new movie Accept. Gameplay re-admission fences held buttons again,
preventing a held movie Accept from becoming an unsolicited Jump.

## Skip-delay clock

Original 82B1DD60 initializes the movie timer at object +32 through caller
82B1DD88. The original Accept handler reads it through caller 82B1E070.
Both use the agreed native input clock. Other timer fields and callers retain
their existing behavior. The native signed delay remains unchanged: -1 is
unskippable; other values are interpreted by the original handler.

Room UI timer ownership is independent of the older two-player Session gate.
During delivery, elapsed-time permission comes from a scoped callback lease,
not the completed-delivery flag: the callback necessarily runs before that
flag can be published. The lease pins context, thread, world target, controls
generation, tick, timer and caller, then expires when the callback returns.
An original clock-query failure now stops the active native Room.

## Validation

The native unskippable fixture forwards actual Accept callbacks to original
82B1DF70 while its -1 guard is set. It checks unchanged movie state and live
action memory, exact callback-only timer permission, duplicate rejection,
all-peer UI readiness and wrong-target rejection. The final focused suite is
`analysis/native-movie-delay-lease-focused.log`: 309 interpreter/source/handler
checks, 116 media-clock checks and the existing broader input/transport suite.
Its state and clock getters are injected; real playback is checked separately.

Private three-world captures:

- `analysis/native-lobby-20261114-140521-0640e922` records the actual Menu
  binding and an unforwarded fresh extra-peer press; natural return stays 447/449.
- `analysis/native-lobby-20261114-141632-84432ce9` proves the extra peer can
  skip an original retail movie through Accept at tick 371. All clients return
  at source 372 / delivery 374, with 1,601 matching updates, restored movement,
  reversal, jumping and landing, and no Jump from held Accept after return.
- `analysis/native-lobby-20261114-142135-6f3357c5` reproduces the premature
  skip caused by mixing a canonical timer stamp with an unowned local elapsed
  read. The delayed-skip verifier rejects that capture.

The delayed fixture uses the original `LOGO_havok.bik:2` queue entry. The
extra human taps Accept at source 319 and presses again at source 369. With
the two-update network delay, original callbacks run at 321 and 371.

```powershell
scripts/test-native-room-menu-roster.ps1 -Players 3 -ScriptedInput -CaptureShutdownHang -MovieSequence -CutsceneSkipInput -MovieSkipDelay 2 -CheckpointReloadTick 300 -CheckpointReloadCount 1 -PipelineCreationThreads 2 -ObserveSeconds 90 -StartupTimeoutSeconds 480 -ReuseVerifiedContent <verified-private-capture>
python scripts/verify-native-movie-sequence.py <capture-directory> --expect-skip
```

The verifier requires a canonical start stamp at 301, elapsed values 2/3 and
7/3 seconds, the first callback leaving the movie active, the later callback
completing it, identical return ticks and actual post-movie controls in all
three worlds. Source pointers are never compared across processes.

`analysis/native-lobby-20261114-142837-b5c1f5d1` passes that verifier with
binary 3AA99E44917C2F7BB8FB3EA207E3D97957FBDACD4FF43C7FE85D3D21A1C35534.
All clients initialize stamp 401000000 at 301, reject the 0.666667-second
press at 321 and complete on the 2.333333-second press at 371. Return is
372/374 in all three worlds. The capture has 1,601 matching updates and matching
actual trajectories, successful post-skip movement/reversal/jump/landing,
no unsolicited Jump from held Accept, normal exits and unchanged normal user
files, configuration and crash reports. The focused process also exits 0.

## Remaining coverage

This adds state-2 movie Accept to the existing movie playback path. Other
movies and skip configurations, queued multiple movies, startup/direct state-1
movies, real state-6 cinematic skipping, choices, credits, network stalls and
complete campaign playthroughs still need testing or implementation. The
native fixture uses isolated SDK inputs; it does not establish manual device,
Internet or sixteen-native-client acceptance.
