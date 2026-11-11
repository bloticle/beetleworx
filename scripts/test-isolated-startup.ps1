param([ValidateRange(5,45)][int]$ObserveSeconds=15,[switch]$CaptureCrash,[switch]$TracePhysicalAllocations,[switch]$SampleNativeRender,[switch]$SampleNativeMain,[switch]$VerifyVideoBlocks,[switch]$VerifyGameConsole)
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$runtimeDirectory=Join-Path $workspace 'build/runtime23'
$executable=Join-Path $runtimeDirectory 'beetleworx.exe'
if($CaptureCrash -and !(Test-Path -LiteralPath (Join-Path $workspace 'tools/procdump/procdump64.exe'))){throw 'CaptureCrash requires the local Microsoft Sysinternals ProcDump tool.'}
if(Get-CimInstance Win32_Process -Filter "Name='beetleworx.exe' OR Name='EM2Recomp.exe'") {throw 'A Beetleworx process is already running; startup test did not launch.'}
$testRoot=Join-Path $workspace ('analysis/isolated-startup-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
$content=Join-Path $testRoot 'pc-assets'
$userDirectory=Join-Path $testRoot 'user_data'
New-Item -ItemType Directory -Path $content,$userDirectory | Out-Null
foreach($entry in Get-ChildItem -LiteralPath (Join-Path $runtimeDirectory 'pc-assets') -Force){
    $target=Join-Path $content $entry.Name
    if($entry.PSIsContainer){New-Item -ItemType Junction -Path $target -Target $entry.FullName | Out-Null}
    elseif($entry.Name -eq 'ConfigFiles.ini'){Copy-Item -LiteralPath $entry.FullName -Destination $target}
    else{New-Item -ItemType HardLink -Path $target -Target $entry.FullName | Out-Null}
}
function ReadUserHashes {
    $result=@{}
    foreach($file in Get-ChildItem -LiteralPath (Join-Path $runtimeDirectory 'user_data') -Recurse -File){
        $result[$file.FullName]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }
    return $result
}
$baseline=ReadUserHashes
$log=Join-Path $testRoot 'startup.log'
$arguments=@(
    ('--em2_user_data_directory='+$userDirectory.Replace('\','/'))
    ('--em2_content_directory='+$content.Replace('\','/'))
    ('--log_file='+$log.Replace('\','/'))
    '--no-em2_online_menu'
)
if($TracePhysicalAllocations){$arguments+= '--em2_trace_platform_allocations=true'}
if($VerifyVideoBlocks){$arguments+= '--em2_verify_video_blocks=true'}
if($VerifyGameConsole){$arguments+= '--em2_verify_game_console_vm=true'}
if($SampleNativeRender){$arguments+='--em2_sample_native_render=true';$arguments+=('--em2_native_sample_file='+(Join-Path $testRoot 'native-samples.tsv').Replace('\','/'))}
if($SampleNativeMain){$arguments+='--em2_sample_native_main=true';if(!$SampleNativeRender){$arguments+=('--em2_native_sample_file='+(Join-Path $testRoot 'native-samples.tsv').Replace('\','/'))}}
if($arguments.Count -ne (4+[int]$TracePhysicalAllocations.IsPresent+[int]$VerifyVideoBlocks.IsPresent+[int]$VerifyGameConsole.IsPresent+2*[int]$SampleNativeRender.IsPresent+[int]$SampleNativeMain.IsPresent+[int]($SampleNativeMain.IsPresent -and !$SampleNativeRender.IsPresent))){throw 'Unexpected startup argument token count.'}
if(-not ('EM2IsolatedStartupWindow' -as [type])){Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class EM2IsolatedStartupWindow {
 public delegate bool Callback(IntPtr window,IntPtr parameter);
 [DllImport("user32.dll")] public static extern bool EnumWindows(Callback callback,IntPtr parameter);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window,out uint process);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr window,StringBuilder name,int capacity);
 [DllImport("user32.dll",SetLastError=true)] public static extern bool PostMessage(IntPtr window,uint message,IntPtr wparam,IntPtr lparam);
}
'@
}
$startInfo=[Diagnostics.ProcessStartInfo]::new($executable)
$startInfo.WorkingDirectory=$runtimeDirectory
$startInfo.UseShellExecute=$false
$startInfo.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
foreach($argument in $arguments){$startInfo.ArgumentList.Add($argument)}
if(Get-CimInstance Win32_Process -Filter "Name='beetleworx.exe' OR Name='EM2Recomp.exe'"){throw 'Another game started during setup; startup test did not launch.'}
$normalConfig=Join-Path $runtimeDirectory 'pc-assets/ConfigFiles.ini'
$normalCrash=Join-Path $runtimeDirectory 'crash.log'
$configHash=(Get-FileHash -LiteralPath $normalConfig).Hash
$crashHash=if(Test-Path -LiteralPath $normalCrash){(Get-FileHash -LiteralPath $normalCrash).Hash}else{''}
$owned=[Diagnostics.Process]::Start($startInfo)
$record=[ordered]@{pid=$owned.Id;started_utc=$owned.StartTime.ToUniversalTime().ToString('o');executable=$executable;exe_sha256=(Get-FileHash -LiteralPath $executable).Hash;arguments=$arguments;user_directory=$userDirectory;content_directory=$content;log=$log;normal_user_hashes=$baseline;normal_config_hash=$configHash;normal_crash_hash=$crashHash;observation_seconds=$ObserveSeconds;gameplay_input_sent=$false;screenshots_taken=$false;isolated=$false;user_files_unchanged=$false;guest_frames_observed=$false}
$record | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $testRoot 'run.json')
Write-Output ("Owned startup PID {0}; artifacts {1}" -f $owned.Id,$testRoot)
if($CaptureCrash){
    $debugger=[Diagnostics.ProcessStartInfo]::new((Join-Path $workspace 'tools/procdump/procdump64.exe'))
    $debugger.UseShellExecute=$false;$debugger.CreateNoWindow=$true
    $debugger.RedirectStandardOutput=$true;$debugger.RedirectStandardError=$true
    $debugger.StandardOutputEncoding=[Text.Encoding]::Unicode
    foreach($argument in @('-accepteula','-e','-t','-mm',[string]$owned.Id,(Join-Path $testRoot 'shutdown-crash.dmp'))){$debugger.ArgumentList.Add($argument)}
    $monitor=[Diagnostics.Process]::Start($debugger)
    $monitorOutput=$monitor.StandardOutput.ReadToEndAsync();$monitorError=$monitor.StandardError.ReadToEndAsync()
    $record.crash_monitor_pid=$monitor.Id
}
try {
    $deadline=[DateTime]::UtcNow.AddSeconds($ObserveSeconds)
    while([DateTime]::UtcNow -lt $deadline -and !$owned.HasExited){
        if(Test-Path -LiteralPath $log){
            [string]$text=Get-Content -LiteralPath $log -Raw
            if($text -match 'Save/settings directory: ([^\r\n]+)'){
                $actual=[IO.Path]::GetFullPath($Matches[1].Trim())
                $record.actual_user_directory=$actual
                if($actual -ne $userDirectory){throw "Save/settings override failed: $actual"}
            }
            if($text -match 'Mounted ([^\r\n]+) at \\Device\\Harddisk0\\Partition1'){
                $actual=[IO.Path]::GetFullPath($Matches[1].Trim());$record.actual_content_directory=$actual
                if($actual -ne $content){throw "Content override failed: $actual"}
            }
            $record.isolated=$record.Contains('actual_user_directory') -and $record.Contains('actual_content_directory')
            $record.guest_frames_observed=$record.guest_frames_observed -or ($text -match 'Guest present callers, cap ')
        }
        Start-Sleep -Milliseconds 250
    }
    if(!$record.isolated){throw 'Startup did not confirm both isolated mounts.'}
    $record.alive_after_observation=!$owned.HasExited
} catch {$record.error=$_.Exception.Message;$record.error_location=$_.ScriptStackTrace}
finally {
    # Close only this process, after checking that Windows has not reused its PID.
    $owned.Refresh()
    if(!$owned.HasExited -and $owned.StartTime.ToUniversalTime().ToString('o') -eq $record.started_utc){
        $script:ownedCloseMessages=0
        $script:ownedWindowClasses=[Collections.Generic.List[string]]::new()
        [EM2IsolatedStartupWindow]::EnumWindows({param($window,$parameter)
            [uint32]$windowProcess=0
            [void][EM2IsolatedStartupWindow]::GetWindowThreadProcessId($window,[ref]$windowProcess)
            if($windowProcess -eq $owned.Id){
                $name=[Text.StringBuilder]::new(256);[void][EM2IsolatedStartupWindow]::GetClassName($window,$name,256);$script:ownedWindowClasses.Add($name.ToString())
                if($name.ToString() -in @('RexWindowClass','SDL_app') -and [EM2IsolatedStartupWindow]::PostMessage($window,0x10,[IntPtr]::Zero,[IntPtr]::Zero)){$script:ownedCloseMessages++}
            }
            return $true
        },[IntPtr]::Zero) | Out-Null
        $record.close_messages=$script:ownedCloseMessages
        $record.owned_window_classes=$script:ownedWindowClasses.ToArray()
        $record.close_requested=$script:ownedCloseMessages -gt 0
        if($record.close_requested){$null=$owned.WaitForExit(10000)}
    }
    $owned.Refresh();$record.process_exited=$owned.HasExited
    if($CaptureCrash -and $monitor.WaitForExit(10000)){
        $record.crash_monitor_exit=$monitor.ExitCode
        $monitorOutput.Result | Set-Content (Join-Path $testRoot 'crash-monitor.log')
        $monitorError.Result | Set-Content (Join-Path $testRoot 'crash-monitor-error.log')
    }
    if($owned.HasExited){$record.exit_code=$owned.ExitCode}
    $after=ReadUserHashes
    $record.user_files_unchanged=$baseline.Count -eq $after.Count
    foreach($path in $baseline.Keys){if(!$after.ContainsKey($path) -or $after[$path] -ne $baseline[$path]){$record.user_files_unchanged=$false}}
    $record.normal_config_unchanged=(Get-FileHash -LiteralPath $normalConfig).Hash -eq $configHash
    $afterCrash=if(Test-Path -LiteralPath $normalCrash){(Get-FileHash -LiteralPath $normalCrash).Hash}else{''}
    $record.normal_crash_unchanged=$afterCrash -eq $crashHash
    $record.private_diagnostics_directory_exists=Test-Path -LiteralPath (Join-Path $userDirectory 'diagnostics') -PathType Container
    $record.private_crash_report_exists=Test-Path -LiteralPath (Join-Path $userDirectory 'diagnostics/crash.log')
    [string]$finalLog=Get-Content -LiteralPath $log -Raw -ErrorAction SilentlyContinue
    $record.physical_allocation_failure_messages=([regex]::Matches($finalLog,'PhysicalHeap::Alloc unable to alloc physical memory in parent heap')).Count
    $record.video_block_verification_passes=([regex]::Matches($finalLog,'Live video block verification passed:')).Count
    $record.video_block_verification_failures=([regex]::Matches($finalLog,'Live video block verification failed:')).Count
    $record.game_console_vm_probe_passes=([regex]::Matches($finalLog,'Game console real-VM probe [0-7] passed:')).Count
    $record.game_console_print_captured=$finalLog.Contains('Game console real-VM print capture passed')
    $record | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $testRoot 'result.json')
    $record | ConvertTo-Json -Depth 4
}
if($record.Contains('error') -or !$record.isolated -or !$record.user_files_unchanged -or !$record.alive_after_observation -or !$record.guest_frames_observed -or !$record.normal_config_unchanged -or !$record.normal_crash_unchanged -or !$record.private_diagnostics_directory_exists -or $record.private_crash_report_exists -or !$record.process_exited -or $record.exit_code -ne 0){exit 1}
if($VerifyVideoBlocks -and ($record.video_block_verification_failures -ne 0 -or $record.video_block_verification_passes -eq 0)){exit 1}
if($VerifyGameConsole -and ($record.game_console_vm_probe_passes -ne 8 -or !$record.game_console_print_captured)){exit 1}
