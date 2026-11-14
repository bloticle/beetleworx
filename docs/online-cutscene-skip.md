# Native cutscene skip input

Online gameplay currently quarantines character controls during original
CutScene restrictions and re-admits fresh bindings afterward. It does not
yet forward skip input or interactive cinematic choices. This work adds the
original skip interpreter as a separately tested building block.

## Original action behavior

The observed CutScene context has three matching actions:

| Action | Native kind | Behavior |
| --- | --- | --- |
| DisplaySkipMovieButton | 2 | Press edge |
| MovieSkipStart | 1 | Delayed, latched hold action |
| MovieSkipStop | 3 | Release edge |

Their user binding is 7, which original `827C43C0` expands to all four
native controller slots. The original decoder maps key `01400009` to the
A-button history populated by `830ACA80`. A room peer number is never used
as a native device index. Holding does not mean dispatching MovieSkipStart
every update: original `827C5F30` applies its native duration and latch.

`NativeCutsceneSkipSource` discovers this exact three-action definition in
the current original state-3 CutScene context. It pins registry, manager,
input, device, threshold and normalized action identities. It runs original
button maintenance and action evaluation on private copies under the existing
source input clock. Other controller slots are null in the private input copy;
the wildcard selector must find the selected real native slot. It evaluates
only the A intent; other buttons, axes, pause, quick exit and player joining
cannot add decisions to this result.

The source starts with fresh neutral private histories. Live action/device
records and the borrowed guest stack are not used as mutable histories. An
exact same-tick sample returns its cached result, conflicting same-tick A
states reject, and a wrong device, epoch, source tick, definition or thread
cannot commit source time. Input epochs must be explicit and even.

## Current integration and limits

Private profiling in a running native Room samples these decisions after the
existing owned physical poll during restriction. It logs results only. It
adds no wire command, UI callback or agreed-world action. Normal launches
without private profiling do not run this probe.

Before online skip can be exposed, it still needs a distinct synchronized UI
intent, delayed-prefix/return fencing, entry-button history policy, exact
native listener/target admission, original callback delivery and deterministic
native skip-timer completion. Already queued gameplay presses must not be
reinterpreted as fresh skip presses. FMV control modes and actual story-scene
skip/choice behavior also need native tests. The balanced restriction fixture
does not prove a real cinematic was skipped.

## Validation

The complete focused online input suite passed with exit 0 in
`analysis/native-cutscene-skip-math-focused.log`. Its new 240 checks run the
original skip/device/action routines for all four wildcard slots, native
hold/latch timing, short taps, cache/conflict rules, wrong-device/epoch/tick
and thread rejection, stale-definition rollback and unrelated-input isolation.
The control-mode getter in the isolated source-lifecycle fixture is injected;
the button/action interpreters are original recompiled functions.

Real-world validation uses:

```powershell
python scripts/verify-native-cutscene-skip-math.py <capture-directory>
```

That verifier also runs the existing native cutscene-return verifier, checking
world agreement, retained actors, restored movement/jumping, unchanged health
and user files, and normal shutdown. Its scope remains three real worlds and
private skip evaluation, not online callback delivery or full-campaign play.

The live candidate
`486C4B3A1152D77E303F2BF11B32B4A8A12C64386115C400146A9659E1FE59D3`
passed `analysis/native-lobby-20261114-122321-8db3fc0d`. All three worlds
admitted the skip source at tick 702, produced prompt/hold/release at
702/710/719, and retained 1,601 matching native updates. Native device slots
0 and 1 produced identical decisions. The original return gate re-admitted
gameplay at source tick 1302 / delivery tick 1304; movement, reversal, jumping
and landing passed afterward. All processes exited normally and normal user
files were unchanged. This is private evaluation evidence, not skip-callback
delivery. The public demo binary remains unchanged.
