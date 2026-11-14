param()
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$runtimeDirectory=Join-Path $workspace 'build/runtime23'
$executable=Join-Path $runtimeDirectory 'beetleworx.exe'
if(Get-CimInstance Win32_Process -Filter "Name='beetleworx.exe' OR Name='EM2Recomp.exe'"){throw 'A Beetleworx process is running; offscreen test did not launch.'}
$testRoot=Join-Path $workspace ('analysis/guest-shutdown-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $testRoot | Out-Null
function UserHashes {
    $result=@{}
    foreach($file in Get-ChildItem -LiteralPath (Join-Path $runtimeDirectory 'user_data') -Recurse -File){$result[$file.FullName]=(Get-FileHash -LiteralPath $file.FullName).Hash}
    return $result
}
$before=UserHashes
$info=[Diagnostics.ProcessStartInfo]::new($executable)
$info.UseShellExecute=$false;$info.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
$info.ArgumentList.Add('--em2_test_guest_shutdown=true')
$info.ArgumentList.Add('--log_file='+($testRoot.Replace('\','/')+'/gpu.log'))
$owned=[Diagnostics.Process]::Start($info)
Write-Output ('Owned guest-shutdown test PID '+$owned.Id+'; '+$testRoot)
while(!$owned.WaitForExit(1000)){}
$after=UserHashes;$unchanged=$before.Count -eq $after.Count
foreach($key in $before.Keys){if(!$after.ContainsKey($key) -or $before[$key] -ne $after[$key]){$unchanged=$false}}
$record=[ordered]@{pid=$owned.Id;exit_code=$owned.ExitCode;exe_sha256=(Get-FileHash -LiteralPath $executable).Hash;normal_user_files_unchanged=$unchanged;gameplay_input_sent=$false;screenshots_taken=$false;mode='cancellable native waits, C++ unwind and APC boundary; no guest game execution'}
$record | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $testRoot 'process.json')
$record | ConvertTo-Json
Get-Content -LiteralPath (Join-Path $testRoot 'gpu.log') -Tail 6
if($owned.ExitCode -ne 0 -or !$unchanged){exit 1}
