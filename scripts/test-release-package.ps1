$ErrorActionPreference='Stop'
$workspace=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$root=Join-Path $workspace ('analysis/release-package-fixture-'+[Guid]::NewGuid().ToString('N'))
$runtime=Join-Path $root 'runtime';$crt=Join-Path $root 'crt';$sdk=Join-Path $root 'sdk';$output=Join-Path $root 'package';$patches=Join-Path $root 'patches'
function WriteFixture([string]$Relative,[string]$Text){$path=Join-Path $root $Relative;New-Item -ItemType Directory -Path (Split-Path $path -Parent) -Force | Out-Null;[IO.File]::WriteAllText($path,$Text)}
foreach($name in @('beetleworx.exe','dxcompiler.dll','dxil.dll')){WriteFixture ('runtime/'+$name) 'inert packaging fixture; never executed'}
foreach($name in @('msvcp140.dll','msvcp140_atomic_wait.dll','vcruntime140.dll','vcruntime140_1.dll')){WriteFixture ('crt/'+$name) 'inert runtime fixture'}
foreach($name in @('UIOptions.pak','UIPauseOptions.pak','UIhud.pak','UIStartGame.pak','UIMainMenu.pak','_Dynamic.pak','shaders.pak')){WriteFixture ('runtime/pc-assets/packfiles/'+$name) 'patched content fixture'}
WriteFixture 'runtime/pc-assets/pc-shader-catalog.em2' 'catalog'
WriteFixture 'runtime/pc-assets/pc-shader-library/rex-fixture/native-profile-v1/translated-dxbc-v1/test.em2dxbc' ('s'*64)
WriteFixture 'runtime/pc-assets/pc-shader-library/rex-fixture/native-profile-v1/archive-preparation-results.json' 'private developer audit'
WriteFixture 'runtime/pc-assets/pc-shader-library/rex-old/native-profile-v1/translated-dxbc-v1/old.em2dxbc' 'stale shader'
WriteFixture 'sdk/licenses/example/LICENSE.txt' 'provided SDK notice'
foreach($unwanted in @('user_data/save.dat','user_data/controls.ini','cache/driver.bin','beetleworx.pdb','beetleworx.map','runtime-tests.log','em2-lowering-tests.exe','EM2Recomp.exe','CMakeCache.txt','pc-assets/default.xex','pc-assets/source-game-directory.txt')){WriteFixture ('runtime/'+$unwanted) 'DO NOT PACKAGE'}
$record=[IO.File]::ReadAllBytes((Join-Path $runtime 'pc-assets/pc-shader-library/rex-fixture/native-profile-v1/translated-dxbc-v1/test.em2dxbc'))
$bundle=[IO.MemoryStream]::new();$writer=[IO.BinaryWriter]::new($bundle);$writer.Write([Text.Encoding]::ASCII.GetBytes('EM2DXPK1'));$writer.Write([uint32]1);$writer.Write([uint32]$record.Length);$writer.Write($record);$writer.Flush()
[IO.File]::WriteAllBytes((Join-Path $runtime 'pc-assets/pc-shader-library/rex-fixture/native-profile-v1/translated-dxbc-v1/library.em2dxbcpack'),$bundle.ToArray());$writer.Dispose();$bundle.Dispose()
$patchRecords=@(foreach($name in @('UIOptions.pak','UIPauseOptions.pak','UIhud.pak','UIStartGame.pak','UIMainMenu.pak','_Dynamic.pak','shaders.pak')){
    WriteFixture ('patches/'+$name+'.em2patch') 'synthetic patch fixture; not executed'
    [ordered]@{name=$name;input_sha256=(Get-FileHash -LiteralPath (Join-Path $runtime ('pc-assets/packfiles/'+$name))).Hash;patch_sha256=(Get-FileHash -LiteralPath (Join-Path $patches ($name+'.em2patch'))).Hash}
})
[ordered]@{format=1;archives=$patchRecords} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $patches 'manifest.json') -Encoding utf8NoBOM
$expected=(Get-FileHash -LiteralPath (Join-Path $runtime 'beetleworx.exe')).Hash
$arguments=@{RuntimeDirectory=$runtime;CrtDirectory=$crt;RexSdk=$sdk;RendererAbi='rex-fixture';PatchDirectory=$patches;ExpectedExeSha256=$expected;Demo=$true}
& "$PSScriptRoot/package-release.ps1" @arguments -Destination $output -Zip | Out-Null
$manifest=Get-Content -LiteralPath (Join-Path $output 'release-manifest.json') -Raw | ConvertFrom-Json
if($manifest.includes_personal_data -or $manifest.includes_separate_launcher -or $manifest.includes_full_modified_game_archives -or $manifest.shader_records -ne 1 -or !$manifest.demo -or $manifest.online_multiplayer -ne 'hidden'){throw 'Release manifest constraints failed.'}
if($manifest.product -cne 'Beetleworx' -or $manifest.executable -cne 'beetleworx.exe' -or $manifest.version -cne '0.1.0-demo.1' -or $manifest.tag -cne 'v0.1.0-demo.1'){throw 'Release branding/version/tag contract failed.'}
# Exercise the real default CRT discovery path: PowerShell variable names are
# case-insensitive, so a local $version previously overwrote the release version.
$crtAutodiscoveryChecked=$false
if(Test-Path -LiteralPath (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe')){
    $autoArguments=$arguments.Clone();$autoArguments.CrtDirectory=''
    $autoPlan=(& "$PSScriptRoot/package-release.ps1" @autoArguments -Destination (Join-Path $root 'auto-crt-plan') -PlanOnly) | ConvertFrom-Json
    if($autoPlan.version -cne '0.1.0-demo.1' -or $autoPlan.tag -cne 'v0.1.0-demo.1'){throw 'CRT discovery clobbered release version/tag.'}
    $crtAutodiscoveryChecked=$true
}
$actual=@(Get-ChildItem -LiteralPath $output -Recurse -File)
if(@($actual | Where-Object Extension -eq '.exe').Count -ne 1){throw 'Release contains an extra executable.'}
foreach($file in $actual){if((Get-Content -LiteralPath $file.FullName -Raw) -eq 'DO NOT PACKAGE'){throw 'Personal/developer/original payload leaked into package.'}}
if(@($actual | Where-Object Extension -eq '.pak').Count -or @($actual | Where-Object Extension -eq '.em2dxbc').Count){throw 'Full PAK archives or loose shader developer records leaked into package.'}
foreach($entry in $manifest.files){if((Get-FileHash -LiteralPath (Join-Path $output $entry.path)).Hash -ne $entry.sha256){throw 'Manifest does not match copied payload.'}}
$archive=[IO.Compression.ZipFile]::OpenRead($output+'.zip');try{if($archive.Entries.Count -ne $actual.Count -or @($archive.Entries | Where-Object FullName -eq 'beetleworx.exe').Count -ne 1){throw 'ZIP payload differs from reviewed package.'}}finally{$archive.Dispose()}
$refused=$false;try{& "$PSScriptRoot/package-release.ps1" @arguments -Destination $output | Out-Null}catch{$refused=$true}
if(!$refused){throw 'Existing package destination was overwritten.'}
foreach($invalidVersion in @('01.0.0','0.1.0-demo.01','0.1','0.1.0-','18446744073709551616.0.0',('0.1.0-'+('x'*122)))){
    $rejected=$false;try{& "$PSScriptRoot/package-release.ps1" @arguments -Destination (Join-Path $root 'bad-version') -Version $invalidVersion | Out-Null}catch{$rejected=$true}
    if(!$rejected -or (Test-Path -LiteralPath (Join-Path $root 'bad-version'))){throw 'Invalid SemVer was accepted.'}
}
$rejected=$false;try{& "$PSScriptRoot/package-release.ps1" @arguments -Destination (Join-Path $root 'bad-tag') -Tag 'v0.2.0' | Out-Null}catch{$rejected=$true}
if(!$rejected -or (Test-Path -LiteralPath (Join-Path $root 'bad-tag'))){throw 'Mismatched update tag was accepted.'}
$badHashOutput=Join-Path $root 'wrong-build';$arguments.ExpectedExeSha256='0'*64;$refused=$false
try{& "$PSScriptRoot/package-release.ps1" @arguments -Destination $badHashOutput | Out-Null}catch{$refused=$true}
if(!$refused -or (Test-Path -LiteralPath $badHashOutput)){throw 'An unapproved build was published.'};$arguments.ExpectedExeSha256=$expected
# Missing required payload is detected before any new destination is created.
$missingOutput=Join-Path $root 'missing-output';$refused=$false
Move-Item -LiteralPath (Join-Path $runtime 'dxil.dll') -Destination (Join-Path $root 'held-dxil.dll')
try{& "$PSScriptRoot/package-release.ps1" @arguments -Destination $missingOutput | Out-Null}catch{$refused=$true}
if(!$refused -or (Test-Path -LiteralPath $missingOutput)){throw 'Incomplete source package was published.'}
[ordered]@{passed=$true;root=$root;payload_files=$manifest.file_count;only_one_executable=$true;personal_original_developer_files_excluded=$true;changed_only_patches=$true;shader_bundle_only=$true;demo_online_hidden=$true;approved_executable_hash_required=$true;crt_autodiscovery_preserves_version=$crtAutodiscoveryChecked;zip_matches_package=$true;manifest_hashes_match=$true;existing_destination_preserved=$true;missing_payload_rejected_before_output=$true;game_process_started=$false} | ConvertTo-Json
