# Native authority session policy

Room wire 30 distinguishes three immutable Hello/Welcome policies:

- Verified independent simulation, encoded as 0.
- Host position/health correction, encoded as 1.
- Native world event authority with host correction, encoded as 2.

`Room::SetNativeWorldAuthority(true)` selects the third policy before Host or
Join and also enables host correction. Neither peer may change the policy
after connecting. Disabling correction while world authority remains selected
is rejected. Rollback transport and this policy are mutually exclusive because
their native execution/acknowledgement contracts differ. An idle caller can
deliberately disable world authority and then correction before selecting
rollback transport.

The original correction-only handshake could not distinguish a client running
the full causal event adapters from one running only movement/health correction.
The explicit mode prevents those peers from being admitted to the same roster.
`OnlineRoomBoot` preserves the requested policy across its asynchronous content
preparation worker and applies it to the new Room before opening a connection.

This change introduces the negotiation contract. It does not turn on the native
event adapters or change the normal lobby default yet. Those adapters still
need to consume this policy in place of private probe switches. Native
prediction, rollback, reconnect and full campaign synchronization remain open.

The same-executable Room regression checks all nine host/guest policy pairs
over actual loopback sockets, rejection before roster admission, immutability,
required correction, and rollback exclusion. These are protocol checks rather
than a claim of native campaign acceptance.

Validation: canonical executable SHA-256
`F9572B9F4BCE80106554559AF2EB76E6227E0BEB102703897555D0E0817AE2B8`
exits 0 under `--em2_test_rollback=true`, including 9,155 Room checks (47 added
policy checks). Retained result and log:
`analysis/native-world-policy-baa7cdf225f94fb193b07ab6dd3f1bdd/`.
