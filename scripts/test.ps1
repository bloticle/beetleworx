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
    & "$PSScriptRoot/build.ps1"
    ctest --test-dir build/runtime23 --output-on-failure
    if ($LASTEXITCODE) { throw 'Runtime regression tests failed.' }
} finally { Pop-Location }
