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
validation. The stored SVOD SHA-1 hash tree is not verified yet; setup
uses SHA-256 source ownership and the existing executable/build checks. GOD
imports have their own source identity and do not resume an ISO import receipt.
These additions are not included in demo.3.

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
copy without importing or launching gameplay. Use the same game executable:

```powershell
.\beetleworx.exe --em2_test_xbox_disc="D:\Games\EM2.iso" --em2_test_xbox_reference="D:\Games\EM2-extracted" --em2_user_data_directory="D:\Checks\EM2" --log_file="D:\Checks\EM2.log"
```

Both the source image and reference files are opened read-only. A successful
result proves that every file discovered by the native disc parser has matching
length and contents in the reference; it does not prove executable compatibility
or successful gameplay. Missing or different reference files fail verification.

ZAR, ZIP/7z/RAR wrappers, legacy
NKit conversions remain unfinished. File-container support also does not make
changed executable code compatible with the existing static translation.

On 2026-11-12 the read-only verifier compared the available USA/Europe EM2
Xbox 360 ISO with its extracted reference: all 2,325 files and 6,975,770,804
file bytes matched at partition offset `0x02080000`. This establishes real-disc
coverage for that ISO, alongside the synthetic compressed-container tests.

Format references: [maxcso](https://github.com/unknownbrackets/maxcso),
[CCI reference format](https://github.com/Team-Resurgent/libCCI/blob/main/FORMAT.md),
[chd-rs](https://github.com/SnowflakePowered/chd-rs),
[nod](https://github.com/encounter/nod/tree/v1.4.4),
[Xenia SVOD reader](https://github.com/xenia-project/xenia/blob/master/src/xenia/vfs/devices/stfs_container_device.cc),
[iso2god-rs package writer](https://github.com/iliazeus/iso2god-rs/blob/master/src/god/con_header.rs).
