$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$upstream = Join-Path $root 'tools/XenonRecomp'
$patch = Join-Path $root 'patches/xenonrecomp.patch'
git -C $upstream apply --reverse --check $patch 2>$null
if ($LASTEXITCODE -eq 0) { return }
git -C $upstream apply --check $patch
if ($LASTEXITCODE) { throw 'XenonRecomp differs from the expected upstream version; patch was not applied.' }
git -C $upstream apply $patch
if ($LASTEXITCODE) { throw 'Could not apply EM2 XenonRecomp fixes.' }
