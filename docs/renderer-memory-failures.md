# Upload-buffer failure cleanup

The pinned UI SDK's D3D12 upload pool called `Release()` explicitly after a
failed `ID3D12Resource::Map`, then released the same resource again through its
local `ComPtr` destructor. This could dereference a freed COM object while
handling a mapping failure. Disassembly of the installed SDK confirms both
release calls on that branch; this is not inferred from the multiplayer hang.

The game now source-builds that pinned pool implementation, retaining the local
`ComPtr` as the sole cleanup owner. Creation, alignment, submission indices,
page reuse, and successful mapping retain the original implementation. The SDK
installation is unchanged. The build script validates the pinned UI archive
SHA-256 before removing its replaced pool object:
`43a9315705a876daf7dd46b0c81a93c55431ea9d6f45144ec1282983d001ed04`.

Run the isolated test with:

```powershell
.\scripts\test-upload-pool.ps1
```

The same canonical executable creates real D3D12 upload resources. A scoped,
thread-local test probe returns `E_OUTOFMEMORY` and `DXGI_ERROR_DEVICE_REMOVED`
at the mapping boundary without exhausting memory or removing the GPU. It
checks exact reference ownership, unchanged failed-request outputs, recovery
on the same pool, actual successful mappings, alignment, page rollover, and
reclaim/reuse only after the requested completion index. Normal rendering never
enables this probe.

Local test `analysis/upload-pool-offscreen-20261114-082415-ef538df0` passed with
process exit zero and unchanged normal user files. The same binary passed the
screen-upscaling/HDR GPU regression at
`analysis/screen-upscale-offscreen-20261114-082424-a4cf77d3`, including zero device
errors. Candidate executable SHA-256:
`BDB01B7684FCFE3087E68CAD1D44FDE63033091124DD800B9379972BFFB15101`.

This fixes one cleanup defect. It does not establish recovery of an entire game
session from real device removal or address general memory exhaustion, the
intermittent title-shutdown hang, or every user-reported crash. No FPS increase
has been measured for this change. It is source work, not a new demo release.

The same candidate also passed the three-client native menu/SDK-input regression
at `analysis/native-lobby-20261114-082615-418ee908`: 1,181 consecutive matching
native updates, actual extra-player movement reversal and jump/landing, all
processes exiting normally, and unchanged normal saves/configuration/crash file.
The multiple-extra-human/chat/voice aggregate verifier requires at least four
clients and was not accepted for this three-client capture. A four-client run
stopped at the commit guard during loading and is not a gameplay pass. The
previous six-client result belongs to the preceding candidate.
