$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$destination = Join-Path $root 'build/runtime23'
$sdkDirectory = if ($env:WindowsSdkVerBinPath) { Join-Path $env:WindowsSdkVerBinPath 'x64' } else { $null }
if (!$sdkDirectory -or !(Test-Path (Join-Path $sdkDirectory 'dxcompiler.dll'))) {
    throw 'Windows SDK shader compiler binaries are unavailable; enter the Visual Studio toolchain first.'
}
foreach ($dll in @('dxcompiler.dll', 'dxil.dll')) {
    Copy-Item -LiteralPath (Join-Path $sdkDirectory $dll) -Destination (Join-Path $destination $dll)
}
New-Item -ItemType Directory -Path (Join-Path $destination 'licenses') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'docs/licenses/xenia-bsd.txt') -Destination (Join-Path $destination 'licenses/xenia-bsd.txt')
