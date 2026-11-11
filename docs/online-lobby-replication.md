# Online lobby replication

The development transport uses wire version 8. Both peers must use the same executable; older wire versions are rejected during handshake.

`Session` now owns the lobby roster on the host and replicates accepted snapshots to the client. Name, character, color and readiness requests carry a monotonically ordered request identifier. Client requests never carry a target peer identifier: the connection determines which player may be edited. Requests are acknowledged explicitly, so queueing a request does not imply the host accepted it. An occupied color or invalid name is rejected without disconnecting the player or mutating the accepted roster.

Snapshots contain bounded, canonical UTF-8 names, character choices, unique palette entries, readiness and a revision. The decoder rejects duplicate peers/colors, malformed names, nonzero unused entries, invalid enums and inconsistent locked rosters. The host rejects clients attempting to send authoritative snapshots; the client rejects replies without a matching outstanding request and regressing snapshots. Appearance changes invalidate readiness for everyone.

The wire format can represent sixteen roster entries. **The current gameplay session still has two input lanes and advertises capacity two.** Neither the palette nor the snapshot codec increases playable capacity. Native menu integration, actual per-actor appearance/name labels, independent additional-player input, level/death ownership and relay-backed Internet matchmaking remain separate work. The public demo keeps its online entry disabled until those paths are ready.

Regression coverage uses the actual TCP session on loopback to verify default roster exchange, duplicate-color rejection, same-character/different-color acceptance, assigned-peer name editing, invalid-name rejection and readiness invalidation. Separate fragmented-packet tests cover sixteen-entry snapshots and malformed roster rejection. These tests do not establish sixteen-player gameplay or Internet connectivity.

The existing native-menu start barrier now sends client readiness through the same host-owned roster. The host freezes that roster and sends its locked snapshot before the native Start message. Both boot owners verify the same locked snapshot before handing the live session to gameplay. Later profile requests are rejected as locked, and changing appearance before Start invalidates the readiness agreement.
