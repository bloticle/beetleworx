# Authored conversation coverage

The private native-menu harness can now start an original Apprentice sequence
on its authored owner rather than substitute an input restriction:

```powershell
scripts/test-native-room-menu-roster.ps1 -Players 3 -ScriptedInput -CaptureShutdownHang -AuthoredConversation -CutsceneSkipInput -CheckpointReloadTick 300 -CheckpointReloadCount 1 -PipelineCreationThreads 2 -ObserveSeconds 90 -StartupTimeoutSeconds 480 -ReuseVerifiedContent <verified-private-capture>
python scripts/inspect-native-authored-conversation.py <new-capture>
```

This mode requires exactly three private humans, scripted SDK input, one
scheduled call at tick 300 or 700, and no competing reload, damage, policy or
disconnect fixture. It does not run in a public demo or ordinary session.
The harness's existing restart scheduling fields select the call boundary;
this fixture neither restarts nor loads the world.

The original `ForceSequence` export (8286B200) receives
`GetEntity("tcy_autotopia_01a_shared.Marker(Conversation) Oswald")`,
`AUT_v1_AreaIntro`, and player number **1**. It runs at the original Lua
scheduler boundary. `IsSequencePlaying` then confirms acceptance through its
original export. No sequence cursor, context, character transform, timer or
quest state is written directly by the fixture. The authored sequence itself
can execute camera, animation, quest and checkpoint scripts, so only isolated
save/content directories may be used.

The owner and resource binding come from `ConversationComponent.Sequences` in
`environments/tomorrowcity/gsa/tcy_autotopia_01a_shared.bin`, inside
`tcy_autotopia_01a_shared.pak`. The retail BSQ contains 21 events and flags
419B. Its runtime resource flag is read at +34, as in original 82924888.
Original 82869428 maps API player numbers 1/2 to native player handles;
the default 0 supplies a null handle. Neither an accepted request nor these
flags alone establishes that character input has been suspended.

Read-only private traces sample the native sequence manager at 833DDE14,
its player at +108 and UI interface at +120. Event progress is sampled at
specific agreed ticks, not continuously. The original UI table is 82118670;
its start and availability callbacks are 82A2D4D8 and 82A2C920. Guest pointers
are reported for diagnosis but excluded when comparing progress across peers.

## What this proves

The inspector separately checks original acceptance, event-cursor progress,
matching sparse conversation samples, ordinary native world consensus and
isolated process/file cleanup. `--require-finished` additionally requires a
sampled inactive sequence after tick 1900. Its report always leaves
`cinematic_skip_verified` false. It does not turn world hashes or conversation
progress into evidence of synchronized cinematic skip handling.

The player-1 capture `analysis/native-lobby-20261114-132436-9d4674ae`
(binary 44FF7C57A08D10DF4EEDC6281732BF53E72621AB20A0A9A9D5232EC023A9B61C)
has 2,057 consecutive matching world updates and matching sparse native
conversation samples. It advances the authored event cursor but does not
enter the native cutscene input state, arm its UI receiver or dispatch skip
callbacks. Conversation completion is not established by that capture.

The earlier-start capture `analysis/native-lobby-20261114-132806-2009ef81`
uses the same binary and starts the sequence at tick 300. Its inspector passes
`--require-finished`: all three clients have matching sampled event progress,
cursor 21 and inactive state at ticks 1600 and 1900, with 2,057 consecutive
matching world updates. All three processes exit normally, and normal user
files, configuration and crash reports remain unchanged. No cinematic UI
receiver or skip callback is observed. The samples establish completion of
this sequence, not continuous equality of every conversation-internal field.

## Distinct paths still needing campaign tests

`EnterCutscene(entity, variant)` (82852A30) sends original entity action 11;
it does not start a cinematic player. `StartSequence` (82772F20, helper
82772D50) selects native **state 2**, which queues movies and frontend
commands. Its [movie-only path now has separate native coverage](online-movie-sequence.md).
Starting this Apprentice conversation does not validate that movie path or
state-6 cinematic completion. Direct movie state 1, choices, nested scenes,
credits/menu routes and complete campaign traversal remain unproven.
