Source-built D3D12 render-target cache for coordinated native HDR integration.

Baseline source:
https://github.com/rexglue/rexglue-sdk/blob/v0.1.0/src/graphics/d3d12/render_target_cache.cpp

The installed public header matches this version ignoring whitespace. The
prepare script pins its normalized-newline hash and the original SDK archive,
and extracts all original DXBC shader arrays with COFF section/extent checks.
The original baseline changed only shader include paths. Current additions
notify per-cache HDR shutdown and append an optional FP16 scene target during
native framebuffer binding. Registration is diagnostic-only until clear,
ownership, resolve and presentation integration is complete. The checksum
declaration is upstream thirdparty/dxbc/DXBCChecksum.h; the implementation remains
in the installed SDK archive (RSA Data Security, Inc. MD5 Message Digest Algorithm).
Original notices are retained. See SDK-LICENSE and the checksum header.

Only the build-local archive loses its original cache object. The installed SDK
is untouched. Do not change class layout without rebuilding all its callers.
This baseline does not promote game buffers or enable HDR. Before promotion,
resource formats, both drawing views (including sRGB), transfer views, shader
packing, cache identity and resolved textures must be changed coherently.
