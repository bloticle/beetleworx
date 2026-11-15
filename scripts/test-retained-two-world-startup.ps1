param([ValidateRange(15,90)][int]$ObserveSeconds=45,[switch]$VerifyMemoryIsolation,[switch]$RequireNativeContinue,[switch]$RequireNativeGameplay,[switch]$RequireNativeIdentity,[switch]$TraceRandomOwners,[switch]$SampleNativeMain,[switch]$TraceNativeEntities,[switch]$TraceNativeUuid,[switch]$TracePathPoints,[switch]$MotionTrajectory,[switch]$PostVelocityTrace,[switch]$MotionInputTrace,[switch]$NavigationTrace,[switch]$AvoidanceTrace,[switch]$TurnLimitTrace,[switch]$TurnLifetimeTrace,[switch]$LowGpuLoad,[switch]$SeedNativeWorld,[switch]$NativeMenuHandoff,[switch]$NativeRoomMenu,[string]$ReuseContentFrom="",[switch]$ClientWithoutSave,[switch]$AutomateGameplayInput,[ValidateSet(30,60,120)][int]$HostFrameCap=30,[ValidateSet(30,60,120)][int]$GuestFrameCap=30,[string]$ReplayInvitationFile="",[ValidatePattern("^([0-9A-Fa-f]{32})?$")][string]$MotionTraceGuid="",[switch]$NoInterpolation,[switch]$SingleView,[switch]$OriginalSplitView,[switch]$ScriptedNativeInput,[switch]$CheckpointReload,[switch]$BareLevelRestart,[switch]$LeaveRunning,[ValidateRange(0,1000000)][uint32]$RestartTick=0,[ValidateRange(60,600)][int]$NativeStartupTimeoutSeconds=240,[ValidateRange(0,1000000)][uint32]$RandomTraceStartTick=0,[uint32]$BehaviorDumpTick=4294967295,[ValidateRange(0,1000000)][uint32]$BehaviorTraceStartTick=0)
$ErrorActionPreference='Stop'
if($NativeRoomMenu){$NativeMenuHandoff=$true;if($ScriptedNativeInput -or $RestartTick){throw 'NativeRoomMenu currently tests authored startup without legacy scripted input/restart.'}}
$nativeGateMarker=if($NativeRoomMenu){'Native Room gameplay first frame consumed:'}else{'Retained online connection attached to native gameplay'}
if($TurnLifetimeTrace){$TurnLimitTrace=$true}
if($NavigationTrace -or $AvoidanceTrace -or $TurnLimitTrace){$MotionInputTrace=$true}
if($BareLevelRestart -and $CheckpointReload){throw 'Choose checkpoint restoration or the bare-level comparison, not both.'}
if(($CheckpointReload -or $BareLevelRestart) -and (!$NativeMenuHandoff -or !$RestartTick)){throw 'Checkpoint reload comparison requires an isolated native-menu scheduled restart.'}
if($SingleView -and $OriginalSplitView){throw 'Choose local single-view acceptance or the original split-view diagnostic.'}
if($OriginalSplitView -and (!$NativeMenuHandoff -or $LeaveRunning)){throw 'OriginalSplitView is an isolated native-menu diagnostic only.'}
if($ScriptedNativeInput -and (!$NativeMenuHandoff -or (!$SingleView -and !$OriginalSplitView) -or $LeaveRunning -or $AutomateGameplayInput)){throw 'Native scripted input requires isolated native-menu single-view mode without manual or window-event input.'}
if($SingleView -and !$NativeMenuHandoff){throw 'SingleView requires the isolated native-menu test.'}
if($ClientWithoutSave -and !$NativeMenuHandoff){throw 'ClientWithoutSave requires the real native-menu handoff mode.'}
if($AutomateGameplayInput -and !$NativeMenuHandoff){throw 'Automated gameplay input requires the isolated native-menu test.'}
if($RestartTick -and !$NativeMenuHandoff){throw 'Scheduled restart requires the isolated native-menu test.'}
if($LeaveRunning -and (!$NativeMenuHandoff -or $RestartTick -or $AutomateGameplayInput)){throw 'Manual handoff requires native menu mode without scripted input/restart.'}
if($ReplayInvitationFile){if(!$NativeMenuHandoff){throw 'Replay requires an isolated native-menu test'};$ReplayInvitationFile=(Resolve-Path -LiteralPath $ReplayInvitationFile).Path}
if($NativeMenuHandoff){$RequireNativeGameplay=$true;$SeedNativeWorld=$true}
if($RequireNativeIdentity){$TraceNativeUuid=$true;$RequireNativeGameplay=$true}
if($TraceNativeUuid){$TraceNativeEntities=$true}
$workspace=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$reuseRoot=$null
if($ReuseContentFrom){
 if(!$NativeRoomMenu){throw 'ReuseContentFrom is restricted to the isolated native Room menu test.'}
 $reuseRoot=(Resolve-Path -LiteralPath $ReuseContentFrom).Path
 $allowed=(Resolve-Path -LiteralPath (Join-Path $workspace 'analysis')).Path
 if((Split-Path $reuseRoot -Parent) -ine $allowed -or !(Split-Path $reuseRoot -Leaf).StartsWith('native-lobby-')){throw 'Reuse source must be an existing native-lobby analysis capture.'}
 foreach($role in @('host','client')){foreach($name in @('UIMainMenu.pak','UIStartGame.pak','UIhud.pak')){
  $previous=Join-Path $reuseRoot ($role+'/pc-assets/packfiles/'+$name)
  $current=Join-Path $workspace ('private/pc-assets/packfiles/'+$name)
  if((Get-FileHash -LiteralPath $previous).Hash -ne (Get-FileHash -LiteralPath $current).Hash){throw 'Reusable native menu assets differ from the current candidate.'}
 }}
}
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
$peers=@();$owned=@();$nativeStartupStarted=$null;$manualHandoff=$false
$record=[ordered]@{root=$root;exe_sha256=(Get-FileHash -LiteralPath $exe).Hash;normal_user_hashes=$before;gameplay_input_sent=$false;screenshots_taken=$false;observation_seconds=$ObserveSeconds;verify_memory_isolation=[bool]$VerifyMemoryIsolation;require_native_continue=[bool]($RequireNativeContinue -or $RequireNativeGameplay);require_native_gameplay=[bool]$RequireNativeGameplay;trace_random_owners=[bool]$TraceRandomOwners;sample_native_main=[bool]$SampleNativeMain;trace_native_entities=[bool]$TraceNativeEntities;random_trace_start_tick=$RandomTraceStartTick;peers=@()}
$record.scripted_native_input=[bool]$ScriptedNativeInput;
$record.single_view=[bool]$SingleView;$record.original_split_view_diagnostic=[bool]$OriginalSplitView;
$record.checkpoint_reload=[bool]$RestartTick -and !$BareLevelRestart;
$record.behavior_dump_tick=$BehaviorDumpTick;$record.behavior_trace_start_tick=$BehaviorTraceStartTick;
$record.restart_tick=$RestartTick;$record.trace_native_uuid=[bool]$TraceNativeUuid;$record.trace_path_points=[bool]$TracePathPoints;$record.motion_trajectory=[bool]$MotionTrajectory;$record.post_velocity_trace=[bool]$PostVelocityTrace;$record.motion_input_trace=[bool]$MotionInputTrace;$record.navigation_trace=[bool]$NavigationTrace;$record.avoidance_trace=[bool]$AvoidanceTrace;$record.turn_limit_trace=[bool]$TurnLimitTrace;$record.turn_lifetime_trace=[bool]$TurnLifetimeTrace;$record.low_gpu_load=[bool]$LowGpuLoad
$record.seed_native_world=[bool]$SeedNativeWorld
$record.replayed_invitation=$ReplayInvitationFile;$record.host_frame_cap=$HostFrameCap;$record.guest_frame_cap=$GuestFrameCap
$record.require_native_identity=[bool]$RequireNativeIdentity
$record.native_room_menu=[bool]$NativeRoomMenu;$record.native_menu_handoff=[bool]$NativeMenuHandoff;$record.client_without_save=[bool]$ClientWithoutSave
$record.native_startup_timeout_seconds=$NativeStartupTimeoutSeconds
function Launch([string[]]$tokens){
 $info=[Diagnostics.ProcessStartInfo]::new($exe);$info.UseShellExecute=$false;$info.WindowStyle=if($LeaveRunning){[Diagnostics.ProcessWindowStyle]::Normal}else{[Diagnostics.ProcessWindowStyle]::Hidden};$info.WorkingDirectory=$runtime
 foreach($token in $tokens){$info.ArgumentList.Add($token)}
 return [Diagnostics.Process]::Start($info)
}
if(-not ('EM2PairWindows' -as [type])){Add-Type -TypeDefinition @'
using System;using System.Text;using System.Runtime.InteropServices;
public static class EM2PairWindows {
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] public static extern bool CreateHardLink(string target,string source,IntPtr reserved);
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
  if($reuseRoot){
   foreach($entry in Get-ChildItem -LiteralPath (Join-Path $reuseRoot ($role+'/pc-assets')) -Force){
    $target=Join-Path $content $entry.Name
    if($entry.PSIsContainer){New-Item -ItemType Junction -Path $target -Target $entry.FullName | Out-Null}
    else{Copy-Item -LiteralPath $entry.FullName -Destination $target}
   }
  }else{
  foreach($entry in Get-ChildItem -LiteralPath (Join-Path $runtime 'pc-assets') -Force){
   $target=Join-Path $content $entry.Name
   if($NativeMenuHandoff -and $entry.PSIsContainer -and $entry.Name -eq 'packfiles'){
    New-Item -ItemType Directory -Path $target | Out-Null
    foreach($archive in Get-ChildItem -LiteralPath $entry.FullName -File){
     $archiveTarget=Join-Path $target $archive.Name
     if($archive.Name -ieq 'UIMainMenu.pak' -or $archive.Name -ieq 'UIStartGame.pak' -or ($SingleView -and $archive.Name -ieq 'UIhud.pak')){
      $nativeMenu=Join-Path $workspace ('private/pc-assets/packfiles/'+$archive.Name)
      if(!(Test-Path -LiteralPath $nativeMenu)){throw 'Build the private native Online Multiplayer archive first.'}
      Copy-Item -LiteralPath $nativeMenu -Destination $archiveTarget
     }elseif(![EM2PairWindows]::CreateHardLink($archiveTarget,$archive.FullName,[IntPtr]::Zero)){Copy-Item -LiteralPath $archive.FullName -Destination $archiveTarget}
    }
   }
   elseif($entry.PSIsContainer){New-Item -ItemType Junction -Path $target -Target $entry.FullName | Out-Null}
   elseif($entry.Name -eq 'ConfigFiles.ini'){Copy-Item -LiteralPath $entry.FullName -Destination $target}
   else{Copy-Item -LiteralPath $entry.FullName -Destination $target}
  }
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
    $value=if($key -eq 'frame_limit'){[Array]::IndexOf(@(30,40,50,60,90,120,144,165,240,0),$(if($role -eq 'host'){$HostFrameCap}else{$GuestFrameCap}))}else{0}
    $pattern='^'+[regex]::Escape($key)+'=.*$'
    if(@($settingsLines | Where-Object {$_ -match $pattern}).Count){$settingsLines=@($settingsLines -replace $pattern,($key+'='+$value))}
    else{$settingsLines+=($key+'='+$value)}
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
   if($MotionTraceGuid){$tokens+=('--em2_behavior_motion_guid='+$MotionTraceGuid)}
   if($TracePathPoints){$tokens+='--em2_behavior_trace_path_points=true'}
   if($MotionTrajectory){$tokens+='--em2_behavior_trace_motion_trajectory=true'}
   if($PostVelocityTrace){$tokens+='--em2_behavior_trace_post_velocity=true'}
   if($MotionInputTrace){$tokens+='--em2_behavior_trace_motion_input=true'}
   if($NavigationTrace){$tokens+='--em2_behavior_trace_navigation=true'}
   if($AvoidanceTrace){$tokens+='--em2_behavior_trace_avoidance=true'}
   if($TurnLimitTrace){$tokens+='--em2_behavior_trace_turn_limits=true'}
   if($TurnLifetimeTrace){$tokens+='--em2_behavior_trace_turn_lifetime=true'}
   $tokens+=('--em2_behavior_trace_start_tick='+$BehaviorTraceStartTick)
   if($BehaviorDumpTick -ne 4294967295){$tokens+=('--em2_behavior_trace_dump_tick='+$BehaviorDumpTick)}
   if($NoInterpolation){$tokens+='--em2_interpolation=false'}
   if($OriginalSplitView){$tokens+='--em2_online_single_view=false'}
   # SingleView validates the production default without a command-line override.
   if($ScriptedNativeInput){$tokens+='--em2_native_lobby_qa_input_sequence=true'}
   if($RestartTick){$tokens+=('--em2_native_lobby_qa_restart_tick='+$RestartTick)}
   if($RestartTick){$tokens+=('--em2_native_lobby_qa_checkpoint_reload='+$(if($BareLevelRestart){'false'}else{'true'}))}
   if($ReplayInvitationFile -and $peer.role -eq 'host'){$tokens+=('--em2_native_lobby_qa_replay_invitation_file='+$ReplayInvitationFile)}
   if($NativeRoomMenu){$tokens+=@('--em2_native_room_lobby=true','--em2_room_native_buttons=true')}
   $tokens+=@('--em2_private_profile=true','--em2_native_online_handoff=true',('--em2_native_lobby_qa_role='+$peer.role),('--em2_native_lobby_qa_invitation_file='+$invite),'--em2_trace_auto_continue=true','--em2_auto_continue=false')
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
    $startupLog=if(Test-Path -LiteralPath $item.peer.log){(Get-Content -LiteralPath $item.peer.log -Raw) ?? ''}else{''}
    foreach($failure in @('Retained native Room stopped:','Native Room gameplay stopped:','Native online lobby handoff failed:','Native online menu preparation failed:','Native lobby QA rejected:','Retained online session stopped:','Online simulation stopped:')){
     if($startupLog.Contains($failure)){throw "Native-menu $($item.peer.role) reported $failure See its startup.log."}
    }
    $handoffDone=$startupLog.Contains('Native lobby completed authored save reload and accepted native Continue')
    $gateAttached=$startupLog.Contains($nativeGateMarker)
    $gameplayObserved=$startupLog -match 'Native gameplay observation v[45]:'
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
 if($LeaveRunning){$manualHandoff=$true;$record.manual_handoff=$true;Write-Output 'Both connected windows are ready for manual testing; automatic shutdown is disabled.';return}
 if($AutomateGameplayInput){
  foreach($phase in @('movement','tools','camera')){& (Join-Path $PSScriptRoot 'test-private-online-input.ps1') -PairRoot $root -Phase $phase}
 }
 $deadline=[DateTime]::UtcNow.AddSeconds($ObserveSeconds)
 while([DateTime]::UtcNow -lt $deadline -and @($owned | Where-Object {$_.process.HasExited}).Count -eq 0){
  ConfirmPrivateMemoryIsolation
  foreach($item in $owned){$tail=Get-Content -LiteralPath $item.peer.log -Raw;if($tail.Contains("Online simulation stopped:") -or $tail.Contains("Retained online session stopped:")){throw "Online observation stopped; see the private native logs."}}
  Start-Sleep -Milliseconds 250
 }
 foreach($item in $owned){
  $item.peer.alive_after_observation=!$item.process.HasExited
  $observed=if(Test-Path -LiteralPath $item.peer.log){(Get-Content -LiteralPath $item.peer.log -Raw) ?? ''}else{''}
  $item.peer.session_failed_before_close=$observed.Contains('Retained online session stopped:') -or $observed.Contains('Online simulation stopped:')
 }
} catch {$record.error=$_.Exception.Message;$record.error_location=$_.ScriptStackTrace}
finally {
 if($record.Contains('error')){Start-Sleep -Milliseconds 1000} # Let the native failure owner finish its bounded diagnostic dump.
 $record.gameplay_input_sent=Test-Path -LiteralPath (Join-Path $root 'automated-input.json')
 if($NativeMenuHandoff -and $null -ne $nativeStartupStarted -and !$record.Contains('native_startup_elapsed_seconds')){
  $record.native_startup_elapsed_seconds=([DateTime]::UtcNow-$nativeStartupStarted).TotalSeconds
 }
 if(!$manualHandoff){
 foreach($item in $owned){
  $p=$item.process;$p.Refresh()
  # Failures can happen before the native window is created. Wait for the
  # owned window rather than sending a close once to a not-yet-existing HWND.
  $closeDeadline=[DateTime]::UtcNow.AddSeconds(30)
  while(!$p.HasExited -and [DateTime]::UtcNow -lt $closeDeadline){
   $current=Get-CimInstance Win32_Process -Filter ("ProcessId="+$p.Id)
   $valid=$current -and $current.ExecutablePath -eq $exe -and $current.CommandLine.Contains($item.peer.user) -and [Math]::Abs(($current.CreationDate.ToUniversalTime()-$item.start.ToUniversalTime()).TotalSeconds) -lt 2
   $item.peer.close_identity_valid=[bool]$valid
   if(!$valid){break}
   $item.peer.close_messages=[EM2PairWindows]::CloseRex([uint32]$p.Id)
   if($item.peer.close_messages -gt 0){break}
   Start-Sleep -Milliseconds 250
   $p.Refresh()
  }
 }
 foreach($item in $owned){
  $p=$item.process;$item.peer.process_exited=$p.WaitForExit(30000)
  if(!$item.peer.process_exited){
   $current=Get-CimInstance Win32_Process -Filter ("ProcessId="+$p.Id)
   if($current -and $current.ExecutablePath -ieq $exe -and $current.CommandLine.Contains($item.peer.user) -and
      [Math]::Abs(($current.CreationDate.ToUniversalTime()-$item.start.ToUniversalTime()).TotalSeconds) -lt 2){
    Stop-Process -Id $p.Id -Force;$item.peer.forced_private_shutdown=$true;$item.peer.process_exited=$p.WaitForExit(10000)
   }
  }
  if($p.HasExited){$item.peer.exit_code=$p.ExitCode}
 }
 }
 foreach($peer in $peers){
  $log=if(Test-Path -LiteralPath $peer.log){(Get-Content -LiteralPath $peer.log -Raw) ?? ''}else{''}
  $peer.retained_prelaunch_ready=$log.Contains('Localhost QA retained session ready before runtime creation')
  $peer.native_menu_opened=$log.Contains('Authored native Online Multiplayer page opened')
  $peer.native_menu_bridge=$log.Contains('Native FrontMenu online SetOption bridge registered')
  $peer.native_menu_real_ready=$log.Contains('Native online lobby start confirmed for ')
  $peer.native_menu_handoff_complete=$log.Contains('Native lobby completed authored save reload and accepted native Continue')
  $peer.native_gate_attached=$log.Contains($nativeGateMarker)
  $peer.native_profile_requested=$log.Contains('Online startup selected native local controller ') -or ($NativeMenuHandoff -and $log.Contains('Offline startup selected native local controller '))
  $peer.native_continue_requested=$log.Contains('Online startup requested native Continue') -or ($NativeMenuHandoff -and $peer.native_menu_handoff_complete)
  $peer.guest_frames_observed=$log.Contains('Guest present callers, cap ')
  $peer.observations=([regex]::Matches($log,'Native gameplay observation v[45]:')).Count
  $peer.native_input_sequence_sent=$ScriptedNativeInput -and [regex]::IsMatch($log,"Native online input sequence: local [01], source tick (120|180), phase '[^']+ forward', joining false")
  $peer.private_crash=Test-Path -LiteralPath (Join-Path $peer.user 'diagnostics/crash.log')
  $peer.isolated=$log.Contains(('Save/settings directory: '+$peer.user)) -and $log.Contains($peer.content)
 }
 $after=UserHashes;$record.user_files_unchanged=$before.Count -eq $after.Count
 foreach($path in $before.Keys){if(!$after.ContainsKey($path) -or $before[$path] -ne $after[$path]){$record.user_files_unchanged=$false}}
 $record.normal_config_unchanged=(Get-FileHash -LiteralPath $config).Hash -eq $configHash
 $afterCrash=if(Test-Path -LiteralPath $crash){(Get-FileHash -LiteralPath $crash).Hash}else{''};$record.normal_crash_unchanged=$afterCrash -eq $crashHash
 $record.gameplay_input_sent=$record.gameplay_input_sent -or @($peers | Where-Object {$_.native_input_sequence_sent}).Count -gt 0
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
 & (Join-Path $PSScriptRoot 'read-paired-gameplay.ps1') -PairRoot $root -MinimumTicks 300 -NativeRoom:$NativeRoomMenu | Set-Content -LiteralPath (Join-Path $root 'gameplay-observations.json')
 $comparison=Get-Content -LiteralPath (Join-Path $root 'gameplay-observations.json') -Raw | ConvertFrom-Json
 $record.native_gameplay_two_ticks_match=[bool]$comparison.two_consecutive_native_ticks_match
 $record.native_gameplay_verified=[bool]$comparison.required_native_ticks_match -and [bool]$comparison.all_shared_ticks_match
 $record | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $root 'result.json')
}
if(($RequireNativeContinue -or $RequireNativeGameplay) -and @($peers | Where-Object {!$_.native_continue_requested}).Count -ne 0){
 $record.validation_failure='Native Continue was not accepted by both peers during the observation.'
 $record | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $root 'result.json')
 Write-Output $record.validation_failure
 exit 1
}
if($RequireNativeGameplay -and !$record.native_gameplay_verified){
 $record.validation_failure=if(@($comparison.different_ticks).Count){'Native gameplay digests differ at ticks '+($comparison.different_ticks -join ',')+'. Longest matching run: '+$comparison.longest_consecutive_match+' ticks.'}else{'Native gameplay observations lack 300 consecutive matches or contain conflicting duplicate ticks.'}
 $record | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $root 'result.json')
 Write-Output $record.validation_failure;exit 1
}
if($SingleView){
 foreach($peer in $peers){
  $slot=if($peer.role -eq 'host'){0}else{1}
  $text=Get-Content -LiteralPath $peer.log -Raw
  $views=[regex]::Matches($text,('Online single-view primary: local '+$slot+', requested ([0-9A-F]{8}), selected ([0-9A-F]{8}), retained ([0-9A-F]{8}), view ([0-9A-F]{8})'))
  $peer.single_view_primary_verified=$false
  if($views.Count){
   $last=$views[$views.Count-1]
   $peer.single_view_primary_verified=$last.Groups[2].Value -ne '00000000' -and $last.Groups[2].Value -eq $last.Groups[3].Value -and ($slot -eq 0 -or $last.Groups[1].Value -ne $last.Groups[2].Value)
  }
  $peer.single_view_layout_verified=$text.Contains(('Online single-view layout: local '+$slot+', split 0,')) -and !$text.Contains(('Online single-view layout: local '+$slot+', split 1,'))
  $peer.single_view_hud_binding_verified=$text.Contains(('Online local HUD property: mode '+(2+$slot)+' '))
 }
 $record.native_single_view_bindings_verified=@($peers | Where-Object {!$_.single_view_primary_verified -or !$_.single_view_layout_verified -or !$_.single_view_hud_binding_verified}).Count -eq 0
 $record | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $root 'result.json')
 if(!$record.native_single_view_bindings_verified){Write-Output 'Native local camera/layout/HUD bindings were not observed on both peers; visual acceptance remains separate.';exit 1}
}
if($OriginalSplitView){
 $record.native_single_view_bindings_verified=$false
 $record.native_input_response_verified=$false
 $record.diagnostic_scope='Original split-view comparison only; local full-window camera/HUD and actor/camera response acceptance are not evaluated.'
 $record.scripted_input_program_completed=$true
 if($ScriptedNativeInput){foreach($peer in $peers){
  $slot=if($peer.role -eq 'host'){0}else{1}
  $text=Get-Content -LiteralPath $peer.log -Raw
  if(!$peer.native_input_sequence_sent -or !$text.Contains(("Native online input sequence: local "+$slot+", source tick 600, phase 'complete', joining false"))){$record.scripted_input_program_completed=$false}
 }}
 $record | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $root 'result.json')
 if(!$record.scripted_input_program_completed){Write-Output 'Original split-view diagnostic did not complete its private input program.';exit 1}
}
if($ScriptedNativeInput -and !$OriginalSplitView){
 $responsePath=Join-Path $root 'native-input-response.json'
 & python (Join-Path $PSScriptRoot 'analyze-native-online-input.py') $root --input-delay 2 --output $responsePath
 $responseExit=$LASTEXITCODE
 $record.native_input_response_verified=$false
 if(Test-Path -LiteralPath $responsePath){$record.native_input_response_verified=[bool](Get-Content -LiteralPath $responsePath -Raw | ConvertFrom-Json).native_input_response_verified}
 $record | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $root 'result.json')
 if($responseExit -ne 0 -or !$record.native_input_response_verified){Write-Output 'Native scripted actor/camera input responses did not pass; see native-input-response.json.';exit 1}
}
if($RestartTick){
 foreach($peer in $peers){
  $text=Get-Content -LiteralPath $peer.log -Raw
  $request=[regex]::Match($text,'Native lobby QA restart requested: tick ([0-9]+), attempted true, status 0, stack true')
  $peer.native_restart_requested=$request.Success -and [uint64]$request.Groups[1].Value -ge $RestartTick
  if($request.Success){$peer.native_restart_tick=[uint64]$request.Groups[1].Value}
 }
 & (Join-Path $PSScriptRoot 'read-paired-gameplay.ps1') -PairRoot $root -MinimumTicks 300 -AfterNativeReload | Set-Content -LiteralPath (Join-Path $root 'gameplay-after-restart.json')
 $restartComparison=Get-Content -LiteralPath (Join-Path $root 'gameplay-after-restart.json') -Raw | ConvertFrom-Json
 $idlePath=Join-Path $root 'reload-idle-positions.json'
 & python (Join-Path $PSScriptRoot 'analyze-native-online-reload.py') $root --output $idlePath
 $idleExit=$LASTEXITCODE
 $record.native_restart_positions_stable=$idleExit -eq 0 -and [bool](Get-Content -LiteralPath $idlePath -Raw | ConvertFrom-Json).idle_reload_positions_stable
 $record.native_restart_verified=$record.native_restart_positions_stable -and [bool]$restartComparison.required_native_ticks_match -and [bool]$restartComparison.all_shared_ticks_match -and @($peers | Where-Object {!$_.native_restart_requested}).Count -eq 0 -and $peers[0].native_restart_tick -eq $peers[1].native_restart_tick
 $record | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $root 'result.json')
 if(!$record.native_restart_verified){Write-Output 'Scheduled native checkpoint reload did not produce settled players and 300 matching post-reload gameplay ticks on both peers.';exit 1}
}
foreach($peer in $peers){
 if($NativeMenuHandoff){if(!$peer.native_menu_opened -or !$peer.native_menu_bridge -or !$peer.native_menu_real_ready -or !$peer.native_menu_handoff_complete){exit 1}}
 elseif(!$peer.retained_prelaunch_ready){exit 1}
 if(!$peer.isolated -or !$peer.guest_frames_observed -or !$peer.process_exited -or $peer.exit_code -ne 0 -or $peer.private_crash -or $peer.session_failed_before_close){exit 1}
}
