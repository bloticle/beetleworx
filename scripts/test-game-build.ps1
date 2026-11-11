param([string]$RuntimeDirectory = '')
$ErrorActionPreference='Stop'
$workspace=Split-Path $PSScriptRoot -Parent
if(!$RuntimeDirectory){$RuntimeDirectory=Join-Path $workspace 'build/runtime23'}
$runtime=(Resolve-Path -LiteralPath $RuntimeDirectory).Path
$fixtures=Join-Path $workspace 'private/game-build-fixtures'
python (Join-Path $PSScriptRoot 'prepare-game-build-fixtures.py') (Join-Path $workspace 'private/default.xex') (Join-Path $workspace 'private/image.bin') (Join-Path $workspace 'private/raw-image.bin') $fixtures
if($LASTEXITCODE){throw 'XEX compatibility fixture generation failed. Run recompile.ps1 first.'}
$output=Join-Path $workspace 'analysis/game-build-compatibility'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$log=Join-Path $output 'gpu.log'
$process=Start-Process -FilePath (Join-Path $runtime 'beetleworx.exe') -WorkingDirectory $runtime -WindowStyle Hidden -PassThru -Wait -ArgumentList @('--em2_test_shader_storage=true',"--em2_test_game_build_directory=`"$fixtures`"","--em2_test_shader_storage_path=`"$output`"","--log_file=`"$log`"")
if($process.ExitCode -ne 0){throw "XEX compatibility tests failed; see $log"}
Write-Output "XEX compatibility tests passed; see $log"
