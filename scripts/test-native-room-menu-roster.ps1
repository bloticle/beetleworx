[CmdletBinding()]
param(
 [ValidateRange(3,16)][int]$Players=3,
 
 [switch]$CooperativeShutdown,
 [ValidateRange(0,10)][int]$MovieSkipDelay=0,
 [switch]$MovieSequence,[switch]$AuthoredConversation,[switch]$CutsceneCycle,[switch]$CutsceneBoundaryMovement,[switch]$CutsceneSkipInput,[switch]$CutsceneHeldReturn,[switch]$UnfencedCutsceneReturn,
 [switch]$ExtraOswald,
 [switch]$ScriptedInput,[switch]$ChatProbe,[switch]$VoiceProbe,[switch]$ButtonTaskProbe,[switch]$PolicyCycle,[switch]$CaptureShutdownHang,
 [switch]$NavigationTrace,[switch]$FaceTargetPoison,[switch]$UnfixedFaceTarget,[switch]$NativeReload,
 [ValidateRange(0,100000)][int]$CheckpointReloadTick=0,
 [ValidateSet(0,1,2,3,4)][int]$DamagePlayer=0,
 [ValidateRange(0,16)][int]$DamageExtraPeer=0,
 [ValidateRange(-1,16)][int]$PipelineCreationThreads=-1,
 [ValidateRange(1,4)][int]$CheckpointReloadCount=1,
 [ValidateRange(600,100000)][int]$CheckpointReloadInterval=700,
 [ValidatePattern('^([a-z0-9_.]{1,63})?$')][string]$NativeLevelTarget='',
 [ValidateCount(1,4)][ValidatePattern('^[a-z0-9_.]{1,63}$')][string[]]$NativeLevelTargets=@(),
 [switch]$DebugCleanLevelLoad,
 [ValidateRange(0,16)][int]$CloseParticipant=0,
 [switch]$ReturnToTitle,[switch]$ReenterAfterReturn,[switch]$RecoveryPrompt,[switch]$RecoveryController,
 [ValidateRange(0,16)][int]$PartialDeliveryParticipant=0,
 [ValidateRange(400,100000)][int]$PartialDeliveryTick=500,
 [string]$ReuseVerifiedContent='', [string]$ReplayInvitationFile='',
 [ValidateRange(20,180)][int]$ObserveSeconds=30,
 [ValidateRange(256,8192)][int]$MinimumFreeMemoryMB=1024,
 [ValidateRange(256,16384)][int]$MinimumFreeCommitMB=1536,
 [ValidateRange(120,600)][int]$StartupTimeoutSeconds=360
)
$ErrorActionPreference='Stop'
if($MovieSkipDelay -and !$MovieSequence){throw 'MovieSkipDelay requires MovieSequence.'}
if($MovieSequence -and ($AuthoredConversation -or $CutsceneCycle -or $Players -ne 3 -or !$ScriptedInput -or $CheckpointReloadTick -ne 300 -or $CheckpointReloadCount -ne 1 -or $DamagePlayer -or $DamageExtraPeer -or $NativeLevelTarget -or $NativeLevelTargets.Count -or $PolicyCycle -or $ReenterAfterReturn -or $CloseParticipant -or $PartialDeliveryParticipant)){throw 'MovieSequence requires three scripted humans and one original StartSequence call at tick 300, without other fixtures.'}
if($AuthoredConversation -and ($MovieSequence -or $CutsceneCycle -or $Players -ne 3 -or !$ScriptedInput -or $CheckpointReloadTick -notin @(300,700) -or $CheckpointReloadCount -ne 1 -or $DamagePlayer -or $DamageExtraPeer -or $NativeLevelTarget -or $NativeLevelTargets.Count -or $PolicyCycle -or $ReenterAfterReturn -or $CloseParticipant -or $PartialDeliveryParticipant)){throw 'AuthoredConversation requires three scripted humans, one original sequence call at tick 300 or 700, and no other fixture.'}
if($CutsceneHeldReturn -and !$CutsceneCycle){throw 'CutsceneHeldReturn requires CutsceneCycle.'}
if($UnfencedCutsceneReturn -and !$CutsceneHeldReturn){throw 'UnfencedCutsceneReturn requires CutsceneHeldReturn.'}
if($CutsceneSkipInput -and !$CutsceneCycle -and !$AuthoredConversation -and !$MovieSequence){throw 'CutsceneSkipInput requires CutsceneCycle, AuthoredConversation or MovieSequence.'}
if($CutsceneBoundaryMovement -and !$CutsceneCycle){throw 'CutsceneBoundaryMovement requires CutsceneCycle.'}
if($CutsceneCycle -and ($Players -ne 3 -or !$ScriptedInput -or $CheckpointReloadTick -ne 700 -or $CheckpointReloadCount -ne 2 -or $CheckpointReloadInterval -ne 600 -or $DamagePlayer -or $DamageExtraPeer -or $NativeLevelTarget -or $NativeLevelTargets.Count -or $PolicyCycle -or $ReenterAfterReturn -or $CloseParticipant -or $PartialDeliveryParticipant)){throw 'CutsceneCycle requires three scripted humans, original restriction at tick 700/release at 1300, and no other fixture.'}
if($DamagePlayer -eq 4){if($Players -ne 3 -or ($DamageExtraPeer -and $DamageExtraPeer -ne 3)){throw 'Whole-party damage requires exactly three players and extra peer 3.'};$DamageExtraPeer=3}
if($DamageExtraPeer -and ($DamageExtraPeer -lt 3 -or $DamageExtraPeer -gt $Players -or ($DamagePlayer -and $DamagePlayer -ne 4) -or !$ScriptedInput -or !$CheckpointReloadTick -or $CheckpointReloadCount -ne 1 -or $NativeLevelTarget -or $NativeLevelTargets.Count -or $PolicyCycle -or $ReenterAfterReturn -or $CloseParticipant -or $PartialDeliveryParticipant)){throw 'Extra damage requires one real extra peer, one scheduled native damage action, scripted input, and no other fixture.'}
if($DamagePlayer -and (!$ScriptedInput -or !$CheckpointReloadTick -or $CheckpointReloadCount -ne 1 -or $NativeLevelTarget -or $NativeLevelTargets.Count -or $PolicyCycle -or $ReenterAfterReturn -or $CloseParticipant -or $PartialDeliveryParticipant)){throw 'Damage fixture requires one scheduled original damage action without a level route, policy cycle, or disconnect injection.'}
if($PolicyCycle -and (!$ScriptedInput -or !$ButtonTaskProbe -or $Players -ne 4 -or $CheckpointReloadTick -or $ReenterAfterReturn -or $CloseParticipant -or $PartialDeliveryParticipant)){throw 'PolicyCycle requires four isolated scripted players and task probes, without reload or fault injection.'}
if($RecoveryController -and !$RecoveryPrompt){throw 'Controller recovery QA requires RecoveryPrompt mode; the HUD auto-click will be disabled.'}
if($RecoveryPrompt -and (!$ReenterAfterReturn -or !$ReturnToTitle)){throw 'Recovery prompt QA requires the native return and re-entry capture.'}
if($ReturnToTitle -and !$CloseParticipant -and !$ReenterAfterReturn){throw 'Private title-return test requires participant-close injection or controlled disconnect/re-entry.'}
if($ReenterAfterReturn -and (!$ReturnToTitle -or !$ScriptedInput -or $CloseParticipant -or $PartialDeliveryParticipant -or $CheckpointReloadTick)){throw 'Private re-entry requires scripted input and title return, without other failure/reload injection.'}
if($PartialDeliveryParticipant -and (!$ScriptedInput -or $CheckpointReloadTick -or $CloseParticipant -or $PartialDeliveryParticipant -gt $Players)){throw 'Partial-delivery injection requires scripted input, an existing participant, no scheduled reload and no window-close injection.'}
if($CloseParticipant -and (!$ScriptedInput -or $CheckpointReloadTick -or $CloseParticipant -gt $Players)){throw 'Participant-close negative test requires scripted input, an existing participant and no scheduled reload.'}
if($NativeLevelTargets.Count -and (!$CheckpointReloadTick -or $NativeLevelTarget -or $NativeLevelTargets.Count -ne $CheckpointReloadCount)){throw 'A level route requires scheduled reloads, matching destination count, and no separate level target.'}
if($DebugCleanLevelLoad -and !$NativeLevelTarget -and !$NativeLevelTargets.Count){throw 'Debug CleanLoadLevel comparison requires a native target.'}
if($UnfixedFaceTarget -and !$FaceTargetPoison){throw 'The original undefined fallback comparison requires explicit isolated allocation fault injection.'}
if($NativeLevelTarget -and !$CheckpointReloadTick){throw 'A native level target requires an explicit scheduled reload tick.'}
$workspace=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$runtime=Join-Path $workspace 'build/runtime23'
$exe=(Resolve-Path -LiteralPath (Join-Path $runtime 'beetleworx.exe')).Path
if($ReplayInvitationFile){$ReplayInvitationFile=(Resolve-Path -LiteralPath $ReplayInvitationFile).Path}
if(Get-CimInstance Win32_Process -Filter "Name='beetleworx.exe' OR Name='EM2Recomp.exe'"){throw 'An existing game is running; nothing was launched.'}
$root=Join-Path $workspace ('analysis/native-lobby-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $root | Out-Null
$packs=Join-Path $root 'shared-packfiles'
if($ReuseVerifiedContent){
 $ReuseVerifiedContent=(Resolve-Path -LiteralPath $ReuseVerifiedContent).Path
 $allowed=(Join-Path $workspace 'analysis/native-lobby-')
 if(!$ReuseVerifiedContent.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase)){throw 'Content reuse requires an owned native-lobby capture.'}
 $previous=Get-Content -LiteralPath (Join-Path $ReuseVerifiedContent 'roster.json') -Raw | ConvertFrom-Json
 if(!$previous.native_room_menu){throw 'Content reuse requires a native-menu capture.'}
 $previousPacks=(Resolve-Path -LiteralPath (Join-Path $ReuseVerifiedContent 'shared-packfiles')).Path
 $sameUI=$true
 foreach($name in @('UIMainMenu.pak','UIStartGame.pak','UIhud.pak')){
  $current=Join-Path $workspace ('private/pc-assets/packfiles/'+$name)
  $prior=Join-Path $previousPacks $name
  if(!(Test-Path -LiteralPath $prior) -or (Get-FileHash -LiteralPath $current).Hash -ne (Get-FileHash -LiteralPath $prior).Hash){$sameUI=$false;break}
 }
 if($sameUI){New-Item -ItemType Junction -Path $packs -Target $previousPacks | Out-Null}
 else{
  Write-Output 'Authored menu/HUD changed; preparing a fresh immutable snapshot instead of testing stale UI.'
  $ReuseVerifiedContent=''
  New-Item -ItemType Directory -Path $packs | Out-Null
 }
}else{New-Item -ItemType Directory -Path $packs | Out-Null}
$normal=Join-Path $runtime 'user_data'
$baseline=@{};foreach($file in Get-ChildItem -LiteralPath $normal -Recurse -File){$baseline[$file.FullName]=(Get-FileHash -LiteralPath $file.FullName).Hash}
$exeHash=(Get-FileHash -LiteralPath $exe).Hash
$normalConfig=Join-Path $runtime 'pc-assets/ConfigFiles.ini';$configHash=(Get-FileHash -LiteralPath $normalConfig).Hash
$normalCrash=Join-Path $runtime 'crash.log';$crashHash=if(Test-Path -LiteralPath $normalCrash){(Get-FileHash -LiteralPath $normalCrash).Hash}else{''}
$records=[Collections.Generic.List[object]]::new();$owned=[Collections.Generic.List[object]]::new()
$record=[ordered]@{root=$root;players=$Players;exe_sha256=$exeHash;native_room_menu=$true;checkpoint_reload_tick=$CheckpointReloadTick;voice_probe=[bool]$VoiceProbe;navigation_trace=[bool]$NavigationTrace;face_target_poison=[bool]$FaceTargetPoison;face_target_unfixed=[bool]$UnfixedFaceTarget;replay_invitation=$ReplayInvitationFile;chat_probe=[bool]$ChatProbe;scripted_sdk_input=[bool]$ScriptedInput;mixed_characters=$true;gameplay_input_sent=$false;screenshots_taken=$false;startup_complete=$false;observation_complete=$false;processes=@()}
$record.capture_shutdown_hang=[bool]$CaptureShutdownHang
$record.cooperative_shutdown_override=[bool]$CooperativeShutdown
$record.damage_extra_peer=$DamageExtraPeer;
$record.extra_dead_input=[bool]$DamageExtraPeer;$record.extra_oswald=[bool]$ExtraOswald;$record.mixed_characters=!$ExtraOswald;
$record.movie_skip_delay=$MovieSkipDelay;$record.movie_sequence=[bool]$MovieSequence;$record.authored_conversation=[bool]$AuthoredConversation;$record.cutscene_held_return=[bool]$CutsceneHeldReturn;$record.unfenced_cutscene_return=[bool]$UnfencedCutsceneReturn;$record.cutscene_skip_input=[bool]$CutsceneSkipInput;$record.cutscene_boundary_movement=[bool]$CutsceneBoundaryMovement;$record.cutscene_cycle=[bool]$CutsceneCycle;$record.whole_party_damage=($DamagePlayer -eq 4);$record.damage_player=$DamagePlayer;$record.pipeline_creation_threads=$PipelineCreationThreads
$record.commit_guard_mb=$MinimumFreeCommitMB;$record.minimum_free_commit_mb=$null
$record.memory_guard_mb=$MinimumFreeMemoryMB;$record.memory_samples=@();$record.minimum_free_memory_mb=$null;$record.peak_owned_working_set_mb=0;$record.peak_owned_private_mb=0
$nextMemorySample=[DateTime]::MinValue
$record.native_reload=[bool]$NativeReload
$record.button_task_probe=[bool]$ButtonTaskProbe
$record.policy_cycle=[bool]$PolicyCycle
$record.checkpoint_reload_count=$CheckpointReloadCount
$record.checkpoint_reload_interval=$CheckpointReloadInterval
$record.native_level_target=$NativeLevelTarget
$record.native_level_targets=@($NativeLevelTargets)
$record.native_level_method=if($NativeLevelTarget -or $NativeLevelTargets.Count){if($DebugCleanLevelLoad){'original-level'}else{'original-transition'}}else{''}
$record.reused_verified_content=$ReuseVerifiedContent
$record.close_participant=$CloseParticipant;$record.failure_observed=$false;$record.leave_boundary_probe=[bool]($CloseParticipant -or $PartialDeliveryParticipant)
$record.partial_delivery_participant=$PartialDeliveryParticipant;$record.partial_delivery_tick=$PartialDeliveryTick;$record.return_to_title_probe=[bool]$ReturnToTitle;$record.reenter_after_return=[bool]$ReenterAfterReturn;$record.recovery_prompt=[bool]$RecoveryPrompt;$record.recovery_controller=[bool]$RecoveryController
if(-not ('BWNativeMenuWindows' -as [type])){Add-Type -TypeDefinition @'
using System;using System.Text;using System.Runtime.InteropServices;
public static class BWNativeMenuWindows {
 public delegate bool Callback(IntPtr window,IntPtr param);
 [StructLayout(LayoutKind.Sequential)] public struct MemoryStatus {public uint length,load;public ulong totalPhysical,availablePhysical,totalPageFile,availablePageFile,totalVirtual,availableVirtual,availableExtendedVirtual;}
 [DllImport("kernel32.dll",SetLastError=true)] static extern bool GlobalMemoryStatusEx(ref MemoryStatus status);
 public static MemoryStatus Memory(){var status=new MemoryStatus();status.length=(uint)Marshal.SizeOf<MemoryStatus>();if(!GlobalMemoryStatusEx(ref status))throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());return status;}
 [DllImport("user32.dll")] public static extern bool EnumWindows(Callback callback,IntPtr param);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window,out uint pid);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr window,StringBuilder name,int count);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window,uint message,IntPtr wparam,IntPtr lparam);
 public static int Close(uint pid){int count=0;EnumWindows((window,param)=>{uint owner;GetWindowThreadProcessId(window,out owner);if(owner==pid){var name=new StringBuilder(256);GetClassName(window,name,256);if(name.ToString()=="RexWindowClass" && PostMessage(window,0x10,IntPtr.Zero,IntPtr.Zero))++count;}return true;},IntPtr.Zero);return count;}
}
'@}
function OfflineCampaignHashes($peer){
 $campaignRoot=Join-Path $peer.user '42560816/00000001';$hashes=[ordered]@{}
 if(Test-Path -LiteralPath $campaignRoot){foreach($file in Get-ChildItem -LiteralPath $campaignRoot -Recurse -File){$hashes[[IO.Path]::GetRelativePath($campaignRoot,$file.FullName)]=(Get-FileHash -LiteralPath $file.FullName).Hash}}
 return $hashes
}
function OwnedProcess($entry){
 if($entry.process.HasExited){return $null}
 # CIM may briefly expose an empty or stale record immediately after PID reuse.
 # Retry only reads, while the exact process handle returned by Start stays live.
 # Every accepted sample still requires all original identity checks.
 $reason='Process metadata unavailable'
 for($attempt=0;$attempt -lt 4;++$attempt){
  if($entry.process.HasExited){return $null}
  try{$p=Get-CimInstance Win32_Process -Filter ('ProcessId='+$entry.record.pid)}catch{$p=$null}
  if($p -and $p.CommandLine -and $p.ExecutablePath -ieq $exe -and $p.CommandLine.Replace('/','\').Contains($entry.record.root) -and [Math]::Abs(($p.CreationDate.ToUniversalTime()-([datetime]$entry.record.started_utc).ToUniversalTime()).TotalMilliseconds) -le 1){return $entry.process}
  if($p){$reason=if(!$p.CommandLine){'Command line unavailable'}elseif($p.ExecutablePath -ine $exe){'Executable path mismatch'}elseif(!$p.CommandLine.Replace('/','\').Contains($entry.record.root)){'Private root mismatch'}else{'Creation time mismatch'}}
  if($attempt -lt 3){Start-Sleep -Milliseconds 75}
 }
 $entry.record.identity_rejected=$true;$entry.record.identity_rejection_reason=$reason;return $null
}
function ObserveControlledDisconnect([int]$Attempts=1){
 if(!$ReenterAfterReturn){return $false}
 if($record.Contains('failure_injection_started_utc')){return $true}
 for($attempt=0;$attempt -lt $Attempts;++$attempt){
  $hostText=if(Test-Path -LiteralPath $records[0].log){(Get-Content -LiteralPath $records[0].log -Raw) ?? ''}else{''}
  if($hostText.Contains('Private native Room completed disconnect injected:')){
   $record.failure_injection_started_utc=[DateTime]::UtcNow.ToString('o')
   Write-Host 'Controlled completed-boundary disconnect observed; waiting for native return and fresh lobby gameplay.'
   return $true
  }
  if($attempt+1 -lt $Attempts){Start-Sleep -Milliseconds 75}
 }
 return $false
}

function SampleOwnedMemory {
 $memory=[BWNativeMenuWindows]::Memory()
 $freeMB=[long]($memory.availablePhysical/1MB);$commitMB=[long]($memory.availablePageFile/1MB)
 $working=0L;$private=0L
 foreach($entry in $owned){$process=$entry.process
  if(!$process.HasExited){$process.Refresh();$working+=$process.WorkingSet64;$private+=$process.PrivateMemorySize64}
 }
 $sample=[ordered]@{utc=[DateTime]::UtcNow.ToString('o');free_mb=$freeMB;free_commit_mb=$commitMB;owned_working_set_mb=[long]($working/1MB);owned_private_mb=[long]($private/1MB)}
 if($record.memory_samples.Count -lt 256){$record.memory_samples+=@($sample)}
 if($null -eq $record.minimum_free_memory_mb -or $freeMB -lt $record.minimum_free_memory_mb){$record.minimum_free_memory_mb=$freeMB}
 $record.peak_owned_working_set_mb=[Math]::Max([long]$record.peak_owned_working_set_mb,[long]$sample.owned_working_set_mb)
 $record.peak_owned_private_mb=[Math]::Max([long]$record.peak_owned_private_mb,[long]$sample.owned_private_mb)
 if($null -eq $record.minimum_free_commit_mb -or $commitMB -lt $record.minimum_free_commit_mb){$record.minimum_free_commit_mb=$commitMB}
 if($commitMB -lt $MinimumFreeCommitMB){throw ('Isolated capture stopped for low commit headroom: '+$commitMB+' MB available; guard '+$MinimumFreeCommitMB+' MB. Only owned test processes will close.')}
 if($freeMB -lt $MinimumFreeMemoryMB){throw ('Isolated capture stopped for low memory: '+$freeMB+' MB available; guard '+$MinimumFreeMemoryMB+' MB. Only owned test processes will close.')}
}
try {
 Write-Output ('Preparing isolated native Room menu roster: '+$root)
 # Finish every link/copy before launching any fingerprint worker. Shared
 # packfiles stay immutable; writable root files and campaign data are copied.
 if(!$ReuseVerifiedContent){foreach($entry in Get-ChildItem -LiteralPath (Join-Path $runtime 'pc-assets/packfiles') -Force){
  $target=Join-Path $packs $entry.Name
  if($entry.PSIsContainer){New-Item -ItemType Junction -Path $target -Target $entry.FullName | Out-Null}
  elseif(@('UIMainMenu.pak','UIStartGame.pak','UIhud.pak') -contains $entry.Name){Copy-Item -LiteralPath (Join-Path $workspace ('private/pc-assets/packfiles/'+$entry.Name)) -Destination $target}
  else{try{New-Item -ItemType HardLink -Path $target -Target $entry.FullName -ErrorAction Stop | Out-Null}catch{Copy-Item -LiteralPath $entry.FullName -Destination $target}}
 }}
 for($index=0;$index -lt $Players;++$index){
  $role=if($index -eq 0){'host'}elseif($index -eq 1){'client'}else{'client-'+($index+1)}
  $folder=Join-Path $root $role;$content=Join-Path $folder 'pc-assets';$user=Join-Path $folder 'user_data'
  New-Item -ItemType Directory -Path $content,$user | Out-Null
  if($ReuseVerifiedContent){
   # Runtime still validates every cache proof against actual NTFS file ID,
   # size and ChangeTime. Reusing immutable packs avoids changing link counts
   # (which would invalidate all proofs) on every isolated test launch.
   foreach($name in @('online-content-identity.bin','online-content-identity.bin.files-v1')){
    $cache=Join-Path $ReuseVerifiedContent ('host/user_data/'+$name)
    if(Test-Path -LiteralPath $cache){Copy-Item -LiteralPath $cache -Destination (Join-Path $user $name)}
   }
  }
  foreach($entry in Get-ChildItem -LiteralPath (Join-Path $runtime 'pc-assets') -Force){
   $target=Join-Path $content $entry.Name
   if($entry.PSIsContainer){New-Item -ItemType Junction -Path $target -Target $(if($entry.Name -eq 'packfiles'){$packs}else{$entry.FullName}) | Out-Null}
   else{Copy-Item -LiteralPath $entry.FullName -Destination $target}
  }
  if($index -eq 0){Copy-Item -LiteralPath (Join-Path $normal '42560816') -Destination $user -Recurse}
  else{New-Item -ItemType Directory -Path (Join-Path $user '42560816') | Out-Null}
  [IO.File]::WriteAllText((Join-Path $user '.em2-native-lobby-qa'),'EM2-native-menu-loopback-QA-v1',[Text.UTF8Encoding]::new($false))
  [string[]]$lines=if(Test-Path -LiteralPath (Join-Path $normal 'pc-settings.ini')){@(Get-Content -LiteralPath (Join-Path $normal 'pc-settings.ini'))}else{@()}
  foreach($key in @('resolution','window_mode','frame_limit','render_scale','hdr_output','material_ambient_occlusion','gameplay_timing')){
   $pattern='^'+[regex]::Escape($key)+'=.*$'
   if(@($lines | Where-Object {$_ -match $pattern}).Count){$lines=@($lines -replace $pattern,($key+'=0'))}else{$lines+=($key+'=0')}
  }
  [IO.File]::WriteAllLines((Join-Path $user 'pc-settings.ini'),$lines,[Text.UTF8Encoding]::new($false))
  $records.Add([ordered]@{role=$role;root=$folder;user=$user;content=$content;log=(Join-Path $folder 'game.log');exe_path=$exe;exe_sha256=$exeHash;normal_user_hashes=$baseline})
 }
 if(Get-CimInstance Win32_Process -Filter "Name='beetleworx.exe' OR Name='EM2Recomp.exe'"){throw 'Another game appeared during preparation; nothing was launched.'}
 if($ReenterAfterReturn){foreach($peer in $records){$peer.offline_campaign_before=OfflineCampaignHashes $peer}}
 foreach($peer in $records){
  SampleOwnedMemory
  $info=[Diagnostics.ProcessStartInfo]::new($exe);$info.UseShellExecute=$false;$info.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden;$info.WorkingDirectory=$runtime
  foreach($arg in @(('--em2_user_data_directory='+$peer.user),('--em2_content_directory='+$peer.content),('--log_file='+$peer.log),'--no-em2_online_menu','--no-em2_demo_build','--em2_private_profile=true','--em2_native_room_lobby=true','--em2_native_online_handoff=true',('--em2_native_lobby_qa_role='+$peer.role),('--em2_native_lobby_qa_players='+$Players),('--em2_native_lobby_qa_invitation_file='+(Join-Path $root 'native-menu-invitation.txt')),'--em2_online_auto_continue=true','--em2_auto_continue=false','--em2_online_world_random=true','--em2_online_single_view=true','--em2_trace_online_observations=true','--em2_trace_auto_continue=true','--em2_trace_online_clothing=true')){$info.ArgumentList.Add($arg)}
  if($MovieSequence){$info.ArgumentList.Add('--em2_native_lobby_qa_movie_sequence=true');$info.ArgumentList.Add('--em2_native_lobby_qa_movie_skip_delay='+$MovieSkipDelay)}
  if($AuthoredConversation){$info.ArgumentList.Add('--em2_native_lobby_qa_authored_conversation=true')}
  if($CutsceneCycle){$info.ArgumentList.Add('--em2_native_lobby_qa_cutscene_cycle=true')}
  if($CutsceneHeldReturn){$info.ArgumentList.Add('--em2_probe_cutscene_held_return=true')}
  if($UnfencedCutsceneReturn){$info.ArgumentList.Add('--em2_probe_unfenced_cutscene_return=true')}
  if($CutsceneSkipInput){$info.ArgumentList.Add('--em2_probe_cutscene_skip_input=true')}
  if($CutsceneBoundaryMovement){$info.ArgumentList.Add('--em2_probe_cutscene_boundary_move=true')}
  if($ExtraOswald){$info.ArgumentList.Add('--em2_native_lobby_qa_extra_oswald=true')}
  if($CooperativeShutdown){$info.ArgumentList.Add('--em2_cooperative_title_shutdown=true')}
  if($PipelineCreationThreads -ge 0){$info.ArgumentList.Add('--d3d12_pipeline_creation_threads='+$PipelineCreationThreads)}
  if($DamageExtraPeer){$info.ArgumentList.Add('--em2_probe_extra_damage_peer='+$DamageExtraPeer)}
  if($DamagePlayer){$info.ArgumentList.Add('--em2_native_lobby_qa_damage_player='+$DamagePlayer)}
  if($ChatProbe){$info.ArgumentList.Add('--em2_probe_online_chat=true')}
  if($VoiceProbe){$info.ArgumentList.Add('--em2_probe_online_voice=true')}
  if($ButtonTaskProbe){$info.ArgumentList.Add('--em2_probe_owned_button_tasks=true')}
  if($PolicyCycle){$info.ArgumentList.Add('--em2_probe_native_policy_cycle=true')}
  if($CheckpointReloadTick){$info.ArgumentList.Add('--em2_native_lobby_qa_restart_tick='+$CheckpointReloadTick);if(!$CutsceneCycle -and !$AuthoredConversation -and !$MovieSequence){$info.ArgumentList.Add('--em2_native_lobby_qa_checkpoint_reload=true')}}
  if($CheckpointReloadTick){$info.ArgumentList.Add('--em2_native_lobby_qa_restart_count='+$CheckpointReloadCount);$info.ArgumentList.Add('--em2_native_lobby_qa_restart_interval='+$CheckpointReloadInterval)}
  if($NativeLevelTarget){$info.ArgumentList.Add('--em2_native_lobby_qa_level_target='+$NativeLevelTarget)}
  if($NativeLevelTargets.Count){$info.ArgumentList.Add('--em2_native_lobby_qa_level_route='+($NativeLevelTargets -join '|'))}
  if($DebugCleanLevelLoad){$info.ArgumentList.Add('--em2_native_lobby_qa_clean_level_load=true')}
  if($NativeReload){$info.ArgumentList.Add('--em2_room_native_reload=true')}
  if($FaceTargetPoison){$info.ArgumentList.Add('--em2_probe_face_target_defaults=true')}
  if($UnfixedFaceTarget){$info.ArgumentList.Add('--em2_probe_face_target_unfixed=true')}
  if($NavigationTrace){foreach($arg in @('--em2_behavior_trace_start_tick=500','--em2_behavior_trace_dump_tick=618','--em2_behavior_motion_guid=000029320000394F00002AAB000002AF','--em2_behavior_trace_motion_input=true','--em2_behavior_trace_motion_trajectory=true','--em2_behavior_trace_navigation=true','--em2_behavior_trace_avoidance=true')){$info.ArgumentList.Add($arg)}}
  if($ReplayInvitationFile -and $peer.role -eq 'host'){$info.ArgumentList.Add('--em2_native_lobby_qa_replay_invitation_file='+$ReplayInvitationFile)}
  if($ScriptedInput){$info.ArgumentList.Add('--em2_room_probe_scripted_input=true')}
  if($CloseParticipant -or $PartialDeliveryParticipant){$info.ArgumentList.Add('--em2_probe_room_leave_boundary=true')}
  if($ReturnToTitle){$info.ArgumentList.Add('--em2_probe_room_return_title=true')}
  if($RecoveryPrompt){$info.ArgumentList.Add('--em2_probe_room_recovery_prompt=true')}
  if($RecoveryController){$info.ArgumentList.Add('--em2_probe_room_recovery_controller=true')}
  if($ReenterAfterReturn){foreach($arg in @('--em2_native_lobby_qa_reenter=true','--em2_room_probe_input_warmup_ticks=180','--em2_probe_room_hud_properties=true')){$info.ArgumentList.Add($arg)};if($peer.role -eq 'host'){$info.ArgumentList.Add('--em2_probe_room_disconnect_tick=800')}}
  if($PartialDeliveryParticipant -and $peer.role -eq $records[$PartialDeliveryParticipant-1].role){$info.ArgumentList.Add('--em2_probe_room_partial_delivery_tick='+$PartialDeliveryTick)}
  $process=[Diagnostics.Process]::Start($info);$peer.pid=$process.Id;$peer.started_utc=$process.StartTime.ToUniversalTime().ToString('o')
  $owned.Add(@{record=$peer;process=$process});$peer | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $peer.root 'process.json')
  Write-Output ('Owned native menu '+$peer.role+' PID '+$process.Id)
 }
 $record.processes=@($records.ToArray());$record | ConvertTo-Json -Depth 9 | Set-Content -LiteralPath (Join-Path $root 'roster.json')
 $deadline=[DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds);$observeUntil=$null;$failureHold=$null
 while([DateTime]::UtcNow -lt $deadline){
  if($ReenterAfterReturn -and !$record.Contains('initial_invitation')){
   $invitationPath=Join-Path $root 'native-menu-invitation.txt'
   if(Test-Path -LiteralPath $invitationPath){
    if($record.Contains('failure_injection_started_utc')){throw 'Initial invitation was not captured before the planned disconnect.'}
    $initial=[IO.File]::ReadAllText($invitationPath).Trim()
    if($initial.Length -in @(62,64) -and $initial -cmatch '\AEM2-[A-Z2-7]+\z'){
     $proof=Join-Path $root 'initial-menu-invitation.txt'
     [IO.File]::WriteAllText($proof,$initial,[Text.UTF8Encoding]::new($false))
     $record.initial_invitation=$proof
     $record.initial_invitation_captured_utc=[DateTime]::UtcNow.ToString('o')
    }
   }
  }
  if($PartialDeliveryParticipant -and !$record.Contains('failure_injection_started_utc')){
   $targetLog=$records[$PartialDeliveryParticipant-1].log
   if((Test-Path -LiteralPath $targetLog) -and ((Get-Content -LiteralPath $targetLog -Raw) ?? '').Contains('Private native Room partial delivery injected:')){
    $record.failure_injection_started_utc=[DateTime]::UtcNow.ToString('o')
   }
  }
  if([DateTime]::UtcNow -ge $nextMemorySample){SampleOwnedMemory;$nextMemorySample=[DateTime]::UtcNow.AddSeconds(5)}
  if($ReenterAfterReturn){[void](ObserveControlledDisconnect)}
  if($record.Contains('failure_injection_started_utc')){
   $failed=0
   for($index=0;$index -lt $owned.Count;++$index){
    if($CloseParticipant -and $index -eq $CloseParticipant-1){continue}
    $entry=$owned[$index]
    if(!(OwnedProcess $entry)){throw 'A surviving participant exited during the negative-path hold.'}
    $text=(Get-Content -LiteralPath $entry.record.log -Raw) ?? ''
    if($ReturnToTitle -and ($text.Contains('Native Room title return failed:') -or $text.Contains('Native online title restore failed:') -or $text.Contains('Native Room title return rejected:'))){throw ($entry.record.role+' rejected native title return; capture is not accepted.')}
    $returned=$text.Contains('Native Room title return complete: original world unloaded, controls retired, original storage refreshed')
    $reentered=!$ReenterAfterReturn -or ($text.Contains('Native online title restore: fresh lobby accepted after original offline save refresh') -and [regex]::Matches($text,'Native Room gameplay first frame consumed:').Count -ge 2 -and [regex]::Matches($text,"phase 'complete' reads").Count -ge 2)
    if($ReenterAfterReturn -and $reentered){
     $start=$text.IndexOf('Native online title restore: fresh lobby accepted after original offline save refresh')
     $second=$text.Substring($start)
     if($second.Contains('Retained native Room stopped:')){throw ($entry.record.role+' second online session stopped; re-entry is not accepted.')}
     # The chat probe adds movement at elapsed 480..491, after the older
     # elapsed-360 completion marker. Observe its actual neutral release plus
     # sixty completed simulation ticks, not a wall-clock hold during movement.
     $origin=[regex]::Match($second,"Private Room SDK input: peer \d+ tick (\d+) elapsed 0 phase '")
     $observations=[regex]::Matches($second,'Native gameplay observation v\d+:[^\r\n]*?tick (\d+),')
     $lastEvent=if($ChatProbe){492}else{360}
     $released=[regex]::IsMatch($second,("Private Room SDK input: peer \d+ tick \d+ elapsed "+$lastEvent+" phase '"))
     $reentered=$origin.Success -and $released -and $observations.Count -gt 0 -and
       [uint64]$observations[$observations.Count-1].Groups[1].Value -ge ([uint64]$origin.Groups[1].Value+$lastEvent+2+60)
    }
    if($text.Contains('Native Room failure context: Beetleworx online failure context v1') -and $text.Contains('Retained native Room stopped:') -and (!$ReturnToTitle -or $returned) -and $reentered){++$failed}
   }
   if($failed -eq $Players-$(if($CloseParticipant){1}else{0})){
    if($null -eq $failureHold){$failureHold=[DateTime]::UtcNow.AddSeconds(3)}
    if([DateTime]::UtcNow -ge $failureHold){$record.failure_observed=$true;break}
   }
   if([DateTime]::UtcNow -gt ([datetime]$record.failure_injection_started_utc).ToUniversalTime().AddSeconds($(if($ReenterAfterReturn){180}elseif($ReturnToTitle){70}else{20}))){throw 'Surviving participants did not retain failure reports before the fixed deadline.'}
   Start-Sleep -Milliseconds 250;continue
  }
  $ready=0;$programs=0;$reentries=0
  foreach($entry in $owned){
   if(!(OwnedProcess $entry) -or $entry.process.HasExited){throw 'A native menu test process exited before observation completed.'}
   $text=if(Test-Path -LiteralPath $entry.record.log){(Get-Content -LiteralPath $entry.record.log -Raw) ?? ''}else{''}
   if(!$PartialDeliveryParticipant){foreach($failure in @('Native online lobby handoff failed:','Native online menu preparation failed:','Native lobby QA rejected:','Retained native Room stopped:','Native Room gameplay stopped:','Native room initialization stopped:')){if($text.Contains($failure)){if($ReenterAfterReturn -and (ObserveControlledDisconnect 4)){continue};throw ($entry.record.role+' reported '+$failure)}}}
   $expectedWorlds=1+$(if($DamagePlayer -eq 4){1}elseif($CheckpointReloadTick -and !$CutsceneCycle -and !$AuthoredConversation -and !$MovieSequence -and !$DamagePlayer -and !$DamageExtraPeer){$CheckpointReloadCount}else{0})
   $expectedPrograms=if($DamagePlayer -eq 4){1}else{$expectedWorlds}
   if([regex]::Matches($text,"phase 'complete' reads").Count -ge $expectedPrograms){++$programs}
   if([regex]::Matches($text,'Native Room gameplay first frame consumed:').Count -ge $expectedWorlds){++$reentries}
   if($text.Contains('Native lobby completed authored save reload and accepted native Continue') -and $text.Contains('Native Room gameplay first frame consumed:')){++$ready}
  }
  if($ready -eq $Players -and $null -eq $observeUntil){$record.startup_complete=$true;$observeUntil=[DateTime]::UtcNow.AddSeconds($ObserveSeconds);Write-Output ('All '+$Players+' native-menu worlds entered gameplay; observing '+$ObserveSeconds+' seconds.')}
  if($CloseParticipant -and $programs -eq $Players){
   $entry=$owned[$CloseParticipant-1];if(!(OwnedProcess $entry)){throw 'The requested participant is no longer owned.'}
   if($ReturnToTitle){foreach($peer in $records){$peer.offline_campaign_before=OfflineCampaignHashes $peer}}
   $messages=[BWNativeMenuWindows]::Close([uint32]$entry.record.pid)
   if($messages -lt 1){throw 'No owned game window accepted the negative-test close request.'}
   $record.participant_close_requested_utc=[DateTime]::UtcNow.ToString('o')
   $record.failure_injection_started_utc=$record.participant_close_requested_utc
   $record.participant_close_pid=$entry.record.pid
   Write-Output ('Closing owned participant '+$CloseParticipant+' after every native input program completed; waiting for failure context on survivors.')
   continue
  }
  if(!$ReenterAfterReturn -and $null -ne $observeUntil -and [DateTime]::UtcNow -ge $observeUntil){
   if($reentries -ne $Players){throw 'A scheduled world did not reach fresh native gameplay on every peer; capture is not accepted.'}
   if($ScriptedInput -and $programs -ne $Players){throw 'The SDK input program did not complete in every scheduled world on every peer; capture is not accepted.'}
   $record.observation_complete=$true;break
  }
  Start-Sleep -Milliseconds 500
 }
 if(!$record.observation_complete -and !$record.failure_observed){throw 'Native menu roster observation timed out; no restart was attempted.'}
}catch{$record.error=$_.Exception.Message;$record.error_detail=$_.Exception.ToString();$record.error_stack=$_.ScriptStackTrace;Write-Output $record.error}
finally{
 # Request all closes before waiting, avoiding one finisher disconnecting
 # peers which are still completing their shared observation interval.
 $record.close_started_utc=[DateTime]::UtcNow.ToString('o')
 $record.close_started_local=[DateTime]::Now.ToString('yyyy-MM-dd HH:mm:ss.fff')
 foreach($entry in $owned){$p=OwnedProcess $entry;if($p){$entry.record.close_messages=[BWNativeMenuWindows]::Close([uint32]$p.Id)}}
 foreach($entry in $owned){
  $p=$entry.process;if(!$p.WaitForExit(10000)){
   $verified=OwnedProcess $entry
   if($verified -and $CaptureShutdownHang){
    $closePosted=$entry.record.close_messages -gt 0
    $entry.record.slow_shutdown=$closePosted;$entry.record.stalled_without_close_request=!$closePosted
    $dumpTool=Join-Path $workspace 'tools/procdump/procdump64.exe'
    if(Test-Path -LiteralPath $dumpTool){
     $dumpName=if($closePosted){'shutdown-hang.dmp'}else{'process-stall.dmp'}
     $dump=Join-Path $entry.record.root $dumpName
     $dumpInfo=[Diagnostics.ProcessStartInfo]::new($dumpTool);$dumpInfo.UseShellExecute=$false;$dumpInfo.CreateNoWindow=$true
     $dumpInfo.RedirectStandardOutput=$true;$dumpInfo.RedirectStandardError=$true
     foreach($arg in @('-accepteula','-mm',[string]$p.Id,$dump)){$dumpInfo.ArgumentList.Add($arg)}
     $dumpProcess=[Diagnostics.Process]::Start($dumpInfo);$dumpOutput=$dumpProcess.StandardOutput.ReadToEndAsync();$dumpError=$dumpProcess.StandardError.ReadToEndAsync()
     if(!$dumpProcess.WaitForExit(15000)){$dumpProcess.Kill();[void]$dumpProcess.WaitForExit(5000)}
     if($dumpProcess.HasExited){
      $entry.record.shutdown_dump_exit=$dumpProcess.ExitCode
      [IO.File]::WriteAllText((Join-Path $entry.record.root 'shutdown-dump-tool.log'),$dumpOutput.GetAwaiter().GetResult()+$dumpError.GetAwaiter().GetResult())
      if(Test-Path -LiteralPath $dump){$entry.record.process_dump=$dump;if($closePosted){$entry.record.shutdown_dump=$dump}}
     }
    }
   }
   # Revalidate after the read-only dump: a slow normal exit must not be killed.
   $verified=OwnedProcess $entry
   if($verified){$verified.Kill();[void]$verified.WaitForExit(10000);$entry.record.forced_close=$true}
  }
  $entry.record.process_exited=$p.HasExited;if($p.HasExited){$entry.record.exit_code=$p.ExitCode}
 }
 $unchanged=@(Get-ChildItem -LiteralPath $normal -Recurse -File).Count -eq $baseline.Count
 foreach($file in $baseline.Keys){if(!(Test-Path -LiteralPath $file) -or (Get-FileHash -LiteralPath $file).Hash -ne $baseline[$file]){$unchanged=$false}}
 foreach($peer in $records){if(Test-Path -LiteralPath $peer.log){$text=(Get-Content -LiteralPath $peer.log -Raw) ?? '';$peer.scripted_sdk_input_observed=$text.Contains('Private Room SDK input:');$peer.scripted_sdk_program_completed=$text.Contains("phase 'complete' reads");if($peer.scripted_sdk_input_observed){$record.gameplay_input_sent=$true}}}
 if($ReturnToTitle -and $record.Contains('failure_injection_started_utc')){foreach($peer in $records){
  $after=OfflineCampaignHashes $peer;$before=$peer.offline_campaign_before
  $peer.offline_campaign_unchanged=$null -ne $before -and $after.Count -eq $before.Count
  if($null -ne $before){foreach($key in $before.Keys){if($after[$key] -ne $before[$key]){$peer.offline_campaign_unchanged=$false}}}
  if(!$peer.offline_campaign_unchanged -and !$record.Contains('error')){$record.error='An isolated original offline campaign changed during title return.'}
 }}
 $record.normal_user_files_unchanged=$unchanged
 $record.normal_config_unchanged=(Get-FileHash -LiteralPath $normalConfig).Hash -eq $configHash
 $afterCrash=if(Test-Path -LiteralPath $normalCrash){(Get-FileHash -LiteralPath $normalCrash).Hash}else{''};$record.normal_crash_unchanged=$afterCrash -eq $crashHash
 $record.processes=@($records.ToArray());$record | ConvertTo-Json -Depth 9 | Set-Content -LiteralPath (Join-Path $root 'result.json')
 Write-Output ('Native menu roster capture: '+$root)
}
if($record.Contains('error') -or !$record.normal_user_files_unchanged -or !$record.normal_config_unchanged -or !$record.normal_crash_unchanged -or @($records | Where-Object {!$_.process_exited -or $_.exit_code -ne 0 -or $_.forced_close -or $_.identity_rejected}).Count){exit 1}
