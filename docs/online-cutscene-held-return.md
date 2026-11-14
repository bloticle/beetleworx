# Held buttons when gameplay resumes

A button held in a suspended context could become a fresh Jump press when
native gameplay bindings were re-admitted. The new private interpreter
histories began neutral, so the still-held A button appeared newly pressed.

The return path now retains the final owned physical sample's remapped digital
button mask. After arming the new source route, it installs that mask before
the first sample. Each blocked bit clears when its corresponding button
releases. Fresh buttons pass immediately. Movement, camera axes and analog
triggers continue through the ordinary source path.

The raw sampler cache stays unchanged. Movement, Jump and complete-button
production receive the same sealed, filtered sample, with one SDK poll per
tick. Installing a fence requires a fresh route with the current Room, controls
generation, owner thread and native binding. It cannot modify a sampled route
or create new input. Actor/world incarnations and native physics remain on
the existing path.

## Reproduction and fixed behavior

Binary `4BCCBAE7C75CDA093A018F1EC211E961431C87C736498DAF5CF8D60609C3AB86`
was used for both three-world captures:

| Capture | Behavior | Unwanted Jump callbacks | Peak height increase, retail Mickey / Oswald / extra Mickey |
| --- | --- | --- | --- |
| analysis/native-lobby-20261114-125821-662a3202 | Private old-behavior switch | 1304 / 1341 | 2.332 / 3.469 / 2.332 |
| analysis/native-lobby-20261114-130159-073dad73 | Default fixed behavior | None | 0 / 0 / 0.034 |

The small extra-Mickey height change in the fixed capture occurs while walking
over the level surface. Both captures retained 1,601 matching native updates
across all three processes. The old behavior reproduced an unsolicited Jump
as gameplay delivery resumed at tick 1304. The fixed capture kept all humans
walking while A remained held, then accepted a fresh Jump and its release at
1521 / 1525, including normal landing. Health stayed unchanged; user files
were unchanged and all processes exited normally.

Validation:

```powershell
python scripts/verify-native-cutscene-held-return.py analysis/native-lobby-20261114-125821-662a3202 --expect-unfenced
python scripts/verify-native-cutscene-held-return.py analysis/native-lobby-20261114-130159-073dad73
```

The harness uses `-CutsceneCycle -CutsceneBoundaryMovement
-CutsceneHeldReturn`. The first capture additionally uses
`-UnfencedCutsceneReturn`; that reproduction switch requires private profiling
and an active native lobby QA fixture. It is off by default.

The complete focused online input suite passed with exit 0 in
`analysis/native-cutscene-return-fence-focused.log`. The 65 native-local-source
checks include the raw cache, sealed provenance, once-only sampling, fresh
buttons during a carried hold, and both original Jump interpreters observing
no synthetic edge followed by a real press/release.

These live captures use original balanced RestrictCutSceneInput and
UnrestrictCutSceneInput calls. They validate the shared re-admission path,
not a complete authored cinematic, movie skip or online campaign playthrough.
The published demo binary remains unchanged.
