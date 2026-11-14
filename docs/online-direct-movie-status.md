# Direct native movie playback in online sessions

The original `PlayMovieInternal` route uses native control state 1, while
`StartSequence` movies use state 2. Both need an agreed playback clock and an
explicit interrupted-input completion boundary when movie entry pauses the
world after input delivery. This document tracks the direct route separately.

## Entry failures and correction

The first private fixture passed `Movies/LOGO_havok.bik`. The original engine
adds the Movies directory itself, producing `D:\Movies\Movies\LOGO_havok.bik`.
That failed open was a fixture error. The fixture now supplies the basename
`LOGO_havok.bik` to the original Lua export.

After that correction the original movie opens and the canonical playback clock
is admitted, but ordinary retail button completion rejects the actor lease.
Direct movie entry disables gameplay controls immediately. Gameplay preflight
correctly disallows that state; completion must instead establish that the
already delivered input still belongs to the same world and actors.

The movie-entry completion path therefore requires the current agreed and
published tick, a validated native state-1/2 movie object, and unchanged retail
actor/controller ownership. It still checks both delivered button lanes and
exact prepared commands. It does not authorize callbacks, mark omitted Motion
updates as consumed, write player state or relax ordinary gameplay admission.
The Jump adapter uses the same explicit completion boundary. The coordinator
retires the interrupted extra-player transaction with its actual consumed mask.
In the direct fixture the native entry update consumes both retail Motion lanes
and the extra-player lane (masks 03/0004); the queued fixture consumes neither
(00/0000). These are actual per-lane receipts, not fabricated equivalence
between the two native transitions.

`-DirectMovie` in `scripts/test-native-room-menu-roster.ps1` calls the original
`PlayMovieInternal` export at tick 300 in three isolated native clients. The
movie verifier accepts `--direct-movie` to check that route independently.

## Validation status

Candidate `9679C4A23B2AD0CD6DB3FF0AD2EB056E7543FC070434F64D8599625A9FD15968`
builds successfully. The focused online-input process exits 0, including a
negative check that ordinary gameplay cannot request the movie-only completion
read to obtain an actor lease. Its log reports 69 retail Movement checks and
42 cutscene epoch/queue checks.

Accepted capture `native-lobby-20261114-145131-800e26e2` passes:

```powershell
python scripts/verify-native-movie-sequence.py <capture> --direct-movie
```

All three native clients enter state 1 at tick 300, retain the original movie
camera, and return at source tick 447 / delivery 449. The verifier compares
1,601 matching world updates and actual retail/extra-player trajectories,
including post-movie movement, reversal, jumping and landing. Health remains
unchanged; no extra-player Jump is dispatched during playback. All processes
exit normally, and normal user files, configuration and crash reports remain
unchanged. No screenshots or normal-session gameplay inputs were used.

Direct-movie skip forwarding is not implemented. Other direct movies, audio
under stalls, high-rate video presentation, Internet recovery, credits, startup
movies and complete campaign playthroughs still need work. This is a native
three-local-client acceptance check for one movie, not a full online claim.

Failure captures:

- `native-lobby-20261114-144250-632826ed`: duplicated Movies directory; failed open.
- `native-lobby-20261114-144705-34d8b973`: corrected filename opens, media clock
  starts at tick 300, then ordinary retail completion rejects the disabled
  gameplay lease with result 10 on all three clients.

Local logs, content, executables, saves and private test data are excluded from
source publication.

## Queued-movie regression

The same candidate also passes `--expect-skip` on
`native-lobby-20261114-145452-49ae2eb2`. The original queued-movie handler denies
the early press at tick 321 (2/3 second elapsed), accepts the later press at
371 (7/3 seconds elapsed), and all three worlds return at source 372 / delivery
374. Its 1,601-update comparison includes actual post-skip movement, reversal,
jumping and landing, unchanged health, and no Jump from the carried skip press.
All exits are normal and normal user files remain unchanged. This checks the
shared completion change against the already accepted state-2 behavior.

The verifier also rejects both failed direct-movie captures listed above.
