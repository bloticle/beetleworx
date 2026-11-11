param([string]$Level='MeanStreet_North.Visit_1')
$ErrorActionPreference='Stop'
if ($Level -notmatch '^[A-Za-z0-9_]+\.[A-Za-z0-9_]+$') {throw 'Expected a native level.state name.'}
$repo=Split-Path $PSScriptRoot -Parent
$staged=Join-Path $repo 'build/runtime23/pc-assets'
$qaRoot=Join-Path $repo ('analysis/swap-qa-'+[DateTime]::Now.ToString('yyyyMMdd-HHmmss-fff'))
$qaAssets=Join-Path $qaRoot 'assets'
$qaSaves=Join-Path $qaRoot 'user_data'
New-Item -ItemType Directory -Path $qaAssets,$qaSaves | Out-Null
foreach($entry in Get-ChildItem -LiteralPath $staged -Force) {
    if($entry.Name -in @('cmdline.txt','ConfigFiles.ini')) {continue}
    $target=Join-Path $qaAssets $entry.Name
    $kind=if($entry.PSIsContainer){'Junction'}else{'HardLink'}
    New-Item -ItemType $kind -Path $target -Target $entry.FullName | Out-Null
}
Copy-Item -LiteralPath (Join-Path $staged 'ConfigFiles.ini') -Destination (Join-Path $qaAssets 'ConfigFiles.ini')
Copy-Item -LiteralPath (Join-Path $staged 'cmdline.txt') -Destination (Join-Path $qaAssets 'cmdline.txt')
$currentSaves=Join-Path $repo 'build/runtime23/user_data'
foreach($entry in Get-ChildItem -LiteralPath $currentSaves -Force) {
    Copy-Item -LiteralPath $entry.FullName -Destination $qaSaves -Recurse
}
[ordered]@{level=$Level;assets=$qaAssets;user_data=$qaSaves} | ConvertTo-Json | Tee-Object -FilePath (Join-Path $qaRoot 'session.json')
