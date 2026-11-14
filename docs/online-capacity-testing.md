# Native multiplayer capacity testing

The private candidate has completed a six-client, six-native-world local test.
Each participant entered through the authored native online menu and original
save reload/Continue flow. The verification covered separate actor/camera
ownership, unique assigned colors, single-window views, original SDK input
interpretation, movement reversal, jump and landing, camera-relative movement,
text chat, encoded voice delivery/decoding, and voice-scene roster publication.
All six processes exited normally. Normal user saves, settings, and crash files
were unchanged.

The capture had 1,131 consecutive matching completed native updates. Its binary
SHA-256 was `1A91BDFDB42DEDEF1D42FEBC471C2928BBCB8020949C21F8775B528AEA40CDBC`.
The local evidence directory is `analysis/native-lobby-20261114-081145-229c57d2`.
This is a private source candidate, not the published demo.6 executable.

## Reproducing the checks

With the canonical executable built and the private test content prepared:

```powershell
.\scripts\test-native-room-menu-roster.ps1 -Players 6 -ScriptedInput -ChatProbe -VoiceProbe -CaptureShutdownHang -ObserveSeconds 75 -StartupTimeoutSeconds 480
python .\scripts\verify-native-menu-active-input.py <capture-directory>
python .\scripts\verify-native-room-chat.py <capture-directory>
python .\scripts\verify-native-room-voice.py <capture-directory>
python .\scripts\verify-native-voice-scene.py <capture-directory>
```

The harness refuses to start beside an existing game. It retains each launched
process handle and checks executable, private root, and creation time before
sending window-close requests or force-closing an owned test process.

`MinimumFreeMemoryMB` defaults to 1,024 MB of available physical memory.
`MinimumFreeCommitMB` defaults to 1,536 MB of remaining Windows commit allowance.
Both are sampled directly through `GlobalMemoryStatusEx`, before each launch
and periodically while observing. A guard stop is a failed/incomplete capture;
it never counts as a capacity pass. Concurrent startup can exhaust commit even
when physical RAM remains available. The record includes both memory minima,
owned process memory samples, and exception details/stack if a test stops.

`CaptureShutdownHang` optionally uses an existing
`tools/procdump/procdump64.exe` installation to capture a process still alive
after the normal-close wait. A process without a successfully posted close
request is recorded as a process stall, not a shutdown hang. Dumps can contain
private process data; do not upload them without reviewing them.

## Failed captures and remaining work

An earlier six-client run reached gameplay but required force-closing its host.
Its log stopped during title-worker shutdown; the later successful run does not
establish that this intermittent failure is fixed.

An eight-client run exhausted memory before completing startup. The harness
then lost access to host process metadata; its host dump shows a running title
and no posted close request, so that dump does not prove a shutdown deadlock.
After adding the commit guard, another six-client startup stopped at 279 MB of
remaining commit allowance and closed all six processes normally. That stopped
run is not gameplay or transition evidence.

These tests do not establish sixteen native clients, full campaign progression,
real Internet conditions, microphone/speaker hardware quality, or complete
prediction/replay/state correction. No new downloadable release includes these
changes; demo.6 remains the public binary.
