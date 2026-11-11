param([string]$GameDirectory = 'D:\Modding\Builds\EM2X360', [string[]]$ExtraArguments = @(), [string]$LogFile = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'build/runtime23/beetleworx.exe'
if (!$LogFile) { $LogFile = Join-Path $root 'analysis/launch.log' }
elseif (![IO.Path]::IsPathRooted($LogFile)) { $LogFile = Join-Path $root $LogFile }
$LogFile = [IO.Path]::GetFullPath($LogFile)
if (!(Test-Path -LiteralPath $exe)) { throw 'Build Beetleworx before launching it.' }
if (Get-Process -Name beetleworx,EM2Recomp -ErrorAction SilentlyContinue | Where-Object { @($exe, (Join-Path (Split-Path $exe -Parent) 'EM2Recomp.exe')) -contains $_.Path }) { throw 'Beetleworx is already running.' }
if (!(Test-Path -LiteralPath (Join-Path $GameDirectory 'default.xex'))) { throw 'Game directory does not contain default.xex.' }
$start = [System.Diagnostics.ProcessStartInfo]::new()
$start.FileName = $exe
$start.WorkingDirectory = Split-Path $exe -Parent
$start.UseShellExecute = $false
$start.ArgumentList.Add($GameDirectory)
$start.ArgumentList.Add('--log_file')
$start.ArgumentList.Add($LogFile)
foreach ($argument in $ExtraArguments) { $start.ArgumentList.Add($argument) }
$process = [System.Diagnostics.Process]::Start($start)
try {
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) { throw "Beetleworx exited with code $($process.ExitCode); inspect analysis/launch.log and build/runtime23/crash.log." }
} finally { $process.Dispose() }
