# Disconnect recovery development status

The native HUD recovery prompt is unfinished and is not included in demo.6.
Its button queues a request; a checked native application boundary must validate
world completion before the original title load and offline-save refresh run.
Controller-only recovery and in-place reconnect are not implemented.

The isolated four-process test entered gameplay and stopped at an injected
disconnect. The backend exposed the available recovery state, but the authored
HUD did not issue its request. The capture timed out and did not pass. The
halted-world UI dispatch needs investigation. Earlier automatic-return tests do
not validate this interactive prompt.

The private RecoveryPrompt test mode exercises the actual authored button and
requires its request before accepting title re-entry. It is disabled by default.
The harness also checks authored UI hashes before reusing content snapshots.

The recovery request unit checks and HUD bytecode roundtrip passed. The broad
native regression log reached its final check after correcting a stale expected
error message; its process exit code was not retained reliably. This does not
establish live prompt acceptance or complete online campaign compatibility.
