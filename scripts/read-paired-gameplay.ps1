param([Parameter(Mandatory)][string]$PairRoot)
$ErrorActionPreference='Stop'
$root=(Resolve-Path -LiteralPath $PairRoot).Path
$marker='Retained online connection attached to native gameplay at tick '
$peers=@()
foreach($role in @('host','client')){
 $file=Join-Path $root ($role+'/startup.log')
 [string]$text=if(Test-Path -LiteralPath $file){Get-Content -LiteralPath $file -Raw}else{''}
 $at=$text.IndexOf($marker,[StringComparison]::Ordinal)
 $ticks=@{};$duplicates=@();$conflicts=@()
 if($at -ge 0){
  foreach($m in [regex]::Matches($text.Substring($at),'Native gameplay observation v4:[^\r\n]*?epoch [0-9]+, tick ([0-9]+),[^\r\n]*?digest ([0-9a-f]{64})(?=\s|$)')){
   $tick=[UInt64]::Parse($m.Groups[1].Value).ToString();$digest=$m.Groups[2].Value
   if($ticks.ContainsKey($tick)){$duplicates+=$tick;if($ticks[$tick] -ne $digest){$conflicts+=$tick}}
   $ticks[$tick]=$digest
  }
 }
 $peers+=@{role=$role;attached=$at -ge 0;observations=$ticks;duplicate_ticks=$duplicates;conflicting_ticks=$conflicts}
}
$matchingTicks=@();$mismatches=@()
foreach($tick in $peers[0].observations.Keys){
 if($peers[1].observations.ContainsKey($tick)){
  if($peers[0].observations[$tick] -eq $peers[1].observations[$tick]){$matchingTicks+=$tick}else{$mismatches+=$tick}
 }
}
$ordered=@($matchingTicks | ForEach-Object {[UInt64]::Parse($_)} | Sort-Object)
$longest=0;$run=0;$last=$null
foreach($tick in $ordered){if($null -ne $last -and $last -lt [UInt64]::MaxValue -and $tick -eq $last+1){$run++}else{$run=1};$last=$tick;$longest=[Math]::Max($longest,$run)}
$valid=@($peers | Where-Object {!$_.attached -or $_.conflicting_ticks.Count -ne 0}).Count -eq 0 -and $mismatches.Count -eq 0 -and $longest -ge 2
@{schema=1;root=$root;scope='Read-only comparison of native post-attachment v4 observations. Matching logs do not prove network acknowledgement, campaign completion or Internet connectivity.';peers=$peers;matching_ticks=$ordered;different_ticks=$mismatches;longest_consecutive_match=$longest;has_two_consecutive_matches=($longest -ge 2);all_shared_ticks_match=($mismatches.Count -eq 0);two_consecutive_native_ticks_match=$valid} | ConvertTo-Json -Depth 7
