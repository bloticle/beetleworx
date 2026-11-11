$ErrorActionPreference='Stop'
$workspace=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$root=Join-Path $workspace ('analysis/paired-observation-parser-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
$marker='Retained online connection attached to native gameplay at tick 0'
$a='a'*64;$b='b'*64
function Observation([string]$tick,[string]$hash){return "Native gameplay observation v4: level 'test' state 'visit_1' interior '' epoch 99, tick $tick, modes 0/0, digest $hash"}
$zero=Observation '0' $a;$one=Observation '1' $a;$two=Observation '2' $a;$bad=Observation '1' $b
$cases=@(
 @{name='matching';host=@($marker,$zero,$one);client=@($marker,$zero,$one);expected=$true},
 @{name='before-attachment';host=@($zero,$one);client=@($zero,$one);expected=$false},
 @{name='nonconsecutive';host=@($marker,$zero,$two);client=@($marker,$zero,$two);expected=$false},
 @{name='different-world';host=@($marker,$zero,$one);client=@($marker,$zero,$bad);expected=$false},
 @{name='conflicting-duplicate';host=@($marker,$zero,$one,$bad);client=@($marker,$zero,$bad);expected=$false},
 @{name='ignored-pre-attachment';host=@($bad,$marker,$zero,$one);client=@($marker,$zero,$one);expected=$true},
 @{name='only-one-peer';host=@($marker,$zero,$one);client=@($zero,$one);expected=$false},
 @{name='no-observations';host=@($marker);client=@($marker);expected=$false}
)
foreach($case in $cases){
 $folder=Join-Path $root $case.name
 foreach($role in @('host','client')){New-Item -ItemType Directory -Path (Join-Path $folder $role) -Force | Out-Null;$case[$role] | Set-Content -LiteralPath (Join-Path $folder ($role+'/startup.log'))}
 $raw=& (Join-Path $PSScriptRoot 'read-paired-gameplay.ps1') -PairRoot $folder
 $raw | Set-Content -LiteralPath (Join-Path $folder 'comparison.json')
 $parsed=$raw | ConvertFrom-Json
 if([bool]$parsed.two_consecutive_native_ticks_match -ne $case.expected){throw ('Observation evidence case failed: '+$case.name)}
}
@{passed=$cases.Count;root=$root;cases=@($cases | ForEach-Object {$_.name})} | ConvertTo-Json
