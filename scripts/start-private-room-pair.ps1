param([string]$ClothingDirectory="",[switch]$TraceInitialization,[switch]$TraceRandomOwners,[switch]$TraceLuaOwners,[switch]$TraceEntities,[switch]$TriggerMathProbe,[switch]$NativeButtons,[switch]$HostCorrection,[switch]$DamageEvents,[switch]$WorldDamageEvents,[switch]$HostCorrectionDrift,[ValidatePattern("^([a-z0-9_.]{1,63})?$")][string]$LevelTarget="",[ValidateRange(60,1000000)][uint32]$LevelTick=600,[ValidateSet("clean","transition")][string]$LevelMethod="clean",[ValidateRange(0,3)][uint32]$DamagePlayer=0,[ValidateRange(1,1000)][uint32]$DamageAmount=1,[switch]$HostOnlyDamage,[switch]$GuestOnlyDamage,[ValidateRange(0,1000000)][uint32]$GuestDamageTick=0,[switch]$ScriptedInput,[switch]$Gameplay,[ValidateSet(30,60,120)][int]$HostFrameCap=30,[ValidateSet(30,60,120)][int]$GuestFrameCap=30,[string]$ReplayInvitationFile="",[string]$ReplayCheckpointDirectory="",[uint32]$BehaviorTraceStartTick=0,[uint32]$BehaviorDumpTick=4294967295,[ValidateRange(1,8)][int]$VerificationWindow=1,[ValidatePattern("^([0-9A-Fa-f]{32})?$")][string]$MotionTraceGuid="")
$ErrorActionPreference='Stop'
if($HostOnlyDamage -and $GuestOnlyDamage){throw 'Select only one damage fixture origin.'}
if($GuestDamageTick -and ($GuestDamageTick -lt 60 -or !$DamagePlayer -or $HostOnlyDamage -or $LevelTarget)){throw 'GuestDamageTick requires a guest damage fixture at tick 60 or later, without a level target.'}
$workspace=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if(Get-CimInstance Win32_Process -Filter "Name='beetleworx.exe' OR Name='EM2Recomp.exe'"){throw 'Close existing game sessions before starting an isolated Room pair.'}
$pair=Join-Path $workspace ('analysis/private-native-room-pair-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $pair | Out-Null
$invitation=Join-Path $pair 'invitation.txt'
$launcher=Join-Path $PSScriptRoot 'start-private-gpu-profile.ps1'
$metadata=Join-Path $workspace 'analysis/current-private-gpu-profile.json'
$hostRecord=$null
if($ClothingDirectory){$ClothingDirectory=(Resolve-Path -LiteralPath $ClothingDirectory).Path}
$traceClothing=[bool]$ClothingDirectory
try{
    & $launcher -Scale 1 -NoGpuProfile -OriginalLighting -GameplayTiming 0 -FrameCap $HostFrameCap -OnlineRoomRole host -TraceOnlineClothing:$traceClothing -OnlineClothingDirectory $ClothingDirectory -OnlineRoomInvitationFile $invitation -OnlineRoomReplayInvitation $ReplayInvitationFile -ReplayCheckpointDirectory $ReplayCheckpointDirectory -TraceRoomInitialization:$TraceInitialization -TraceRoomRandomOwners:$TraceRandomOwners -TraceRoomLuaOwners:$TraceLuaOwners -TraceRoomEntities:$TraceEntities -OnlineRoomGameplay:$Gameplay -OnlineTriggerMathProbe:$TriggerMathProbe -OnlineNativeButtons:$NativeButtons -OnlineHostCorrection:$HostCorrection -OnlineRoomPlayerDamageEvents:$DamageEvents -OnlineRoomWorldDamageEvents:$WorldDamageEvents -OnlineHostCorrectionDrift:$HostCorrectionDrift -OnlineRoomLevelTarget $LevelTarget -OnlineRoomLevelTick $LevelTick -OnlineRoomLevelMethod $LevelMethod -OnlineRoomDamagePlayer $(if($GuestOnlyDamage){0}else{$DamagePlayer}) -OnlineRoomDamageAmount $DamageAmount -OnlineRoomScriptedInput:$ScriptedInput -OnlineRoomBehaviorTraceStart $BehaviorTraceStartTick -OnlineRoomBehaviorDumpTick $BehaviorDumpTick -OnlineRoomVerificationWindow $VerificationWindow -OnlineRoomMotionTraceGuid $MotionTraceGuid
    $hostRecord=Get-Content -LiteralPath $metadata -Raw | ConvertFrom-Json
    if($hostRecord.online_room_role -ne 'host'){throw 'Host launch did not produce its expected private metadata.'}
    $hostRecord | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $pair 'host-process.json')
    & $launcher -Scale 1 -NoGpuProfile -OriginalLighting -GameplayTiming 0 -FrameCap $GuestFrameCap -OnlineRoomRole guest -TraceOnlineClothing:$traceClothing -OnlineClothingDirectory $ClothingDirectory -OnlineRoomInvitationFile $invitation -AllowConcurrentRoomProbe -ReplayCheckpointDirectory $ReplayCheckpointDirectory -TraceRoomInitialization:$TraceInitialization -TraceRoomRandomOwners:$TraceRandomOwners -TraceRoomLuaOwners:$TraceLuaOwners -TraceRoomEntities:$TraceEntities -OnlineRoomGameplay:$Gameplay -OnlineTriggerMathProbe:$TriggerMathProbe -OnlineNativeButtons:$NativeButtons -OnlineHostCorrection:$HostCorrection -OnlineRoomPlayerDamageEvents:$DamageEvents -OnlineRoomWorldDamageEvents:$WorldDamageEvents -OnlineHostCorrectionDrift:$HostCorrectionDrift -OnlineRoomLevelTarget $LevelTarget -OnlineRoomLevelTick $(if($GuestDamageTick){$GuestDamageTick}else{$LevelTick}) -OnlineRoomLevelMethod $LevelMethod -OnlineRoomDamagePlayer $(if($HostOnlyDamage){0}else{$DamagePlayer}) -OnlineRoomDamageAmount $DamageAmount -OnlineRoomScriptedInput:$ScriptedInput -OnlineRoomBehaviorTraceStart $BehaviorTraceStartTick -OnlineRoomBehaviorDumpTick $BehaviorDumpTick -OnlineRoomVerificationWindow $VerificationWindow -OnlineRoomMotionTraceGuid $MotionTraceGuid
    $guestRecord=Get-Content -LiteralPath $metadata -Raw | ConvertFrom-Json
    if($guestRecord.online_room_role -ne 'guest'){throw 'Guest launch did not produce its expected private metadata.'}
    $guestRecord | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $pair 'guest-process.json')
    [ordered]@{root=$pair;invitation=$invitation;host=$hostRecord;guest=$guestRecord;external_movement_or_camera_input_sent=$false;native_coop_initialization="canonical engine bootstrap";screenshots=$false} |
        ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $pair 'pair.json')
    Write-Output ('Private native Room pair: '+$pair)
}catch{
    if($hostRecord){
        $owned=Get-CimInstance Win32_Process -Filter ('ProcessId='+$hostRecord.pid)
        $expected=(Resolve-Path -LiteralPath (Join-Path $workspace 'build/runtime23/beetleworx.exe')).Path
        if($owned -and $owned.ExecutablePath -ieq $expected -and $owned.CommandLine.Contains($invitation.Replace('\','/')) -and
            [Math]::Abs(($owned.CreationDate.ToUniversalTime()-$hostRecord.started_utc.ToUniversalTime()).TotalSeconds) -lt 2){Stop-Process -Id $hostRecord.pid -Force}
    }
    throw
}
