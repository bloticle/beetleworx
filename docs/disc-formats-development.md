# Disc format support

The public demo.3 accepts uncompressed Xbox 360 ISO images and extracted EM2
folders. Its Wii reader accepts RVZ, ISO/GCM, WIA, WBFS, CISO, GCZ and NFS.
The original RVZ mount and embedded reader have been compared against the same
owned disc: full file inventory and sampled beginning/end bytes match. This
does not establish full campaign compatibility or retail testing of each format.

Development source additionally reads Xbox CSO v1/v2, ZSO, CCI and standalone CHD directly, with
bounded DEFLATE and LZ4 decoding and a single decompressed-block cache. It
recognizes containers by their contents. Raw images named `.xiso`, `.img` or
`.bin` work through the same XDVDFS parser. Choosing `default.xex` inside a
complete extracted folder selects that folder.

Development setup also accepts raw split Xbox images named `name.1.iso`,
`name.2.iso`, ...; `name.iso.0`, `name.iso.1`, ...; or `name.iso.001`,
`name.iso.002`, ... (and the equivalent `.xiso` names). Select any part and
keep all parts together. Reads cross part boundaries directly without creating
an intermediate ISO. Parts remain locked against writes while open, the set is
limited to 32 parts and 16 GiB total, and missing middle/first parts or empty
parts are rejected. The combined bytes use the same filesystem, title and
executable validation as a single ISO. A missing final part is detected only
when it truncates required filesystem data; these formats have no part-count
manifest. Split-container formats use their own reader; these raw split naming
rules do not concatenate compressed containers. Native setup regressions and
31 independently generated split/import cases pass, covering both extensions,
all three conventions, selection of first and two-digit final parts, exact
extracted bytes, ISO-equivalent resume identity, and missing/empty parts.
On 2026-11-12 an owned 8,738,846,720-byte retail ISO split into three
`EM2.iso.001`/`.002`/`.003` files passed the native read-only comparison after
selecting the third part: all 2,325 files and 6,975,770,804 game bytes matched
the extracted reference. The unaligned cuts at 4,294,967,303 and 6,442,450,957
bytes fall inside a packfile and a movie, exercising reads across both part
boundaries and 64-bit offsets. The assembled source SHA-256 matched the known
original during fixture creation. This proves that retail split set's file
reads, alongside synthetic extraction/resume tests; it does not establish a
complete setup/gameplay run or retail testing of every naming convention.
Run `ctest -R split-iso-content-import` in the runtime build directory.
These additions are not in demo.3.

CCI supports both single files and numbered `name.1.cci`, `name.2.cci`, ...
parts. Select any numbered part and keep the complete set together. The reader
opens parts read-only and joins their logical sectors; no temporary ISO is
created. It checks header/index extents, raw sector lengths, compressed padding
and missing middle parts. The current bounded reader accepts up to 32 parts.
Tests cover mixed raw/LZ4 sectors, padding, split-boundary reads and equivalent
ISO/CCI import resume. Retail CCI images still need validation.

CHD decoding is linked into the same executable through pinned `chd` 0.4.0.
It exposes flat Xbox logical disc bytes directly to setup without an intermediate
ISO. The bridge bounds hunk/map/metadata sizes, verifies stored hunk CRCs, and
resolves self references without recursive decoding. Tests cover raw CHD v5,
DEFLATE CHD v3, valid self references, cross-hunk and partial-final-hunk reads,
ISO/CHD resume, bad CRCs, cycles, oversized maps and required-parent rejection.
Retail CHDs and the other decoder codecs still need validation. Parent-dependent
CHDs and CD-sector/audio layouts are not accepted; this reader is for Xbox
images and is not a Wii CHD mount. These additions are not included in demo.3.

Development setup also reads Xbox 360 GOD/SVOD packages directly. Select the
package header (use All files for extensionless headers), its containing folder,
or its `.data` folder. Keep every `Data0000`, `Data0001`, ... fragment together.
The reader covers external legacy/enhanced layouts and single-file SVOD, checks
the package title and filesystem, and reads across hash-table/fragment boundaries.
It derives readable size from physical fragment geometry because converter
allocation counts may be byte-swapped or too small after trimming. Originals
remain read-only; setup extracts the game files without creating an ISO.
Synthetic tests cover all three layouts, two-fragment reads, folder detection,
missing fragments and ambiguous package folders. Retail GOD packages still need
validation. The reader verifies each master hash table against the package
root and previous-fragment link, then verifies every data hash table and block
before exposing its bytes. Partial final blocks are hashed at their actual
length. One verified data group is cached, and failed checks invalidate it.
Corruption tests cover all tree layers, later fragments and cache recovery.
This checks consistency with the declared root; Xbox package signatures are
not authenticated. Setup also uses SHA-256 source ownership and the existing
executable/build checks. GOD
imports have their own source identity and do not resume an ISO import receipt.
These additions are not included in demo.3.

Development setup also reads ZArchive v1 (`.zar`) with the existing embedded
Zstandard decoder. It accepts one extracted EM2 game tree at the archive root
or inside a wrapping folder, or one raw ISO/XISO/IMG/BIN file stored in the
archive. A contained ISO is read directly without writing an intermediate image.
The whole-archive SHA-256 checksum is verified with cancellable progress before
extraction. Metadata, directory depth, names and block ranges are bounded;
cyclic, multiply-parented, duplicate and unsafe paths are rejected. The
verified archive hash provides resume ownership without a second full
uncompressed hashing pass; an archive uses its own import receipt rather than
resuming an ISO receipt. Tests cover raw/compressed blocks, cross-block reads,
extended names, empty files, wrapped trees, a contained ISO, checksum failures,
malformed/truncated metadata, cancellation and failed-decode cache recovery.
On 2026-11-12 a private archive generated from the owned USA/Europe EM2
reference passed native checksum verification and a full read-only comparison:
all 2,325 files and 6,975,770,804 file bytes matched. The archive was
6,518,365,763 bytes and exercised compression/file offsets above 4 GiB. It was
built by a private Python format writer; archives from third-party producers and
other game builds still need testing. This comparison verifies archive contents,
not a full setup/gameplay run. Nested compressed disc containers and Wii archive
mounts are not implemented. This reader is not in demo.3.

Development setup also reads single-volume ZIP/ZIP64 archives with Stored or
DEFLATE entries. It accepts one extracted EM2 tree (including implicit parent
folders or a Unicode wrapping folder), or one contained raw ISO/XISO/IMG/BIN.
Contained ISOs are read directly without an intermediate image. All entry CRCs
are verified before extraction, local/central headers and data descriptors are
checked, and links, unsafe paths, duplicate names and overlapping records are
rejected. Bounded DEFLATE checkpoints every 8 MiB allow backward/random reads
without repeatedly decoding a large ISO from the beginning; decoder allocation
is capped at 128 MiB. A shared read-only archive interface now serves ZIP and
ZArchive. ZIP receipts use the verified container bytes as source ownership.
ZIP validation walks payloads in physical order, hashing their compressed bytes
while checking decoded CRCs. Headers, gaps and the directory are included in
the same forward hash pass, avoiding a second full read of the archive.
The independent Python `zipfile` regression suite covers extraction, resume,
ZIP64, 32/64-bit streaming descriptors, empty files/directories, stored and
deflated ISOs, checkpoint seeks, misleading extensions and corrupt inputs.
Password-protected/multi-volume ZIPs and other ZIP compression methods are not
implemented. On 2026-11-12 a Python-produced ZIP64 archive containing the owned
8,738,846,720-byte retail ISO passed CRC validation and a full comparison with
the extracted game: all 2,325 files and 6,975,770,804 game bytes matched. The
compressed entry was 8,614,081,629 bytes, exercising real sizes and seeks above
4 GiB. A back-to-back test of the old/new readers reduced validation plus
filesystem preparation from 106.869 to 71.999 seconds; cache and machine load
can affect timing. The new path removes one complete 8.61 GB archive read.
The full read-only comparison also passed after the change. This does not
establish a complete setup/gameplay run from that archive or compatibility
with every ZIP producer/build. Run `ctest -R zip-content-import` in the runtime
build directory for the 26 independent fixture cases. This is not in demo.3.

Development source also mounts extracted EM1 Wii discs directly. In the Wii
image picker, choose **Use an extracted game folder instead**. Select the disc
root, `DATA`, or `files`; the reader accepts Dolphin-style `sys/boot.bin` plus
`files/packfiles/levels.pak`, or game files beside `sys`. The original Wii disc
header must be present and identify Epic Mickey. No copying or conversion is
required. The mount is read-only; after changing source files, remount it.
Redirected/junction paths inside the selected source are rejected. This folder
reader is not included in demo.3 and does not complete EM1 campaign support.

Tests use synthetic Xbox filesystems with mixed raw/compressed blocks, partial
last blocks, alignment padding, cross-block reads and malformed indices and
LZ4 streams. Extraction and verified resume are exercised across different
containers containing identical logical disc bytes. Original images are never
written. Real retail compressed Xbox images still need validation. These new
Xbox readers are not included in demo.3 and no further binary release is implied.

Developers can compare every game file in an image against a known extracted
copy without importing or launching gameplay. Add
`--em2_test_xbox_destination="D:\Checks\extracted"` to exercise extraction into
a new/owned folder before the comparison. Use the same game executable:

```powershell
.\beetleworx.exe --em2_test_xbox_disc="D:\Games\EM2.iso" --em2_test_xbox_reference="D:\Games\EM2-extracted" --em2_user_data_directory="D:\Checks\EM2" --log_file="D:\Checks\EM2.log"
```

Both the source image and reference files are opened read-only. A successful
result proves that every file discovered by the native disc parser has matching
length and contents in the reference; it does not prove executable compatibility
or successful gameplay. Missing or different reference files fail verification.

7z/RAR wrappers, legacy
NKit conversions remain unfinished. File-container support also does not make
changed executable code compatible with the existing static translation.

On 2026-11-12 the read-only verifier compared the available USA/Europe EM2
Xbox 360 ISO with its extracted reference: all 2,325 files and 6,975,770,804
file bytes matched at partition offset `0x02080000`. This establishes real-disc
coverage for that ISO, alongside the synthetic compressed-container tests.

Format references: [maxcso](https://github.com/unknownbrackets/maxcso),
[CCI reference format](https://github.com/Team-Resurgent/libCCI/blob/main/FORMAT.md),
[XbWrite split-image conventions](https://github.com/PlayingKarrde/XbWrite),
[chd-rs](https://github.com/SnowflakePowered/chd-rs),
[ZIP specification](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT),
[ZArchive reader/writer reference](https://github.com/Exzap/ZArchive/tree/965b66c8d67b6b7e30fd63b3b75aa91a99ff303b),
[nod](https://github.com/encounter/nod/tree/v1.4.4),
[Xenia SVOD reader](https://github.com/xenia-project/xenia/blob/master/src/xenia/vfs/devices/stfs_container_device.cc),
[iso2god-rs package writer](https://github.com/iliazeus/iso2god-rs/blob/master/src/god/con_header.rs).
