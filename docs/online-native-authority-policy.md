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

The development source now selects mode 2 for normal Room lobbies.
`NativeAuthorityEnabled` connects that immutable mode to the mapped native
adapters for Damage, AI target decisions, spawner batch capture/eligibility/
replay, retirements, projectile impacts, Shooter launches and death rewards.
This enables the complete dependency set together. The same helper keeps the
per-feature switches available only under the private profile for comparisons;
normal lobbies do not enable the private profile or any fault-injection switch.
Each adapter retains its original native caller, world, owner thread, tick,
registry and payload checks. Offline play and ordinary correction-only Rooms
retain their previous execution paths.

This is the mapped event authority implementation, not general replication of
every script or native component. Unsupported event shapes still stop with a
diagnostic. Native prediction, rollback, reconnect and full campaign
synchronization remain open. Published demo.15 has the earlier correction-only
behavior until a new package is built and published.

The same-executable Room regression checks all nine host/guest policy pairs
over actual loopback sockets, rejection before roster admission, immutability,
required correction, and rollback exclusion. These are protocol checks rather
than a claim of native campaign acceptance.

Validation: canonical executable SHA-256
`F9572B9F4BCE80106554559AF2EB76E6227E0BEB102703897555D0E0817AE2B8`
exits 0 under `--em2_test_rollback=true`, including 9,155 Room checks (47 added
policy checks). Retained result and log:
`analysis/native-world-policy-baa7cdf225f94fb193b07ab6dd3f1bdd/`.

## Native adapter integration evidence

The integration candidate first passed the rollback/transport suite (including
9,155 Room checks) and the lobby admission suite in
`analysis/native-authority-wiring-57341a35f9b941968de7c69eae1ab66e/`.
After improving the retained-menu policy assertion and diagnostic wording, the
final executable SHA-256 is
`043F7A781658486A1B7EEFDFE29141E3620736ACF02662E9B9B2373EC1BDDBC7`.
It passes the admission suite, including 47 real TCP/save/profile/handoff checks
with explicit world-authority assertions, in
`analysis/native-authority-menu-48f2033d16864edf89e0fbd65f2bce3c/`.

Four actual native worlds ran that final binary in
`analysis/private-native-room-roster-20261115-171022-6f8bff7c/`, with mixed
Mickey/Oswald characters and scripted SDK input. Each process's live command
line selects the negotiated production policy and enables **zero** individual
authority probe switches. The private profile remains enabled for isolated
files, automated input and observation; this is not a public internet capture.

The route loads `ftl_gulch.main` at 700,
`tcy_autotopia_visit1.main` at 1400 and `rbc_falls.entrancev1` at 2100. All four
native v7 observation hashes match at ticks 800, 1500, 2300 and 2400. The host
captures the original enemy death reward at 322, and all three guests replay
the original callback with the same three clone identities. Damage batches,
AI target decisions and spawn batches also reach the guest adapters. The
pre-close snapshot has no native/Room stop. All processes continue past 3100;
the later socket-stop messages follow the first intentional close at
17:14:28.002 UTC. All four exit 0 and ordinary user files remain unchanged.

`policy-launch-evidence.json`, `pre-close-evidence.json` and
`production-authority-evidence.json` retain these checks and each normal exit.
This establishes the mapped authority path through these entries and combat
events. It does not establish complete levels, arbitrary campaign scripts,
sixteen native players, internet latency behavior, prediction or rollback.
