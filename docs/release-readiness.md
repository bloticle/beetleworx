# Release preparation

The player package uses the existing `beetleworx.exe`. First launch selects an
Xbox 360 `.iso` or an extracted game folder; native import and content setup run
in that executable. Developer tools are not required by players.

Create a new snapshot after the coordinated build and validation finish:

```powershell
python ./scripts/build-content-patches.py
python ./scripts/prepare-rust-notices.py
./scripts/package-release.ps1 -PlanOnly
./scripts/package-release.ps1 -Demo -ExpectedExeSha256 <validated-final-sha256> -Destination D:/Releases/Beetleworx-PublicDemo-20261110 -Zip
```

The packager uses an explicit allowlist: the game executable, DirectX compiler
DLLs, app-local x64 Visual C++ runtime, seven binary menu/shader patches, PC
shader catalog, the current renderer ABI's translated shader bundle, supplied
license notices and the short player guide. The manifest records every payload
hash and size. It refuses an existing destination and detects a changing build
during copying. A final demo snapshot requires the approved full executable
SHA-256; a mismatch fails before creating output. The optional ZIP contains
exactly the reviewed folder payload and manifest. It does not execute the game. Full modified PAK archives are not
included. Patch preparation preserves original stored bytes for unchanged
resources; the game reconstructs archives from the player's files during setup,
with bounded copy/literal decoding, exact zlib validation, source/output SHA-256
checks and atomic file replacement. A patch for another game dump is rejected.

Original game data, personal saves/settings, local driver/pipeline caches,
symbols/maps, test executables, generated sources, developer logs and research
captures are excluded. Prepared PC shader records are retained to avoid throwing
away the shader-preparation work. They are distinct from machine-specific driver
caches. Packaging never scans the staged game-content junction tree.

The existing native `EM2DXPK1` bundle contains all 9,710 prepared records in one
file. Packaging verifies that its records match the loose preparation outputs
byte-for-byte, rejecting stale, changed, missing, duplicate, truncated or trailing
records. It includes the bundle instead of thousands of individual files. The
current allowlisted plan has 29 payload files; this is a file-count improvement,
not a claim that shader bytecode or the executable became substantially smaller.
Python is used by developer packaging verification; players need no Python or
separate shader preparation tool. The native loader checks record checksums and
shader/resource metadata when loading the bundle.

Disc import validates XDVDFS tables and Epic Mickey 2's XEX title ID before
writing output. It streams bounded chunks, rejects unsafe names/cycles/extents,
uses SHA-256 to bind interrupted extraction to its image, and publishes complete
files atomically. Existing extracted files are verified against the image;
conflicting files are not overwritten. Folder setup uses junctions and hard
links to reuse owned assets; different volumes require copying some files.
Partial folder imports retain their original source ownership. Diagnostic and
explicit content-directory launches never open a setup picker.

## Current demo scope (updated 2026-11-10)

Online play is deferred from this demo at the user's request. The authored
main-menu entry and auto-open paths honor `PublicDemoBuild()`. Native option
callbacks also return inert demo values before creating the online controller;
prelaunch Host/Join and ready handoff are blocked. Underlying network code remains
available for development builds. Menu visibility alone is not the gate.

The retained online requirements below describe future work and are not
prerequisites that this limited public testing demo claims to satisfy. The final
snapshot still waits for the coordinated animation/ink/naming changes and the
root agent's validated executable hash. Do not publish an earlier working tree
snapshot merely because packaging fixtures pass.

## Distribution target

Release acceptance covers the complete player experience, not merely a build or
package. The following requirements remain open until their runtime evidence is
recorded:

| Requirement | Current evidence | Required acceptance |
| --- | --- | --- |
| Distance-based shadows | Persistent native menu with Fixed/Quality/Balanced/Performance, live application, tested preset ordering/hysteresis and native script callbacks; normal-selection Balanced allocation verified with real D24FS8/PCF binding; full/reduced replacement cycle verified | Broader visible-quality acceptance and representative EM2/EM1/co-op scenes; retain original caster/material rules; see [distance shadows](distance-shadows.md) |
| Sixteen online players | `Session` transports two input streams; host-replicated profiles enforce unique colors; native lobby name/readiness controls and sixteen-entry roster codec are implemented; fourteen additional native actors have passed construction/admission/cleanup | Sixteen actual independently controllable actors, per-instance pants and matching overhead usernames, host-enforced color uniqueness, synchronized interactions and transitions on separate machines |
| Invite-only and public lobbies | Existing native menu and two-player preparation path | Authenticated host policy, invite admission, public discovery/matchmaking, disconnect/rejoin handling and tested Internet connectivity |
| Simple content mounting | EM2 ISO/folder first-launch picker; native Game Library page selects/persists EM1 RVZ, remembered-source startup verified | Human acceptance of the picker and missing/moved-source recovery; source replacement while mounted remains unavailable |
| Normal EM1 access | Private GV direct entry registers native local player/controller and reloads through CleanLoadLevel; collision lifetime verified | Accessible campaign/level entry through normal UI and gameplay, working controls, correct rendering, safe saves and transitions |
| EM1 online access | Not accepted | Both peers validate matching source/content and enter, play and transition through the same EM1 level |
| Existing planned features | Individual diagnostics and partial implementations | Gameplay acceptance of timing, abilities/animations/damage, rebinding, character swapping, rendering options and console tools |
| Wider distribution | Experimental allowlisted packager | Dependency/license inventory, reproducible clean build, clean-machine installation, upgrade/uninstall behavior and no bundled personal or original game data |

Public matchmaking and easy Internet joining require a deployed, tested service
and connectivity path. A roster-only implementation, LAN discovery, or a manual
IP workflow does not satisfy this requirement.

After the preceding requirements pass, implement the requested modding layer:
changed-file/property overrides without packfile repacking; regular image/model
formats with automatic conversion and cache invalidation; dependency/conflict
handling; and in-game entity inspection that exposes asset provenance and can
create texture/model swaps. Mod creation must not modify the original sources.

### Existing experimental package gates

- Validate the native picker and importer with a real retail Epic Mickey 2 ISO.
  Synthetic parser/extraction fixtures do not establish retail-image acceptance.
- Finish gameplay acceptance for high FPS, movement/contacts and level changes.
- Finish actual multiplayer actor/transport/UI integration, then test separate
  machines and real network conditions. Roster models are not playable slots.
- Configure and test the external services needed by Discord invites and easy
  Internet joining. Do not describe a local-only setup as router-free online play.
- Audit the full static SDK/compiler/runtime license inventory and project
  licensing before public distribution. The installed SDK's supplied notices
  alone are not a completed dependency audit.

The package manifest deliberately labels these snapshots experimental. Building
a minimal package does not certify the unfinished game or network features.

### Beetleworx release and updater contract

The only player executable is `beetleworx.exe`; the CMake target remains
`EM2Recomp` as an internal build identifier. Current symbol outputs are
`beetleworx.map` and `beetleworx.pdb`, and normal diagnostics use
`beetleworx.log`. Repository paths, `em2_*` flags, Xbox title identifiers and
`user_data` locations retain their existing names. Historical captures keep
their recorded module names; crash/sample readers accept those old records.

The format-2 release manifest includes `product: "Beetleworx"`,
`executable: "beetleworx.exe"`, `version` and `tag`. Packaging defaults to
`0.1.0-demo.1` / `v0.1.0-demo.1`; `-Version` and `-Tag` support later releases.
Versions follow [SemVer 2](https://semver.org/), including prerelease and build
metadata. A tag must exactly equal the version or `v` plus the version.
The final executable hash remains mandatory for a demo snapshot.

Future publication target: `Bloticle/beetleworx`. No repository or release is
created by these scripts. After the coordinated build and release acceptance,
the publication workflow is:

1. Prepare the approved version/tag and build the ZIP with its root manifest.
2. Create the repository and release only when publication is authorized.
3. Use the exact manifest tag, mark demo versions as prereleases, and attach the
   complete `Beetleworx-PublicDemo-20261110.zip` asset.
4. Check the published asset's SHA-256 digest against the local ZIP hash before
   directing players to it. The updater consumes GitHub's asset digest and the
   package manifest; it does not install an unverified download. GitHub exposes
   the digest in its [release-asset API](https://docs.github.com/en/rest/releases/assets).

Verify the final local ZIP without extracting or running it:

```powershell
python ./scripts/verify-release-zip.py D:/Releases/Beetleworx-PublicDemo-20261110.zip --demo --version 0.1.0-demo.1 --tag v0.1.0-demo.1 --expected-exe-sha256 <validated-final-sha256>
```

This checks the exact ZIP inventory, allowed payload paths, native updater ZIP
bounds, every manifest file hash and the approved executable hash, then reports
the ZIP SHA-256 for comparison with the published asset. The test does not
replace the native transaction and mapped-image tests.

The release snapshot and ZIP contain no alternate game executable. Old runtime
images must be closed and retired by the coordinated build owner after the
rename; packaging explicitly excludes a leftover `EM2Recomp.exe`.

Format references used for the native reader:
[Xenia's disc device](https://github.com/xenia-project/xenia/blob/master/src/xenia/vfs/devices/disc_image_device.cc),
[original Xbox-Linux XDVDFS documentation](https://multimedia.cx/xdvdfs.html), and
[UnleashedRecomp's partition reader](https://github.com/hedge-dev/UnleashedRecomp/blob/main/UnleashedRecomp/install/iso_file_system.cpp).

### Pinned Rust/Lua notices

The package now includes `licenses/rust-dependencies.txt`: 128,347 bytes covering
44 resolved dependencies, including build-time and optional source packages.
`scripts/prepare-rust-notices.py` generates it offline from the locked Windows
Cargo graph and exact cached source notices, deduplicating identical texts. It
also preserves supplemental copyright/Unicode/WHATWG notices. The package step
checks both the Cargo.lock hash and notice-content hash without needing Cargo
or a Rust compiler. Positive validation and deliberate stale-lock/changed-text
checks passed. Regenerate after changing dependencies.

`mlua-sys` lacks a top-level notice in its crate archive. Both it and `mlua`
identify the same repository and revision
`e7fa8d75bb3947648605562694d03c1ce4c82b20`; generation verifies that relationship
before including the shared project-root LICENSE. It fails if that provenance
changes. This is not a generic MIT notice substituted for an unknown source.

The wider dependency audit remains open. Local inventory
`analysis/release-dependency-audit.json` finds only `SDL2/LICENSE.txt` in the
installed SDK notice directory. Matching SDK/static dependency notices,
FFmpeg/libmspack configuration and source obligations, compiler/runtime notices
and project licensing still require resolution. The checked package plan is
29 payload files and remains explicitly experimental.

### Live screen-upscaler settings correction

The native settings callback previously returned before its deferred presenter
branch for both Screen upscaler and CAS / FSR sharpness. Their settings persisted,
but their live application code was unreachable. Both setting IDs now reach that
branch, preserving revision checks and UI-thread application. Build
`B58044A0B71C97D648C69B2DED630B856963434DF593076D3D1ABFF74E78F409` passes the
full runtime regressions and 29-file package dry run. This is a callback routing
fix; no game input or screenshots were used to claim visual acceptance.
