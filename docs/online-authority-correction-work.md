# Host-authoritative correction development

The initial playable target now uses host-owned state correction without
requiring a native rollback checkpoint. Full rollback remains a later goal.

## Implemented

- A typed authority-state packet on the retained authenticated Room TCP/relay
  channel. It carries completed simulation tick, world token, roster revision,
  player mask, peer identity, active flag, position and linear velocity.
- Host-only publication after actual local native completion. Client-authored
  updates, duplicate actors, invalid masks, nonfinite/out-of-range coordinates,
  stale ticks and mismatched world/roster scope are rejected.
- Bounded eight-sample receiver queue and outbound backpressure reserve for
  controls, chat and voice. New native worlds reset the state timeline.
- Private `em2_probe_room_authority_state` capture in native gameplay completion:
  resolves both retail and extra players from the native roster and samples
  actual character physics rather than interpolated rendering poses. It is
  additionally gated by `em2_private_profile`; disabled by default.

## Evidence

`analysis/authority-state-channel-build.log` compiled successfully. Canonical
`build/runtime23/beetleworx.exe --em2_test_rollback=true` exited 0 in isolated
`analysis/authority-state-test-c7fae0741a3d459687c40fce978c4bff`. The Room suite
passed 8,999 checks, including real TCP delivery of distinct actor position and
velocity states, single-consumption behavior, host-only publication, duplicate
identity rejection, old-world rejection and nonfinite coordinate rejection.
Those are transport checks, not evidence of native correction or a playthrough.

`analysis/authority-native-capture-build.log` compiles the native capture wiring.
That initial capture-only increment was followed by native application, below.

## Native player correction implemented and exercised

- `SetHostCorrectionMode` explicitly negotiates the mode in Hello/Welcome
  (Room wire version 10). The existing verified-lockstep mode remains separate.
  The first authority implementation uses a single completed-update window.
- Host snapshots include the actual completion digest. Clients retain their
  actual local diagnostic digest and acknowledge the host digest separately,
  only after native correction. History scope and fingerprints distinguish
  authority acknowledgements from independent-world equality. The rollback
  capability requirements are unchanged.
- Native clients consume the exact current update's state and resolve current
  roster actors locally. Position and velocity changes use original setters
  `82473F58`, `824741C0` and `8275C0C8`, preserving the body-centre/feet offset.
  Each correction requires native readback within 0.002 units. It does not run
  the teleport action or copy native physics objects.
- Large discontinuities discard presentation history without resetting the
  agreed simulation clock, current update flag or timing remainder.
- Private QA can inject one 0.5m native error and require the host snapshot to
  repair it through the same application path.

`analysis/authority-native-apply-build2.log` compiled successfully. The actual
canonical executable SHA256 is
`F31491A93E681BCCC6AFE63AB9A89E2D754152939B2B20AED865126737644BEA`.
The isolated headless Room suite exited 0 with 9,006 checks in
`analysis/authority-apply-test-eb73c9a6d9ef40a58542041fb355e881`.
New TCP tests retain a different actual client digest while requiring an
exactly-once applied-host-state acknowledgement before releasing the next update.

Actual two-window evidence is retained in
`analysis/private-native-room-pair-20261115-052918-eb6e2f68/authority-result-before-shutdown.json`.
Both instances automatically loaded `tcy_autotopia_visit1.sectionc_first`.
The guest corrected an injected 0.5m error at tick 39 (squared error 0.25), and
native readback passed. Automated W and Space input moved guest Oswald and host
Mickey through the existing key listener; before/after native position words
are in `correction-input.json`. Both roles exceeded 1,900 completed observations
with no native gameplay, initialization or retained-Room failure before shutdown.
Retained-handle finishers confirmed both processes exited 0 and normal user
files were unchanged. Finisher logs describe only the finisher's own input;
the separate input manifest records the authorized test input.

This proves one-level two-player native position/velocity correction and input
routing. It does not prove internet latency behavior, a campaign playthrough,
combat/death authority, level transition correction or 16-player gameplay.

## Existing world-character correction

The next increment adds stable-GUID position/velocity samples for non-player
character proxies, explicitly excluding the admitted players. Up to 128 samples
fit in each authority packet; larger native inventories rotate across updates.
Wire version 11 rejects duplicate/zero GUIDs, invalid vectors and oversized
batches. It never treats packet IDs as pointers or as commands to spawn/delete.
Guests resolve GUIDs against the current native registry, apply original physics
setters and validate readback. Missing entities and active-state differences are
counted separately; lifecycle creation/destruction is still unfinished.

`analysis/authority-world-character-build.log` compiled successfully. The
headless Room suite exited 0 with 9,009 checks in
`analysis/authority-world-test-45bd95972acf4327b99ddf1a71d6f37c`, including a real
TCP full 128-character batch, distinct GUIDs/velocities, overflow/duplicate/zero-ID
rejection and unchanged explicit applied-state acknowledgement behavior.

Actual native evidence is
`analysis/private-native-room-pair-20261115-053755-84c5d993/authority-world-result.json`.
Build SHA256 was
`DA0654AD11F7495CE298B4B65D77900B20B59B317C422DC587238629326439FF`.
The guest repaired a non-player 0.5m native error at tick 39, resolving GUID
`000069A2/00002470/00002946/000036F9`, with squared error 0.25 and successful
native readback. Both characters still moved/jumped through the normal input
listener. Both roles completed over 390 observations without native gameplay
failure; reported world batches contained two characters with zero missing or
active-state mismatches. This is one-level evidence for existing character
position/velocity synchronization, not combat, AI decision/state replication,
rigid props, entity lifecycle or campaign completion.

The subsequent one-line change routes large non-player discontinuities through
the same presentation reset used for players; that threshold was not exercised
by the 0.5m fault.

## Health-state correction

Authority snapshots now carry optional signed integer Health values for players
and existing world characters (wire version 12). Optional presence is distinct
from zero health; negative/sentinel values remain signed. The bounded full
128-character batch still fits the existing 8KB packet limit.

`native_actor_health.cpp` reads Health through original entity int-property
dispatch `827C9CE0` (vtable+204) and writes through `827CA5F0` (vtable+268), using
the actual `833DBB14` descriptor. It validates the entity/component routes,
preserves caller context/stack/FP/error state, and requires native readback.
It does not assign raw health members or fabricate Damage events.

`analysis/authority-health-build2.log` compiled successfully. The existing
headless Room suite exited 0 with 9,009 checks in
`analysis/authority-health-test-cfb0b6dc5a6e49cab8d3b552eec8427c`. Full-batch TCP
roundtrips now also preserve absent, zero, negative and maximum-int health values.

Actual two-window evidence is
`analysis/private-native-room-pair-20261115-054801-d8dd7734/authority-health-result.json`.
Build SHA256 was
`0199E20E29B30A76120EE92857C050146AF4D424B5287B650DBCA4C0BEE0B1DB`.
At tick 39, private guest health was changed from 5 to 4 through the native
property setter, then repaired to the received host value 5 with successful
readback. Both roles exceeded 430 observations without gameplay failure and
movement/jump input still changed their native positions. Retained-handle
finishers confirmed both processes exited 0 with normal user files unchanged.

This proves nonlethal health-property correction, not authoritative damage
causality, hit reactions, invulnerability, death or respawn. Those effects must
not be inferred from a corrected Health integer.

The following fixture change permits the existing original TransitionToLevel
test at the exact script-scheduler boundary in a validated isolated Room probe.
It is selected by `start-private-room-pair.ps1 -LevelTarget <retail state>
-LevelTick <tick>`. Both peers call the original transition at the agreed tick;
this does not claim host-only asynchronous transition support.

## Mean Street transition investigation

`analysis/authority-level-fixture-build.log` compiled the isolated native
transition fixture. The first actual pair,
`analysis/private-native-room-pair-20261115-055053-6de10b7b`, both executed the
original `TransitionToLevel("meanstreet_north.visit_1")` at agreed tick 180,
loaded Mean Street and rebuilt admission at raw tick 181. After the intro
cutscene returned, both stopped because character observation retained only one
physical proxy. This was a failed test, not a successful transition acceptance;
the outcome is recorded in that pair's `authority-level-result.json`.

The observer previously required a live controller/body even when a native
Motion physics bridge was explicitly inactive, and omitted Motion components
whose physics bridge had been retired. That discarded logical character
identities during authored physical detachment. `native_character_physics.cpp`
now observes those as inactive GUIDs with empty physical state; active bodies
still require the original typed controller/body, finite vectors and stable
ownership. Targeted observation follows the same rule. It does not activate a
body, manufacture motion, relax active-body validation or disable world proofs.

`analysis/authority-inactive-body-build2.log` compiled successfully. Isolated
`analysis/authority-inactive-test-67c5f9475dc948cc9e2cd825c3597723` exited 0:
40 native character-physics checks, including detached controller/body/bridge
cases and transactional rejection of missing active bodies, plus the existing
9,009 Room checks. Intentional invalid-memory/nonfinite fixtures emit rejection
diagnostics before their passing summary.

The actual transition retry,
`analysis/private-native-room-pair-20261115-055753-be805123`, continued in Mean
Street past tick 1500 without either original observation/disconnect failure.
Its early W/Space attempt did not move either character, so its recorded
`authority-level-result.json` remains a failed playability check.

A further run used the original `CleanLoadLevel` entry path through the same
native scheduler fixture. Evidence is
`analysis/private-native-room-pair-20261115-060355-44b348d1/authority-level-result.json`.
Both roles loaded Mean Street, rebuilt admission at raw tick 181, and continued
past tick 7800 without a native gameplay failure. An early input attempt again
showed no movement. Later, after entry had progressed, Enter/W/Space input moved
guest Oswald and host Mickey independently; the other actor's position remained
unchanged during the guest test. The later successful result does not establish
Enter as the cause: their positions had already changed before that attempt.
The fixture now selects `-LevelMethod clean` or `transition`, defaulting to clean.

This run used executable SHA256
`B844E00617F312593EA3F1538BDD0A8635E2BEE982A17482F4C2172B93088609`.
Retained-handle finishers confirmed both processes exited 0 and normal user
files were unchanged. The proven scope is paired native entry, lifecycle-safe
observation/correction and eventual post-entry movement in those two levels.
It is not proof of immediate control availability, host-only asynchronous loads,
all levels, death/respawn or a full campaign.

## Bounded authority acknowledgement pipeline

The first implementation restricted authority mode to one outstanding update.
That restriction is now removed: Room and startup preparation accept the
negotiated existing 1..8 update credit. The host may publish later completed
results while older application acknowledgements are delayed. Clients still
apply only the exact matching native update; no future physics is substituted.

Authority publication, reception and application acknowledgements are now
contiguous from the current world's first tick. The client can hold only one
taken correction until it acknowledges application. It cannot fetch/discard the
next result or acknowledge a later tick first. Authority mode never coalesces
unapplied snapshots; sparse diagnostic mode retains its previous coalescing.
World rebootstrap resets all send/receive/taken/applied tracking.

`analysis/authority-pipeline-build2.log` compiled successfully. The isolated
headless suite exited 0 in
`analysis/authority-pipeline-test-a356ae7444f4467dbdbcb02f6bb0917b`, including the
40 character-physics checks and 9,054 Room checks. A real two-peer TCP test
withholds application acknowledgements, completes and retains eight distinct
authority results, blocks the ninth, rejects out-of-order application locally,
then releases one credit by applying the oldest result. The ninth result enters
without replacing the seven older unapplied results, and acknowledgements drain
in exact tick order.

This removes the explicit one-update acknowledgement cap. It does not remove
the all-peer input barrier, prove internet FPS gains or provide prediction.
The native eight-credit level-change check is recorded in
`analysis/private-native-room-pair-20261115-062330-3569d790/authority-pipeline-result.json`.
Executable SHA256 was
`4BE63A447FC7FC8D1BD502DC9A0F8C35EB5FCD4C27ADE3BF15AC1B6CF264F6EB`.
Both roles negotiated credit 8, loaded Mean Street through original CleanLoadLevel,
rebuilt admission and continued past tick 6075 without a native gameplay failure.
Enter/W/Space input after entry moved guest Oswald and host Mickey independently.
Both retained-handle finishers confirmed exit 0 and normal user files unchanged.

For a concrete pacing comparison, the same Mean Street ticks 240..360 took
7.716s (15.552 native updates/s) in the prior one-credit guest capture
`analysis/private-gpu-profile-20261115-060357-7e06d0f4/game.log`, versus 4.012s
(29.910 native updates/s) in the eight-credit guest capture
`analysis/private-gpu-profile-20261115-062332-31a3295b/game.log`. Both used the
same two-instance fixture, level target and 30 FPS cap. This is a measured native
simulation-cadence improvement in that local interval, not a rendering-FPS or
internet benchmark. The input barrier and lack of prediction still remain.

## Required next

1. Smooth presentation error offsets separately from corrected physical state;
   the current small-error path relies on existing transform interpolation.
2. Remove the all-peer input barrier and single-update correction wait through
   held-input handling, snapshot buffering and prediction where needed. Current
   input transport remains delayed and still waits for every participating peer.
   The initial single-update acknowledgement restriction has been removed;
   overall internet pacing still needs verification and input transport work.
3. Synchronize gameplay-critical enemies, props, damage, death, resources,
   scripted events and loading. Player position correction alone is insufficient
   for a campaign with independent script/physics worlds.
4. Exercise aiming, combat, death and multiple levels,
   then joining/rejoining and the full 16-player roster.

No full world authority, prediction or rollback is claimed. The normal Room
menu now enables host correction automatically for hosts and guests; the
low-level Room adapter retains its explicit mode negotiation. Inputs still
wait for every participant. The isolated probe remains available through
`scripts/start-private-room-pair.ps1 -Gameplay -NativeButtons -HostCorrection`;
`-HostCorrectionDrift` adds a one-shot native fault for correction checks.

## Normal internet lobby and damage checks

The final executable SHA256 is
`21E9643693E41CE767ED66B567EA74AAACF718F8434DD9272C77D0F20BBD1673`.
`analysis/native-lobby-20261115-065014-fb17f4ce/result.json` records successful
normal-menu two-player admission into Autotopia through the public HTTPS relay
and a 20-second observation. Host correction ran on the guest with two players
and three other character proxies, with no missing proxies or active mismatches
in the sampled reports. Normal user files, configuration and crash report were
unchanged. This is a same-PC internet-relay smoke check, not cross-PC or campaign
validation; no gameplay input was sent during that capture.

`analysis/private-native-room-pair-20261115-063442-5562a589/authority-real-damage-result.json`
records host-only native Damage(p,1): host health fell from five to four, the
guest initially retained five, and correction restored four on the next native
observation. This proves health correction, not replication of hit reactions.

`analysis/private-native-room-pair-20261115-064128-303ab9e1/authority-fatal-result.json`
records native fatal damage on both peers, original GameOver and native respawn,
followed by movement of both characters. A later fall caused another natural
reload without stopping the connection. Host-only death and all other scripted
events remain unverified. Broader world authority and reconnect remain open.

## Corrected death timing (wire 13)

The demo-12 executable failed host-only fatal damage. Health reached zero on the
guest after update 180, so its original GameOver began at 181 while the host
began at 180. The exact terminal input boundary disconnected them. Allowing a
single extra terminal prefix update only moved the failure to native reload;
replaying native Damage after the guest update also retained the one-step skew.
Those failed candidates were not published. Their results are in the
`host-only-fatal-result.json` files in these private pair captures:

- `analysis/private-native-room-pair-20261115-070726-89053a31`
- `analysis/private-native-room-pair-20261115-071244-08c11f0f`
- `analysis/private-native-room-pair-20261115-071658-da4d9ebf`

The replacement retains the exact terminal input checks. A correction guest
receives the matching host result before its original native update, replays a
living player's authoritative zero through the retail Damage-export sequence,
then executes its native update. Position/velocity and other health corrections
still finish after that update. Receiving a result does not acknowledge or
fabricate native completion: the real local diagnostic remains separate and the
application acknowledgement follows original post-update completion.

Host loads cannot produce a completed old-world snapshot. Wire 13 therefore
adds a host-only, world/roster/tick/sequence-scoped outer-load notice. It lets a
waiting guest enter the matching native update, with no loader vote, teardown
agreement or invented completion. Both peers must still enter and agree the
actual native loader barriers. The new path passes 9,059 Room TCP checks.

`analysis/private-native-room-pair-20261115-072438-a7462f28/host-only-fatal-result.json`
records the successful native check with executable SHA256
`654E934975DDE686804C7ECA04DED50C749AF46271894AEBC09CDDF6E1EB5726`.
Only the host ran the fatal-damage fixture. The guest applied native death before
update 180; both entered GameOver at 180 and rebootstrap at cancelled tick 241,
raw first 242. Both characters moved independently after respawn; updates
continued past 1,600 with no gameplay stop and no missing/inactive world proxies
in the final sampled correction reports. Owned processes exited normally and
normal user files were unchanged.

The lethal correction uses the retail export's generic physical damage stimulus,
not the host's full original damage cause, instigator or event payload. General
combat-event replication and client-only false-death recovery remain unfinished.
This does not establish full campaign synchronization or rollback.

The same executable also passed native CleanLoadLevel to Mean Street in
`analysis/private-native-room-pair-20261115-072703-cfb6e396/authority-level-result.json`.
Both rebootstrap at raw tick 181. Input at 1177..1370 did not move the actors;
after the entry sequence progressed, independent input at 4149..4338 moved both.
There was no gameplay stop before owned shutdown. The guest logged a cancelled
host-state wait 14 ms after Window closing; this remains noisy shutdown reporting,
not a crash or a successful reconnect. Normal user files were unchanged.

## Damage authority

A guest-only fatal Damage fixture exposed the reverse failure in demo 13:
the host remained alive and corrected the guest's health back to five, but the
guest retained its native terminal control state. The host rejected that input.
The failed capture is
`analysis/private-native-room-pair-20261115-073310-37cdb941/guest-only-fatal-result.json`.

The next candidate suppresses independently generated native Damage broadcasts
targeting admitted human players in a correction guest's owned agreed update.
NPC and prop dispatch remains enabled until their event/lifecycle replication
exists; suppressing it now would prevent local destruction without providing a
replacement. Offline gameplay,
host damage, raw initialization and native teardown retain original dispatch.
Applying authoritative lethal state opens a thread-local native replay scope,
so original death listeners remain available and the scope unwinds on errors.
This establishes damage ownership; it does not replicate full damage payloads,
enemy AI, spawning, props, death causes or hit reactions.

`analysis/private-native-room-pair-20261115-073709-115574ee/guest-only-fatal-result.json`
records successful guest-only fatal-damage rejection with SHA256
`DE4069F071297BE65A7969687346394C5BA332CA955565F3C20C9061F252799A`:
health remained five, no terminal GameOver was entered, and subsequent input
moved both players. Normal user files were unchanged and both processes closed.
Host-caused death replay requires a separate live check before publishing.

The final player-scoped candidate SHA256 is
`CE06C9540FACFE7A7FA32E796CB867CA8D81AE6BBEFB621F2233BB53209A6AC6`.
`analysis/private-native-room-pair-20261115-074122-6d356d11/damage-authority-result.json`
records one continuous session: host-only fatal damage at 180, both original
GameOver entries at 180 and native respawn at 241/raw242, followed by guest-only
fatal damage at 360. Exactly the two new player handles were suppressed; other
native Damage callbacks still ran. Both players then moved independently and
the session continued past 1,000 without a gameplay stop before owned shutdown.
This also checks that the native replay scope closed after the real respawn.
Normal user files were unchanged. Direct death paths outside this dispatcher,
enemy AI/lifecycle and complete combat payload replication remain unverified.
