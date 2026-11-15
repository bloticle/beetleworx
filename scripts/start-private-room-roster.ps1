param([ValidateRange(3,16)][int]$Players=3,[switch]$ScriptedInput,[switch]$MixedCharacters,[switch]$TraceClothing,[string]$ClothingDirectory="",[switch]$BindClothing,[switch]$ClothingLifecycle,[switch]$NativeButtons,[switch]$TraceRandomOwners,[uint32]$RandomTraceStartTick=0,[string]$ReplayInvitationFile="",[switch]$OriginalEventOrder,[ValidateSet(30,60,120)][int]$HostFrameCap=30,[ValidateSet(30,60,120)][int]$GuestFrameCap=30,[switch]$HostAuthority,[switch]$TraceEntities,[ValidateCount(0,4)][ValidatePattern("^[a-z0-9_.]{1,63}$")][string[]]$LevelTargets=@(),[ValidateRange(60,1000000)][uint32]$LevelTick=700,[ValidateRange(600,1000000)][uint32]$LevelInterval=700,[ValidateSet("clean","transition")][string]$LevelMethod="transition")
$ErrorActionPreference='Stop'
if($ClothingLifecycle -and (!$ScriptedInput -or !$BindClothing)){throw 'ClothingLifecycle requires ScriptedInput and BindClothing.'}
$workspace=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if(Get-CimInstance Win32_Process -Filter "Name='beetleworx.exe' OR Name='EM2Recomp.exe'"){throw 'Close existing game sessions before starting an isolated Room roster.'}
$root=Join-Path $workspace ('analysis/private-native-room-roster-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $root | Out-Null
$invitation=Join-Path $root 'invitation.txt'
$records=[Collections.Generic.List[object]]::new()
# One coherent private authority preset, matching the two-peer acceptance path.
# This never changes normal game settings or enables probes in public lobbies.
$authorityOptions=@{}
if($HostAuthority){
 foreach($name in @('OnlineHostCorrection','OnlineRoomPlayerDamageEvents','OnlineRoomWorldDamageEvents','OnlineAiTargetAuthority','OnlineSpawnBatches','OnlineSpawnEligibility','OnlineSpawnReplay','OnlineEntityRetirements','OnlineProjectileImpacts','OnlineShooterLaunches','TraceRoomAiDecisions')){$authorityOptions[$name]=$true}
 $authorityOptions['OnlineRoomVerificationWindow']=8
}
$authorityOptions['TraceRoomEntities']=[bool]$TraceEntities
$authorityOptions['OnlineRoomLevelTargets']=@($LevelTargets)
$authorityOptions['OnlineRoomLevelTick']=$LevelTick
$authorityOptions['OnlineRoomLevelInterval']=$LevelInterval
$authorityOptions['OnlineRoomLevelMethod']=$LevelMethod
if($ClothingDirectory){$ClothingDirectory=(Resolve-Path -LiteralPath $ClothingDirectory).Path;$TraceClothing=$true}
try {
 for($index=0;$index -lt $Players;$index++){
  $role=if($index -eq 0){'host'}else{'guest'}
  $frameCap=if($index -eq 0){$HostFrameCap}else{$GuestFrameCap}
  $replay=if($index -eq 0){$ReplayInvitationFile}else{''}
  & (Join-Path $PSScriptRoot 'start-private-gpu-profile.ps1') -Scale 1 -NoGpuProfile -OriginalLighting -GameplayTiming 0 -FrameCap $frameCap -OnlineRoomRole $role -OnlineRoomInvitationFile $invitation -OnlineRoomExpectedPlayers $Players -AllowConcurrentRoomProbe:($index -gt 0) -OnlineRoomGameplay -OnlineRoomScriptedInput:$ScriptedInput -OnlineRoomMixedCharacters:$MixedCharacters -TraceOnlineClothing:$TraceClothing -OnlineClothingDirectory $ClothingDirectory -BindOnlineClothing:$BindClothing -OnlineClothingLifecycle:$ClothingLifecycle -OnlineNativeButtons:$NativeButtons -OriginalEventOrder:$OriginalEventOrder -TraceRoomRandomOwners:$TraceRandomOwners -OnlineRandomTraceStartTick $RandomTraceStartTick -OnlineRoomReplayInvitation $replay -OnlineRoomBehaviorTraceStart 1000000 @authorityOptions
  $record=Get-Content -LiteralPath (Join-Path $workspace 'analysis/current-private-gpu-profile.json') -Raw | ConvertFrom-Json
  if($record.online_room_role -ne $role){throw 'Roster launch did not produce its expected private metadata.'}
  $records.Add($record)
  $record | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $root ('process-'+$index+'.json'))
 }
 [ordered]@{root=$root;invitation=$invitation;players=$Players;processes=@($records.ToArray());external_gameplay_input_sent=$false;scripted_sdk_input=[bool]$ScriptedInput;host_authority=[bool]$HostAuthority;level_targets=@($LevelTargets);level_tick=$LevelTick;level_interval=$LevelInterval;level_method=$LevelMethod;mixed_characters=[bool]$MixedCharacters;scope='Real native worlds with extra-human admission and authoritative Room frames; optional synthetic SDK input is interpreted and transported normally. Independent extra cameras and full action coverage remain separate.'} |
  ConvertTo-Json -Depth 9 | Set-Content -LiteralPath (Join-Path $root 'roster.json')
 Write-Output ('Private native Room roster: '+$root)
}catch{
 $canonical=(Resolve-Path -LiteralPath (Join-Path $workspace 'build/runtime23/beetleworx.exe')).Path
 foreach($record in $records){
  $owned=Get-CimInstance Win32_Process -Filter ('ProcessId='+$record.pid)
  if($owned -and $owned.ExecutablePath -ieq $canonical -and $owned.CommandLine.Replace('/','\').Contains([string]$record.root) -and
    [Math]::Abs(($owned.CreationDate.ToUniversalTime()-([datetime]$record.started_utc).ToUniversalTime()).TotalSeconds) -lt 2){Stop-Process -Id $record.pid -Force}
 }
 throw
}
