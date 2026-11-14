# Direct native movie playback in online sessions

The original `PlayMovieInternal` route uses control state 1; `StartSequence`
movies use state 2. They have different input handlers and entry boundaries.
Direct playback now uses an agreed media clock, synchronized hold-to-skip,
and the original completion/exit path. The new held-skip acceptance capture passes the complete post-movie
observation described below.

## Native implementation

State-1 input handler `82B1EDF8` accepts CutScene prompt/start/stop actions and
stores allow-skipping (+21), held (+20), configured duration (+16) and timer
(+24). Original update `82B1ED88` only queries playback and latches completion;
it contains no held-duration consumer. The separate ending-movie update
`82B22548` consumes the corresponding hold fields and configured duration.

Agreed UI intent reaches the original state-1 profile callback. The receiver
pins native mode, object, table, context, subscription, controls generation and
thread. A carried entry button must release, every human must certify the UI
phase, and original private action interpretation determines prompt and hold.
Only audited timer-start return site **82B1EF94** stamps +24 with agreed time.
Unrelated clocks keep native time.

The new consumer runs once from `NativeRoomGameplay::Deliver`, after its neutral
input transaction and UI callbacks. It validates the receiver lease, current
agreed tick, original allow/held/duration fields and movie-clock ownership. It
rejects invalid, future or noncanonical stamps and invalid durations. When the
configured hold completes it calls original movie-stop routine `8283B5C0`.
Original movie update/exit handles completion, audio, controls and state return.
No completion flag, health, character transform or state stack is written
directly. The consumer requires an owned online receiver; offline playback is
unchanged. Visible prompt presentation has not been separately inspected.

Movie entry closes the already-delivered input transaction using unchanged
actor/controller ownership, preserving actual Motion receipts. The direct
fixture consumes masks 03/0004; queued playback consumes 00/0000. Neither path
fabricates omitted work or relaxes ordinary gameplay admission.

## Accepted natural playback and regression

Candidate `9679C4A23B2AD0CD6DB3FF0AD2EB056E7543FC070434F64D8599625A9FD15968`:

- `native-lobby-20261114-145131-800e26e2` passes `--direct-movie`: original
  export at tick 300, original movie camera, shared return 447/449, 1,601
  matching world updates and actual post-movie movement, reversal, jumping
  and landing. Health and normal user files remain unchanged; exits are normal.
- `native-lobby-20261114-145452-49ae2eb2` passes `--expect-skip`: queued-movie
  Accept denies the early press at 321, accepts at 371, and returns 372/374.
  The same 1,601-update and post-control checks pass without a held-A Jump.

## Held-skip validation

`-DirectMovie -CutsceneSkipInput` requests the original Havok logo at tick 300
and holds A from the extra human starting at source 319, continuing through
return. It changes neither the game setting nor the original movie asset.
Use:

```powershell
python scripts/verify-native-movie-sequence.py <capture> --direct-movie --expect-skip
```

The verifier reads the duration logged from native fields after each original
action. It requires a canonical hold stamp, stop at that duration, original
return, matching world/actor trajectories, restored movement and jumping,
no carried-button Jump, unchanged health/user files and normal process exits.
It rejects a natural movie end as evidence of a successful skip.

The rebuilt focused test exits 0 with 320 interpreter, lifecycle, callback-guard
and hold-timing checks, including configured half-second and three-second
boundaries, released/unskippable holds and invalid clocks/durations. This is
separate from native multi-client acceptance.

Accepted held-skip candidate:
`7A479099A775DFE6DAAEADB0CD2A04F77F902E75E0D600D88E8EF734A77B41DC`.
Capture `native-lobby-20261114-152152-1e983c5d` passes the command above:

- Native prompt at 321; hold-start at 329, with stamp 429000000.
- Both native settings samples show duration 3.0 and allowed=1; the hold sample
  shows held=1. Original movie stop occurs at tick 419, exactly 90 ticks later.
- Original state return is source 421 / first delivery 423 on every client.
- All 1,601 compared updates and actual retail/extra-player trajectories match.
  Post-skip movement, reversal, jumping and landing pass. No carried-A Jump
  occurs; health remains unchanged. All processes exit normally and normal
  user files, configuration and crash reports remain unchanged.

The final focused process (`native-direct-movie-skip-duration-focused.log`) exits
0. The verifier also rejects all three late-hold/host-timed diagnostic captures
as proof of a successful skip. These results establish one remote-player
request through the configured native hold and original return in three local
clients; they do not establish the remaining coverage below.

## Diagnostic captures

- `144250-632826ed`: fixture mistakenly prefixed Movies; the engine added it
  again and the file open failed. The original API requires the basename.
- `144705-34d8b973`: file opens; ordinary retail completion rejects controls
  disabled by movie entry. Fixed by the phase-specific completion boundary.
- `150630-176c6417`: UI callbacks arrive, but a call-instruction/return-address
  mix-up leaves the timer host-timed. The corrected guard uses EF94, not EF90.
- `151122-ad9a2aa5` and `151708-eee32c90`: canonical timer, natural completion.
  Read-only field sampling in the latter establishes a native duration of 3.0
  seconds. Starting the hold at 379 leaves only 68 ticks before natural end,
  insufficient for the configured 90 ticks. These captures do not establish
  that the original UI-hook scheduling itself caused failure. The explicit
  agreed-delivery placement is retained as the clearer ownership boundary.

These are local `native-lobby-20261114-` captures. Logs, original content,
executables, saves and private diagnostics are excluded from source publication.

## Remaining coverage

This covers one movie in three local native clients. Other movies, configured
hold durations, a live unskippable direct movie, nested scenes, high-rate video
presentation, audio/network stalls, credits, startup movies, Internet recovery
and complete campaign traversal still require validation or implementation.
No full online campaign or sixteen-native-client claim follows from these tests.
