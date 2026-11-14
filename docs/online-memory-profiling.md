# Native multiplayer memory investigation

Three isolated game processes reach Mean Street South through the original
Autotopia → Mean Street North → South transitions, but the complete route is
not yet an accepted regression: available system commit falls below the
unchanged 1536 MiB guard before the final observation window completes.

The opt-in `em2_profile_world_memory` diagnostic (`-WorldMemory` in the isolated
Room harness) distinguishes several quantities:

- decoded texture-cache entry count and host resource-byte accounting;
- shared GPU heaps and constant/shared/index upload-pool pages;
- native shader, pipeline and render-target cache entry counts;
- sampled process private bytes and available system commit;
- DXGI adapter node 0 process-local GPU memory usage/budget, separately for local
  and nonlocal segments, with query status reported.

These quantities are not interchangeable. Texture resource bytes do not measure
CPU commit. DXGI usage is process GPU accounting, not total system commit.
Cache entry counts do not estimate driver memory per entry. On a multi-node
adapter, the DXGI report covers node 0 only. Unsupported queries are reported
without changing renderer behavior. The diagnostic is off by default and
performs no eviction, GPU fences, residency changes or settings changes.

## Rejected decoded-texture eviction experiment

A private candidate marked native texture submission boundaries at original
level loads, then retired decoded textures unused since that boundary only
after their last-use GPU fence completed. It preserved the native texture LRU,
binding reset, destruction, memory watches and accounting. Textures reused by a
later submission survived. A renderer-worker metadata fixture passed native
construction/destruction, exact fence completion, reuse, accounting, binding
reset and repeated-boundary checks; it was not rendered-pixel proof.

The live host retired 312 decoded entries (93,716,480 resource bytes) at the
North transition and rebuilt North to roughly 55 MiB of decoded textures. That
did not materially reduce sampled aggregate private memory for the three
processes: 4,856 MiB versus 4,865 MiB in the preceding shared-upload-only run.
These are separate runs, not a controlled FPS or total-memory comparison.
The capture still reached the commit guard at 1,184 MiB available. All three
processes exited normally; normal user files, settings and crash files were
unchanged. This does not establish a completed South route or a speedup.

The texture-retirement implementation was removed from active source rather
than enabled. Its source and matching symbols were retained privately for
reproduction. Further investigation should correlate native pipeline/target
growth and DXGI usage with process-private growth before selecting another
optimization. The separate shared-upload cache candidate remains default-off;
see [its measured scope](shared-upload-cache.md).

## Expanded capture

The next three-process capture, with texture eviction removed, returned valid
DXGI queries for both segments. The host began with 841 shaders and 669 native
pipelines; those counts stayed fixed through the observed route. Its private
usage rose from 1,188,057,088 to 1,724,805,120 bytes while DXGI local usage rose
from 169,861,120 to 640,073,728 bytes. Render-target entries rose from 3 to 15,
then stayed at 15 through the North/South load boundary.

Those fixed shader/pipeline counts describe the host, not every client. The
cold clients began with 6 shaders and 4 pipelines, and reached about 212–214
shaders and 138–139 pipelines. They started roughly 365 MiB below the host in
private usage despite the same reported initial DXGI usage. Only the host
obtained the installation's persistent-renderer storage lease; the other
processes continued without persistence, as intended by its owner lock.
These observations make warm-cache/preload residency worth a controlled
comparison; they do not establish its exact byte cost or justify disabling
shader persistence for users.

The expanded capture again reached the commit guard (1,035 MiB available) and
all three processes exited normally with normal user files unchanged. These
results narrow the investigation; neither full-route completion nor a measured
FPS improvement is claimed.
