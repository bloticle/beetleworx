# Building Beetleworx

Use a Windows x64 development machine with PowerShell 7, Visual Studio C++ build tools and a Windows SDK, CMake, Python 3.11 or newer, Java, 7-Zip, and Rust with the MSVC toolchain. Install the [ReXGlue 0.1 SDK](https://github.com/rexglue/rexglue-sdk) locally and pass its directory to the build script.

```powershell
git clone --recurse-submodules https://github.com/bloticle/beetleworx.git
cd beetleworx
$game = 'C:\Games\Epic Mickey 2 Xbox 360'
$sdk = 'C:\SDKs\rexglue-sdk'
./scripts/bootstrap.ps1
./scripts/recompile.ps1 -GameDirectory $game
./scripts/build.ps1 -GameDirectory $game -RexSdk $sdk
./scripts/run.ps1 -GameDirectory $game
```

Use your own extracted Xbox 360 disc files. The folder must contain `default.xex`, `ConfigFiles.ini`, and `packfiles`. The scripts generate translated code and menu patches locally. The optional local EMR animation GLB is used if present. Source builds without it use retail animations. Keep the extracted game folder in place because the development build links to its files.

The first build downloads the Rust dependencies pinned in `Cargo.lock`. Later builds can use the Cargo cache; set `CARGO_NET_OFFLINE=true` if you need an offline build after those dependencies are cached.

The build also generates renderer test inputs locally. The clear-shader fixture extracts its shader from the decoded executable and creates rectangle geometry from dimensions.

The build checks the decoded executable and import layout before generating code. The supplied USA/Europe disc was verified to contain the supported executable. At runtime, a different XEX container is accepted only if its untouched decoded image and execution profile (including imports and TLS) match the translation exactly. Encryption or compression differences alone do not require recompiling. Executables with different code, data or execution layouts still need a verified port of the native hooks and address mappings; this does not establish support for every regional or updated build.

The Windows executable is `build/runtime23/beetleworx.exe`. Its saves and settings are in `build/runtime23/user_data`.

Run the tool and runtime regression tests with the same game and SDK directories:

```powershell
./scripts/test.ps1 -GameDirectory $game -RexSdk $sdk
```

The compatibility tests generate private fixtures from your decoded executable, accept a different encoding of the same image, and reject changed image bytes, imports, entry points and truncated headers. They run in `beetleworx.exe` without guest execution. These synthetic fixtures verify the compatibility checks; they do not establish compatibility with another retail build.

To check the live updater explicitly, use a new directory under `analysis`:

```powershell
$check = Join-Path (Resolve-Path analysis) ('live-update-' + [Guid]::NewGuid().ToString('N'))
./build/runtime23/beetleworx.exe "--em2_test_live_update_path=$check" "--log_file=$check.log"
```

This downloads the published first demo and its newest update through the native WinHTTP transport, verifies their hashes, and applies the normal transactional installer to an isolated copy. It checks that user-file markers survive. It does not launch either downloaded executable or update the normal installation. This network check is opt-in and is not part of the offline regression suite.

The separate native camera clock is experimental and disabled by default. `--em2_present_camera=true` opts into presentation-rate updates for the primary offline normal gameplay camera above 30 FPS. World simulation stays at 30 Hz; camera position queries use the same interpolation phase as rendered characters. Scripted cameras, online sessions and unverified analog input retain the original update path. Controller action curves and focused movement still need validation.

For a passive developer capture using an isolated copy of the current save and settings:

```powershell
./scripts/start-private-gpu-profile.ps1 -Scale 1 -FrameCap 60 -GameplayTiming 0 -PresentCamera -TraceTiming -TraceCameraBackground -NoGpuProfile -FinalDemoAssets
```

The background diagnostic requires all three camera switches, zero cached stick intent and no pending mouse motion. It does not sample or consume background input. Its measured camera cadence does not establish focused control feel. The regression suite also checks device-axis conversion against the original producer, raw getter and action consumer for every int16 value on both axes across six deadzones; action binding and curve validation is separate.
