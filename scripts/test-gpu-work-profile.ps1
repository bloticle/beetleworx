param([switch]$TransferOnly,[switch]$BoundedTransfers,[switch]$ZeroStencilOnly,[switch]$ZeroStencil,[switch]$StencilCopyProbe,[switch]$StencilValue,[switch]$SkipStencilScratchClear,[switch]$StencilPlaneBarriers,[switch]$CoalesceStencilCopies,[switch]$BoundedDivision,[switch]$FusedDepthStencil,[switch]$WiiBlendOnly)
$ErrorActionPreference='Stop'
if($FusedDepthStencil -and (!$StencilValue -or !$ZeroStencilOnly)){throw 'Fused transfer validation requires -StencilValue -ZeroStencilOnly.'}
$workspace=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$runtimeDirectory=Join-Path $workspace 'build/runtime23'
$executable=Join-Path $runtimeDirectory 'beetleworx.exe'
if(Get-CimInstance Win32_Process -Filter "Name='beetleworx.exe' OR Name='EM2Recomp.exe'"){throw 'A Beetleworx process is running; private GPU test did not launch.'}
$testRoot=Join-Path $workspace ('analysis/gpu-work-profile-offscreen-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $testRoot | Out-Null
function UserHashes {
    $result=@{}
    foreach($file in Get-ChildItem -LiteralPath (Join-Path $runtimeDirectory 'user_data') -Recurse -File){$result[$file.FullName]=(Get-FileHash -LiteralPath $file.FullName).Hash}
    return $result
}
$before=UserHashes
$info=[Diagnostics.ProcessStartInfo]::new($executable)
$info.UseShellExecute=$false;$info.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
foreach($argument in @('--em2_test_shader_storage=true','--em2_profile_gpu_draws=true',('--em2_gpu_draw_profile_directory='+$testRoot.Replace('\','/')+'/shaders'),('--em2_test_shader_storage_path='+$testRoot.Replace('\','/')),('--log_file='+$testRoot.Replace('\','/')+'/gpu.log'))){$info.ArgumentList.Add($argument)}
if($TransferOnly){$info.ArgumentList.Add('--em2_test_transfer_scissors=true')}
if($WiiBlendOnly){$info.ArgumentList.Add('--em1_test_blend_pixels=true');$info.ArgumentList.Add('--d3d12_debug=true')}
if($ZeroStencilOnly){$info.ArgumentList.Add('--em2_test_zero_stencil=true')}
if($ZeroStencil){$info.ArgumentList.Add('--em2_skip_proven_zero_stencil=true')}
if($StencilValue){$info.ArgumentList.Add('--em2_stencil_plane_copy=true');$info.ArgumentList.Add('--d3d12_debug=true')}
if($SkipStencilScratchClear){if(!$StencilValue){throw 'Scratch-clear omission validation requires -StencilValue.'};$info.ArgumentList.Add('--em2_skip_stencil_scratch_clear=true')}
if($StencilPlaneBarriers){if(!$StencilValue){throw 'Stencil-plane barrier validation requires -StencilValue.'};$info.ArgumentList.Add('--em2_stencil_plane_barriers=true')}
if($CoalesceStencilCopies){if(!$StencilValue){throw 'Stencil copy coalescing validation requires -StencilValue.'};$info.ArgumentList.Add('--em2_coalesce_stencil_copies=true')}
if($StencilCopyProbe){$info.ArgumentList.Add('--em2_probe_stencil_plane_copy=true');$info.ArgumentList.Add('--d3d12_debug=true')}
if($BoundedTransfers){$info.ArgumentList.Add('--em2_tight_transfer_scissors=true')}
if($BoundedDivision){$info.ArgumentList.Add('--em2_bounded_transfer_division=true')}
if($FusedDepthStencil){$info.ArgumentList.Add('--em2_fuse_depth_stencil_transfers=true')}
$owned=[Diagnostics.Process]::Start($info)
Write-Output ('Owned GPU-work validation PID '+$owned.Id+'; '+$testRoot)
while(!$owned.WaitForExit(1000)){}
$after=UserHashes;$unchanged=$before.Count -eq $after.Count
foreach($key in $before.Keys){if(!$after.ContainsKey($key) -or $before[$key] -ne $after[$key]){$unchanged=$false}}
$record=[ordered]@{pid=$owned.Id;exit_code=$owned.ExitCode;exe_sha256=(Get-FileHash -LiteralPath $executable).Hash;normal_user_files_unchanged=$unchanged;gameplay_input_sent=$false;screenshots_taken=$false;mode=$(if($WiiBlendOnly){'isolated Wii blend GPU pixels; no guest execution'}elseif($TransferOnly){'isolated stencil clipping coverage/timings; no guest execution'}elseif($StencilCopyProbe){'debug-layer copy preflight discards candidate lists; accepted R8 route validated with actual GPU pixels'}elseif($ZeroStencilOnly){'actual native zero-stencil transfer coverage; no guest execution'}else{'full offscreen renderer tests with selected-work profiler; no guest execution'});bounded_transfers=[bool]$BoundedTransfers;zero_stencil_enabled=[bool]$ZeroStencil;stencil_value_enabled=[bool]$StencilValue;scratch_clear_omission=[bool]$SkipStencilScratchClear;stencil_plane_barriers=[bool]$StencilPlaneBarriers;stencil_copy_coalescing=[bool]$CoalesceStencilCopies;bounded_division=[bool]$BoundedDivision}
$record.fused_depth_stencil=[bool]$FusedDepthStencil
$record | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $testRoot 'process.json')
$record | ConvertTo-Json
Get-Content -LiteralPath (Join-Path $testRoot 'gpu.log') -Tail 6
if($owned.ExitCode -ne 0 -or !$unchanged){exit 1}
