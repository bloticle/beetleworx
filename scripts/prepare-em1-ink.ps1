param([string]$RuntimeDirectory = '', [string]$OutputDirectory = '')
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if(!$RuntimeDirectory){$RuntimeDirectory=Join-Path $workspace 'build/runtime23'}
if(!$OutputDirectory){$OutputDirectory=Join-Path $workspace 'private/em1-ink'}
$runtime=(Resolve-Path -LiteralPath $RuntimeDirectory).Path
$executable=Join-Path $runtime 'beetleworx.exe'
$archive=Join-Path $runtime 'pc-assets/packfiles/shaders.pak'
if(Get-Process -Name beetleworx,EM2Recomp -ErrorAction SilentlyContinue | Where-Object { @($executable, (Join-Path (Split-Path $executable -Parent) 'EM2Recomp.exe')) -contains $_.Path }){throw 'Close the canonical game before preparing ink materials.'}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$output=(Resolve-Path -LiteralPath $OutputDirectory).Path
$candidate=Join-Path $output ('shaders-'+[Guid]::NewGuid().ToString('N')+'.pak')
python (Join-Path $PSScriptRoot 'prepare-em1-ink-effects.py') --source-pak $archive --output-dir $output --output-pak $candidate --replace-output
if($LASTEXITCODE){throw 'Original ink material preparation failed; installed archive unchanged.'}
$effect=Join-Path $output 'EM1I_FX_Env_Mask_VertAlpha.fxlo'
$gpuRoot=Join-Path $output ('gpu-'+[Guid]::NewGuid().ToString('N'))
$log=Join-Path $output 'gpu-validation.log'
$process=Start-Process -FilePath $executable -WorkingDirectory $runtime -WindowStyle Hidden -PassThru -Wait -ArgumentList @('--em2_test_shader_storage=true','--em1_test_ink_pixels=true',"--em1_test_ink_effect_path=`"$effect`"","--em2_test_shader_storage_path=`"$gpuRoot`"","--log_file=`"$log`"")
if($process.ExitCode -ne 0){throw "Original ink GPU validation failed; installed archive unchanged. See $log"}
$report=Get-Content -LiteralPath (Join-Path $gpuRoot 'ink/ink-pixel-results.json') -Raw | ConvertFrom-Json
if($report.draws -ne 48 -or $report.invalid -ne 0 -or $report.device_errors -ne 0){throw 'Original ink GPU report did not pass; installed archive unchanged.'}
# Unlink first: content views may share the original archive through a hardlink.
Remove-Item -LiteralPath $archive
Move-Item -LiteralPath $candidate -Destination $archive
Write-Output ('Prepared original Mickey ink material: '+$archive)
