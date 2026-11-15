param([Parameter(Mandatory)][string]$PairRoot,[switch]$AcceptPrompt)
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$root=(Resolve-Path -LiteralPath $PairRoot).Path
if((Split-Path $root -Parent) -ine (Join-Path $workspace 'analysis') -or !(Split-Path $root -Leaf).StartsWith('private-native-room-pair-')){throw 'Expected an isolated native Room pair'}
$pair=Get-Content (Join-Path $root 'pair.json') -Raw | ConvertFrom-Json
$exe=(Resolve-Path (Join-Path $workspace 'build/runtime23/beetleworx.exe')).Path
if(-not ('BeetleworxCorrectionInput' -as [type])){Add-Type -TypeDefinition @"
using System;using System.Text;using System.Collections.Generic;using System.Runtime.InteropServices;
public static class BeetleworxCorrectionInput {
 public delegate bool Callback(IntPtr w,IntPtr p);
 [DllImport("user32.dll")]public static extern bool EnumWindows(Callback callback,IntPtr p);
 [DllImport("user32.dll")]public static extern uint GetWindowThreadProcessId(IntPtr w,out uint pid);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern int GetClassName(IntPtr w,StringBuilder b,int count);
 [DllImport("user32.dll")]public static extern bool PostMessage(IntPtr w,uint m,IntPtr a,IntPtr b);
 [DllImport("user32.dll")]public static extern uint MapVirtualKey(uint code,uint kind);
 public static IntPtr[] Find(uint pid){var list=new List<IntPtr>();EnumWindows((w,p)=>{uint owner;GetWindowThreadProcessId(w,out owner);if(owner==pid){var b=new StringBuilder(256);GetClassName(w,b,256);if(b.ToString()=="RexWindowClass")list.Add(w);}return true;},IntPtr.Zero);return list.ToArray();}
}
"@}
function OwnedWindow([string]$role){
 $capture=$pair.$role
 $process=Get-CimInstance Win32_Process -Filter ('ProcessId='+$capture.pid)
 if(!$process -or $process.ExecutablePath -ine $exe -or [Math]::Abs(($process.CreationDate.ToUniversalTime()-([datetime]$capture.started_utc).ToUniversalTime()).TotalSeconds) -gt 2 -or !$process.CommandLine.Contains('--em2_room_probe_host_correction=true') -or !$process.CommandLine.Contains($capture.root.Replace('\','/'))){throw 'Owned correction process identity differs'}
 if((Get-FileHash $exe -Algorithm SHA256).Hash -ine $capture.exe_sha256){throw 'Owned executable hash differs'}
 $windows=@([BeetleworxCorrectionInput]::Find($capture.pid));if($windows.Count -ne 1){throw 'Expected one owned game window'}
 return $windows[0]
}
function Observation([string]$role){
 $line=Get-Content (Join-Path $pair.$role.root 'game.log') -Tail 100 | Where-Object {$_ -match 'Native gameplay observation v5:.*tick (\d+),.*bits ([0-9A-F/]+), life'} | Select-Object -Last 1
 if(!$line -or $line -notmatch 'tick (\d+),.*bits ([0-9A-F/]+), life'){throw 'No completed native observation'}
 return @{tick=[uint64]$Matches[1];positions=$Matches[2]}
}
$results=@()
foreach($role in @('guest','host')){
 $window=OwnedWindow $role
 $before=Observation $role
 foreach($key in $(if($AcceptPrompt){@(0x0d,0x57,0x20)}else{@(0x57,0x20)})){
  $scan=[BeetleworxCorrectionInput]::MapVirtualKey($key,0)
  $down=[IntPtr](1 -bor ($scan -shl 16));$up=[IntPtr]([int64]0xC0000001 -bor ([int64]$scan -shl 16))
  try{
   if(![BeetleworxCorrectionInput]::PostMessage($window,0x100,[IntPtr]$key,$down)){throw 'Input down failed'}
   Start-Sleep -Milliseconds $(if($key -eq 0x57){900}else{200})
  }finally{[void][BeetleworxCorrectionInput]::PostMessage($window,0x101,[IntPtr]$key,$up)}
  Start-Sleep -Milliseconds 300
 }
 Start-Sleep -Milliseconds 700
 $after=Observation $role
 $results+=@{role=$role;before=$before;after=$after;input=$(if($AcceptPrompt){'Enter, W then Space through existing window key listener'}else{'W then Space through existing window key listener'});movement_observed=($before.positions -ne $after.positions)}
}
$results | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $root 'correction-input.json')
$pair.external_movement_or_camera_input_sent=$true
$pair | Add-Member -Force -NotePropertyName automated_input_manifest -NotePropertyValue 'correction-input.json'
$pair | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $root 'pair.json')
$results | ConvertTo-Json -Depth 5
