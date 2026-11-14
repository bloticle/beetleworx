# Native online GameOver lifecycle

When every admitted human dies, the original game enters control state 7.
Its Movement/Jump bindings change to the authored CutScene input context.
An online producer pinned to the old bindings must not keep delivering them.

The gameplay owner admits a terminal GameOver phase only after a completed
native update, an exact all-human native death count, the original state-7
getter, the frozen Room roster/world and every admitted extra actor agree.
This is not a general permission to ignore a changed input context.

`NativeGameOverV4` carries an explicit no-gameplay command. Its pad, movement,
button schema and Jump events must be exactly neutral. The transport keeps a
separate terminal phase without advancing or discarding gameplay edge history.
It cannot return to gameplay until the original world rebootstrap resets it.
Normal actor coordinators reject this command type.

Previously produced delayed gameplay commands remain in authoritative history
for their existing delay window, but no gameplay callbacks or pad actions are
dispatched during native GameOver. The owner still requires a consumed frame,
the original native world update, its real completed-state digest and peer
proof consensus. This is not synthetic world completion.

The original death/checkpoint scripts decide when to load. Their existing
Begin/End loader rendezvous drains the old world and negotiates the new world.
Fresh admission rebuilds sources, actor receivers and camera ownership. This
change does not force a restart, revive actors by writing health, or reuse
stale input bindings after the transition.

## Checks

The focused canonical input suite checks typed no-action fields, delayed-prefix
schema/source identity, invalid metadata, terminal transport history and its
non-resumption rule. Its real sixteen-peer TCP fixture verifies exact terminal
command delivery and proof barriers; it does not establish sixteen native
clients or campaign completion.

The private live fixture is:

```powershell
.\scripts\test-native-room-menu-roster.ps1 -Players 3 -ScriptedInput -CaptureShutdownHang -DamagePlayer 4 -PipelineCreationThreads 2 -CheckpointReloadTick 700 -ObserveSeconds 100 -StartupTimeoutSeconds 480
python .\scripts\verify-native-party-gameover.py <capture-directory>
```

It requires all three original native fatal-damage paths, state 7/dead count 3,
matching native loader points and a fresh world. It then checks restored
physical movement, reversal, jumping, landing, matching independent worlds,
normal shutdown and unchanged normal user files. The first world's input
program ends at fatal damage; only the fresh world's program must finish.

Arbitrary story cutscenes still require a synchronized context/receiver epoch
lifecycle. This terminal phase cannot be used to bypass that work. Other
checkpoints, crowded groups, sixteen native clients and full-campaign Internet
play require separate captures. No binary release accompanies source changes.

## Verified native capture

Candidate SHA-256:
`6B3B4F9510BCB8593C2A58F1A5876F0B5A0527121A9F1F8533C379775FDE0EDB`.
The three-world capture `analysis/native-lobby-20261114-105911-347fa9c3`
passed `verify-native-party-gameover.py`. Original fatal damage at tick 700
produced native dead counts 1, 2 and 3 and control state 7. All peers agreed
the original checkpoint loader Begin/End at tick 761, rebootstrap at 762 and
fresh gameplay admission at 763. The old damage observer retired its load-2
target when load 3 appeared, without retargeting replacement actors.

All three players responded to their own fresh SDK movement/reversal and Jump
inputs. Native jump heights were 1.52 units for both Mickeys and 2.24 for
Oswald; all landed. Retail health remained 5/5 after the checkpoint. The
verifier matched 1,201 consecutive post-reload world digests, retail states
and extra-player trajectories across all three actual native processes.
Every process exited normally through ordered cooperative guest shutdown;
normal saves, settings and crash files were unchanged.

The earlier `analysis/native-lobby-20261114-104729-a0a5fa88` capture remains
rejected after a system commit-headroom guard and unsafe long test movements.
`analysis/native-lobby-20261114-105219-f82420bf` also remains rejected: its
recovery/control program completed, but one client did not shut down.
A real dump located its main guest thread in the original 826B57A8 GPU-fence
spin loop after interrupts had drained. The existing 8269FAC8 poll now checks
cooperative guest cancellation before forwarding unchanged to the original
helper. The focused shutdown fixture checks real-wrapper cancellation before
guest access, PPC/host-environment preservation and normal/inactive forwarding;
the successful live capture includes that fix. No forced thread termination
is used by the fix.

The same final executable also passed the focused online input suite (exit 0,
`analysis/native-gameover-focused-complete.log`) and the focused cooperative
shutdown suite (exit 0, `analysis/native-gameover-shutdown-tests.log`). In the
live run, cooperative guest joins took 75, 8 and 9 ms. These checks complement
the live capture rather than replacing its gameplay/trajectory requirements.

## Exact passive retail quarantine

A subsequent review found that simply publishing a newer neutral frame left
retail quarantine comparing against the previous gameplay frame. The owner now
begins and finishes an explicit passive retail tick during native GameOver.
It pins the same registry, actor/controller identities, controls generation,
exact neutral publication and agreed native clock. It advances the retained
quarantine frame without claiming Movement delivery or Motion consumption.
Active delivery cannot begin inside that transaction; callback capabilities
cannot be rearmed there. Missing/stale identity at completion faults it.

The focused canonical online input suite passed with 67 retail Movement checks,
including the passive-frame cases, in
`analysis/native-passive-quarantine-tests.log` (exit 0). They reject quarantine
from neutral publication alone, wrong registry/clock, duplicate admission or
completion, active delivery inside a passive tick and non-neutral passive
input. Both original retail Movement callbacks remain suppressed during a
valid passive frame, and quarantine remains after its completion.

The final candidate SHA-256
`45D7673094318AEC72B48DD43BA701DC54EA4C7BCE50640E1106895B48D9A396`
passed the live three-world GameOver verifier again in
`analysis/native-lobby-20261114-111628-49c94218`: native restriction/death and
loader timing remained 700/761/763, all three humans regained movement,
reversal, Jump and landing, and 1,201 post-reload updates matched. All processes
exited normally and normal files were unchanged. This does not enable general
story-cutscene return; that separate retained-world failure is reproduced in
the [input-context investigation](online-input-context-lifecycle.md).
