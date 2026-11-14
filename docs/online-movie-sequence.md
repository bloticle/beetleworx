# Native online movie sequences

The source now supports the original `StartSequence` movie queue after native
online gameplay admission. It retains the original Bink decoder and native
movie state, with an agreed clock for playback scheduling. This is separate
from interactive cinematic skip routing. Movie Accept now has
[separate skip-delay and callback coverage](online-movie-skip.md).

## Entry and return

Original 82772F20/82772D50 configures control state 2. Its object table is
8215B2B0 and input handler is 82B1DF70. That same state accepts `.bik`,
MainMenu, EndGame, StartScreen, Credits and empty entries; it is not a generic
Gamebryo animation player. Online admission checks the original state target
and allows only movie entries, classified as kind 0 by original 82B1D850.
An empty completed queue is accepted only for a previously owned movie target.

Starting the movie can pause the world partway through an admitted update.
Retail and extra-player input callbacks have already run, while the engine
intentionally omits subsequent Motion callbacks. The adapter closes this as
a distinct interrupted transaction. It records the actual consumed masks,
retains quarantine and never marks omitted work consumed or replays it later.
Ordinary frames still require complete native Motion delivery.

During the movie, typed no-action frames retain the existing even input
epoch. Gameplay camera look and local view replacement defer to the original
movie view. Original completion restores gameplay; fresh producer/receiver
leases and the existing delayed-prefix and held-button fences handle return.
Actors, physics transforms and the world are not replaced.

## Playback clock

Local Bink completion previously differed by one update between clients. In
the failing capture below, peer 2 resumed the world at tick 452 while the host
remained paused. Merely allowing the input state did not fix that desync.

`online_movie_clock.cpp` scopes original MovieManager open/query calls
8283BB58 and 8283C210 to the owned native movie. At nine audited Bink playback
timer return sites, original 8333AC20 runs and retains its native effects;
its returned millisecond value then uses agreed 30 Hz time. Between native
updates it holds the last completed tick. The scope validates the live manager,
current Bink handle, context, world owner and tick. Decode/open performance
measurements and native busy-wait timers keep their original time source.
The decoder, native frame selection and completion logic remain original.

## Validation

The private fixture invokes `StartSequence(GetPlayer(),"LOGO_havok.bik:0",
true,false,"")` through the original Lua export at agreed tick 300. The retail
file has 284 frames at 2997/50 FPS. No movie assets are included in this repo.

```powershell
scripts/test-native-room-menu-roster.ps1 -Players 3 -ScriptedInput -CaptureShutdownHang -MovieSequence -CheckpointReloadTick 300 -CheckpointReloadCount 1 -PipelineCreationThreads 2 -ObserveSeconds 90 -StartupTimeoutSeconds 480 -ReuseVerifiedContent <verified-private-capture>
python scripts/verify-native-movie-sequence.py <capture-directory>
```

Evidence from the isolated native runs:

- `analysis/native-lobby-20261114-133744-46bd1ad2`: original movie entry
  reproduces the incomplete retail Motion failure at tick 300.
- `analysis/native-lobby-20261114-134415-531ac8ca`: interrupted completion
  succeeds with actual masks 00/0000; gameplay camera binding then fails at 301.
- `analysis/native-lobby-20261114-134713-46bfed0b`: entry and movie view pass;
  local playback completion diverges at tick 452, last verified 451.
- `analysis/native-lobby-20261114-135645-e78ea324`: final candidate
  2F4336727771076394BC887844605D81195EA98DFA819499E9CC104EF404086D passes the
  verifier. All three worlds return at source 447 / delivery 449, with 1,601
  matching updates and matching actual actor trajectories through post-movie
  movement, reversal, jumping and landing. Health remains unchanged. Normal
  user files, configuration and crash reports remain unchanged; exits are normal.

`analysis/native-movie-clock-focused.log` also exits 0: 116 clock fraction,
wrap and caller checks, 37 cutscene epoch/queue checks, 294 original skip
interpreter/guard checks and the ordinary incomplete-frame rejection checks.

## Remaining coverage

This proves one original movie sequence in three local native clients.
Playback scheduling currently follows the agreed 30 Hz update clock; smooth
high-rate movie presentation and audio behavior under network stalls need
separate checks. Other movies, queued multi-movie sequences, other skip configurations,
other direct-movie skip configurations, startup movies, credits/end-game/menu routes, Internet
stall recovery and a complete online campaign still require validation or
implementation. Original state-6 authored cinematic completion remains a
separate native test. No broader release-readiness claim follows from this run.

The separate [direct state-1 route](online-direct-movie-status.md) now has
three-client natural-playback and restored-control coverage through the original
`PlayMovieInternal` export. It has a different entry-consumption boundary and
its own verifier mode; its skip path has separate coverage.
