# Distribution content patches

`scripts/build-content-patches.py` compares the owned Xbox 360 source archives
with the staged PC versions. For unchanged resources it preserves the source's
stored payload instead of recompressing it. Changed resources retain the staged
bytes. It verifies that every decoded resource, name, order and type is identical
to the staged version, then generates bounded source-copy/literal commands.

The current seven patches total approximately 233 KiB. This replaces complete
modified archives of approximately 19 MiB in the distribution payload. These are
distribution patches, not the requested general mod authoring system. Asset and
shader licensing still requires a complete release audit.

The `EM2DP01` format uses little-endian integers:

| Offset | Size | Value |
| --- | --- | --- |
| 0 | 8 | `EM2DP01` followed by NUL |
| 8 | 8 | Original file size |
| 16 | 8 | Reconstructed file size |
| 24 | 8 | Uncompressed command stream size |
| 32 | 32 | SHA-256 of original file |
| 64 | 32 | SHA-256 of reconstructed file |
| 96 | Remaining | One exact zlib stream containing commands |

Command 0 contains a source offset and length, both uint64, and copies from the
validated original. Command 1 contains a uint64 length followed by literal bytes.
Zero lengths, unknown commands, overflowing extents, truncation, trailing zlib
data and output hash mismatches are rejected. Commands cannot name files.
Original/output/patch files are limited to 64 MiB; decoded commands to 96 MiB.
No partially decoded output is published.

Setup writes a uniquely created temporary file, flushes it, then replaces the
destination directory entry. It never writes through existing original hard
links. Source and destination cannot resolve to the same path. Previously
reconstructed files are reusable only after size/hash verification. Setup checks
directory separation before patching and rejects a packfiles junction into the
original source. A wrong game dump leaves that archive's destination unchanged.

Run the isolated tests in the existing executable:

```powershell
./build/runtime23/beetleworx.exe --em2_test_content_setup=true --em2_test_content_patch_directory=D:/Repos/em2recomp/private/content-patches
```

These include synthetic malformed-input and installation tests plus native
reconstruction/hash validation of all seven real patches. Fixture output stays
in a unique test directory; normal saves/settings and source archives are not
modified. The packager requires the patch manifest to match the staged archives
and patch hashes and includes only the seven allowlisted patches and manifest.
