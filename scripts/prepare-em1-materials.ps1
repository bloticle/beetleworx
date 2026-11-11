param([string]$RuntimeDirectory = '', [string]$OutputDirectory = '')
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if(!$RuntimeDirectory){$RuntimeDirectory=Join-Path $workspace 'build/runtime23'}
if(!$OutputDirectory){$OutputDirectory=Join-Path $workspace 'private/em1-materials'}
$runtime=(Resolve-Path -LiteralPath $RuntimeDirectory).Path
$executable=Join-Path $runtime 'beetleworx.exe'
$archive=Join-Path $runtime 'pc-assets/packfiles/shaders.pak'
if(!(Test-Path -LiteralPath $executable -PathType Leaf) -or !(Test-Path -LiteralPath $archive -PathType Leaf)){throw 'Build and stage the canonical runtime first.'}
if(Get-Process -Name beetleworx,EM2Recomp -ErrorAction SilentlyContinue | Where-Object { @($executable, (Join-Path (Split-Path $executable -Parent) 'EM2Recomp.exe')) -contains $_.Path }){throw 'Close the canonical game before preparing its materials.'}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$output=(Resolve-Path -LiteralPath $OutputDirectory).Path
$log=Join-Path $output 'gpu-validation.log'
$process=Start-Process -FilePath $executable -WorkingDirectory $runtime -WindowStyle Hidden -PassThru -Wait -ArgumentList @('--em2_test_shader_storage=true','--em1_test_blend_pixels=true',"--em2_test_shader_storage_path=`"$output`"","--log_file=`"$log`"")
if($process.ExitCode -ne 0){throw "EM1 material GPU validation failed; see $log"}
$candidate=Join-Path $output ('shaders-'+[Guid]::NewGuid().ToString('N')+'.pak')
python (Join-Path $PSScriptRoot 'prepare-em1-blend-effects.py') --source-pak $archive --pixel-test (Join-Path $output 'wii-blend') --output $candidate --replace-existing-candidates --vertex-alpha-candidate --dual-uv-candidate --cutout-candidate --require-simple-alpha --require-property-cutouts --require-masked-property-cutout
if($LASTEXITCODE){throw 'EM1 material archive generation failed; installed archive unchanged.'}
# Remove the directory entry first: an earlier content view may be hard-linked
# to the source game. Never copy modified bytes through that link.
Remove-Item -LiteralPath $archive
Move-Item -LiteralPath $candidate -Destination $archive
Write-Output ('Prepared EM1 materials: '+$archive)
