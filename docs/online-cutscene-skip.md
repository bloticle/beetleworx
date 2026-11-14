# Native cutscene skip input

The source now contains synchronized skip intent and an original native
cinematic callback receiver. It remains experimental: skipping a real authored
story scene has not been verified, cinematic choices are unfinished, and movie
mode is not supported by this receiver. The public demo binary is unchanged.

## Native control states

Original state registration distinguishes these interfaces:

| Mode | Purpose | Native input handler |
| --- | --- | --- |
| 1 | Movie | 82B1EDF8 |
| 3 | Input restriction | 83206DC8 |
| 6 | Authored cinematic | 82B210B0 |
| 7 | GameOver | Separate terminal lifecycle |

The registration is in original `82A2B910` code in
`generated/ppc/ppc_recomp.260.cpp`. RestrictCutSceneInput selects mode 3.
It uses a CutScene input context but does not select the authored cinematic
handler. The mode-3 fixture therefore proves restriction and return behavior;
it cannot prove story skipping. The first private callback candidate correctly
rejected the mode-3 target (`analysis/native-lobby-20261114-123544-92eb4141`,
handler 83206DC8). No cinematic callbacks ran in that capture.

`ReadNativeControlTarget` reads the original state-object interface without
changing its state. The gameplay owner recognizes mode 6 separately and pins
its original 82B210B0 handler. The selected CutScene context and exact profile
subscription must also be present before skip intent can be produced.

## Original action behavior

| Action | Native kind | Behavior |
| --- | --- | --- |
| DisplaySkipMovieButton | 2 | Press edge |
| MovieSkipStart | 1 | Delayed, latched hold action |
| MovieSkipStop | 3 | Release edge |

Their user binding is 7, which original 827C43C0 expands to all four native
controller slots. The original decoder maps key 01400009 to the A-button
history populated by 830ACA80. A room peer number is never a native device
index. Original 827C5F30 supplies the hold duration and latch.

`NativeCutsceneSkipSource` pins registry, manager, input, device, threshold and
normalized action definitions. Original button maintenance and action evaluation
run on private histories under the existing source input clock. These operations
do not mutate the live device/action records or authorize callbacks by themselves.
Same-tick retries are cached; a conflicting A state, skipped tick, wrong device,
epoch, definition or thread cannot commit source time.

## Synchronized UI delivery

Room protocol 9 introduces NativeCutsceneUiV6. It contains only A intent and
an explicit even input epoch. Other buttons, triggers, axes, pause, movement,
Jump events and button-action schemas reject. The actor coordinator and Jump
stream reject this command type. Old protocol binaries cannot join these
source builds; a future binary package needs its own release version.

A held entry button must release after target admission before it can produce
UI intent. The target's admission boundary also fences the existing network
delay. Every human must certify UI availability in the shared frame before
any callback can fire. Any human can request skip; the receiver evaluates the combined hold
through the original native action interpreter. Only its three skip decisions
reach the original profile listener and mode-6 handler. The native authored
skip guard is retained. Character controls remain neutral and quarantined.

The original handler's +88 timer uses agreed input ticks at exact native start
and elapsed callsites (82B211B8 / 82B22628). Other clocks remain unchanged.
A target/context retirement destroys the receiver and timer lease; delayed UI
commands are retained in history without being replayed into characters.
A death immediately after return may drain the previous even-epoch UI prefix
inside the existing delay window, without dispatching it.

## Validation and remaining work

`analysis/native-cutscene-ui-cohort-focused.log` passed with exit 0. It includes:

- 294 original-interpreter, source-lifecycle and guarded native-receiver checks.
  The receiver invokes original 82B210B0 for prompt, hold and release under its
  unskippable guard. Duplicate delivery and non-neutral character publication
  reject. This fixture injects the control getters and agreed clock.
- 24 cutscene epoch/intent checks and 31 terminal-prefix checks, including a
  queued UI prefix when GameOver starts after input re-admission.
- Real sixteen-peer TCP transport coverage for typed UI down states, exact
  authoritative echoes, input-epoch return and completion barriers. This is
  not sixteen native game clients.

The isolated three-world guard run uses `-CutsceneCycle
-CutsceneBoundaryMovement -CutsceneSkipInput` and is checked with:

```powershell
python scripts/verify-native-cutscene-ui-guards.py <capture-directory>
```

It checks native restriction/return, restored movement and jumping, matching
updates and unchanged health/user files. It also requires zero cinematic
callbacks despite both carried entry input and a fresh extra-peer hold.

The [held-button return fix](online-cutscene-held-return.md) has a native
three-world reproduction and fixed comparison. Real mode-6 scene completion,
timer-driven skip, nested cinematics, cinematic choices, movie mode 1 and
cutscenes before gameplay admission still need native campaign tests and any
corresponding fixes. No complete online playthrough
claim follows from the isolated fixtures.

The [authored conversation fixture](online-authored-conversation.md) now
completes the original Autotopia intro in three matching native worlds.
That Apprentice sequence does not enter the cinematic input state, so its
completion does not establish skip delivery. State-2 `StartSequence` movies
now have [separate native entry/playback/return coverage](online-movie-sequence.md);
their skip controls and direct movie state 1 remain unverified or unfinished.

The real guard capture `analysis/native-lobby-20261114-124527-59fe6d76`
(binary `1ADCC49ACB1B2EAD686A4B6E31EDFC2278DF223AEAFD66C493D1EED8309D5773`)
passed with 1,601 matching updates. It observed private prompt/hold/release
math at 702/710/719 for all peers, and 839/847/859 for the extra peer's fresh
hold. None reached cinematic callbacks in mode 3. Input returned at source
1302 / delivery 1304; movement, reversal, jumping and landing passed afterward.
All three processes exited normally and normal user files stayed unchanged.
The final all-peer UI-readiness change is covered by the focused native
receiver fixture; it does not turn this restriction capture into a real scene
skip test.
