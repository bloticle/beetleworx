# Native online level transitions

Extra humans previously retained an outdated task-restriction count after a
script changed the retail character's permissions. A four-player journey from
Autotopia to Mean Street and back exposed extra Oswald accepting electric-stream
input while the retail Oswald was restricted. On returning, both were restricted;
that rejection was consistent with the native rules, not a missing tool task.

The adapter now observes the original per-character restriction add/remove
writers (82A27E20 / 82A27F80), coalesces affected profiles, and reapplies the
existing native permission reader before the next input frame. It does not
register new rules, force-enable tools, change the retail policy, or mutate an
extra task list inside an active action callback. Notifications clear at world
teardown. Invalid manager/profile notifications are rejected; a refresh failure
stops the session instead of continuing with partially updated permissions.

Validation used four independent local game processes, the compiled default
reload support, and original TransitionToLevel calls at agreed ticks 700 and
1400. Destinations were meanstreet_north.visit_2 and then
tcy_autotopia_visit1.sectionc_first. Each world required fresh actor admission,
separate verified input history, matching native body trajectories, movement
reversal, jump/landing, spin and tool dispatch. The three incarnations had
661, 698, and 987 consecutive matching completed native updates. All four exited
normally; normal user saves, settings, and crash data remained unchanged.

Twenty-four electric-stream checks compare actual extra/retail task sets,
original handlers, restriction counters, and callback results. Both unrestricted
acceptance and authored rejection were exercised. The old capture fails the
stronger check for its Mean Street permission mismatch. Reapplying the policy
from its reader alone did not fix the issue; the final fix observes its writers.
The final build's native regression suite exited zero, including manager/profile
routing, duplicate coalescing and no-pending/no-read caller-preservation checks.

The isolated harness accepts NativeLevelTargets for a bounded itinerary of up
to four known retail states; its destination count must match the scheduled
load count. Existing single-target and checkpoint cases remain supported.
Example, with the built canonical executable and prepared game content:

```powershell
.\scripts\test-native-room-menu-roster.ps1 -Players 4 -ScriptedInput -VoiceProbe -ChatProbe -ButtonTaskProbe -CheckpointReloadTick 700 -CheckpointReloadCount 2 -NativeLevelTargets @('meanstreet_north.visit_2','tcy_autotopia_visit1.sectionc_first') -ObserveSeconds 110 -StartupTimeoutSeconds 480
python .\scripts\verify-native-world-reentry.py <capture-directory> --expect-default
```

These are scheduled original transitions in isolated profiles. They do not prove
walking through every campaign portal, later restriction removal/unblocking,
all task-policy setter variants, full quest/cutscene/death progression, sixteen
native clients, Internet conditions, or authoritative recovery/replay. The
read-only task probes are default-off and require the private profile switch.
No new downloadable release includes this work; demo.6 remains the published
binary.
