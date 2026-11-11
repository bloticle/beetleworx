param([string]$GameDirectory = 'D:\Modding\Builds\EM2X360')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$source = (Resolve-Path -LiteralPath $GameDirectory).Path
$destination = Join-Path $root 'build/runtime23/pc-assets'
New-Item -ItemType Directory -Path $destination -Force | Out-Null

# Read-only content view: large original directories are junctions, ordinary
# files are hard links, and only modified menus/config are independent files.
# Never write through the original-file links.
foreach ($entry in Get-ChildItem -LiteralPath $source -Force) {
    if ($entry.Name -eq 'packfiles' -or $entry.Name -eq 'ConfigFiles.ini') { continue }
    $target = Join-Path $destination $entry.Name
    if (!(Test-Path -LiteralPath $target)) {
        $type = if ($entry.PSIsContainer) { 'Junction' } else { 'HardLink' }
        New-Item -ItemType $type -Path $target -Target $entry.FullName | Out-Null
    }
}
$packDestination = Join-Path $destination 'packfiles'
New-Item -ItemType Directory -Path $packDestination -Force | Out-Null
foreach ($entry in Get-ChildItem -LiteralPath (Join-Path $source 'packfiles') -Force) {
    if ($entry.Name -in @('UIOptions.pak','UIPauseOptions.pak','UIhud.pak','UIStartGame.pak','UIMainMenu.pak','_Dynamic.pak','shaders.pak')) { continue }
    $target = Join-Path $packDestination $entry.Name
    if (!(Test-Path -LiteralPath $target)) {
        $type = if ($entry.PSIsContainer) { 'Junction' } else { 'HardLink' }
        New-Item -ItemType $type -Path $target -Target $entry.FullName | Out-Null
    }
}
Set-Content -LiteralPath (Join-Path $destination 'source-game-directory.txt') -Value $source -Encoding utf8NoBOM
Write-Output 'Staged read-only PC content view; original game assets preserved.'
