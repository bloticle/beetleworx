param([switch]$TraceInitialization,[switch]$Gameplay)
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if(Get-CimInstance Win32_Process -Filter "Name='beetleworx.exe' OR Name='EM2Recomp.exe'"){throw 'Close existing game sessions before starting an isolated Room pair.'}
$pair=Join-Path $workspace ('analysis/private-native-room-pair-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $pair | Out-Null
$invitation=Join-Path $pair 'invitation.txt'
$launcher=Join-Path $PSScriptRoot 'start-private-gpu-profile.ps1'
$metadata=Join-Path $workspace 'analysis/current-private-gpu-profile.json'
$hostRecord=$null
try{
    & $launcher -Scale 1 -NoGpuProfile -OriginalLighting -GameplayTiming 0 -FrameCap 30 -OnlineRoomRole host -OnlineRoomInvitationFile $invitation -TraceRoomInitialization:$TraceInitialization -OnlineRoomGameplay:$Gameplay
    $hostRecord=Get-Content -LiteralPath $metadata -Raw | ConvertFrom-Json
    if($hostRecord.online_room_role -ne 'host'){throw 'Host launch did not produce its expected private metadata.'}
    $hostRecord | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $pair 'host-process.json')
    & $launcher -Scale 1 -NoGpuProfile -OriginalLighting -GameplayTiming 0 -FrameCap 30 -OnlineRoomRole guest -OnlineRoomInvitationFile $invitation -AllowConcurrentRoomProbe -TraceRoomInitialization:$TraceInitialization -OnlineRoomGameplay:$Gameplay
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
