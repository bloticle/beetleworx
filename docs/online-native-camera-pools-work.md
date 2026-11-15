# Native camera pool ownership during online gameplay

## Reproduced rejection

The previous online route reached `ftl_gulch.main` and stopped at tick 1943:
`native player one camera entered a scripted target transition`.
Host evidence: `analysis/private-gpu-profile-20261115-144745-333464cf/game.log`.
The active controller was A37A5490, with temporary mode A3DD4740 and vtable
8220EF98. It retained the correct +20 controller owner, but was not one of the
fixed mode pointers at controller +392 through +432.

The original constructor `82A109C8` also initializes eleven native mode pools
at +580 through +740. Each has data/count/capacity/controller-owner fields.
The snapshot pool at +692 is initialized by `82A16E60`; `82A176A8` selects or
extends it, constructs 128-byte modes via `831DB0D8`, and invokes their native
+48 owner setter. Snapshot setter `8290EE68` writes the controller at +20.
`831DB1E0`/`831DB228` store/read its captured camera pose.

## Live ownership evidence

The first snapshot-only candidate, pair
`analysis/private-native-room-pair-20261115-145416-9d00bc32`, passed the first
snapshot transition. A read-only live capture found A3DD4740 at index 0 of the
controller's +692 pool, count/capacity 4/4, both owner links A37A5490.
Its `snapshot-camera-pool-result.json` retains the observation.

That candidate then stopped at tick 754: active blend A3DD59C0 (8220EEC8)
contained Normal A38F5A30 and child A3DD54C0 (8220EDA8). The child was a valid
member of the +596 pool, with the same owner. `camera-failure-graph.json` and
`camera-native-pools.json` retain that distinct rejection and all pool entries.
Both processes closed normally, exit 0, normal files unchanged.

The audited pool mapping is:

| Controller offset | Native initializer | Mode vtable |
| --- | --- | --- |
| 580 | 82A16750 | 8220D818 |
| 596 | 82A16850 | 8220EDA8 |
| 612 | 82A16950 | 821125F8 |
| 628 | 82A16A60 | 8220EEC8 |
| 644 | 82A16B60 | 82112640 |
| 660 | 82A16C60 | 82124138 |
| 676 | 82A16D60 | 82123F50 |
| 692 | 82A16E60 | 8220EF98 |
| 708 | 82A16950 | 821125F8 |
| 724 | 82A16A60 | 8220EEC8 |
| 740 | 82A16750 | 8220D818 |

`analysis/native-camera-pools-audit.json` records the original constructor
pool-offset/factory-call checks, five exact PPC instruction assertions, image
hash and constructor-body hash. Live pool types/members are separate evidence.

## Ownership repair

Ongoing co-op readiness now recognizes these exact pool specializations. A
pooled mode still needs a known native vtable/interface, its own +20 controller
owner, the pool's separate controller owner, bounded readable data, valid
count/capacity (maximum 4096), and membership inside the used extent. Merely
placing an address in unused capacity cannot authorize it. Fixed modes and
original authored Hint/Blend handling retain their existing checks, including
bounded depth, ancestor-cycle rejection and child ownership.

Initial attachment remains Normal-camera-only. The change does not force the
camera back to Normal, change native mode pointers, or remove the world/roster
lease. The native camera can finish its original transitions.

## Regression evidence

The same executable's online-input suite exits 0, including 60 new native pool
cases across six pooled types: valid ownership, foreign node/pool owners, unused
entries, count/capacity overflow, unreadable storage, wrong types, empty pools
and invalid interfaces. Existing authored/blend graph and admission checks
remain in the suite. Final executable SHA-256:
`BC4B253AD0FD0564FCB4656D355EF928447BDD41B5C8EC421C39ADABC5D8D14D`.
Evidence: `analysis/native-camera-owned-pools-test-result.json` and its log root.

## First complete camera acceptance

Pair `analysis/private-native-room-pair-20261115-145808-bb279f2b` enters Gulch
at 700, re-admits at 701 and passes both prior temporary-camera failures.
Both peers continue beyond 2200 with no native Room stop before the first
intentional window close. Read-only `camera-owned-graph.json` observes the
active 8220EDA8 mode A3DD54C0, owner A37A5490; `camera-native-pools.json`
confirms membership in that same controller's +596 pool. Both processes exit
normally, code 0, normal files unchanged. Exact boundaries and last native
observations are in the pair's `camera-owned-pools-result.json`.

This run is not a passed two-level route: the second scheduled test target was
skipped because the private trigger still required a Normal camera. That
private level-transition fixture now accepts already-owned runtime cameras;
other action fixtures retain Normal-only admission.

## Completed route check

Pair `analysis/private-native-room-pair-20261115-150146-c324877d` completes
original Gulch loader Begin/End at tick 700, fresh admission 701, then Autotopia
Main loader Begin/End at 1400 and fresh admission 1401 on both peers. Both
continue through actual native observations at 1800 in Autotopia Main before
requested normal shutdown. No selected native Room stop, AI mismatch or spawn
replay failure occurs before the first intentional close. Shutdown disconnect
messages after that boundary remain explicitly outside the acceptance window.

Both processes exit 0 with normal user files unchanged. Exact loader events,
admissions, final observations, selected failures and shutdown results are in
the pair's `camera-pools-route-result.json`. Final route-build SHA-256:
`C1523CFC3A3BE8F525B0D4DF22E4F7560589CB4B9C28F87261A7D3522812B42D`.
This build changes only the private QA level-trigger admission after the
ownership candidate's successful regression suite; its build log recompiles
only `game_console.cpp`. No public binary release accompanies this source fix.

Passing camera ownership checks does not establish complete authored camera
synchronization, campaign progression, prediction/rollback or full enemy sync.
