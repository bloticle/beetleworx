$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$upstream = Join-Path $root 'tools/XenonRecomp'
foreach($name in @('xenonrecomp.patch','xenonrecomp-raw-image.patch')) {
    $patch = Join-Path $root "patches/$name"
    git -C $upstream apply --reverse --check $patch 2>$null
    if ($LASTEXITCODE -eq 0) { continue }
    git -C $upstream apply --check $patch
    if ($LASTEXITCODE) { throw "XenonRecomp differs from the expected upstream version; $name was not applied." }
    git -C $upstream apply $patch
    if ($LASTEXITCODE) { throw "Could not apply $name." }
}
