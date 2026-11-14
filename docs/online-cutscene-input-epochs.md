# Native cutscene input epochs

A retained native world can disable and restore player input without changing
levels. Reusing gameplay bindings through that transition is unsafe. Treating
it as a new actor or forcing a level reload also loses the native lifecycle.

Room commands now carry `input_epoch` separately from `actor_epoch`. A newly
admitted world starts at input epoch 1. Native cutscene suspension advances to
an even epoch; returning gameplay advances to the next odd epoch. The actor,
world, controls generation and lobby identity remain pinned. Normal native
frame coordinators reject a different input epoch even if actor IDs and button
schemas happen to match. Room wire protocol 8 serializes both epochs, and the
versioned consumed-input fingerprint includes both.

## Native entry and return

The owner requires the original control-state getter to report state 3, plus
an actual selected CutScene context or positive balanced restriction counters
in both original retail player-info records. The latter covers the native
transition before the input selector has updated. A missing Movement binding
alone never authorizes suspension. Retained roster/world ownership and a
completed native update are required as well.

Commands already produced before suspension retain their old epoch and may
drain only within the negotiated delay window. The owner records and consumes
them but does not dispatch their gameplay actions after the native restriction.
Later frames must contain exact typed no-gameplay commands for the suspended
epoch. The real physical SDK source is still polled once per source tick;
its input is discarded during this no-gameplay phase. This keeps sampling
clocks and mouse/controller state current without fabricating action decisions.
Native physics, scripts, observations and per-tick proof consensus continue.
Retail quarantine tracks each exact neutral published frame.

The original unrestriction can restore control state 0 before selecting the
normal input context. The owner waits for its captured context **name**, not an
old pointer or definition allocation, while continuing real passive native
updates. A 120-update bound prevents an indefinite return wait. Once selected,
it captures fresh Movement/Jump/device routes, retail button receivers, native
button producer clocks and owned Jump edge streams. Existing definitions are
not resurrected by restoring their bytes. Actor generation stays unchanged;
input epoch and local producer/receiver leases change explicitly.

A fresh coordinator starts at the delayed first-delivery tick. Previously
produced suspended commands drain without gameplay dispatch until then. New
commands must use the resumed epoch, fresh schema and source clock. Native
Jump histories reset only through the explicit generation-pinned re-admission;
old-epoch Jump commands remain invalid. Native GameOver remains a distinct
terminal phase that can return only through the original world load.

## Reproduction

```powershell
.\scripts\test-native-room-menu-roster.ps1 -Players 3 -ScriptedInput -CaptureShutdownHang -CutsceneCycle -PipelineCreationThreads 2 -CheckpointReloadTick 700 -CheckpointReloadCount 2 -CheckpointReloadInterval 600 -ObserveSeconds 90 -StartupTimeoutSeconds 480
python .\scripts\verify-native-cutscene-return.py <capture-directory>
```

The fixture calls original `RestrictCutSceneInput()` at native tick 700 and
original `UnrestrictCutSceneInput()` at 1300 through the protected native Lua
scheduler. It does not write control state/definitions or request a reload.
It includes an earlier native Jump and a new Jump press queued immediately
before restriction. That queued press must not reach native character callbacks
inside the restricted phase. Each participant later supplies fresh movement,
reversal and Jump inputs; physical response and landing are checked in every
independent world. `-ExtraOswald` selects an Oswald extra through the lobby.

This fixture exercises original input restriction and return, not an entire
cinematic director or every story scene. Interactive cutscene choices, skip
input and other actions during the no-gameplay phase are not routed by this
change. They require native UI/context-specific input support; source input
must not simply be forwarded to stale gameplay listeners. Full campaigns,
all context combinations, all held-button cases, Internet conditions and
sixteen native clients remain separate validation work. Source publication
does not create a new binary release.

## Verified captures

Candidate `DEB40212827A2D84E001ED2AFD194F6CAC39882D2C0DA22A7DA90AA1832A4F0F`
passed the three-native-world verifier with an extra Mickey in
`analysis/native-lobby-20261114-114740-4a1f2e0c` and an extra Oswald in
`analysis/native-lobby-20261114-115138-cbfb00a1`. Each capture matched 1,601
consecutive native updates, including physical movement/reversal and native
Jump/landing after return. Extra Mickey jumped 1.52 units; extra Oswald jumped
2.24. All processes exited normally and normal files were unchanged.

The actual native sequence was restriction at tick 700, control-mode return at
1300, an empty selector at source tick 1301, restored `MainGame` plus fresh
binding admission at 1302, and first new-epoch delivery at 1304. The earlier
Jump reached original callbacks at ticks 241/245. The queued boundary press
never reached native Jump callbacks during suspension; the new-epoch Jump
reached them at 1521/1525. Actor and world incarnations remained unchanged.

The same candidate passed the separate native GameOver/checkpoint regression
in `analysis/native-lobby-20261114-115451-ba72fc42`, including 1,201 matching
post-load updates, restored controls and normal exits. Terminal GameOver
recovery therefore remains distinct from retained-world cutscene return.

Candidate `C4C458FDD718809DE7C9A170B1C5C7B87ACC40DDAEB7CD01DC711A57B595464D`
adds only the optional private SDK boundary-movement fixture. It passed
`analysis/native-lobby-20261114-115918-111858bf` with
`-CutsceneBoundaryMovement`. All three players held SDK movement over entry,
yet stopped during the native restriction: measured XZ drift between ticks
760 and 1300 was zero. Health stayed unchanged, fresh movement/reversal and
Jump/landing worked afterward, and 1,601 native updates matched across all
three worlds. All processes exited normally; normal user files were unchanged.

Earlier captures remain rejected: `analysis/native-lobby-20261114-113355-e7bba549`
required the selected context too early; `analysis/native-lobby-20261114-113807-0cf0f6c6`
attempted re-admission before the return selector settled; and
`analysis/native-lobby-20261114-114334-65a623c7` never passed bootstrap because
the context reader incorrectly required a heap-string header on native literal
strings. The corrected reader follows the existing bounded C-string approach.

The final C4C458 candidate also passed the complete focused online input suite
(exit 0, `analysis/native-cutscene-all-focused.log`), including 11 dedicated
input-epoch stream/prefix checks and 8,990 real-transport Room checks. The
sixteen-peer fixture covers suspended/resumed epochs, exact wire echoes,
canonical consumed-input fingerprints and completed-proof barriers. It does
not substitute for sixteen actual native game clients. Protocol 8 changes
are source-only here; a future binary package needs its own release version,
and source builds must not be mixed with older protocol-7 demo binaries.


Private cutscene UI inventory
-----------------------------

Private profiling now records the original UI_Global and CutScene action
contexts, listeners and bindings once per input epoch. The inventory is
read-only: it does not evaluate actions, dispatch callbacks or transmit
process addresses. Ordinary launches do not enable this trace.

Candidate `616521E1D7794A2FB7FF15017FC976E029478D637DDD080AA78B3858D5708DC2`
compiled and passed the three-world boundary-movement regression in
`analysis/native-lobby-20261114-121030-69160243`: 1,601 matching updates,
controls restored at source tick 1302 / delivery tick 1304, successful
movement and jumping afterward, normal process exits and unchanged user
files. Verification used `scripts/verify-native-cutscene-return.py`.

The observed native skip actions form a pressed/held/released sequence
(`DisplaySkipMovieButton`, `MovieSkipStart`, `MovieSkipStop`) with a special
user binding. This inventory does not implement online cutscene skipping or
interactive cinematic choices. Those remain unfinished, along with full
campaign and sixteen-native-client validation.
