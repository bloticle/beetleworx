# Disconnect recovery development status

The native HUD recovery prompt now updates while the online world is halted.
The renderer invokes only PCOnlineRecoveryTick on the current container-owned
movie through the original GFx Invoke ABI. It does not advance movie timelines,
onEnterFrame callbacks, or gameplay. The button queues a request; a checked
native application boundary validates world completion before the original
title load and offline-save refresh run. Duplicate requests are coalesced.
Unsafe failed updates continue to require a restart.

An isolated four-process capture passed the strict disconnect/title/rejoin
verifier: each authored HUD button issued exactly one request before transition;
original offline campaigns were unchanged; every process returned to title,
refreshed saves, joined a fresh lobby identity, and resumed native gameplay.
The second session passed independent actor movement, camera, jump, spin, and
tool callback checks, plus authored chat, generated voice transport/decode,
and voice-position checks. This was local isolated testing with scripted input.
It does not establish cross-machine microphone/speaker quality, arbitrary
campaign transitions, sixteen native clients, or full campaign completion.

The first interactive capture failed because halted movies did not run their
frame callbacks. The first capture after fixing dispatch reached recovery but
failed its full action verifier: the absolute-tick private chat probe overlapped
the warmed-up action program. The probe now opens after that program. The action
verifier was unchanged; a new complete capture passed. Earlier failed captures
were retained as failures. The native regression suite also passed with a
retained process exit code of zero; HUD compile/decompile checks passed.

Controller recovery now polls the original local device on the stopped native
world owner thread, independently of gameplay sampling. A must be released
before a new press can queue recovery. Disconnected input cannot confirm;
unsafe boundaries cannot confirm. Polling bypasses agreed network input and
gameplay remapping without consuming or publishing a simulation sample. The
native HUD advertises A / Enter.

A second isolated four-process capture passed strict controller recovery and
fresh-session gameplay validation. Its SDK driver supplied held A, held A,
release, then a new A press. Each local peer produced exactly one controller
request after release, and the HUD auto-click was disabled. All four restored
offline saves and entered a fresh lobby with active movement, camera, jump,
spin and tool callbacks. Second-session chat, generated voice transport/decode,
and voice-position checks also passed. Sampler regressions verify distinct raw
and agreed inputs, bounded local users, disconnects, scope restoration, and
unchanged canonical input/cache; the native suite exited zero.

Physical controller hardware and human visual interaction remain unverified.
In-place reconnect is not implemented. These source changes are not included
in demo.6; no new downloadable release has been published.
