$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    git submodule update --init --recursive
    if ($LASTEXITCODE) { throw 'Could not initialize XenonRecomp dependencies.' }
    if (!(Test-Path tools/llvm23/clang+llvm-23.1.4-x86_64-pc-windows-msvc/lib/clang/23/include/stddef.h)) {
        New-Item -ItemType Directory -Force tools/downloads | Out-Null
        $package = 'tools/downloads/llvm-23.1.4.tar.zst'
        $url = 'https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.4/clang%2Bllvm-23.1.4-x86_64-pc-windows-msvc.tar.zst'
        $expected = '1a3f2f998e230b5791cc4e2bfe11a1f7c07fe42c96ff44f227c9db0bedfaf091'
        if (!(Test-Path $package)) {
            curl.exe -L --fail --silent --show-error $url -o $package
            if ($LASTEXITCODE) { throw 'LLVM download failed.' }
        }
        if ((Get-FileHash $package -Algorithm SHA256).Hash.ToLower() -ne $expected) { throw 'LLVM checksum mismatch.' }
        $sevenZip = @("$env:ProgramFiles\7-Zip\7z.exe", "D:\Program Files\7-Zip\7z.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
        if (!$sevenZip) { throw '7-Zip is required to extract the portable LLVM toolchain.' }
        & $sevenZip x $package -otools/downloads -y -bso0 -bsp0
        if ($LASTEXITCODE) { throw 'LLVM decompression failed.' }
        $prefix = 'clang+llvm-23.1.4-x86_64-pc-windows-msvc'
        & $sevenZip x tools/downloads/llvm-23.1.4.tar -otools/llvm23 "$prefix/bin/clang-cl.exe" "$prefix/bin/lld-link.exe" "$prefix/bin/llvm-mt.exe" "$prefix/lib/clang/*" -y -bso0 -bsp0
        if ($LASTEXITCODE) { throw 'LLVM extraction failed.' }
    }
    & "$PSScriptRoot/build-tools.ps1"
} finally { Pop-Location }
