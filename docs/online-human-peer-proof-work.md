# Native human identity in world proofs

This is an online re-entry repair in development. Full campaign/world authority,
prediction, native rollback and reconnect remain incomplete.

## Reproduced mismatch and independent reconstruction

Three-world roster `analysis/private-native-room-roster-20261115-160325-e130fa41`
reaches verified 699, loads Gulch at 700, and fails bootstrap at 701. Host and
guests log matching world names, modes, player positions, resources, random
calls/seeds/stream, and all character physics. The extra human native GUID agrees;
the two retail native human GUIDs differ. Character records are sorted by raw
GUID, so the corresponding record order also differs.

`analysis/reconstruct-bootstrap-701.py` independently reconstructs all three
EXACT original v5 SHA-256 digests from those recorded native values. This proves
that the local human identity bytes and their sort order explain the full hash
mismatch; no live proof was changed. Evidence:
`analysis/private-native-room-roster-20261115-160325-e130fa41/bootstrap-701-reconstruction.json`.
The proposed typed encoding of those same physical states independently hashes
to `b6fc8bb3932c989efdc642e407e0bdc74cf546c6d10c7642947b5c257879d34e`.
The runtime fixture checks that exact encoding against the recorded Gulch state.

Native Room already identifies human Damage targets, spawner anchors and launch
owners by stable room peer, rather than assuming native runtime GUID equality.
The world proof must use the same explicitly owned human reference semantics.
NPC identities remain native GUIDs; they are not normalized away.

## Implementation

Observation domains v7-peers and v8-passive-peers carry typed human references
by room peer, distinct from typed NPC GUIDs. Room wire 29 rejects earlier proof
semantics at connection time. Legacy v4/v5/v6 observation domains remain available
when no Native Room human binding metadata is provided.

Before hashing, the native owner binds both actual PlayerManager handles through
a stable native registry snapshot and matches their observed GUIDs. Every extra
human must match its generation-pinned input registration, registry, entity,
controller, listener, handle and action profile. Bindings must cover the exact
locked room roster mask, with distinct native identities and unchanged registry
lifetime. This reads actual actor ownership; metadata cannot nominate an arbitrary
NPC as a human. Local bindings are published transactionally and logged when
changed. Native GUIDs, handles, registry entries and physics are never rewritten.

The proof validates raw input record ordering/uniqueness, then encodes verified
human records in peer order and remaining NPC records in GUID order. It includes
peer membership and every original active/state/position/velocity field. Retail
positions must still match their actual native body records. Missing active human
bodies, invalid/duplicate/foreign peer metadata, nonfinite data, partial coverage
and inactive garbage fail. Explicit passive domains retain complete human peer
membership while allowing the original authored detached-body phases.

## Verification

Regression fixtures cover remapped local human IDs, all sixteen peer slots,
independent NPC identities and every NPC physics field, extra-human physics,
invalid bindings, passive membership and the independently encoded real native
Gulch state. This does not prove live sixteen-client campaign support.
Candidate executable SHA-256:
`C18DE82E03D3A3687C8B436442944741D7D639E90E19364F2A2860A6D8CC6271`.
Same-executable regression exits 0: 81 native character/typed-proof checks,
18 visual retirement guards, 147 reward codec checks and 9108 Room transport
checks. Evidence: `analysis/native-human-peer-proof-test-result.json` and its log.

## Native three-world result

Roster `analysis/private-native-room-roster-20261115-161839-e8df734e` uses three
actual native worlds, mixed human characters and scripted SDK inputs, with the
full private Damage/AI/birth/retirement/impact/launch authority preset. Gulch
loads at 700 and all peers agree at fresh admission 701. The native retail GUIDs
still differ exactly as in the failed run: host P0 00006D99/.../000065FA versus
guest P0 00003E06/.../000015F5, and host P1 00005E64/.../00000B61 versus guest
P1 00005222/.../0000432B. The extra actor GUID is 0000268E/.../00005281 on all
three. Actual validated ownership maps those locals to the same peers. No GUID
is renamed. The real v7 hash on all three is the independently predicted
`b6fc8bb3932c989efdc642e407e0bdc74cf546c6d10c7642947b5c257879d34e`.

Autotopia loads at 1400 and all peers agree at 1401 with hash
`976d1dda497dd134bd6f62e29e1ad0b7bbfdf9e43d6895bd7181aec4cbe9940e`.
All peers continue beyond tick 5800; the preserved pre-shutdown complete
observations reach 5874/5878/5886 respectively. No online/native observation stop
is present before intentional shutdown. The earlier drop and particle-drain
repairs remain enabled and pass in this same route.

Evidence: the roster's `peer-proof-live-evidence.json` includes both exact load
proofs, corresponding native peer bindings, later observations, pre-shutdown
failure audit and each process's normal exit. All three close normally, exit 0,
with normal user saves/settings unchanged. This establishes this three-player
route, not sixteen native clients, arbitrary levels/latency, full campaign
combat/quest synchronization, prediction, rollback or reconnect. Public demo.15
is unchanged; general world-authority adapters remain private/default-off.

## Four native worlds and Rainbow Falls entry

The four-world transition route at
`analysis/private-native-room-roster-20261115-162805-d9c79c63` admitted Gulch,
Autotopia and Mean Street North visit 2. Loading `rbc_falls.main` next did not
admit the extra actors: the original start markers have no supporting floor in
that base-zone state. A clean-load comparison reproduced the problem. This was
not a stale Mean Street spawn transform.

The original `levels/master.lvlc` shows that `rbc_falls.entrancev1` also loads
`entrance`, `rbfvisit1` and the visit-1 enemies. The standalone level browser and
validated `level` command now resolve Rainbow Falls Main through that original
entrance. Raw Lua level APIs still permit exact internal states. Other catalog
destinations remain unchanged.

Four actual native worlds using the full private authority preset and mixed
characters entered that state in roster
`analysis/private-native-room-roster-20261115-164647-36371199`. The intro briefly
had no live character hull; admission waited rather than manufacturing one.
All four produced their first v7 observation at tick 714 with digest
`1c1debaa8817a3d6d2e2b658630ccde42b05f2705acd7ab339a446797385fd76`.
At tick 6000 all four still agreed on
`a95a8573918fc350173b9247b41ab481c47f210c6c30e9555823bd33f67584e3`.
The first intentional window close was 16:52:18.137 UTC; the recorded socket
stops occurred afterwards, beginning 16:52:18.328 UTC. All processes exited 0
and normal user files remained unchanged. `entrance-evidence.json` preserves
the exact observations, stop messages and completion records. This is entry
and sustained synchronization evidence, not completion of Rainbow Falls or
sixteen-player campaign acceptance.
