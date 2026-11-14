# Cooperative native title shutdown

A captured hang confirmed that the pinned SDK's asynchronous title termination
killed the guest main thread while it owned the console logger mutex. The UI
then waited on that abandoned mutex while logging its next shutdown step.
The failed evidence remains at
`analysis/native-lobby-20261114-084202-75353bc4`; it is not a passing capture.

The current source defaults to cooperative guest exit before native module
teardown. It source-builds the pinned SDK's `XThread` and `XObject` objects;
the installed SDK remains unchanged. The build checks kernel archive SHA-256
`c25f4aedda3c6c7f90ea2bfcab932cd12c50f9d3cd456317f99ecca1cd18997b`
before removing the replaced objects from the build-local archive.

## Exit ordering

Host audio callbacks and GPU guest interrupts drain while title workers can
still finish their existing work. A separate manual-reset stop event then
wakes cooperative guest waits. Cancellation unwinds C++ scopes to the guest
execution boundary and returns through the SDK's normal thread-exit cleanup.
The game-frame entry also checks for cancellation, so ordinary frame pacing
does not depend on another gameplay action occurring.

A retained registry keeps host thread handles reachable after their public
guest handles close. Shutdown joins the union of that registry and the kernel
object table, resumes suspended guests so they can leave their execution
scopes, and requires two quiescent snapshots before module teardown. References
are released outside the registry lock. No timeout falls back to killing a
thread; a remaining worker keeps the join pending and produces a diagnostic.

Single-object and wait-any calls include the independent stop event.
Wait-all uses bounded native atomic waits without consuming a partial set.
Signal-and-wait performs its signal exactly once before any remaining wait.
Ordinary host threads retain the original wait behavior. Short sleeps retain
the original yield behavior. Cancellation inside a native APC is caught before
returning to the Windows dispatcher; the outer guest wait then unwinds normally.
The APC lock and temporary allocation use scoped cleanup.

The debug-only `em2_cooperative_title_shutdown` flag is a startup selection,
defaulting to true. The process captures that selection so a later flag edit
cannot enable joining for threads that started without cancellation support.
It is not an in-game graphics or gameplay setting.

## Checks

```powershell
.\scripts\test-guest-shutdown.ps1

.\scripts\test-native-room-menu-roster.ps1 -Players 3 -ScriptedInput -CaptureShutdownHang -DamagePlayer 1 -PipelineCreationThreads 2 -CheckpointReloadTick 700 -ObserveSeconds 85 -StartupTimeoutSeconds 480
python .\scripts\verify-native-player-respawn.py <capture-directory>
python .\scripts\verify-native-guest-shutdown.py <capture-directory>
```

The isolated wait regression uses real Windows events, semaphores, threads and
an APC. It covers cancellation of single/any/all waits, atomic signal-and-wait,
sleep and alertable sleep, C++ scope unwinding, host mutex release, unchanged
semaphore counts after a partial wait-all timeout, exactly one signal across a
split timeout, and the fixed startup selection. The final test also confirms
the compiled default is true. It executes no guest game code and does not
change ordinary user files.

The default-path Oswald native capture at
`analysis/native-lobby-20261114-090352-7dfa2e60` passed native damage, recovery,
restored controls and shutdown checks across three independent worlds, with
1,911 consecutive matching updates. Guest joins took
4–20 ms. Its candidate SHA-256 was `05888DF6330B7AA22DA1F696A2526EBFDF6FA3932A01664332AB2D56487076BE`.

The final Mickey capture at
`analysis/native-lobby-20261114-090734-3d93fedb` passed the same checks with
1,927 consecutive matching updates. Guest joins took
5–40 ms. Its candidate SHA-256 was `B85D359C6AD4DE1A1421C95ABC5E208105AA7629A2C535669BBD485C55925DC5`.
All six processes exited with code zero, with no force-close, and normal saves,
settings and crash files remained unchanged. The Mickey candidate also includes
the startup-selection guard and passed the isolated wait/APC regression at
`analysis/guest-shutdown-20261114-090732-0cc30624`.

The shutdown verifier requires original native menu/world entry, matching
worlds, all four ordered shutdown phases on the same UI thread, main-thread
watcher completion, final host teardown and normal process exit. The old
forced-close capture is rejected.

## Scope

This addresses the confirmed abandoned-logger-lock failure during title close.
It is not evidence for every loader, in-flight save, debugger suspension or
campaign shutdown state. Extra-human death/replacement, whole-party checkpoint
recovery, sixteen native clients, Internet playthroughs, prediction and state
correction still require work and verification. No FPS increase is claimed.
This source change is not included in the existing demo.6 download.
