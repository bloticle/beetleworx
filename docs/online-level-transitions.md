# Native online task restrictions and level transitions

Extra humans must follow native task restrictions after scripts add or remove
them. A four-player Autotopia / Mean Street round trip exposed extra Oswald
accepting electric-stream input while the retail Oswald was restricted. A
balanced removal test then exposed the reverse failure: the retail Oswald
became usable again, but the extra Oswald stayed blocked.

The native policy reader remains an admission operation. Reusing it during
play can leave a counter set when the cached count reaches zero. Runtime
updates now queue the original per-task add/remove operations in order,
including repeated or nested operations. They affect only the named task and
use the original native counter operations instead of resetting all tasks.
Task names/reasons are copied, and recipients are pinned to their actor
generations. A replacement actor cannot receive an earlier incarnation's
queued change. Initial admission reads the current policy separately.

The owner drains updates before the next input frame, outside unfinished
input/action dispatch. Each operation validates the live task owner and the
native counter result. The 64-entry queue fails the session on overflow or
invalid data instead of dropping state or retrying a partly applied batch.
World teardown clears queued changes. Retail policy and native rule definitions
are not rewritten.

## Verification

A balanced four-process fixture called the original restriction writer at
agreed tick 300 and removed that one restriction at tick 500. Original SDK
input attempted electric-stream start/end while blocked, then tried again
after release. All four worlds rejected the first attempt and accepted the
second, with matching extra/retail task sets, handlers and counter flags.
The capture contained 1,247 consecutive matching native updates. Both extra
humans continued moving, jumping and landing; unrelated spin/paint callbacks
remained valid. The earlier implementation fails this same unblocking verifier.

The same executable also passed an original TransitionToLevel round trip at
ticks 700 and 1400: meanstreet_north.visit_2, then
tcy_autotopia_visit1.sectionc_first. The initial and two re-entered worlds had
661, 698 and 1,021 consecutive matching completed updates. Verification requires
fresh actor admission, separate verified input histories, matching native
trajectories, movement reversal, jump/landing, spin and task-policy-aware tool
callbacks. Twenty-four electric-stream checks cover both unrestricted and
restricted states. All four processes exited normally in both captures; normal
user saves, settings and crash files remained unchanged.

The native regression suite checks manager/profile routing, exact recipient
generations, ordered nested notifications, owned string data, queue overflow,
and no-pending caller/environment preservation. Its final result is recorded
with the process exit code and executable hash alongside the local captures.

With a built canonical executable and prepared content, the private fixtures
can be reproduced with:

```powershell
.\scripts\test-native-room-menu-roster.ps1 -Players 4 -ScriptedInput -ChatProbe -VoiceProbe -ButtonTaskProbe -PolicyCycle -ObserveSeconds 65
python .\scripts\verify-native-task-policy-cycle.py <capture-directory>

.\scripts\test-native-room-menu-roster.ps1 -Players 4 -ScriptedInput -VoiceProbe -ChatProbe -ButtonTaskProbe -CheckpointReloadTick 700 -CheckpointReloadCount 2 -NativeLevelTargets @('meanstreet_north.visit_2','tcy_autotopia_visit1.sectionc_first') -ObserveSeconds 110 -StartupTimeoutSeconds 480
python .\scripts\verify-native-world-reentry.py <capture-directory> --expect-default
```

PolicyCycle requires the isolated four-player SDK fixture and cannot be combined
with scheduled reloads or fault injection. It changes only its private world.
NativeLevelTargets supports up to four known retail states, one per scheduled
load. Task probes and the cycle fixture are default-off private diagnostics.

These tests establish one balanced native restriction cycle and scheduled
original transitions between two levels. They do not prove every campaign
portal, all policy setter variants, full quest/cutscene/death progression,
sixteen native clients, Internet conditions, or authoritative replay/correction.
No new downloadable release includes this work; demo.6 remains the published
binary.

### Additional route investigation

The transition verifier now checks the actual participating roster (3–16
clients) rather than assuming four clients. It retains the same per-world
history, actor trajectory, native permission and normal-exit requirements.
This change passes the existing four-client Autotopia → Mean Street North →
Autotopia capture; it does not establish support for sixteen native clients.

A three-client route from Autotopia through Mean Street North to Mean Street
South reached the South world, but the local machine's available-commit guard
ended the run before the required observation window completed. That route is
not an accepted regression result yet. The memory guards remain unchanged.

For investigation, the isolated harness accepts `-WorldMemory`, which enables
`em2_profile_world_memory`. This default-off diagnostic queues read-only
GPU-worker snapshots of texture-cache usage, shared GPU heaps, upload-pool
pages and available system commit at load/update boundaries. It does not trim
caches, alter rendering quality or change native gameplay. No performance or
memory-reduction fix is claimed by this diagnostic change.
