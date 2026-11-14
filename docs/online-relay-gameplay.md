# Native gameplay through the hosted relay

Two native game processes on one Windows machine have completed an original
Autotopia → Mean Street North transition while connected through the deployed
HTTPS/WebSocket relay. This tests the real relay path, not a direct TCP shortcut.
It does not establish separate-machine compatibility, broad Internet conditions,
full campaign completion, sixteen players, prediction or rollback.

## Joining after slow file verification

The first hosted gameplay capture failed before any game payload crossed the
relay. The guest opened its WebSocket before native content verification had
finished. That admitted a host-side game socket which could not receive a Hello
yet; the native 15-second handshake deadline expired during preparation.

The guest now looks up the room and publishes a local endpoint for native boot
preparation, then waits for the prepared local game to connect before opening
the relay WebSocket. Native compatibility checking and handshake deadlines are
unchanged. Local preparation is cancellable and bounded to ten minutes. The
guest status `ready` indicates that its local endpoint is available; it does
not itself establish remote admission.

In the passing run, guest verification took 22.81 seconds. Its WebSocket opened
after verification and both worlds entered native gameplay. The earlier run
closed at roughly fifteen seconds with zero transmitted game payload.

## Accepted scope

- Two actual processes used eight-character hosted room codes through the
  ordinary native Host/Join backend. TLS WebSocket and bridging phases were
  observed in both logs. Private automation did not use the clipboard, LAN
  discovery, router mapping or Discord party publication.
- Both worlds agreed on 661 consecutive native updates in Autotopia and 2,815
  in North, with separate original load/admission and input-history scopes.
- Retail Oswald moved, reversed, jumped and landed in both levels. His native
  jump height was about 2.24 units and his landing returned within 0.01 units
  of the pre-jump height. Mickey stayed neutral in this fixture; this is not
  an independent movement test of both retail characters.
- Native HUD chat delivered both senders exactly once, in the same host order,
  with their assigned colors. Authored editor/history readback and resumed
  Oswald movement after chat closed passed. This does not prove visual layout
  or manual typing on another machine.
- Both processes exited normally with exit code 0, and normal user saves,
  settings and crash files were unchanged. Available commit stayed above the
  unchanged guard (minimum 3,687 MiB). Both attempts used the same 720p
  internal/output fixture settings.

Gameplay and chat transferred approximately 2.93 MB host-to-guest and 1.36 MB
guest-to-host. The guest recorded the host closing during the requested normal
shutdown; no relay failure occurred before that shutdown request.

Voice was disabled in this accepted capture. The separate hosted voice-suite
check was not run. The new delayed-preparation assertion in that hosted media
fixture is compiled but remains unexecuted. Real microphone/proximity audio,
Discord invitations, reconnection and longer campaigns remain unverified.

## Developer fixture

`-RelayOrigin` opts the owned native-menu harness into a plain HTTPS relay
origin. The normal build defaults do not enable private automation. Its existing
private-root marker and role/path validation remain required. A relay run cannot
replay a loopback invitation. `-Players 2` uses the existing retail Oswald input
program; extra-human task comparisons still require at least three players.

```powershell
.\scripts\test-native-room-menu-roster.ps1 -Players 2 -RelayOrigin 'https://beetleworx-relay.ryanckoop1.workers.dev' -WorldMemory -ScriptedInput -ChatProbe -CaptureShutdownHang -PipelineCreationThreads 2 -CheckpointReloadTick 700 -CheckpointReloadCount 1 -NativeLevelTargets @('meanstreet_north.visit_2') -ObserveSeconds 180 -StartupTimeoutSeconds 480
python .\scripts\verify-native-world-reentry.py <capture-directory> --expect-default
python .\scripts\verify-native-room-chat.py <capture-directory> --native-hud
```

The verifier requires actual relay phases, preparation before guest admission,
native positions for Oswald's motion, matching consecutive world updates and
normal process exits. It rejects a relay failure before normal shutdown. The
previous four-client loopback transition/chat captures also pass the updated
verifiers. Setup recovery and the focused online regression suite exited 0.
No new binary release includes this source change yet.
