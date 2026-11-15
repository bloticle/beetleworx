# Native Shooter birth before slot cleanup

## Exact failure

The post-respawn Autotopia run in pair
`analysis/private-native-room-pair-20261115-143717-88160e56` stopped at guest
tick 1510. The next host operation belongs to dropwingbubbleweapon,
GUID 000033FE/00005F57/000046E2/00001E6F, and creates a projectile from
template 00069771. Earlier births at 993 and 1250 and launches at 999 and
1256 matched. The guest's extra independent preparation at 1257 was suppressed.

Private repeat `analysis/private-native-room-pair-20261115-144227-dc9910a9`
added read-only before/after tracing around original `827B2E28` and exact
pre-state diagnostics. At tick 1510:

| Role | Native operation order | Count / occupied / mask / tracked handles |
| --- | --- | --- |
| Host | Prepare third projectile | 2/2/1/2 -> 3/3/1/3 |
| Host | Original slot cleanup | 3/3/1/3 -> 3/2/0/2 |
| Guest | Original slot cleanup first | 2/2/1/2 -> 2/1/0/1 |
| Guest | Missing birth replay at completion | Rejected: local 2/1/0 vs host parent pre-state 2/2/1 |

The original cleanup is called by `828B90B0` at return site `828B91C8`.
It resolves tracked native handles and clears expired slots through original
container helpers. Merely assigning host counter words would bypass those
containers and conceal the ordering error. Exact evidence is retained in the
pair's `spawn-cleanup-order-result.json`. Both diagnostic processes closed
normally with exit 0 and normal user files unchanged.

## Repair candidate

At that audited cleanup call site, an admitted guest can replay the next
missing host Shooter preparation BEFORE cleanup if its owner/anchor and all
three parent pre-state words still match exactly. Only top-level, self-owned,
flagged Shooter operations with captured RNG are admitted. A consumed or
mismatched prefix, another owner, collection/nested operation or changed state
cannot be forced into this point.

Replay uses the existing original `828B92A8` outer preparation, full parent and
child identity checks, original pending-shot registration and borrowed-stack
restoration. It stops after the relevant outer burst rather than draining
other owners' commands. Original `827B2E28` then performs cleanup normally.
No native counters, slot maps or pending-shot lists are assigned by the adapter.
Unadmitted operations retain the existing completion replay and its failures.
This is a narrow ordering boundary, not a complete causal journal or rollback.

Pure admission regressions cover the reproduced pre-state, each changed word,
wrong owner/anchor, unaudited shape, missing RNG, duplicate consumption and an
already-divergent earlier prefix.

## Verified candidate

Canonical executable SHA-256:
`12A598FCF7738E38F803DD6F33B56513FD2BBDADD24C1CF8A05A8F612F37DAAC`.
The same-executable rollback-chain regression exits 0, including 183 spawn
codec/admission checks and 9099 Room checks. Evidence:
`analysis/native-spawn-order-test-result.json` and its referenced log directory.

Private pair `analysis/private-native-room-pair-20261115-144744-b8730d4e`
passes the original failure. At tick 1510 the guest replays the missing host
preparation before cleanup, with full parent/child/pending-list checks. Both
peers then log identical cleanup state transitions:
`3/3/00000001/3 -> 3/2/00000000/2`. The same replay point also runs at 480,
1250 and 1769. There is no pre-state mismatch before the run stops elsewhere.

Both peers retain the prior death/respawn fix (terminal entry 714, checkpoint
load 775, fresh admission 776), then agree original Gulch loader Begin/End at
1900 and fresh admission 1901. The host subsequently rejects a scripted player
one camera target transition. This is a distinct remaining lifecycle issue;
the final scheduled Autotopia Main destination at 3100 was not reached.
The route is not accepted as full campaign or combat synchronization.

Both owned processes close normally with exit 0 and normal user files unchanged.
Exact cleanup/replay/admission records and later errors are retained in the
pair's `spawn-cleanup-replay-result.json`. Source publication includes no new
public binary release.
