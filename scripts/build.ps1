param([string]$GameDirectory = 'D:\Modding\Builds\EM2X360', [string]$RexSdk = 'D:\Repos\rexglue-sdk', [switch]$NoOptimize, [switch]$ThinLTO, [int]$Jobs = 2, [switch]$SkipShaderPreparation)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/enter-toolchain.ps1"
Push-Location $root
try {
    $gameExecutable = Join-Path $root 'build/runtime23/beetleworx.exe'
    if (Get-Process -Name beetleworx,EM2Recomp -ErrorAction SilentlyContinue | Where-Object { @($gameExecutable, (Join-Path (Split-Path $gameExecutable -Parent) 'EM2Recomp.exe')) -contains $_.Path }) {
        throw 'Close Beetleworx before building; its executable and menu archives must be updated together.'
    }
    $stamp='generated/ppc/em2_game_build.h'
    if(!(Test-Path -LiteralPath $stamp)){throw 'Run scripts/recompile.ps1 with your extracted game folder first.'}
    $sourceHash=(Get-FileHash -LiteralPath (Join-Path $GameDirectory 'default.xex')).Hash
    if((Get-Content -LiteralPath $stamp -Raw) -notmatch ('"'+$sourceHash+'"')){
        throw 'This game folder uses a different executable. Run scripts/recompile.ps1 with this folder first.'
    }
    & "$PSScriptRoot/bootstrap-pc-menu.ps1"
    python "$PSScriptRoot/build-pc-menu.py" --game-dir $GameDirectory
    if ($LASTEXITCODE) { throw 'In-game PC menu build failed.' }
    python "$PSScriptRoot/build-online-startup-menu.py" --game-dir $GameDirectory
    if ($LASTEXITCODE) { throw 'Native online startup menu build failed.' }
    python "$PSScriptRoot/build-online-lobby-menu.py" --game-dir $GameDirectory
    if ($LASTEXITCODE) { throw 'Native online lobby menu build failed.' }
    python "$PSScriptRoot/build-bloom-shaders.py" --game-dir $GameDirectory
    if ($LASTEXITCODE) { throw 'Bloom shader build failed.' }
    python "$PSScriptRoot/inspect-scene-shaders.py" --game-dir $GameDirectory
    if ($LASTEXITCODE) { throw 'Scene shader input audit failed.' }
    python "$PSScriptRoot/inspect-material-shaders.py" --game-dir $GameDirectory
    if ($LASTEXITCODE) { throw 'Material shader input audit failed.' }
    python "$PSScriptRoot/prepare-em1-character-lighting.py" --source-pak (Join-Path $GameDirectory 'packfiles/shaders.pak')
    if ($LASTEXITCODE) { throw 'EM1 character lighting binding audit failed.' }
    python "$PSScriptRoot/prepare-pc-shader-catalog.py" --game-dir $GameDirectory --output private/pc-shader-catalog.em2
    if ($LASTEXITCODE) { throw 'PC shader catalog extraction failed.' }
    & "$PSScriptRoot/stage-pc-assets.ps1" -GameDirectory $GameDirectory
    New-Item -ItemType Directory -Path build/runtime23/pc-assets/packfiles -Force | Out-Null
    Copy-Item -LiteralPath private/pc-shader-catalog.em2 -Destination build/runtime23/pc-assets/pc-shader-catalog.em2
    foreach ($menu in @('UIOptions.pak','UIPauseOptions.pak','UIhud.pak','UIStartGame.pak','UIMainMenu.pak','_Dynamic.pak','shaders.pak')) {
        # Unlink an existing staged original before copying modified content.
        # Copying over a hard link would modify the source game's archive.
        $menuTarget=Join-Path $root "build/runtime23/pc-assets/packfiles/$menu"
        if(Test-Path -LiteralPath $menuTarget) {Remove-Item -LiteralPath $menuTarget}
        Copy-Item -LiteralPath (Join-Path private/pc-assets/packfiles $menu) -Destination build/runtime23/pc-assets/packfiles
    }
    cmake -S runtime -B build/runtime23 -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_COMPILER=$llvm/clang-cl.exe" "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$($root.Replace('\','/'))/build/runtime23" "-DEM2_OPTIMIZE=$(-not $NoOptimize)" "-DEM2_THIN_LTO=$($ThinLTO.IsPresent)" "-DEM2_GAME_DIRECTORY=$($GameDirectory.Replace('\','/'))" "-DREXSDK=$($RexSdk.Replace('\','/'))"
    if ($LASTEXITCODE) { throw 'Runtime configuration failed.' }
    cmake --build build/runtime23 --parallel $Jobs
    if ($LASTEXITCODE) { throw 'Runtime build failed.' }
    & "$PSScriptRoot/stage-runtime.ps1"
    & "$PSScriptRoot/prepare-em1-materials.ps1"
    & "$PSScriptRoot/prepare-em1-ink.ps1"
    # Keep the release patch input identical to the installed shader archive.
    $releaseShaders=Join-Path $root 'private/pc-assets/packfiles/shaders.pak'
    if(Test-Path -LiteralPath $releaseShaders){Remove-Item -LiteralPath $releaseShaders}
    Copy-Item -LiteralPath (Join-Path $root 'build/runtime23/pc-assets/packfiles/shaders.pak') -Destination $releaseShaders
    python "$PSScriptRoot/prepare-pc-shader-catalog.py" --game-dir (Join-Path $root 'build/runtime23/pc-assets') --output (Join-Path $root 'private/pc-shader-catalog.em2')
    if($LASTEXITCODE){throw 'Mounted shader catalog preparation failed.'}
    Copy-Item -LiteralPath (Join-Path $root 'private/pc-shader-catalog.em2') -Destination (Join-Path $root 'build/runtime23/pc-assets/pc-shader-catalog.em2')
    python "$PSScriptRoot/build-content-patches.py" --game-dir $GameDirectory
    if($LASTEXITCODE){throw 'Changed-only content patch preparation failed.'}
    if(-not $SkipShaderPreparation) {
        $prepareRoot=Join-Path $root 'analysis/build-pc-shader-preparation'
        New-Item -ItemType Directory -Force -Path $prepareRoot | Out-Null
        $prepareScales=@(1)
        $settingsPath=Join-Path $root 'build/runtime23/user_data/pc-settings.ini'
        if(Test-Path -LiteralPath $settingsPath){
            foreach($settingLine in Get-Content -LiteralPath $settingsPath){
                if($settingLine -match '^render_scale=([0-2])$'){$selectedScale=1+[int]$Matches[1];if($selectedScale -ne 1){$prepareScales+=$selectedScale};break}
            }
        }
        foreach($prepareScale in $prepareScales){
            $prepareLog=Join-Path $prepareRoot "prepare-${prepareScale}x.log"
            $prepare=Start-Process -FilePath $gameExecutable -WorkingDirectory (Split-Path $gameExecutable) -WindowStyle Hidden -PassThru -Wait -ArgumentList @('--em2_test_shader_storage=true','--em2_prepare_pc_shader_library=true',"--em2_pc_shader_prepare_scale=$prepareScale","--em2_test_shader_storage_path=`"$prepareRoot`"","--log_file=`"$prepareLog`"")
            if($prepare.ExitCode -ne 0){throw "PC shader preparation failed; see $prepareLog"}
        }
    }
} finally { Pop-Location }
