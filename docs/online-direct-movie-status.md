# Direct movie playback: development status

This branch contains unfinished native online support for the original
`PlayMovieInternal` route (control state 1). The tested queued-movie route
(control state 2) remains on main. This is source work in progress, not a demo
release or a claim of complete online campaign support.

The changes validate direct movie objects and distinguish their native input
handler from the queued-movie handler. Movie entry, retained ownership and
playback-clock checks now recognize both native routes. The private three-client
harness has a `-DirectMovie` fixture that invokes the original Lua export rather
than writing playback state directly.

## Validation and known failure

The canonical executable builds successfully and the focused online input test
exits 0. The three-native-client run invokes the original export successfully
at tick 300 on each client (Lua status 0, stack preserved), but all three stop
with `Native retail button/jump completion rejected (result 10)` during movie
entry. The observation therefore fails; playback, return to gameplay and
synchronized direct-movie skipping are not established. Direct-movie skip
forwarding is not implemented.

Local diagnostic capture: `native-lobby-20261114-144250-632826ed`.
Candidate SHA-256:
`3D454B7F5AA510EF37E74649815DFEE9F868F121F8E28513122A5E102682F130`.
All three test processes exit normally and normal user files, configuration
and crash-report files remain unchanged. Local logs, game content, executables,
saves and private test data are excluded from this source publication.

Next step: diagnose why the original direct-movie transition fails the
interrupted retail input completion checks, then repeat the live fixture before
merging into main. Natural playback and held-skip completion need separate
acceptance checks.
