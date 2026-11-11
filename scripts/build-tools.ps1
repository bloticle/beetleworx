$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/enter-toolchain.ps1"
Push-Location $root
try {
    & "$PSScriptRoot/apply-patches.ps1"
    cmake -S . -B build/tools -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=$llvm/clang-cl.exe" "-DCMAKE_CXX_COMPILER=$llvm/clang-cl.exe"
    if ($LASTEXITCODE) { throw 'Tool configuration failed.' }
    cmake --build build/tools --target XenonRecomp XenonAnalyse em2-inspect em2-disasm em2-analysis-tests em2-lowering-fixture --parallel 2
    if ($LASTEXITCODE) { throw 'Tool build failed.' }
} finally { Pop-Location }
