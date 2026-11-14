# Mapped precompiled shader packs

Packed PC shader libraries now use read-only mapped files by default. Startup
still validates every record, checksum, shader key, DXBC program and binding
layout. It retains a small key index and decodes a record when the renderer
requests it, rather than retaining every profile's decoded payload in private
memory. Requested records remain cached. Loose-file precedence, budgets,
corrupt-cache fallback and native shader/pipeline preloading are preserved.

The mapped file excludes in-place writes and truncation. Pack export uses
Windows `ReplaceFileW` for an existing destination so current readers retain
their original validated file while later readers see its replacement. New
destinations use a move without overwriting a competing writer. Export streams
unrequested indexed records without decoding the entire pack into memory.

`em2_mapped_shader_packs=false` retains the eager path for diagnosis. This is
an internal cache-loading switch, not a rendering-quality or gameplay option.

## Verification and measurements

- All 10,275 records in the packaged library produced byte-identical serialized
  records through the eager and mapped paths, with duplicate skipping both on
  and off. Before the comparison requested their payloads, the mapped library
  had zero decoded records; the eager library had all 10,275.
- File-lifetime checks verified blocked in-place writes, export from a mapped
  source, replacement while an old view remained open, distinct old/new reader
  results, and rejection of a corrupted replacement. The diagnostic exited 0.
- The shader-library fixture passed 27 translation profiles, reopening,
  concurrent lookup, packed/loose overlays and 28 corrupt-record rejections,
  with zero failures and actual process exit 0. This checks translated bytecode
  and binding metadata; it is not a full rendered-image comparison.
- Four sequential full-startup runs alternated eager/mapped/eager/mapped.
  Both paths preloaded 841 shaders and 669 pipelines, with identical initial
  DXGI local usage. Mean private usage fell from about 1,133 MiB to 955 MiB,
  a reduction of about 178 MiB (16%). All four processes exited normally and
  normal user saves/settings were unchanged.

The full startup checkpoint remained roughly 8.2 seconds in those runs. The
pack-only loading component was faster in the comparison, but this does not
establish a gameplay FPS increase or faster complete level loading. The
separate multiplayer route remains subject to the system memory guard; see
[the investigation](online-memory-profiling.md).

With mapped storage and the separate private upload-trim candidate enabled,
three native processes agreed on 661 updates in Autotopia, 698 in North and
157 in South before the unchanged commit guard stopped the capture at
1,154 MiB available. All three exited normally and normal user files were
unchanged. This is partial gameplay evidence, not complete-route acceptance.

The comparison can run without enumerating thousands of loose originals by
copying the existing `library.em2dxbcpack` into a test-only directory containing
only `translated-dxbc-v1/library.em2dxbcpack`, then running the canonical game:

```powershell
.\build\runtime23\beetleworx.exe --em2_test_shader_storage=true --em2_test_shader_library_load_path=<test-library-root> --em2_test_shader_storage_path=<new-output-directory>
.\build\runtime23\beetleworx.exe --em2_test_shader_storage=true --em2_test_pc_shader_library=true --em2_test_shader_storage_path=<another-new-output-directory>
```

Use actual process exit codes and the generated `shader-load-results.json`.
The oversized comparison against the loose preparation tree was deliberately
cancelled after a stack capture identified filesystem-open waits; it is not
counted as a passing test.
