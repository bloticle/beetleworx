param([string]$GameDirectory = 'D:\Modding\Builds\EM2X360', [string]$RexSdk = 'D:\Repos\rexglue-sdk', [int]$Jobs = 2, [switch]$SkipShaderPreparation)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/enter-toolchain.ps1"
Push-Location $root
try {
    & "$PSScriptRoot/build-tools.ps1"
    ctest --test-dir build/tools --output-on-failure
    if ($LASTEXITCODE) { throw 'Function analysis tests failed.' }
    New-Item -ItemType Directory -Force generated/tests | Out-Null
    & build/tools/em2-lowering-fixture.exe generated/tests/instructions.cpp
    if ($LASTEXITCODE) { throw 'Fixture translation failed.' }
    & "$PSScriptRoot/build.ps1" -GameDirectory $GameDirectory -RexSdk $RexSdk -Jobs $Jobs -SkipShaderPreparation:$SkipShaderPreparation
    ctest --test-dir build/runtime23 --output-on-failure
    if ($LASTEXITCODE) { throw 'Runtime regression tests failed.' }
} finally { Pop-Location }
