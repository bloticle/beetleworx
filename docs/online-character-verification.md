# Native character verification

The experimental native Room gameplay path includes recognized character
proxies in its completed-tick proof, including NPCs outside the two retail
player slots. A matching player position and random-call stream alone cannot
establish that those characters agree.

The sampler reads the native entity registry in GUID order and follows each
BasicMotion component's physics bridge and character proxy. It records the
identity, activation, Motion state, position and body velocity. Inactive
proxies retain their identity and activation without interpreting unused
motion fields. Guest addresses, allocation order, reference-count bytes and
local clock epochs are excluded.

Capture runs on the completed-world owner. It invokes no native callbacks and
writes no guest state. It revalidates component routes and sampled values
before publishing the complete set. Memory validation does not pin objects
against concurrent engine destruction; the caller must own a quiescent tick.

Gameplay observation v5 hashes those records and the two retail player GUIDs.
Both players must be present, and their active positions must agree with the
existing player capture. Missing identities, duplicate or unordered GUIDs,
nonfinite values and partial coverage reject verification. The set is bounded
to 4,096 proxies. Bootstrap and legacy paths retain the exact v4 encoding.

Private tracing retains the latest four character snapshots for failure
reports. The comparison script accepts both v4 and v5 logs;
`--minimum-character-ticks N` requires actual v5 gameplay observations rather
than counting bootstrap-only or older player-only captures as character proof.

This covers the recognized BasicMotion character-proxy layout. It does not
verify rigid props, complete AI/Lua state, mission flags, inventory or every
engine/worker random source. It adds disagreement detection, not rollback or
resynchronization. The public online entry remains disabled while native
synchronization, complete controls and actor lifecycle remain unfinished.
