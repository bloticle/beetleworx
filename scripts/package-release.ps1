param(
    [string]$RuntimeDirectory='',
    [string]$Destination='',
    [string]$CrtDirectory='',
    [string]$RexSdk='',
    [string]$RendererAbi='',
    [string]$PatchDirectory='',
    [string]$ExpectedExeSha256='',
    [string]$Version='0.1.0-demo.1',
    [string]$Tag='',
    [switch]$PlanOnly,
    [switch]$Demo,
    [switch]$Zip
)
$ErrorActionPreference='Stop'
$semver=[regex]::Match($Version,'\A(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?\z')
if(!$semver.Success -or $Version.Length -ge 128){throw 'Version must be a valid SemVer 2 version shorter than 128 characters.'}
foreach($index in 1..3){[uint64]$component=0;if(![uint64]::TryParse($semver.Groups[$index].Value,[ref]$component)){throw 'Version components must fit the native updater integer range.'}}
foreach($identifier in $semver.Groups[4].Value.Split('.')){if($identifier -match '^[0-9]+$' -and $identifier.Length -gt 1 -and $identifier.StartsWith('0')){throw 'Numeric SemVer prerelease identifiers cannot have leading zeroes.'}}
if(!$Tag){$Tag='v'+$Version}
if($Tag -cne $Version -and $Tag -cne ('v'+$Version)){throw 'Tag must exactly match Version, optionally prefixed with v.'}
function FileHash([string]$Path){
    $stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    try{return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream))}
    finally{$stream.Dispose()}
}
$workspace=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if(!$RuntimeDirectory){$RuntimeDirectory=Join-Path $workspace 'build/runtime23'}
$RuntimeDirectory=(Resolve-Path -LiteralPath $RuntimeDirectory).Path
if(!$Destination){$label=if($Demo){'Beetleworx-PublicDemo-'}else{'Beetleworx-'};$Destination=Join-Path $workspace ('build/releases/'+$label+[DateTime]::UtcNow.ToString('yyyyMMdd'))}
$Destination=[IO.Path]::GetFullPath($Destination)
if(Test-Path -LiteralPath $Destination){throw 'Release destination already exists. Choose a new folder; packaging never overwrites a previous release.'}
if($Zip -and (Test-Path -LiteralPath ($Destination+'.zip'))){throw 'Release ZIP already exists. Choose a new destination.'}
if($ExpectedExeSha256 -and $ExpectedExeSha256 -notmatch '^[0-9A-Fa-f]{64}$'){throw 'ExpectedExeSha256 must be the complete SHA-256 of the validated final executable.'}
if($Demo -and !$PlanOnly -and !$ExpectedExeSha256){throw 'Demo packaging requires -ExpectedExeSha256 from the validated final build.'}
if($Destination.StartsWith($RuntimeDirectory+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Release destination must be outside the runtime build folder.'}
if(!$RexSdk){
    $cache=Join-Path $RuntimeDirectory 'CMakeCache.txt'
    if(Test-Path -LiteralPath $cache){$sdkLine=Get-Content -LiteralPath $cache | Where-Object {$_ -match '^REXSDK:PATH='} | Select-Object -First 1;if($sdkLine){$RexSdk=$sdkLine.Substring('REXSDK:PATH='.Length)}}
}
if(!$RendererAbi){
    $renderer=Join-Path $RexSdk 'lib/rexgraphics.lib'
    if(!(Test-Path -LiteralPath $renderer)){throw 'Cannot determine the current shader ABI. Supply -RexSdk or -RendererAbi.'}
    $RendererAbi='rex-'+(FileHash $renderer).Substring(0,16).ToLowerInvariant()
}
if($RendererAbi -notmatch '^rex-[A-Za-z0-9_-]+$'){throw 'Invalid renderer ABI directory name.'}
if(!$CrtDirectory){
    $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if(Test-Path -LiteralPath $vswhere){
        $vs=& $vswhere -latest -products '*' -property installationPath
        if($vs){
            $redist=Join-Path $vs 'VC/Redist/MSVC'
            $redistVersion=Get-ChildItem -LiteralPath $redist -Directory | Where-Object {$_.Name -match '^\d+\.\d+\.\d+$'} | Sort-Object {[Version]$_.Name} -Descending | Select-Object -First 1
            if($redistVersion){$crt=Get-ChildItem -LiteralPath (Join-Path $redistVersion.FullName 'x64') -Directory | Where-Object {$_.Name -like 'Microsoft.VC*.CRT'} | Select-Object -First 1;if($crt){$CrtDirectory=$crt.FullName}}
        }
    }
}
if(!$CrtDirectory -or !(Test-Path -LiteralPath $CrtDirectory)){throw 'The app-local x64 Visual C++ runtime is missing. Supply -CrtDirectory pointing to Microsoft.VC*.CRT.'}
$files=[Collections.Generic.List[object]]::new()
$names=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
function AddFile([string]$Source,[string]$Relative){
    $item=[IO.FileInfo]::new($Source)
    if(!$item.Exists){throw "Required release file is missing: $Source"}
    if([IO.Path]::IsPathRooted($Relative) -or $Relative -match '(^|[\\/])\.\.([\\/]|$)'){throw 'Unsafe package-relative path.'}
    if(!$names.Add($Relative)){throw "Duplicate release payload path: $Relative"}
    $files.Add([pscustomobject]@{source=$item.FullName;path=$Relative.Replace([IO.Path]::DirectorySeparatorChar,'/');bytes=$item.Length;sha256=(FileHash $item.FullName)})
}
foreach($name in @('beetleworx.exe','dxcompiler.dll','dxil.dll')){AddFile (Join-Path $RuntimeDirectory $name) $name}
if($ExpectedExeSha256 -and ($files | Where-Object path -eq 'beetleworx.exe').sha256 -ne $ExpectedExeSha256){throw 'The executable differs from the approved final build. Validate the current build before packaging.'}
foreach($name in @('msvcp140.dll','msvcp140_atomic_wait.dll','vcruntime140.dll','vcruntime140_1.dll')){if(!(Test-Path -LiteralPath (Join-Path $CrtDirectory $name))){throw "Required app-local runtime DLL missing: $name"}}
foreach($dll in Get-ChildItem -LiteralPath $CrtDirectory -File -Filter '*.dll'){AddFile $dll.FullName $dll.Name}
if(!$PatchDirectory){$PatchDirectory=Join-Path $workspace 'private/content-patches'}
$patchManifestPath=Join-Path $PatchDirectory 'manifest.json'
if(!(Test-Path -LiteralPath $patchManifestPath -PathType Leaf)){throw 'Build changed-only content patches with scripts/build-content-patches.py before packaging.'}
$patchManifest=Get-Content -LiteralPath $patchManifestPath -Raw | ConvertFrom-Json
if($patchManifest.format -ne 1 -or @($patchManifest.archives).Count -ne 7){throw 'Invalid content patch manifest.'}
foreach($archive in @('UIOptions.pak','UIPauseOptions.pak','UIhud.pak','UIStartGame.pak','UIMainMenu.pak','_Dynamic.pak','shaders.pak')){
    $record=@($patchManifest.archives | Where-Object name -eq $archive)
    if($record.Count -ne 1){throw "Content patch manifest is missing or repeats $archive"}
    $patch=Join-Path $PatchDirectory ($archive+'.em2patch')
    $staged=Join-Path $RuntimeDirectory ('pc-assets/packfiles/'+$archive)
    if((FileHash $staged) -ne $record[0].input_sha256 -or (FileHash $patch) -ne $record[0].patch_sha256){throw "Content patch is stale or corrupt: $archive. Rebuild content patches before packaging."}
    AddFile $patch ('pc-assets/patches/'+$archive+'.em2patch')
}
AddFile $patchManifestPath 'pc-assets/patches/manifest.json'
AddFile (Join-Path $RuntimeDirectory 'pc-assets/pc-shader-catalog.em2') 'pc-assets/pc-shader-catalog.em2'
$library=Join-Path $RuntimeDirectory ('pc-assets/pc-shader-library/'+$RendererAbi+'/native-profile-v1')
if(!(Test-Path -LiteralPath $library -PathType Container)){throw 'Prepare the current PC shader library before packaging.'}
$records=@(Get-ChildItem -LiteralPath $library -Recurse -File -Filter '*.em2dxbc')
if(!$records.Count){throw 'The prepared PC shader library is empty.'}
$bundle=Join-Path $library 'translated-dxbc-v1/library.em2dxbcpack'
if(!(Test-Path -LiteralPath $bundle -PathType Leaf)){throw 'Prepare the native shader library bundle before packaging.'}
$bundleReport=& python (Join-Path $PSScriptRoot 'verify-prepared-shader-pack.py') $library
if($LASTEXITCODE -ne 0){throw 'Prepared shader bundle differs from its loose source records; re-run shader preparation.'}
$bundleInfo=$bundleReport | ConvertFrom-Json
if($bundleInfo.records -ne $records.Count -or $bundleInfo.format -ne 'EM2DXPK1'){throw 'Prepared shader bundle verification result is invalid.'}
AddFile $bundle ('pc-assets/pc-shader-library/'+$RendererAbi+'/native-profile-v1/translated-dxbc-v1/library.em2dxbcpack')
AddFile (Join-Path $workspace 'docs/QUICK_START.txt') 'START HERE.txt'
if($Demo){
    AddFile (Join-Path $workspace 'docs/demo-channel.txt') 'demo-channel.txt'
    AddFile (Join-Path $workspace 'docs/DEMO_TESTING.txt') 'TESTING & KNOWN ISSUES.txt'
}
$rustNotices=& python (Join-Path $PSScriptRoot 'prepare-rust-notices.py') --check
if($LASTEXITCODE -ne 0){throw 'Rust/Lua dependency notices are stale or invalid. Run scripts/prepare-rust-notices.py before packaging.'}
AddFile (Join-Path $workspace 'docs/licenses/rust-dependencies.txt') 'licenses/rust-dependencies.txt'
AddFile (Join-Path $workspace 'docs/licenses/xenia-bsd.txt') 'licenses/xenia-bsd.txt'
AddFile (Join-Path $workspace 'runtime/em1_compat/zstd/LICENSE') 'licenses/zstd-bsd.txt'
AddFile (Join-Path $workspace 'runtime/em1_compat/zlib/LICENSE') 'licenses/zlib.txt'
if($RexSdk -and (Test-Path -LiteralPath (Join-Path $RexSdk 'licenses'))){foreach($notice in Get-ChildItem -LiteralPath (Join-Path $RexSdk 'licenses') -Recurse -File){AddFile $notice.FullName ('licenses/sdk/'+[IO.Path]::GetRelativePath((Join-Path $RexSdk 'licenses'),$notice.FullName))}}
$summary=[ordered]@{format=2;product='Beetleworx';executable='beetleworx.exe';status='Experimental development package; release acceptance and license inventory remain open';exe_sha256=($files | Where-Object path -eq 'beetleworx.exe').sha256;shader_abi=$RendererAbi;shader_records=$records.Count;file_count=$files.Count;payload_bytes=($files | Measure-Object bytes -Sum).Sum;content_patch_format='EM2DP01';includes_full_modified_game_archives=$false;includes_full_original_game=$false;includes_personal_data=$false;includes_separate_launcher=$false;files=@($files | Sort-Object path | Select-Object path,bytes,sha256)}
$summary.version=$Version;$summary.tag=$Tag
if($Demo){$summary.status='Beetleworx public testing demo - EM2 and EM1 courtyard compatibility preview';$summary.demo=$true;$summary.online_multiplayer='hidden';$summary.expected_exe_sha256=$ExpectedExeSha256.ToUpperInvariant()}
if($PlanOnly){$summary | ConvertTo-Json -Depth 5;return}
New-Item -ItemType Directory -Path $Destination | Out-Null
foreach($entry in $files){
    $target=Join-Path $Destination $entry.path
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    Copy-Item -LiteralPath $entry.source -Destination $target
    if((FileHash $target) -ne $entry.sha256){throw "Release snapshot changed or copy failed: $($entry.path). Discard this incomplete output folder."}
}
if((FileHash (Join-Path $RuntimeDirectory 'beetleworx.exe')) -ne $summary.exe_sha256){throw 'The executable changed during packaging. Discard this incomplete output folder and retry after the build finishes.'}
$summary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Destination 'release-manifest.json') -Encoding utf8NoBOM
Write-Output ('Release snapshot: '+$Destination)
Write-Output ('Payload: '+$files.Count+' files, '+[Math]::Round($summary.payload_bytes/1MB,1)+' MiB; saves, original game, driver caches, symbols and diagnostic executables excluded.')
if($Zip){
    [IO.Compression.ZipFile]::CreateFromDirectory($Destination,($Destination+'.zip'),[IO.Compression.CompressionLevel]::Optimal,$false)
    Write-Output ('Release ZIP: '+$Destination+'.zip')
    Write-Output ('ZIP SHA256: '+(FileHash ($Destination+'.zip')))
}
