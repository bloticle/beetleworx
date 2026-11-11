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
function Get-OwnedProfile {
 $p=Get-CimInstance Win32_Process -Filter ('ProcessId='+$capture.pid)
 if(!$p){return $null}
 $actualExe=[IO.Path]::GetFullPath($p.ExecutablePath)
 if(@($canonicalExe,$legacyExe) -notcontains $actualExe -or ($script:exe -and $actualExe -ne $script:exe) -or !$p.CommandLine.Replace('/','\').Contains($rootPath) -or
    [Math]::Abs(($p.CreationDate.ToUniversalTime()-([datetime]$capture.started_utc).ToUniversalTime()).TotalMilliseconds) -gt 1){throw 'Private profile process identity mismatch.'}
 if(!$script:imageVerified){
  if((Get-FileHash -LiteralPath $actualExe).Hash -ne $capture.exe_sha256){throw 'Private profile executable hash differs from its capture record.'}
  $script:exe=$actualExe;$script:imageVerified=$true
 }
 return Get-Process -Id $capture.pid
}
$result=[ordered]@{root=$rootPath;pid=$capture.pid;exe_sha256=$capture.exe_sha256;completed=$false;observation_seconds=$ObserveSeconds;gameplay_input_sent=$false}
try {
 $deadline=[DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
 $observedUntil=$null
 while([DateTime]::UtcNow -lt $deadline){
  if(!(Get-OwnedProfile)){throw 'Private process exited before completion.'}
  if($null -eq $observedUntil -and (Select-String -LiteralPath (Join-Path $rootPath 'game.log') -SimpleMatch -Pattern $CompletionMarker -Quiet)){
   $result.marker_seen_utc=[DateTime]::UtcNow.ToString('o')
   $observedUntil=[DateTime]::UtcNow.AddSeconds($ObserveSeconds)
  }
  if($null -ne $observedUntil -and [DateTime]::UtcNow -ge $observedUntil){$result.completed=$true;break}
  Start-Sleep -Milliseconds 1000
 }
 if(!$result.completed){throw 'Private observation timed out.'}
}catch{$result.error=$_.Exception.Message}
finally {
 $p=Get-OwnedProfile
 if($p){
  [void]$p.CloseMainWindow()
  if(!$p.WaitForExit(10000)){$p=Get-OwnedProfile;if($p){$p.Kill();[void]$p.WaitForExit(10000);$result.forced_close=$true}}
 }
 $normal=Join-Path $workspace 'build/runtime23/user_data'
 $unchanged=@(Get-ChildItem -LiteralPath $normal -Recurse -File).Count -eq @($capture.normal_user_hashes.PSObject.Properties).Count
 foreach($f in $capture.normal_user_hashes.PSObject.Properties){
  if(!(Test-Path -LiteralPath $f.Name) -or (Get-FileHash -LiteralPath $f.Name).Hash -ne $f.Value){$unchanged=$false}
 }
 $result.normal_user_files_unchanged=$unchanged
 if($capture.em1_source){
  $source=Get-Item -LiteralPath $capture.em1_source.path -ErrorAction SilentlyContinue
  $result.em1_source_metadata_unchanged=$null -ne $source -and !$source.PSIsContainer -and $source.Length -eq $capture.em1_source.length -and $source.LastWriteTimeUtc.Ticks -eq ([datetime]$capture.em1_source.last_write_utc).ToUniversalTime().Ticks
 }
 $result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $rootPath 'completion-result.json')
 $result | ConvertTo-Json
}
if(!$result.completed -or !$result.normal_user_files_unchanged -or ($capture.em1_source -and !$result.em1_source_metadata_unchanged)){exit 1}
