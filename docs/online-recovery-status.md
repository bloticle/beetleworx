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

Mouse/keyboard interaction still needs human visual acceptance. Controller-only
recovery and in-place reconnect are not implemented. This source update is not
included in demo.6; no new downloadable release has been published.
