# Indexed native enemy spawns (private)

## Failure and audited caller

The private Autotopia test
`analysis/private-native-room-pair-20261115-125115-cbc5c15c` failed at tick 451
in `tcy_autotopia_visit1.sectionc_first`, before the scheduled clean load of
`tcy_autotopia_visit1.main` at 700. Guest native spawn capture did not equal the
host's complete parent payload. This is a failed gameplay test, despite both
isolated processes subsequently closing normally with unchanged normal files.

Both roles entered parent `827B3C60` from `827B4B40`, with the same owner/anchor
`0013D0B5/00000000/00000000/00000000`, selector 0, flag 1, no collection,
and pre-state count/choice/used 1/1/1. The host CRT state was `C7BEE89D`, while
the guest had `7D26A250`. The resulting Ranged Blotworx GUIDs differed, as did
post-call random state. Earlier authority preparation covered Shooter and Lua
collection callers, so this native caller ran independently.

Generated original `827B4AB0` has a bounded indexed spawn loop: it reads an
index through the component interface, calls the parent with r7=0/r6=1, ignores
the returned entity, checks component state and continues its original loop.
The adapter now admits this exact return site and ABI into the existing strict
host-input/RNG preparation. It still requires matching native owner, anchor,
pre-state and host command order; original construction, callbacks and loop
bookkeeping remain in place. Full captured parent equality is still required.
No generic arbitrary engine RPC or unchecked counter overwrite is introduced.

## Test input after a load

Source review confirms that completed native reload calls Detach/Configure,
which constructs a fresh private SDK input driver. Its movement/tool schedule
already restarts in the new world. An added per-world scope-reset experiment
was redundant and was removed; the repeated live candidate below includes that
opt-in test-only experiment, while the final source retains the existing driver.
No public input behavior changes are needed for this spawn fix.

## Limits

This change prepares matching indexed calls. Reconstructing an entirely missing
indexed outer loop, its continuation and all script/quest side effects is not
established. Native missing-spawn replay still rejects unsupported parent
shapes. Combat, transitions and full campaigns need separate evidence; a
matching spawn does not prove those systems are synchronized.

## Repeated live result

`analysis/private-native-room-pair-20261115-125434-4890e9d6` passed the
previous tick-451 failure: both original calls began with CRT `C7BEE89D`,
produced Ranged Blotworx `0000188A/000051E9/00001D76/00002300`, ended with
CRT `B10A729A` and counters count/choice/used 2/2/1. The guest's complete
parent-payload equality check passed, including ordered captured children.

Both performed the scheduled clean load at tick 700 into Autotopia Main and
continued through final complete observations host 2095 / guest 2093 before
requested window closure. Post-load SDK input logs include jump, camera,
movement after camera turn, tool X at 982 and tool RT at 1032, finishing that
program at 1062. This proves delivery through original interpreters, not that
every tool hit or enemy encounter was manually inspected. There were 1395/1393
host/guest observations in the loaded Main world before closure.

No native Room stop, AI mismatch or spawn replay failure was found before
`Window closing, shutting down...`. The guest then logged a missing host update
while cooperative exit cancelled its wait; this shutdown diagnostic is retained
in `indexed-spawn-result.json`, not silently treated as an error-free run.
Both processes exited 0 and normal files were unchanged. The live candidate
hash was `731FB195E97CB37F67B7876E028B1A4B6D7CDF8E947AC3C21FF2C81C20CDFE8E`;
it still included the redundant opt-in SDK scope reset described above.

The final source removes that test experiment, retains the indexed-spawn fix,
and builds successfully. The final canonical executable passed 9095 Online Room
checks with exit code 0. This is transport regression evidence, distinct from
the live original-spawn verification above. Final executable SHA-256:
`25C7AE324B2EA79F08FAE1BEE9DB5AC56648CB83B999B3D33936C9AC6C1F1099`.
Local regression log: `analysis/native-indexed-spawn-final-1f300fd7cb5848e8a0b5b639a59be92c/rollback.log`.

No new public demo was published. The authority adapter remains experimental
and default-off; this result does not establish rollback, late joining or
complete enemy/campaign synchronization.
