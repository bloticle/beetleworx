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

The build checks the decoded executable and import layout before generating code. The supplied USA/Europe disc was verified to contain the supported executable. An executable with a different layout needs its own port of the native hooks and address mappings. The generated Windows executable checks that it is paired with the Xbox executable used for translation.

The Windows executable is `build/runtime23/beetleworx.exe`. Its saves and settings are in `build/runtime23/user_data`.
