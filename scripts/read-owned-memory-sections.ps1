param([Parameter(Mandatory=$true)][string]$PairRoot)
# Read-only query of pagefile-backed section names; never writes game memory or sends input.
$ErrorActionPreference='Stop'
$targetRoot=(Resolve-Path -LiteralPath $PairRoot).Path
$exe=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../build/runtime23/beetleworx.exe')).Path
$targets=@(Get-CimInstance Win32_Process -Filter "Name='beetleworx.exe' OR Name='EM2Recomp.exe'" | Where-Object {$_.ExecutablePath -eq $exe -and $_.CommandLine.Contains($targetRoot)})
if($targets.Count -ne 2){throw 'Expected exactly two live private test processes; no handles queried.'}
if(-not ('EM2SectionNames' -as [type])){Add-Type -TypeDefinition @"
using System;using System.Collections.Generic;using System.Runtime.InteropServices;
public static class EM2SectionNames {
 [StructLayout(LayoutKind.Sequential)] struct Entry {public IntPtr Object,Process,Handle;public uint Access;public ushort Backtrace,Type;public uint Attributes,Reserved;}
 [StructLayout(LayoutKind.Sequential)] struct Unicode {public ushort Length,Maximum;public IntPtr Buffer;}
 [DllImport("ntdll.dll")] static extern int NtQuerySystemInformation(int c,IntPtr b,uint n,out uint needed);
 [DllImport("ntdll.dll")] static extern int NtQueryObject(IntPtr h,int c,IntPtr b,uint n,out uint needed);
 [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access,bool inherit,uint pid);
 [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
 [DllImport("kernel32.dll")] static extern bool DuplicateHandle(IntPtr p,IntPtr h,IntPtr target,out IntPtr copy,uint access,bool inherit,uint options);
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
 static string Query(IntPtr h,int kind,IntPtr buffer){uint need;if(NtQueryObject(h,kind,buffer,65536,out need)<0)return "";var u=Marshal.PtrToStructure<Unicode>(buffer);return u.Buffer==IntPtr.Zero?"":Marshal.PtrToStringUni(u.Buffer,u.Length/2);}
 public static string[] Read(uint pid) {
  var result=new List<string>();var process=OpenProcess(0x40,false,pid);if(process==IntPtr.Zero)throw new Exception("Cannot open owned process");
  IntPtr all=IntPtr.Zero,buffer=IntPtr.Zero;
  try {
   uint capacity=1048576;int status;uint need;
   while(true){all=Marshal.AllocHGlobal((int)capacity);status=NtQuerySystemInformation(64,all,capacity,out need);if(status>=0)break;Marshal.FreeHGlobal(all);all=IntPtr.Zero;if(status!=unchecked((int)0xC0000004)||capacity>=67108864)throw new Exception("Handle query failed");capacity=Math.Max(capacity*2,need+65536);if(capacity>67108864)throw new Exception("Handle query exceeds 64 MiB bound");}
   buffer=Marshal.AllocHGlobal(65536);long count=Marshal.ReadIntPtr(all).ToInt64();int size=Marshal.SizeOf<Entry>();var types=new Dictionary<ushort,string>();
   for(long i=0;i<count;++i){var e=Marshal.PtrToStructure<Entry>(IntPtr.Add(all,checked(16+(int)i*size)));if(e.Process.ToInt64()!=pid)continue;IntPtr copy;
    if(!DuplicateHandle(process,e.Handle,GetCurrentProcess(),out copy,0,false,2))continue;
    try{string type;if(!types.TryGetValue(e.Type,out type)){type=Query(copy,2,buffer);types[e.Type]=type;}if(type=="Section"){var name=Query(copy,1,buffer);if(name!="")result.Add(name);}}finally{CloseHandle(copy);}
   }
   return result.ToArray();
  }finally{if(all!=IntPtr.Zero)Marshal.FreeHGlobal(all);if(buffer!=IntPtr.Zero)Marshal.FreeHGlobal(buffer);CloseHandle(process);}
 }
}
"@}
$records=@()
foreach($target in $targets){$records+=@{pid=$target.ProcessId;start_utc=$target.CreationDate.ToUniversalTime().ToString('o');sections=@([EM2SectionNames]::Read([uint32]$target.ProcessId))}}
$records | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $targetRoot 'section-names.json')
$records | ConvertTo-Json -Depth 5
