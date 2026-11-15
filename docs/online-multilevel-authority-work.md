# Consecutive native level transitions (private)

## Original-transition check

`analysis/private-native-room-pair-20261115-125823-fe664592` ran the current
private native authority adapters from the shared Autotopia checkpoint, using
the canonical executable with SHA-256
`25C7AE324B2EA79F08FAE1BEE9DB5AC56648CB83B999B3D33936C9AC6C1F1099`.

At tick 700 both called the original `TransitionToLevel` helper for
`tcy_autotopia_visit1.main`. Both logged original-transition, agreed Begin/End
loader votes, and fresh raw-world admission at 701. This differs from the
earlier `CleanLoadLevel` comparison. They continued to complete observations
host 1446 / guest 1444 before requested normal window closure, including the
post-load private SDK movement/tool schedule. No native Room stop, AI mismatch
or spawn replay failure was found before closure. Both processes exited 0,
with normal user files unchanged. Selected evidence is in
`original-transition-result.json` in that pair directory.

This verifies the engine transition helper, not a complete authored quest
route, every cutscene camera or all enemy encounters.

## Consecutive route launcher

The two-window private launcher now exposes the engine's existing scheduled
level-route fixture. It does not add an engine RPC or new loading mechanism.
Use one to four known retail state names and an interval of at least 600 ticks:

```powershell
./scripts/start-private-room-pair.ps1 -Gameplay -NativeButtons -HostCorrection `
  -VerificationWindow 8 -DamageEvents -WorldDamageEvents -AiTargetAuthority `
  -TraceAiDecisions -TraceEntities -SpawnBatches -SpawnEligibility -SpawnReplay `
  -EntityRetirements -ProjectileImpacts -ShooterLaunches -ScriptedInput `
  -LevelTargets @('tcy_autotopia_visit1.main','meanstreet_north.visit_2','rbc_falls.main') `
  -LevelTick 700 -LevelInterval 900 -LevelMethod transition
```

Both peers receive the same route, count, first tick and interval through the
existing fixture. Their private process metadata records the route and
interval. A separate single target or Damage fixture cannot be combined with
the route. Parameter validation restricts names and caps the route at four;
the engine still validates each name against its retail catalog. Invalid names
are not converted to arbitrary Lua. Without a route, existing single-target
behavior is preserved.

Both scripts passed PowerShell parsing; a conflicting route/single-target
request was rejected before launching a process. The live route check is
recorded separately below. These scripts intentionally refuse to start while
an unrelated game session is running. They use private copies of user data and
the same canonical game executable, with original native input interpreters.


## Three-transition result: failed

The new launcher produced pair
`analysis/private-native-room-pair-20261115-130021-ff37aab4` with matching
three-destination metadata on both peers. Both completed Autotopia Main at 700
and Mean Street at 1600, including fresh admissions at 701 and 1601. The host
requested Rainbow Falls at 2500, but crashed during old-world teardown before
third-world admission. This is not a successful three-level acceptance test.

The host report identifies process 8960, started 13:00:23.188 UTC, and an
unhandled write access violation at 13:02:58.703 UTC. Its private crash log and
matching saved linker map resolve the fault to `__imp__sub_82E29700+0x287`, the
native pool-free routine, through `8272C5A0`, `8272EDC0`, component teardown
`82752F80`, and original entity/component destruction `825172F0`. It attempted
to write through guest pointer `224E5330`, with a nonzero size of 1. The existing
zero-byte cleanup guard does not apply. No broad invalid-free ignore was added;
the corrupted owner/allocation must be identified.

The guest's socket reset and missing-host-result error followed the host crash.
They are secondary failures, not evidence of a rate limit or an intentional host
lobby close. Full selected evidence, crash report and symbolized stack are in
`route-result.json` in that pair directory. The remaining guest was closed
normally with its normal user files unchanged. The host terminated by an
unhandled exception, not a normal completed test.

The crash reporter now includes the formerly omitted guest GPRs, including
r18-r27, which can retain component/container ownership in native teardown.
This reads the already located guest context inside the exception guard and
adds no gameplay-time allocation validation or free-list mutation. The existing
actual exception-filter test now checks representative preserved registers at
full width and continues to verify that reporting leaves the context unchanged.


## Final validation

The updated crash reporter builds in the canonical executable. The actual
exception-filter test passed all six checks, including full-width nonvolatile
register output, inaccessible callback memory and unchanged native context.
The enclosing online-input regression completed with exit 0. The separate
Online Room transport run passed all 9095 checks with exit 0. Both logs are in
`analysis/native-route-crash-report-a4c8fbb206b84fc2913fd50e9fbd7958/`.
Final diagnostic-build SHA-256:
`3811130F3FC7F86252BB8FA80E8795A95A62E3DDCE15E8570E86A5027C8B96D9`.

These checks validate diagnostics and transport regressions; they do not fix or
prove recovery from the observed native teardown corruption. No new public
binary release was published for this investigation.

## Follow-up: observed teardown corruption repaired

The original failure above was traced to a stale native texture wrapper freeing
a header address after it had been reused for an InputOutput routing array.
The ownership fix passed the previously failing three-destination route on both
peers, with heap-history recording disabled. See
[the ownership investigation and validation](online-native-io-storage-work.md)
for exact lifetime evidence, regression coverage and remaining limits.
This does not establish complete online campaign or combat synchronization.
