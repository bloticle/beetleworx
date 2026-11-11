param([ValidateRange(15,90)][int]$ObserveSeconds=45,[switch]$VerifyMemoryIsolation,[switch]$RequireNativeContinue,[switch]$RequireNativeGameplay,[switch]$RequireNativeIdentity,[switch]$TraceRandomOwners,[switch]$SampleNativeMain,[switch]$TraceNativeEntities,[switch]$TraceNativeUuid,[switch]$LowGpuLoad,[switch]$SeedNativeWorld,[switch]$NativeMenuHandoff,[switch]$ClientWithoutSave,[ValidateRange(60,600)][int]$NativeStartupTimeoutSeconds=240,[ValidateRange(0,1000000)][uint32]$RandomTraceStartTick=0)
$ErrorActionPreference='Stop'
if($ClientWithoutSave -and !$NativeMenuHandoff){throw 'ClientWithoutSave requires the real native-menu handoff mode.'}
if($NativeMenuHandoff){$RequireNativeGameplay=$true;$SeedNativeWorld=$true}
if($RequireNativeIdentity){$TraceNativeUuid=$true;$RequireNativeGameplay=$true}
if($TraceNativeUuid){$TraceNativeEntities=$true}
$workspace=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$runtime=Join-Path $workspace 'build/runtime23'
$exe=Join-Path $runtime 'beetleworx.exe'
if(Get-CimInstance Win32_Process -Filter "Name='beetleworx.exe' OR Name='EM2Recomp.exe'"){throw 'A game is already running; no test processes started.'}
$prefix=if($NativeMenuHandoff){'analysis/native-lobby-'}else{'analysis/retained-two-world-'}
$root=Join-Path $workspace ($prefix+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $root | Out-Null
function UserHashes {
 $h=@{};foreach($f in Get-ChildItem -LiteralPath (Join-Path $runtime 'user_data') -Recurse -File){$h[$f.FullName]=(Get-FileHash -LiteralPath $f.FullName).Hash};return $h
}
$before=UserHashes
$config=Join-Path $runtime 'pc-assets/ConfigFiles.ini';$configHash=(Get-FileHash -LiteralPath $config).Hash
$crash=Join-Path $runtime 'crash.log';$crashHash=if(Test-Path -LiteralPath $crash){(Get-FileHash -LiteralPath $crash).Hash}else{''}
$peers=@();$owned=@();$nativeStartupStarted=$null
$record=[ordered]@{root=$root;exe_sha256=(Get-FileHash -LiteralPath $exe).Hash;normal_user_hashes=$before;gameplay_input_sent=$false;screenshots_taken=$false;observation_seconds=$ObserveSeconds;verify_memory_isolation=[bool]$VerifyMemoryIsolation;require_native_continue=[bool]($RequireNativeContinue -or $RequireNativeGameplay);require_native_gameplay=[bool]$RequireNativeGameplay;trace_random_owners=[bool]$TraceRandomOwners;sample_native_main=[bool]$SampleNativeMain;trace_native_entities=[bool]$TraceNativeEntities;random_trace_start_tick=$RandomTraceStartTick;peers=@()}
$record.trace_native_uuid=[bool]$TraceNativeUuid;$record.low_gpu_load=[bool]$LowGpuLoad
$record.seed_native_world=[bool]$SeedNativeWorld
$record.require_native_identity=[bool]$RequireNativeIdentity
$record.native_menu_handoff=[bool]$NativeMenuHandoff;$record.client_without_save=[bool]$ClientWithoutSave
$record.native_startup_timeout_seconds=$NativeStartupTimeoutSeconds
function Launch([string[]]$tokens){
 $info=[Diagnostics.ProcessStartInfo]::new($exe);$info.UseShellExecute=$false;$info.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden;$info.WorkingDirectory=$runtime
 foreach($token in $tokens){$info.ArgumentList.Add($token)}
 return [Diagnostics.Process]::Start($info)
}
if(-not ('EM2PairWindows' -as [type])){Add-Type -TypeDefinition @'
using System;using System.Text;using System.Runtime.InteropServices;
public static class EM2PairWindows {
 [StructLayout(LayoutKind.Sequential)] public struct Io {public ulong ReadOperations,WriteOperations,OtherOperations,ReadBytes,WriteBytes,OtherBytes;}
 [DllImport("kernel32.dll")] public static extern bool GetProcessIoCounters(IntPtr process,out Io io);
 public delegate bool Callback(IntPtr w,IntPtr p);
 [DllImport("user32.dll")] public static extern bool EnumWindows(Callback c,IntPtr p);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr w,out uint p);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr w,StringBuilder s,int n);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr w,uint m,IntPtr a,IntPtr b);
 public static int CloseRex(uint pid){int count=0;EnumWindows((w,p)=>{uint owner;GetWindowThreadProcessId(w,out owner);if(owner==pid){var name=new StringBuilder(256);GetClassName(w,name,256);if(name.ToString()=="RexWindowClass"&&PostMessage(w,0x10,IntPtr.Zero,IntPtr.Zero))++count;}return true;},IntPtr.Zero);return count;}
}
'@}
function ConfirmPrivateMemoryIsolation {
 if(!$VerifyMemoryIsolation -or $record.Contains('memory_mapping_names_distinct')){return}
 $present=@($peers | Where-Object {(Test-Path -LiteralPath $_.log) -and (Select-String -LiteralPath $_.log -Pattern 'Guest present callers, cap ' -SimpleMatch -Quiet)})
 if($present.Count -ne 2){return}
 & (Join-Path $PSScriptRoot 'read-owned-memory-sections.ps1') -PairRoot $root *> (Join-Path $root 'section-query.log')
 $sections=Get-Content -LiteralPath (Join-Path $root 'section-names.json') -Raw | ConvertFrom-Json
 $names=@();foreach($section in $sections){$guest=@($section.sections | Where-Object {$_ -match '\\xenia_memory_[0-9]+$'});if($guest.Count -ne 1){throw 'Expected one named guest memory section per owned process'};$names+=$guest[0]}
 $record.memory_mapping_names_distinct=$names.Count -eq 2 -and $names[0] -ne $names[1]
 if(!$record.memory_mapping_names_distinct){throw 'Owned peers share a guest memory mapping'}
}
try {
 foreach($role in @('host','client')){
  $folder=Join-Path $root $role;$content=Join-Path $folder 'pc-assets';$user=Join-Path $folder 'user_data'
  New-Item -ItemType Directory -Path $content,$user | Out-Null
  foreach($entry in Get-ChildItem -LiteralPath (Join-Path $runtime 'pc-assets') -Force){
   $target=Join-Path $content $entry.Name
   if($NativeMenuHandoff -and $entry.PSIsContainer -and $entry.Name -eq 'packfiles'){
    New-Item -ItemType Directory -Path $target | Out-Null
    foreach($archive in Get-ChildItem -LiteralPath $entry.FullName -File){
     $archiveTarget=Join-Path $target $archive.Name
     if($archive.Name -ieq 'UIMainMenu.pak' -or $archive.Name -ieq 'UIStartGame.pak'){
      $nativeMenu=Join-Path $workspace ('private/pc-assets/packfiles/'+$archive.Name)
      if(!(Test-Path -LiteralPath $nativeMenu)){throw 'Build the private native Online Multiplayer archive first.'}
      Copy-Item -LiteralPath $nativeMenu -Destination $archiveTarget
     }else{New-Item -ItemType HardLink -Path $archiveTarget -Target $archive.FullName | Out-Null}
    }
   }
   elseif($entry.PSIsContainer){New-Item -ItemType Junction -Path $target -Target $entry.FullName | Out-Null}
   elseif($entry.Name -eq 'ConfigFiles.ini'){Copy-Item -LiteralPath $entry.FullName -Destination $target}
   else{New-Item -ItemType HardLink -Path $target -Target $entry.FullName | Out-Null}
  }
  # Independent native campaign copies; default PC graphics/controls settings.
  if($ClientWithoutSave -and $role -eq 'client'){New-Item -ItemType Directory -Path (Join-Path $user '42560816') | Out-Null}
  else{Copy-Item -LiteralPath (Join-Path $runtime 'user_data/42560816') -Destination $user -Recurse}
  if($NativeMenuHandoff){[IO.File]::WriteAllText((Join-Path $user '.em2-native-lobby-qa'),'EM2-native-menu-loopback-QA-v1',[Text.UTF8Encoding]::new($false))}
  if($LowGpuLoad){
   # Only private preferences are changed. Preserve normal HDR/lighting choices
   # while keeping two simultaneous proof worlds at 720p/windowed/30 FPS.
   $normalSettings=Join-Path $runtime 'user_data/pc-settings.ini'
   [string[]]$settingsLines=if(Test-Path -LiteralPath $normalSettings){@(Get-Content -LiteralPath $normalSettings)}else{@()}
   foreach($key in @('resolution','window_mode','frame_limit','render_scale')){
    $pattern='^'+[regex]::Escape($key)+'=.*$'
    if(@($settingsLines | Where-Object {$_ -match $pattern}).Count){$settingsLines=@($settingsLines -replace $pattern,($key+'=0'))}
    else{$settingsLines+=($key+'=0')}
   }
   [IO.File]::WriteAllLines((Join-Path $user 'pc-settings.ini'),[string[]]$settingsLines,[Text.UTF8Encoding]::new($false))
  }
  $identityLog=Join-Path $folder 'identity.log'
  $id='';$metrics=@{}
  if(!$NativeMenuHandoff){
  $fingerprint=Launch @('--em2_verify_online_identity=true',('--em2_content_directory='+$content),('--em2_user_data_directory='+$user),('--em2_identity_checkpoint_directory='+(Join-Path $user '42560816')),('--log_file='+$identityLog),'--no-em2_online_menu')
  Write-Output "Fingerprint $role PID $($fingerprint.Id)"
  while(!$fingerprint.WaitForExit(1000)){}
  if($fingerprint.ExitCode -ne 0){throw "Identity verification failed for $role"}
  $io=[EM2PairWindows+Io]::new();[void][EM2PairWindows]::GetProcessIoCounters($fingerprint.Handle,[ref]$io)
  $metrics=@{seconds=($fingerprint.ExitTime-$fingerprint.StartTime).TotalSeconds;cpu_seconds=$fingerprint.TotalProcessorTime.TotalSeconds;read_operations=$io.ReadOperations;read_bytes=$io.ReadBytes}
  $text=Get-Content -LiteralPath $identityLog -Raw
  if($text -notmatch 'Online file identities: build=([0-9a-f]{64}), content=([0-9a-f]{64}), checkpoint=([0-9a-f]{64})'){throw 'Missing independently computed identity'}
  $id=Join-Path $folder 'identity.txt';@('EM2-QA-IDENTITY-1',$Matches[1],$Matches[2],$Matches[3],[Guid]::NewGuid().ToString('N')) | Set-Content -LiteralPath $id -Encoding ascii
  }
  $peers+=@{role=$role;folder=$folder;content=$content;user=$user;identity=$id;identity_log=$identityLog;fingerprint_metrics=$metrics;log=(Join-Path $folder 'startup.log')}
 }
 $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
 $invite=Join-Path $root $(if($NativeMenuHandoff){'native-menu-invitation.txt'}else{'invite.txt'})
 foreach($peer in $peers){
  $tokens=@(('--em2_user_data_directory='+$peer.user),('--em2_content_directory='+$peer.content),('--log_file='+$peer.log),'--no-em2_online_menu','--em2_trace_online_observations=true','--em2_trace_players=true')
  if($NativeMenuHandoff){
   $tokens+=@('--em2_native_online_handoff=true',('--em2_native_lobby_qa_role='+$peer.role),('--em2_native_lobby_qa_invitation_file='+$invite),'--em2_trace_auto_continue=true','--em2_auto_continue=false')
  }else{
   $tokens+=@('--em2_online_qa_prelaunch=true',('--em2_online_qa_role='+$peer.role),('--em2_online_qa_identity_file='+$peer.identity),('--em2_online_qa_port='+$port),('--em2_online_qa_invite_file='+$invite))
  }
  $tokens+=('--em2_trace_online_random_start_tick='+$RandomTraceStartTick)
  $tokens+='--em2_online_auto_continue=true'
  if($TraceNativeUuid){$tokens+='--em2_trace_online_uuid=true'}
  if($SeedNativeWorld){$tokens+='--em2_online_world_random=true'}
  if($TraceRandomOwners){$tokens+='--em2_trace_online_random_owners=true'}
  if($TraceNativeEntities){$tokens+='--em2_trace_native_entities=true';$tokens+=('--em2_native_entity_directory='+(Join-Path $peer.folder 'entities'))}
  if($SampleNativeMain){$tokens+='--em2_sample_native_main=true';$tokens+=('--em2_native_sample_file='+(Join-Path $peer.folder 'native-main.tsv'))}
  if($owned.Count -eq 0 -and (Get-CimInstance Win32_Process -Filter "Name='beetleworx.exe' OR Name='EM2Recomp.exe'")){throw 'Another game appeared; test did not launch.'}
  $p=Launch $tokens;$owned+=@{process=$p;start=$p.StartTime;path=$p.MainModule.FileName;peer=$peer};$peer.pid=$p.Id;$peer.arguments=$tokens
  Write-Output "Owned $($peer.role) PID $($p.Id); artifacts $root"
  if($peer.role -eq 'host' -and !$NativeMenuHandoff){
   $deadline=[DateTime]::UtcNow.AddSeconds(20)
   while(!(Test-Path -LiteralPath $invite) -and !$p.HasExited -and [DateTime]::UtcNow -lt $deadline){Start-Sleep -Milliseconds 100}
   if(!(Test-Path -LiteralPath $invite)){throw 'Host did not publish invitation'}
  }
 }
 if($NativeMenuHandoff){
  $nativeStartupStarted=$owned[0].start.ToUniversalTime();$nativeStartupDeadline=$nativeStartupStarted.AddSeconds($NativeStartupTimeoutSeconds)
  Write-Output "Waiting up to $NativeStartupTimeoutSeconds seconds for both actual native-menu handoffs and gameplay observations. Hashing and authored startup remain enabled."
  while($true){
   $readyCount=0
   foreach($item in $owned){
    if($item.process.HasExited){throw "Native-menu $($item.peer.role) exited during startup with code $($item.process.ExitCode)."}
    [string]$startupLog=if(Test-Path -LiteralPath $item.peer.log){Get-Content -LiteralPath $item.peer.log -Raw}else{''}
    foreach($failure in @('Native online lobby handoff failed:','Native online menu preparation failed:','Native lobby QA rejected:','Retained online session stopped:','Online simulation stopped:')){
     if($startupLog.Contains($failure)){throw "Native-menu $($item.peer.role) reported $failure See its startup.log."}
    }
    $handoffDone=$startupLog.Contains('Native lobby completed authored save reload and accepted native Continue')
    $gateAttached=$startupLog.Contains('Retained online connection attached to native gameplay')
    $gameplayObserved=$startupLog.Contains('Native gameplay observation v4:')
    if($handoffDone -and $gateAttached -and $gameplayObserved){++$readyCount}
   }
   ConfirmPrivateMemoryIsolation
   if($readyCount -eq 2){$record.native_startup_complete=$true;break}
   if([DateTime]::UtcNow -ge $nativeStartupDeadline){throw 'Timed out waiting for real native-menu startup/gameplay on both peers; no readiness was fabricated.'}
   Start-Sleep -Milliseconds 250
  }
  $record.native_startup_elapsed_seconds=([DateTime]::UtcNow-$nativeStartupStarted).TotalSeconds
  Write-Output "Both native-menu worlds are in gameplay after $($record.native_startup_elapsed_seconds) seconds; starting the separate $ObserveSeconds second observation."
 }
 $deadline=[DateTime]::UtcNow.AddSeconds($ObserveSeconds)
 while([DateTime]::UtcNow -lt $deadline -and @($owned | Where-Object {$_.process.HasExited}).Count -eq 0){
  ConfirmPrivateMemoryIsolation
  Start-Sleep -Milliseconds 250
 }
 foreach($item in $owned){
  $item.peer.alive_after_observation=!$item.process.HasExited
  [string]$observed=if(Test-Path -LiteralPath $item.peer.log){Get-Content -LiteralPath $item.peer.log -Raw}else{''}
  $item.peer.session_failed_before_close=$observed.Contains('Retained online session stopped:') -or $observed.Contains('Online simulation stopped:')
 }
} catch {$record.error=$_.Exception.Message;$record.error_location=$_.ScriptStackTrace}
finally {
 if($NativeMenuHandoff -and $null -ne $nativeStartupStarted -and !$record.Contains('native_startup_elapsed_seconds')){
  $record.native_startup_elapsed_seconds=([DateTime]::UtcNow-$nativeStartupStarted).TotalSeconds
 }
 foreach($item in $owned){
  $p=$item.process;$p.Refresh()
  # Failures can happen before the native window is created. Wait for the
  # owned window rather than sending a close once to a not-yet-existing HWND.
  $closeDeadline=[DateTime]::UtcNow.AddSeconds(30)
  while(!$p.HasExited -and [DateTime]::UtcNow -lt $closeDeadline){
   $current=Get-CimInstance Win32_Process -Filter ("ProcessId="+$p.Id)
   $valid=$current -and $current.ExecutablePath -eq $exe -and $current.CommandLine.Contains($item.peer.user) -and [Math]::Abs(($current.CreationDate.ToUniversalTime()-$item.start.ToUniversalTime()).TotalMilliseconds) -lt 1
   $item.peer.close_identity_valid=[bool]$valid
   if(!$valid){break}
   $item.peer.close_messages=[EM2PairWindows]::CloseRex([uint32]$p.Id)
   if($item.peer.close_messages -gt 0){break}
   Start-Sleep -Milliseconds 250
   $p.Refresh()
  }
 }
 foreach($item in $owned){$p=$item.process;$item.peer.process_exited=$p.WaitForExit(30000);if($p.HasExited){$item.peer.exit_code=$p.ExitCode}}
 foreach($peer in $peers){
  [string]$log=if(Test-Path -LiteralPath $peer.log){Get-Content -LiteralPath $peer.log -Raw}else{''}
  $peer.retained_prelaunch_ready=$log.Contains('Localhost QA retained session ready before runtime creation')
  $peer.native_menu_opened=$log.Contains('Authored native Online Multiplayer page opened')
  $peer.native_menu_bridge=$log.Contains('Native FrontMenu online SetOption bridge registered')
  $peer.native_menu_real_ready=$log.Contains('Native online lobby start confirmed for ')
  $peer.native_menu_handoff_complete=$log.Contains('Native lobby completed authored save reload and accepted native Continue')
  $peer.native_gate_attached=$log.Contains('Retained online connection attached to native gameplay')
  $peer.native_profile_requested=$log.Contains('Online startup selected native local controller ') -or ($NativeMenuHandoff -and $log.Contains('Offline startup selected native local controller '))
  $peer.native_continue_requested=$log.Contains('Online startup requested native Continue') -or ($NativeMenuHandoff -and $peer.native_menu_handoff_complete)
  $peer.guest_frames_observed=$log.Contains('Guest present callers, cap ')
  $peer.observations=([regex]::Matches($log,'Native gameplay observation v4:')).Count
  $peer.private_crash=Test-Path -LiteralPath (Join-Path $peer.user 'diagnostics/crash.log')
  $peer.isolated=$log.Contains(('Save/settings directory: '+$peer.user)) -and $log.Contains($peer.content)
 }
 $after=UserHashes;$record.user_files_unchanged=$before.Count -eq $after.Count
 foreach($path in $before.Keys){if(!$after.ContainsKey($path) -or $before[$path] -ne $after[$path]){$record.user_files_unchanged=$false}}
 $record.normal_config_unchanged=(Get-FileHash -LiteralPath $config).Hash -eq $configHash
 $afterCrash=if(Test-Path -LiteralPath $crash){(Get-FileHash -LiteralPath $crash).Hash}else{''};$record.normal_crash_unchanged=$afterCrash -eq $crashHash
 $record.peers=$peers;$record | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $root 'result.json');$record | ConvertTo-Json -Depth 6
}
if($record.Contains('error') -or !$record.user_files_unchanged -or !$record.normal_config_unchanged -or !$record.normal_crash_unchanged){exit 1}
if($VerifyMemoryIsolation -and !$record.memory_mapping_names_distinct){exit 1}
if($TraceNativeUuid){
 $identityReport=Join-Path $root 'native-identity.json'
 & python (Join-Path $PSScriptRoot 'compare-online-uuid.py') $root --output $identityReport
 $record.native_identity_verified=$false
 if($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $identityReport)){
  $identityComparison=Get-Content -LiteralPath $identityReport -Raw | ConvertFrom-Json
  $record.native_identity_verified=[bool]$identityComparison.identity_coverage.native_identity_verified
 }
 $record | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $root 'result.json')
 if($RequireNativeIdentity -and !$record.native_identity_verified){
  $record.validation_failure='Complete native actor identity sets did not match across verified snapshots.'
  $record | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $root 'result.json')
  Write-Output $record.validation_failure;exit 1
 }
}
if($RequireNativeGameplay){
 & (Join-Path $PSScriptRoot 'read-paired-gameplay.ps1') -PairRoot $root | Set-Content -LiteralPath (Join-Path $root 'gameplay-observations.json')
 $comparison=Get-Content -LiteralPath (Join-Path $root 'gameplay-observations.json') -Raw | ConvertFrom-Json
 $record.native_gameplay_two_ticks_match=[bool]$comparison.two_consecutive_native_ticks_match
 $record | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $root 'result.json')
}
if(($RequireNativeContinue -or $RequireNativeGameplay) -and @($peers | Where-Object {!$_.native_continue_requested}).Count -ne 0){
 $record.validation_failure='Native Continue was not accepted by both peers during the observation.'
 $record | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $root 'result.json')
 Write-Output $record.validation_failure
 exit 1
}
if($RequireNativeGameplay -and !$record.native_gameplay_two_ticks_match){
 $record.validation_failure=if(@($comparison.different_ticks).Count){'Native gameplay digests differ at ticks '+($comparison.different_ticks -join ',')+'. Longest matching run: '+$comparison.longest_consecutive_match+' ticks.'}else{'Native gameplay observations lack two consecutive matches or contain conflicting duplicate ticks.'}
 $record | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $root 'result.json')
 Write-Output $record.validation_failure;exit 1
}
foreach($peer in $peers){
 if($NativeMenuHandoff){if(!$peer.native_menu_opened -or !$peer.native_menu_bridge -or !$peer.native_menu_real_ready -or !$peer.native_menu_handoff_complete){exit 1}}
 elseif(!$peer.retained_prelaunch_ready){exit 1}
 if(!$peer.isolated -or !$peer.guest_frames_observed -or !$peer.process_exited -or $peer.exit_code -ne 0 -or $peer.private_crash -or $peer.session_failed_before_close){exit 1}
}
