param(
 [Parameter(Mandatory)][string]$Root,
 [Parameter(Mandatory)][string]$CompletionMarker,
 [ValidateRange(30,600)][int]$TimeoutSeconds=180,
 [ValidateRange(0,120)][int]$ObserveSeconds=0
)
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$rootPath=(Resolve-Path -LiteralPath $Root).Path
$analysisRoot=[IO.Path]::GetFullPath((Join-Path $workspace 'analysis'))+[IO.Path]::DirectorySeparatorChar
if(!$rootPath.StartsWith($analysisRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Capture must be inside this workspace analysis directory.'}
$capture=Get-Content -LiteralPath (Join-Path $rootPath 'process.json') -Raw | ConvertFrom-Json
if($capture.root -ne $rootPath){throw 'Capture root mismatch.'}
$canonicalExe=[IO.Path]::GetFullPath((Join-Path $workspace 'build/runtime23/beetleworx.exe'))
# Retained captures may belong to the name used before the product rename.
# Never require a not-yet-built new image to identify and close that old capture.
$legacyExe=[IO.Path]::GetFullPath((Join-Path $workspace 'build/runtime23/EM2Recomp.exe'))
$exe=if($capture.exe_path){[IO.Path]::GetFullPath([string]$capture.exe_path)}else{''}
if($exe -and @($canonicalExe,$legacyExe) -notcontains $exe){throw 'Recorded executable is outside the canonical runtime paths.'}
$imageVerified=$false
if(-not ('BWProfileWindows' -as [type])){Add-Type -TypeDefinition @'
using System;using System.Text;using System.Runtime.InteropServices;
public static class BWProfileWindows {
 public delegate bool Callback(IntPtr window,IntPtr param);
 [DllImport("kernel32.dll",SetLastError=true)] public static extern IntPtr OpenProcess(uint access,bool inherit,uint pid);
 [DllImport("kernel32.dll",SetLastError=true)] public static extern bool GetExitCodeProcess(IntPtr process,out uint code);
 [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
 [DllImport("user32.dll")] public static extern bool EnumWindows(Callback callback,IntPtr param);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window,out uint pid);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr window,StringBuilder name,int count);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window,uint message,IntPtr wparam,IntPtr lparam);
 public static int Close(uint pid){int count=0;EnumWindows((window,param)=>{uint owner;GetWindowThreadProcessId(window,out owner);if(owner==pid){var name=new StringBuilder(256);GetClassName(window,name,256);if(name.ToString()=="RexWindowClass" && PostMessage(window,0x10,IntPtr.Zero,IntPtr.Zero))++count;}return true;},IntPtr.Zero);return count;}
}
'@}
function Test-OwnedHandleExited {
 if($exitHandle -eq [IntPtr]::Zero){return $false}
 [uint32]$ownedExit=259
 return [BWProfileWindows]::GetExitCodeProcess($exitHandle,[ref]$ownedExit) -and $ownedExit -ne 259
}
function Get-OwnedProfile {
 # The retained, previously verified handle is authoritative for its original
 # process. Never inspect or close a reused PID after that process has exited.
 if(Test-OwnedHandleExited){return $null}
 $p=Get-CimInstance Win32_Process -Filter ('ProcessId='+$capture.pid)
 if(!$p){return $null}
 if(!$p.ExecutablePath -or !$p.CommandLine){
  # CIM can briefly return a terminating entry whose path was already removed.
  if(Test-OwnedHandleExited){return $null}
  throw 'Private profile identity metadata unavailable for a live process.'
 }
 $actualExe=[IO.Path]::GetFullPath($p.ExecutablePath)
 if(@($canonicalExe,$legacyExe) -notcontains $actualExe -or ($script:exe -and $actualExe -ne $script:exe) -or !$p.CommandLine.Replace('/','\').Contains($rootPath) -or
    [Math]::Abs(($p.CreationDate.ToUniversalTime()-([datetime]$capture.started_utc).ToUniversalTime()).TotalMilliseconds) -gt 1){throw 'Private profile process identity mismatch.'}
 if(!$script:imageVerified){
  if((Get-FileHash -LiteralPath $actualExe).Hash -ne $capture.exe_sha256){throw 'Private profile executable hash differs from its capture record.'}
  $script:exe=$actualExe;$script:imageVerified=$true
 }
 # The intentional probe exit may occur between CIM inspection and this
 # process lookup. A missing process is terminal, not a failed identity check.
 $owned=Get-Process -Id $capture.pid -ErrorAction SilentlyContinue
 if(!$owned){return $null}
 try{$ownedStart=$owned.StartTime.ToUniversalTime()}catch{
  if(!(Get-CimInstance Win32_Process -Filter ('ProcessId='+$capture.pid))){return $null}
  throw
 }
 if([Math]::Abs(($ownedStart-([datetime]$capture.started_utc).ToUniversalTime()).TotalMilliseconds) -gt 1){throw 'Private profile process identity changed during lookup.'}
 return $owned
}
$result=[ordered]@{root=$rootPath;pid=$capture.pid;exe_sha256=$capture.exe_sha256;completed=$false;observation_seconds=$ObserveSeconds;gameplay_input_sent=$false}
$exitHandle=[IntPtr]::Zero
try {
 # Retain the verified original process handle before observing. A native fault
 # or deliberate failed-probe exit can happen before the completion marker poll.
 $initial=Get-OwnedProfile
 if($initial){$exitHandle=[BWProfileWindows]::OpenProcess(0x101000,$false,[uint32]$initial.Id)}
 $deadline=[DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
 $observedUntil=$null
 while([DateTime]::UtcNow -lt $deadline){
  if(!(Get-OwnedProfile)){
   # This explicit private probe exits before a partially restored world could
   # resume. Accept only its exact success marker, with no observation request;
   # the retained verified process handle and zero exit status are checked below.
   if($capture.native_frame_checkpoint -and $ObserveSeconds -eq 0 -and $CompletionMarker -eq 'Private native frame component checkpoint complete:' -and
      (Select-String -LiteralPath (Join-Path $rootPath 'game.log') -SimpleMatch -Pattern $CompletionMarker -Quiet)){
    $result.completed=$true;$result.native_frame_checkpoint_checked=$true;break
   }
   throw 'Private process exited before completion.'
  }
  if($null -eq $observedUntil -and (Select-String -LiteralPath (Join-Path $rootPath 'game.log') -SimpleMatch -Pattern $CompletionMarker -Quiet)){
   $result.marker_seen_utc=[DateTime]::UtcNow.ToString('o')
   $observedUntil=[DateTime]::UtcNow.AddSeconds($ObserveSeconds)
   # Startup has completed. Give the requested observation its full separate
   # bounded interval, including one poll's slack at the final deadline.
   $deadline=$observedUntil.AddSeconds(2)
  }
  if($null -ne $observedUntil){$result.observed_seconds=[Math]::Round(([DateTime]::UtcNow-($observedUntil.AddSeconds(-$ObserveSeconds))).TotalSeconds,3)}
  if($null -ne $observedUntil -and [DateTime]::UtcNow -ge $observedUntil){$result.completed=$true;break}
  Start-Sleep -Milliseconds 1000
 }
 if(!$result.completed){throw 'Private observation timed out.'}
}catch{$result.error=$_.Exception.Message}
finally {
 $p=$null
 try{$p=Get-OwnedProfile}catch{$result.completed=$false;$result.error=$_.Exception.Message}
 if($p){
  if($exitHandle -eq [IntPtr]::Zero){$exitHandle=[BWProfileWindows]::OpenProcess(0x101000,$false,[uint32]$p.Id)}
  $result.close_messages=[BWProfileWindows]::Close([uint32]$p.Id)
  if(!$p.WaitForExit(10000)){$p=Get-OwnedProfile;if($p){$p.Kill();[void]$p.WaitForExit(10000);$result.forced_close=$true}}
 }
 if($exitHandle -ne [IntPtr]::Zero){
  try{[uint32]$exitCode=259;if([BWProfileWindows]::GetExitCodeProcess($exitHandle,[ref]$exitCode) -and $exitCode -ne 259){$result.process_exited=$true;$result.exit_code=$exitCode}else{$result.completed=$false;$result.error='The owned process exit could not be confirmed.'}}
  finally{[void][BWProfileWindows]::CloseHandle($exitHandle)}
 }else{$result.completed=$false;if(!$result.Contains('error')){$result.error='No verified process handle was available for exit status.'}}
 if($result.forced_close -or ($result.Contains('exit_code') -and $result.exit_code -ne 0)){$result.completed=$false}
 $normal=Join-Path $workspace 'build/runtime23/user_data'
 $unchanged=@(Get-ChildItem -LiteralPath $normal -Recurse -File).Count -eq @($capture.normal_user_hashes.PSObject.Properties).Count
 foreach($f in $capture.normal_user_hashes.PSObject.Properties){
  if(!(Test-Path -LiteralPath $f.Name) -or (Get-FileHash -LiteralPath $f.Name).Hash -ne $f.Value){$unchanged=$false}
 }
 $result.normal_user_files_unchanged=$unchanged
 if($capture.native_frame_checkpoint){
  $result.native_frame_checkpoint_checked=[bool](Select-String -LiteralPath (Join-Path $rootPath 'game.log') -SimpleMatch -Pattern 'Private native frame component checkpoint complete:' -Quiet)
  if(!$result.native_frame_checkpoint_checked){$result.completed=$false}
 }
 if($capture.native_frame_replay){
  $result.native_frame_replay_checked=[bool](Select-String -LiteralPath (Join-Path $rootPath 'game.log') -SimpleMatch -Pattern 'Private native frame component replay matched:' -Quiet)
  if(!$result.native_frame_replay_checked){$result.completed=$false}
 }
 if($capture.native_frame_scripts){
  $result.native_frame_scripts_checked=[bool](Select-String -LiteralPath (Join-Path $rootPath 'game.log') -SimpleMatch -Pattern 'Private native frame scripts restored:' -Quiet)
  if(!$result.native_frame_scripts_checked){$result.completed=$false}
 }
 if($capture.native_script_persistence){
  $probeText=Get-Content -LiteralPath (Join-Path $rootPath 'game.log') -Raw
  $terminal=[regex]::Matches($probeText,'Native script persistence complete: passed (\d+)/(\d+);')
  $result.native_script_checks_passed=$false
  if($terminal.Count){
   $last=$terminal[$terminal.Count-1]
   $result.native_script_checks_completed=[int]$last.Groups[1].Value
   $result.native_script_checks_expected=[int]$last.Groups[2].Value
   $result.native_script_checks_passed=$result.native_script_checks_expected -gt 0 -and $result.native_script_checks_completed -eq $result.native_script_checks_expected
  }
  if(!$result.native_script_checks_passed){$result.completed=$false}
  if($capture.native_physics_restore){
   $result.native_physics_restore_checked=$probeText.Contains('Private actual physics immediate restore passed:')
   if(!$result.native_physics_restore_checked){$result.completed=$false}
  }
  if($capture.native_script_restore){
   $result.native_script_restore_checked=$probeText.Contains('Private LIVE script immediate restore passed:')
   if(!$result.native_script_restore_checked){$result.completed=$false}
  }
  if($capture.native_physics_restore -and $capture.native_script_restore){
   $result.native_combined_restore_checked=$probeText.Contains('Private combined native component restore passed:')
   if(!$result.native_combined_restore_checked){$result.completed=$false}
  }
 }
 if($capture.native_roster_revision -eq 1){
  $probeText=Get-Content -LiteralPath (Join-Path $rootPath 'game.log') -Raw
  $expected=[int]$capture.extra_player_instances
  $result.native_roster_admitted=[regex]::IsMatch($probeText,('Owned native player roster admission: requested '+$expected+' registered '+$expected+' mask [0-9A-F]+ active true frame bindings true'))
  $result.native_owned_teardown_completed=[regex]::IsMatch($probeText,('Extra native player probe: completed success true requested '+$expected+' created '+$expected+' remaining 0 retail handles unchanged true'))
  $result.native_group_rollback_completed=[regex]::IsMatch($probeText,('Owned native player group rollback: requested '+$expected+' constructed '+$expected+' registered '+($expected-1)+' complete true; no actor admitted'))
  if($capture.reject_owned_player){if(!$result.native_group_rollback_completed){$result.completed=$false}}
  elseif(!$result.native_roster_admitted -or !$result.native_owned_teardown_completed){$result.completed=$false}
 }
 if($capture.owned_player_input){
  $probeLog=Join-Path $rootPath 'game.log'
  $result.gameplay_input_sent=[bool](Select-String -LiteralPath $probeLog -Pattern 'Owned actor live input delivery started:' -SimpleMatch -Quiet)
  $result.native_input_program_completed=[bool](Select-String -LiteralPath $probeLog -Pattern 'Owned actor live input completed: success true' -SimpleMatch -Quiet)
  $result.native_owned_teardown_completed=[bool](Select-String -LiteralPath $probeLog -Pattern 'Extra native player probe: completed success true' -SimpleMatch -Quiet)
  if(!$result.native_input_program_completed -or !$result.native_owned_teardown_completed){$result.completed=$false}
 }
 if($capture.em1_source){
  $source=Get-Item -LiteralPath $capture.em1_source.path -ErrorAction SilentlyContinue
  $result.em1_source_metadata_unchanged=$null -ne $source -and !$source.PSIsContainer -and $source.Length -eq $capture.em1_source.length -and $source.LastWriteTimeUtc.Ticks -eq ([datetime]$capture.em1_source.last_write_utc).ToUniversalTime().Ticks
 }
 $result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $rootPath 'completion-result.json')
 $result | ConvertTo-Json
}
if(!$result.completed -or !$result.normal_user_files_unchanged -or ($capture.em1_source -and !$result.em1_source_metadata_unchanged)){exit 1}
