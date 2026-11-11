param([Parameter(Mandatory=$true)][string]$PairRoot)
# Read only the two verified private test processes. Never writes guest memory.
$ErrorActionPreference='Stop'
$root=(Resolve-Path -LiteralPath $PairRoot).Path
$exe=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../build/runtime23/beetleworx.exe')).Path
if(-not ('EM2OwnedWords' -as [type])){Add-Type -TypeDefinition @"
using System;using System.Runtime.InteropServices;
public static class EM2OwnedWords {
[DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint a,bool i,uint p);
[DllImport("kernel32.dll")] static extern bool ReadProcessMemory(IntPtr p,IntPtr a,byte[] b,IntPtr n,out IntPtr read);
[DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr p);
public static uint Word(uint pid,uint guest){var p=OpenProcess(0x10,false,pid);if(p==IntPtr.Zero)throw new Exception("Owned process is gone");try{var b=new byte[4];IntPtr n;if(!ReadProcessMemory(p,new IntPtr(0x100000000L+guest),b,new IntPtr(4),out n)||n.ToInt64()!=4)throw new Exception("Unreadable owned guest word");return ((uint)b[0]<<24)|((uint)b[1]<<16)|((uint)b[2]<<8)|b[3];}finally{CloseHandle(p);}}
}
"@}
$targets=@(Get-CimInstance Win32_Process -Filter "Name='beetleworx.exe' OR Name='EM2Recomp.exe'" | Where-Object {$_.ExecutablePath -eq $exe -and $_.CommandLine.Contains($root)})
if($targets.Count -ne 2){throw 'Exactly two private owned processes must be live.'}
$rows=@()
foreach($p in $targets){
 $id=[uint32]$p.ProcessId
 $role=if($p.CommandLine.Contains((Join-Path $root 'host'))){'host'}else{'client'}
 $log=Get-Content -LiteralPath (Join-Path $root ($role+'/startup.log')) -Raw
 $owners=[regex]::Matches($log,'Native online menu phase: table ([0-9A-F]{8}), owner ([0-9A-F]{8})')
 if(!$owners.Count){throw 'No observed native state owner to validate memory base.'}
 $last=$owners[$owners.Count-1];$owner=[Convert]::ToUInt32($last.Groups[2].Value,16);$expected=[Convert]::ToUInt32($last.Groups[1].Value,16)
 if([EM2OwnedWords]::Word($id,$owner) -ne $expected){throw 'Observed menu was retired or guest base differs; snapshot refused.'}
 $profile=[EM2OwnedWords]::Word($id,[uint32]0x833DAB54L);$modal=[EM2OwnedWords]::Word($id,[uint32]0x833DDC60L)
 $rows+=@{pid=$id;role=$role;start_utc=$p.CreationDate.ToUniversalTime().ToString('o');owner=('{0:X8}' -f $owner);table=('{0:X8}' -f $expected);state16=[EM2OwnedWords]::Word($id,$owner+16);profile=('{0:X8}' -f $profile);selected=[EM2OwnedWords]::Word($id,$profile+296);status0=[EM2OwnedWords]::Word($id,$profile+20);status1=[EM2OwnedWords]::Word($id,$profile+28);status2=[EM2OwnedWords]::Word($id,$profile+36);modal=('{0:X8}' -f $modal);modal_flag=[EM2OwnedWords]::Word($id,$modal+48);modal_active=[EM2OwnedWords]::Word($id,$modal+4)}
 $after=Get-CimInstance Win32_Process -Filter ("ProcessId="+$id)
 if(!$after -or $after.CreationDate -ne $p.CreationDate){throw 'Owned process changed during snapshot.'}
}
$rows | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root 'native-menu-readonly-snapshot.json')
$rows | ConvertTo-Json
