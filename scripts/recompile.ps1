param([string]$GameDirectory = 'D:\Modding\Builds\EM2X360')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    if (!(Test-Path build/tools/em2-inspect.exe)) { & "$PSScriptRoot/build-tools.ps1" }
    New-Item -ItemType Directory -Force private,analysis,generated/ppc,config | Out-Null
    # Keep unchanged generated files eligible for incremental native compilation.
    $previousSources = @{}
    Get-ChildItem -LiteralPath generated/ppc -File | ForEach-Object {
        $previousSources[$_.Name] = @((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash, $_.LastWriteTimeUtc)
    }
    Copy-Item -LiteralPath (Join-Path $GameDirectory 'default.xex') -Destination private/candidate.xex
    & build/tools/em2-inspect.exe private/candidate.xex private/candidate-image.bin > analysis/candidate-image.json
    if ($LASTEXITCODE) { throw 'XEX inspection failed.' }
    python scripts/check-game-image.py private/candidate.xex private/candidate-image.bin analysis/candidate-image.json
    if ($LASTEXITCODE) { throw 'The selected executable layout has no verified native hook profile.' }
    Move-Item -LiteralPath private/candidate.xex -Destination private/default.xex -Force
    Move-Item -LiteralPath private/candidate-image.bin -Destination private/image.bin -Force
    Move-Item -LiteralPath analysis/candidate-image.json -Destination analysis/image.json -Force
    $stamp = 'generated/ppc/em2_game_build.h'
    if(Test-Path -LiteralPath $stamp){Remove-Item -LiteralPath $stamp}
    python scripts/configure-game.py --game-dir $GameDirectory
    if ($LASTEXITCODE) { throw 'Register helper detection failed.' }
    & build/tools/tools/XenonRecomp/XenonAnalyse/XenonAnalyse.exe private/default.xex config/em2_switch_tables.toml *> analysis/analyse.log
    if ($LASTEXITCODE) { throw 'Switch table analysis failed; see analysis/analyse.log.' }
    python scripts/recover-switches.py
    if ($LASTEXITCODE) { throw 'EM2 inline switch recovery failed.' }
    # The upstream emitter does not remove obsolete translation units.
    Get-ChildItem -LiteralPath generated/ppc -File | Where-Object { $_.Name -match '^ppc_.*\.(cpp|h)$' } | ForEach-Object {
        Remove-Item -LiteralPath $_.FullName
    }
    & build/tools/tools/XenonRecomp/XenonRecomp/XenonRecomp.exe config/em2.toml runtime/ppc_context.h *> analysis/recompile.log
    if ($LASTEXITCODE) { throw 'Recompilation failed; see analysis/recompile.log.' }
    if (!(Test-Path generated/ppc/ppc_func_mapping.cpp)) { throw 'XenonRecomp produced no function map.' }
    python scripts/compact-declarations.py
    if ($LASTEXITCODE) { throw 'Declaration compaction failed.' }
    Get-ChildItem -LiteralPath generated/ppc -File | ForEach-Object {
        if ($previousSources.ContainsKey($_.Name)) {
            $previous = $previousSources[$_.Name]
            if ((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash -eq $previous[0]) {
                $_.LastWriteTimeUtc = $previous[1]
            }
        }
    }
    python scripts/audit-translation.py
    if ($LASTEXITCODE) { throw 'Translation audit failed; see analysis/translation-summary.json.' }
    python scripts/check-game-image.py private/default.xex private/image.bin analysis/image.json --stamp $stamp
    if ($LASTEXITCODE) { throw 'Could not bind the translation to its verified executable.' }
    Get-Content analysis/recompile.log -Tail 20
} finally { Pop-Location }
