# Disc format support

The public demo.3 accepts uncompressed Xbox 360 ISO images and extracted EM2
folders. Its Wii reader accepts RVZ, ISO/GCM, WIA, WBFS, CISO, GCZ and NFS.
The original RVZ mount and embedded reader have been compared against the same
owned disc: full file inventory and sampled beginning/end bytes match. This
does not establish full campaign compatibility or retail testing of each format.

Development source additionally reads Xbox CSO v1/v2 and ZSO directly, with
bounded DEFLATE and LZ4 decoding and a single decompressed-block cache. It
recognizes containers by their contents. Raw images named `.xiso`, `.img` or
`.bin` work through the same XDVDFS parser. Choosing `default.xex` inside a
complete extracted folder selects that folder.

Tests use synthetic Xbox filesystems with mixed raw/compressed blocks, partial
last blocks, alignment padding, cross-block reads and malformed indices and
LZ4 streams. Extraction and verified resume are exercised across different
containers containing identical logical disc bytes. Original images are never
written. Real retail compressed Xbox images still need validation. These new
Xbox readers are not included in demo.3 and no further binary release is implied.

CHD, GOD/SVOD, CCI, ZAR, ZIP/7z/RAR wrappers, extracted Wii folders and legacy
NKit conversions remain unfinished. File-container support also does not make
changed executable code compatible with the existing static translation.

Format references: [maxcso](https://github.com/unknownbrackets/maxcso),
[nod](https://github.com/encounter/nod/tree/v1.4.4).
