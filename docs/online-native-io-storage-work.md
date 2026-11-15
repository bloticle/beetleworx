# Native texture ownership fix across level loads

## Cause

The original-transition route Autotopia Main -> Mean Street North -> Rainbow
Falls reproduced a native teardown crash at tick 2500. The affected
InputOutputComponent belongs to meanstreetnorth_shared.LogicTimerMarker 01,
GUID 0016214F/00000000/00000000/00000000, component A22A79A0.
Its routing array was correctly allocated. A stale native texture wrapper
later freed that array's reused address.

The failed host capture in
`analysis/private-gpu-profile-20261115-135050-8da447f1` contains contiguous
heap records 1-3159809 with no reader gaps. Exact pointer A4C2F408:

| Sequence | Operation | Native caller | Meaning |
| --- | --- | --- | --- |
| 1730399 | Allocate 52 bytes | 8265499C, parent 8267C384 | Texture header creation |
| 2091886 | Free request | 82681788 | Original fenced resource cleanup |
| 2091891 | Allocate 44 bytes | 82654A88, parent 82759874 | Valid routing-array allocation |
| 2679391 | Free request | 8267CC74 | Stale texture destructor frees reused address |

Rendering later reused storage for a NiPropertyState. Its vtable 82040E98
overrode the array allocation prefix. Final array teardown interpreted that
vtable as an offset: A4C2F408 - 82040E98 = 22BEE570, matching the crash
write at host address 122BEE570. This was not borrowed array storage or a
networking timeout.

## Repair

Track native texture wrappers created by 8267C260 with owner-generation
identities. At the original fenced header free (82681788), clear matching
wrapper header/backing references while preserving its resource reference and
original allocator/fence operations. A wrapper already in its destructor
consumes retirement after releasing its resource, before its own cleanup.
Pending queue ownership is transferred explicitly. Old destructor completions
cannot unregister a newer owner at a reused address.

The resource consumer removes the entry before this free site. The callback
therefore validates the original call site, manager type, header kind and
recorded ownership rather than queue membership. An initial candidate
incorrectly required membership and failed the live route; the final regression
exercises the actual callback with an empty queue. No broad invalid-free
suppression or ignored networking mismatch was added.

## Validation

Canonical executable SHA-256:
D3FB19E6198BE7F25EEEFD4C46BBA9A5D73D66473ACC6B7A8964ADCE4B912EB2.

The same-executable regression exited 0, including owner generations,
pending/completed retirement, destructor phase, foreign/changed owner rejection,
and actual post-dequeue context/memory/FP/error preservation. Heap-history
fixtures and all 9095 Online Room transport checks also passed. Evidence:
`analysis/native-texture-dequeued-check.json` and its referenced log.

Private pair `analysis/private-native-room-pair-20261115-141130-b32dc1f7`
completed all three original transitions, with fresh admissions at raw ticks
701, 1601 and 2501 on both peers. Heap-history recording was disabled. The host
retired texture header A4C2D348 during Mean Street loading. A read-only snapshot
at tick 2224 found that exact address reused as the affected logic timer's
valid routing array, with allocation prefix offset 8 / size 52 and count 1 /
capacity 1. Both peers subsequently entered Rainbow Falls.

Both processes exited normally with code 0 and normal user files unchanged.
No selected native Room failure/stop or AI/spawn mismatch occurred before the
first intentional window close. Shutdown disconnects after that boundary are
excluded explicitly. Full evidence is in the pair's
`texture-retirement-route-result.json` and the host's
`protected-io-array-result.json`.

This proves repair of the observed lifetime error on this route. It does not
prove complete campaign synchronization, native rollback, every texture factory,
all cross-thread destructor interleavings, or HDR/experimental-lighting visuals.
No public binary release is implied by the source update.

## Optional diagnostics

Private-only `start-private-room-pair.ps1 -HeapHistory` enables default-off
em2_trace_native_heap_history. The final bounded ring retains 131072 events
(about 6 MiB), allocated lazily only when enabled. Events contain allocation
completions/free requests, raw size operands, caller/thread and three bounded
PPC parent return sites. A free request is not proof of successful release.
Crash reports use a nonblocking lookup; missing/evicted data remains explicit.
Read-only external streaming supplied the complete failing history.

A temporary 4194304-entry diagnostic ring altered reuse/timing and passed the
route before any repair. That run was not accepted as evidence of a fix; final
live acceptance uses no heap-history ring. Ordinary gameplay collects no history.
