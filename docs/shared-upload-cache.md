# Shared-memory upload cache retirement

`em2_trim_shared_upload_cache` is a default-off memory measurement candidate.
It wraps the pinned SDK's D3D12 shared-memory completion handler, calls the
original handler first, and retires old spare upload pages on that GPU worker.
Other upload pools, texture caches, GPU shared-memory heaps and rendering
quality are unchanged.

The active writable page is always retained, along with its used/flushed
counters. The next 8 MiB of spare pages remain available for reuse. Additional
spare pages may be destroyed only after their native submission fence has
completed and they have aged by at least 120 submissions. Submitted pages are
never changed. A submission is a GPU fence unit, not a gameplay tick.

The wrapper uses the native pool's declared members without changing its layout
or modifying the installed SDK. The build bridge checks the pinned SDK archive
hash and requires exactly one matching D3D12 completion definition. It preserves
the original implementation under `EM2OriginalSharedUploadCompletion`.

The canonical executable's `--em2_test_online_input=true` regression exercises
real native upload-pool Request/Reclaim behavior with instrumented page
destructors. It checks incomplete fences, retirement age, spare reuse, active
partial writes, list tail integrity and exact cleanup. It does not establish
live GPU correctness or a performance gain on its own.

The isolated multi-client harness accepts `-TrimSharedUploads` to enable the
candidate and `-WorldMemory` to record GPU-worker cache counts alongside system
commit measurements. Memory guards remain unchanged. Enablement requires live
measurement; no FPS improvement is assumed from releasing idle upload pages.

## Current evidence

The focused online regression, including the pool checks above, exited 0.
In a three-process Autotopia → Mean Street North → Mean Street South run, all
worlds agreed on 661 updates in Autotopia and 698 updates in North. The North
host snapshot retained five shared upload pages (10 MiB); the prior untrimmed
route retained eighteen (36 MiB). These are cache measurements at observed
boundaries, not an isolated FPS comparison or a total-memory reduction claim.

The clients admitted the South world and agreed on only fourteen updates before
the unchanged 1536 MiB system-commit guard stopped the capture (1228 MiB was
available). This is not a successful complete-route test. All three processes
exited normally and normal user files, settings and crash files were unchanged.
The candidate therefore remains off by default. Further work must address the
remaining memory pressure and measure allocation churn before default use.
