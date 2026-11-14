# Online netcode requirements

Packaged copies compare the release version declared in `release-manifest.json`,
not executable hashes. Matching versions can connect despite different executable
bytes; prerelease identifiers must match, while SemVer build metadata is ignored.
Protocol, game-content, session and checkpoint checks remain independent. An
unversioned development tree retains executable-hash compatibility checks.

The current experimental native Room uses deterministic input lockstep and
completed-world verification. It has separate UDP voice delivery and native
text chat. Prediction, rollback/replay, authoritative correction and general
world resynchronization are not implemented. A verified gameplay mismatch
currently stops the session. Online demo packages enable the native lobby through
the supplied hosted-relay configuration. The EM1 preview remains offline.

Native checkpoint-memory reload now coordinates unload and fresh actor/input/
camera admission across the frozen Room. Two four-process native captures
passed, including 1,148 consecutive matching updates after re-entry in the
clean-default build, active movement/jump/tool callbacks and normal shutdown.
An additional four-process capture passed three consecutive checkpoint reloads,
with 661/698/698/1,068 matching native updates in its four world incarnations.
This verifies repeated same-level checkpoint reloads, not arbitrary transitions or
full campaign recovery. A separate Autotopia-to-Mean-Street load agreed across
all peers but never reached fresh playable actor admission; that capture is
rejected. See `analysis/online-native-cross-level-entry.md`.
Invalidated bindings still cannot be reused, and
shutdown drains failure diagnostics before stopping guest threads. The earlier
safe-failure baseline remains in `analysis/online-stopped-room-rendering.md`.

Room now also has a sixteen-peer ordered loader-rendezvous transport API:
exact consumed tick and session/world/roster scope, nested Begin/End ordering,
all-peer host agreement, proof blocking and a nonextending deadline. The
extended loader/rebootstrap path passes 186 actual-socket checks. A native
loader bridge is enabled for retained Native Room sessions through
`em2_room_native_reload`, with fresh controls/actor admission and Room wire
version 7. Unsafe transitions and mismatches still stop; no completion digest
is invented for the cancelled transition frame. See
`analysis/online-native-world-reentry.md` and `analysis/online-room-loader-barrier.md`.
Native camera visibility is deferred as a whole transaction during unavailable
online render bindings, preserving its cached state until a valid model can be
used. The four-process missing-collision debug load no longer faults in the
camera hide helper; it remains rejected as unplayable. Evidence and a separate
early-startup failure are recorded in
`analysis/online-native-camera-visibility-reentry.md`.
The offline level browser and console now distinguish nine incremental zone
states from load requests. Wrong-level zone requests are rejected in the actual
native VM before changing state, rather than clearing their prerequisite world.
This corrects a direct-entry error but does not repair Mean Street's remaining
player placement. See `analysis/native-level-state-loading.md`.

The release target is host-authoritative cooperative play for up to sixteen
players, with local prediction and rollback/replay, latency handling, and
recoverable state synchronization. Fixes to individual deterministic engine
bugs remain useful, but do not fulfill those requirements on their own.

## Authority and state

The host owns campaign progression, objectives, actor creation/destruction,
NPC decisions, damage, pickups, character identity and color allocation.
Clients send validated input intentions for their own admitted actor. Wire
identity uses stable native entity IDs plus spawn generations and world
epochs; native addresses never become remotely supplied write targets.

State replication needs versioned schemas, acknowledged snapshot baselines,
bounded delta history and a full-state fallback when a baseline is missing.
Lifecycle and campaign events need ordered, idempotent delivery. High-rate
movement snapshots can be sequenced independently of reliable events and voice.

Every native state schema requires actual engine capture/restore support.
Restoration must account for physics contacts/broadphase, scene and Motion
caches, action state, owned resources and entity lifetimes. Matching body
positions alone does not establish a corrected world. The reproduced NPC
face-target failure demonstrates why cached direction/action state matters.

## Prediction and rollback

Current Room now retains the latest 256 consumed gameplay frames with exact
session/world/roster scope, local completion proofs and ordered host verification.
Only complete verified ranges are readable; missing/expired/unverified data is
never substituted with neutral input. This supplies input-history preparation,
not native state restoration or replay. Evidence: analysis/online-input-history.md.

Local input needs immediate native simulation with a bounded history of input,
native actor state and authoritative tick acknowledgements. Correction restores
the host state at the acknowledged tick and replays the remaining inputs through
the original native action/physics paths. Presentation smoothing follows the
corrected simulation state. Remote actors require snapshot interpolation with
bounded extrapolation appropriate to packet loss.

Replay must not duplicate damage, pickups, objective advancement, audio or
effects. Each consequential event needs a stable transaction identity and an
explicit relationship to host acknowledgement. Rewind/replay must use safe
native execution boundaries and preserve PPC context and host FP environment.
Input/history gaps, actor respawn and world transitions need explicit policies.

## Recovery and error handling

The Mean Street startup uses the original loading wait before its interior
teleport, installed in the actual module environment with VM stack checks.
Camera and input validation now accepts proved native camera graphs, movement
frame handles and unrelated observer subscription changes while retaining
actor identity and action-definition checks. The four-peer normal campaign
transition to Mean Street passes: 1,163 consecutive matching completed updates
after fresh admission, plus native movement/reversal/jump/landing/tool callbacks
for both extra actors in every replica. The debug CleanLoadLevel comparison
remains rejected: it disables abilities for both retail and extra actors.
Normal gameplay uses TransitionToLevel; no ability flags or physics are forced.
This verifies one transition, not arbitrary transitions, party travel through
every scripted door, campaign completion or state correction. See
`analysis/online-native-authored-camera.md` for captures and limits.

Malformed packets require validation before allocation or native calls: length,
version, sender authority, session/world/spawn epochs, sequence ranges, finite
numeric values, resource limits and snapshot baseline availability. Duplicate,
stale, reordered and missing packets need distinct handling.

Corrections and full resynchronizations require bounded attempts and deadlines,
complete-state validation, atomic publication at native safe points, and an
explicit success acknowledgement. A partially restored world cannot resume.
Reconnect must restore current authoritative state and retire the previous
connection/actor generation. Unrecoverable failures need a useful explanation
and an exportable report containing build/protocol identity, tick, affected peer,
last agreed state, correction attempts and diagnostic context.

Current Room mismatch errors include the full 64-bit tick, actual peer, last
verified tick and both proof hashes. This improves diagnosis; it is not state
recovery. Existing private captures and strict mismatch detection remain evidence
for development. They cannot substitute for real prediction/correction tests.

The first transport failure now freezes an owner-thread diagnostic snapshot
before connections/queued inputs are released. It preserves the original
phase, full-width ticks, verified/pending boundary, roster mask, build/content/
checkpoint hashes and exact bootstrap/gameplay mismatch proofs. Repeated
disconnects and later native errors cannot replace that first cause. Native
status copies the bounded report for thread-safe support preview/export; it
contains no invitation/session nonce, address, player name, chat, media key,
save bytes or guest pointer. This is diagnostic context, not a restorable
world snapshot or automatic recovery.

Checked original save-root restoration is available for the future leave
consumer. It refuses active save-package routing changes, validates the native
summary cache and reuses the manager's original root. Native content-manager
round-trip fixtures pass without changing the offline save. The operation is
not yet connected to world unload, SaveManager refresh or lobby reopening. See
`analysis/online-content-root-restoration.md`.

Native leave preparation now distinguishes completed local post-update from
authoritative consensus. A private four-process partial-delivery test rejects
retirement eligibility at tick 500 with only tick 499 physically completed;
ordinary disconnect survivors separately pass the completed-update check.
No failed native update is published complete. Direct native disconnect now
freezes the original report phase before teardown. These are verified boundary
guards, not a working return-to-title action or restoration. See
`analysis/online-native-leave-boundary.md`.

Error handling must distinguish failures instead of hiding them behind a catch-all:

- Lost voice frames use bounded concealment/silence; media-device or codec failure
  disables that media path while the game and text chat continue.
- Invalid optional messages are rejected before touching native state. Repeated
  abuse needs rate limits and peer isolation, rather than unbounded retries.
- Missing authoritative state pauses affected simulation and requests a bounded
  full-state recovery. Never mark a mismatching world verified to keep it moving.
- Failed restoration leaves the old world unpublished and offers a clean return
  to the lobby. Retry budgets cannot reset indefinitely on partial progress.
- Native access violations, corrupt ownership and unsafe physics state are not
  recoverable by swallowing an exception. Preserve a crash report and terminate
  safely when a coherent native world cannot be established.

Each recovery path needs deliberate failure injection and evidence that another
peer remains healthy. Diagnostics must exclude invitation secrets and media keys.

A private completed-update disconnect test now returns three surviving native
sessions through the original title loader, retires their extra cameras, and
restores their original offline storage and SaveManager state. A saved host and
empty guest profiles pass separate native archive checks, with unchanged save
bytes. This return path remains disabled by default and is not reconnect or
world-state correction. Manual lobby re-entry, repeated recovery, interrupted IO
and unsafe-update recovery still need validation. Evidence and rejected earlier
candidates are recorded in `analysis/online-native-title-return.md`.

The latest native transition investigation also separates presentation reads
from world proof: the local extra-player health HUD uses a substituted actor,
whereas canonical resource observation must sample the same retail actors on
every peer. The scoped resource-read guard preserves the ordinary HUD and
original health getter. The debug-loader comparison matches 3,305 completed observations
(2,641 after re-entry) but still fails playable jump acceptance. A later normal
campaign-transition capture passes the unchanged gameplay criteria, including
jumping and landing, with 1,163 consecutive matching updates after re-entry.
Both observations and active gameplay evidence are required. Camera-mode
and actor-subscription validation now permits legitimate authored transitions
without weakening actor identity or action-definition checks. Evidence is in
`analysis/online-native-authored-camera.md`.

## Acceptance

- Actual native engine capture, restore and replay, including collision and
  gameplay side effects; fixture-only state math does not prove this.
- Injected latency, jitter, packet loss, duplication, reordering, missing
  baselines and corrupted/stale data. Measure control latency and corrections.
- Recovery from deliberate actor/world mismatches, with coherent native state
  and no duplicated effects or hidden hash exceptions.
- Sixteen actual independently controlled game sessions, then separate physical
  machines and real remote networks. Socket count alone is not this proof.
- Campaign objectives, cutscenes, transitions, deaths/respawns, reconnects and
  host/session shutdown across a complete playthrough.
- Native lobby/voice/control UI, unique pants/name colors, name tags,
  invite-only permissions, public matchmaking, Discord invites and easy joining.
- Explicit microphone opt-in, devices/PTT/muting/levels/speaking indicators,
  party and proximity voice with actor/camera/world ownership validated.
- Authenticated Internet session setup and relay/NAT behavior. Current UDP
  encryption does not secure the existing unencrypted Room control connection.

These are unfinished release requirements. Current implementation evidence is
recorded separately under analysis/online-native-chat.md,
analysis/online-voice-audio.md and analysis/online-voice-udp.md.
