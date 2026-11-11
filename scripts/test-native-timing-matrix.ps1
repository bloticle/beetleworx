param(
 [ValidateSet(40,60,120)][int[]]$Caps=@(40,60,120),
 [ValidateRange(15,60)][int]$ObserveSeconds=25,
 [switch]$LightweightRenderer
)
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$exe=[IO.Path]::GetFullPath((Join-Path $workspace 'build/runtime23/beetleworx.exe'))
if(Get-CimInstance Win32_Process -Filter "Name='beetleworx.exe' OR Name='EM2Recomp.exe'"){throw 'A game is running; timing matrix did not start.'}
$root=Join-Path $workspace ('analysis/native-timing-matrix-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $root | Out-Null
$rows=@()
function OwnedProcess($capture){
 $p=Get-CimInstance Win32_Process -Filter ("ProcessId="+$capture.pid)
 if(!$p){return $null}
 if($p.ExecutablePath -ne $exe -or !$p.CommandLine.Replace('/','\').Contains($capture.root) -or
    [Math]::Abs(($p.CreationDate.ToUniversalTime()-([datetime]$capture.started_utc).ToUniversalTime()).TotalMilliseconds) -gt 1){
  throw 'Private timing process identity changed.'
 }
 return Get-Process -Id $capture.pid
}
foreach($cap in $Caps){foreach($variable in @($false,$true)){
 $capture=$null
 $row=[ordered]@{cap=$cap;variable_physics=$variable;lightweight_renderer=[bool]$LightweightRenderer;gameplay_input_sent=$false;screenshots_taken=$false;observation_complete=$false}
 try {
  $options=@{Scale=1;NoGpuProfile=$true;TraceTiming=$true;GameplayTiming=1;EnhancedLighting=$true;FrameCap=$cap;NativePhysicsVariable=$variable}
  if($LightweightRenderer){
   # Private timing diagnostic only: reduce GPU work to try reaching the cap.
   # This is not a same-quality performance comparison or a normal-setting change.
   $options.EnhancedLighting=$false;$options.OriginalLighting=$true;$options.RenderPath='rov'
  }
  & (Join-Path $PSScriptRoot 'start-private-gpu-profile.ps1') @options
  $capture=Get-Content (Join-Path $workspace 'analysis/current-private-gpu-profile.json') -Raw | ConvertFrom-Json
  $row.capture_root=$capture.root;$row.exe_sha256=$capture.exe_sha256;$row.pid=$capture.pid
  $log=Join-Path $capture.root 'game.log'
  $deadline=[DateTime]::UtcNow.AddSeconds(120)
  $ready=$null
  while([DateTime]::UtcNow -lt $deadline){
   if(!(OwnedProcess $capture)){throw 'Private game exited before the level loaded.'}
   if(Test-Path -LiteralPath $log){
    [string]$text=Get-Content -LiteralPath $log -Raw
    if([string]::IsNullOrEmpty($text)){Start-Sleep -Milliseconds 500;continue}
    $loads=[regex]::Matches($text,"Automatic startup native level '([^']+)' load result true")
    foreach($load in $loads){if($load.Groups[1].Value -notmatch '^(mainmenu|frontend)\.'){$ready=$load;break}}
    if($ready){break}
   }
   Start-Sleep -Milliseconds 500
  }
  if(!$ready){throw 'Native Continue did not load a gameplay level within 120 seconds.'}
  $row.level=$ready.Groups[1].Value
  $row.observation_started_utc=[DateTime]::UtcNow.ToString('o')
  $deadline=[DateTime]::UtcNow.AddSeconds($ObserveSeconds)
  while([DateTime]::UtcNow -lt $deadline){
   if(!(OwnedProcess $capture)){throw 'Private game exited during timing observation.'}
   Start-Sleep -Milliseconds 500
  }
  [string]$text=Get-Content -LiteralPath $log -Raw
  $observed=$text.Substring($ready.Index+$ready.Length)
  $diagnostics=@($observed -split '\r?\n' | Where-Object {$_ -match 'Native body root cadence|Native physics (dispatch|integration:)|Native camera (dispatch|spring mode)|CPU present cadence'})
  $diagnostics | Set-Content -LiteralPath (Join-Path $root ("cap-$cap-variable-$variable.log"))
  $row.diagnostic_lines=$diagnostics.Count
  $row.body_root_intervals=@($diagnostics | Where-Object {$_ -match 'Native body root cadence'}).Count
  $row.physics_intervals=@($diagnostics | Where-Object {$_ -match 'Native physics integration:'}).Count
  if(!$row.physics_intervals){throw 'No native physics integration intervals were observed after level load.'}
  $row.observation_complete=$true
 }catch{$row.error=$_.Exception.Message}
 finally {
  if($capture){
   $owned=OwnedProcess $capture
   if($owned){
    [void]$owned.CloseMainWindow()
    if(!$owned.WaitForExit(10000)){
     # The user authorized force-closing; revalidate this private process first.
     $owned=OwnedProcess $capture
     if($owned){$owned.Kill();[void]$owned.WaitForExit(10000);$row.forced_close=$true}
    }
   }
   $unchanged=$true
   $normal=Join-Path $workspace 'build/runtime23/user_data'
   if(@(Get-ChildItem -LiteralPath $normal -Recurse -File).Count -ne @($capture.normal_user_hashes.PSObject.Properties).Count){$unchanged=$false}
   foreach($item in $capture.normal_user_hashes.PSObject.Properties){
    if(!(Test-Path -LiteralPath $item.Name) -or (Get-FileHash -LiteralPath $item.Name).Hash -ne $item.Value){$unchanged=$false}
   }
   $row.normal_user_files_unchanged=$unchanged
  }
  $rows+=$row
  [ordered]@{rows=$rows;scope='Native scheduler/body-root/camera observations in private copies. No movement input; stationary repeats are valid. Requested cap is not proof of achieved display FPS. Contacts, jumping and input feel still need gameplay validation.'} |
   ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $root 'result.json')
 }
 if($row.Contains('error') -or !$row.normal_user_files_unchanged){throw "Timing matrix stopped; inspect $root/result.json"}
}}
Write-Output "Timing matrix captures complete: $root/result.json"
