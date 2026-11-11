Pinned native direct-resolve shader dependencies

Upstream: https://github.com/xenia-project/xenia
Commit: 95a5c3ee250f80c3b9d139658649d9ffb6db3eec
Exact source URLs and SHA-256 values are in manifest.json. Original source
copyright/license notices are retained. This overlay does not modify the SDK.

Release mapping: LICENSE here is identical after CRLF normalization to
docs/licenses/xenia-bsd.txt. scripts/package-release.ps1 already includes that
file as licenses/xenia-bsd.txt; reuse that packaged notice instead of adding a
duplicate. Retain the upstream copyright headers when redistributing source.

The project-owned adapter is runtime/direct_color_resolve.hlsl. It keeps the
upstream destination/endian/half-pixel equations and replaces EDRAM reads with
a narrowly guarded native RGBA8 source. Matching the installed SDK is checked
by an actual GPU raw-byte comparison; upstream provenance alone is not proof.

Production shortcut remains unselected until ownership/lifetime correctness
and measured benefit are proved. The default is Off.
