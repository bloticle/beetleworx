$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) { throw 'Visual Studio C++ build tools are required.' }
Import-Module (Join-Path $installation 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $installation -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$llvm = Join-Path $root 'tools\llvm23\clang+llvm-23.1.4-x86_64-pc-windows-msvc\bin'
if (!(Test-Path (Join-Path $llvm 'clang-cl.exe'))) { throw 'Run scripts/bootstrap.ps1 to install LLVM 23.1.4.' }
$ninja = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
$env:PATH = "$llvm;$ninja;$env:PATH"
