param([ValidateSet(1,2,3)][int]$Scale=2,[switch]$BenchmarkTransfers,[switch]$BenchmarkZeroStencil,[switch]$BenchmarkStencilValue,[switch]$BenchmarkStencilClear,[switch]$BenchmarkStencilBarriers,[switch]$BenchmarkStencilCopies,[switch]$BenchmarkBoundedDivision,[switch]$BenchmarkFusedDepthStencil,[switch]$BenchmarkDrawExtent,[switch]$BenchmarkDirectColorResolve,[switch]$StencilSurvey,[switch]$StencilClearProbe,[switch]$CpuDrawExtents,[ValidateRange(120,600)][int]$StencilSurveyFrames=600,[switch]$ManualContinue,[ValidateSet('rtv','rov')][string]$RenderPath='rtv',[switch]$OriginalLighting,[switch]$NoGpuProfile,[ValidateRange(1,4096)][int]$GpuReportGroups=16,[switch]$TraceShadowPreparation,[switch]$TraceShadowGpuState,[switch]$OriginalRenderBindings,[switch]$DistanceShadows,[switch]$BenchmarkDistanceShadows,[ValidateSet(-1,2,3)][int]$ShadowCastMode=-1,[switch]$OpaqueShadowCasters,[switch]$ShadowPixels,[switch]$ShadowDepthPixels,[switch]$TraceTiming,[switch]$DetailedMotionTrace,[switch]$PresentCamera,[switch]$TraceCameraBackground,[ValidateSet(-1,0,1)][int]$GameplayTiming=-1,[switch]$EnhancedLighting,[switch]$NativePhysics60,[switch]$NativePhysicsVariable,[ValidateSet(30,40,50,60,90,120,144,165,240,0)][int]$FrameCap=60,[ValidatePattern("^([a-z0-9_.]{1,47})?$")][string]$BootLevel="",[ValidatePattern("^([a-z0-9_.]{1,47})?$")][string]$StartLevel="",[string]$Em1SourceRvz='',[switch]$Em1PublicDemo,[switch]$Em1RememberSource,[switch]$Em1ProbeLevelEntry,[switch]$Em1AccelerateCollision,[switch]$Em1ValidateCollisionActivation,[switch]$Em1ValidateCollisionReload,[switch]$QuietEm1Loader,[switch]$Em1DiskArchiveProbe,[string]$Em1RegisterLevel="",[string]$Em1ValidateLevel='',[switch]$Em1ValidateSceneConstruction,[switch]$Em1ValidateInventoryWrites,[string]$Em1ValidateLevelModels='',[string]$Em1ValidateLevelKeyframes='',[string]$Em1ValidateLevelAnimationManagers='',[switch]$Em1ValidateAnimationPlayback,[switch]$Em1ValidateJigsawRays,[switch]$Em1RegistryRoundtrip,[switch]$Em1ValidateAnimations,[switch]$DirectResolveSurvey,[switch]$Em1ValidateModel,[switch]$Em1ValidateLegacyModel,[switch]$Em1ValidateStaticModels,[switch]$Em1ValidateStaticPhysicsResource,[switch]$Em1ValidateBehaviorResources,[string]$Em1NativeStaticResource='',[switch]$Em1PreviewCourtyard,[switch]$Em1EnterCourtyard,[switch]$Em1CapturePreview,[switch]$Em1LegacyMaterials,[switch]$Em1BlendCandidate,[switch]$Em1VertexAlphaCandidate,[switch]$Em1DualUvCandidate,[switch]$Em1CutoutCandidate,[switch]$Em1UvProbe,[switch]$Em1UvCheckerProbe,[switch]$Em1VerifyNativeGlobals,[switch]$Em1VerifyNativeTrigger,[switch]$Em1VerifyNativeTriggerActivation,[switch]$Em1ValidateParticleAssets,[switch]$Em1VerifyParticleActivation,[switch]$Em1ParticleMaterialCandidate,[switch]$Em1ValidateGearAssets,[switch]$Em1VerifyNativeGearActivation,[switch]$Em1VerifyGearUnload,[switch]$Em1VerifyGearRotation,[ValidateRange(0,14)][int]$ExtraPlayerInstances=0,[ValidateRange(0,900)][int]$ExtraPlayerHoldSeconds=0,[switch]$OnlineMovementProbe,[ValidateSet("", "host", "guest")][string]$OnlineRoomRole="",[string]$OnlineRoomInvitationFile="",[ValidateRange(2,16)][int]$OnlineRoomExpectedPlayers=2,[switch]$AllowConcurrentRoomProbe,[switch]$TraceRoomInitialization,[switch]$TraceRoomEntities,[switch]$OnlineRoomGameplay,[string]$OnlineRoomReplayInvitation="",[uint32]$OnlineRoomBehaviorTraceStart=0,[uint32]$OnlineRoomBehaviorDumpTick=4294967295,[ValidateRange(1,8)][int]$OnlineRoomVerificationWindow=1,[switch]$OwnedPlayerReference,[switch]$RejectOwnedPlayer,[switch]$TraceMickeyEars,[switch]$InstanceMickeyEars,[switch]$Em1TraceDrawState,[string]$DebugLevelRequest="",[switch]$OriginalInkCycle,[string]$EmrClipProbeDirectory="",[switch]$FinalDemoAssets)
$ErrorActionPreference='Stop'
if($RejectOwnedPlayer -and !$OwnedPlayerReference){throw 'RejectOwnedPlayer requires OwnedPlayerReference.'}
if($OwnedPlayerReference -and $ExtraPlayerInstances -ne 1){throw 'OwnedPlayerReference requires exactly one extra player.'}
if($TraceCameraBackground -and (!$PresentCamera -or !$TraceTiming)){throw 'TraceCameraBackground requires PresentCamera and TraceTiming; it never samples or consumes input.'}
if($Em1PublicDemo -and (!$Em1SourceRvz -or $StartLevel -or $BootLevel -or $Em1EnterCourtyard)){throw 'Em1PublicDemo needs an RVZ and a fresh direct demo startup, without other level overrides.'}
if($Em1PublicDemo){$ManualContinue=$true}
if($ShadowDepthPixels){$ShadowPixels=$true}
if($StartLevel -and ($BootLevel -or $ManualContinue)){throw 'StartLevel uses normal automatic Continue; it cannot be combined with BootLevel or ManualContinue.'}
if($ShadowPixels -and $Scale -ne 1){throw 'ShadowPixels currently requires Scale 1 for exact native tiled R32_FLOAT readback.'}
if($BootLevel -and !$PSBoundParameters.ContainsKey('QuietEm1Loader')){$QuietEm1Loader=$true}
if($Em1ValidateInventoryWrites -and !$Em1ValidateSceneConstruction){throw 'Inventory mutation validation requires scene construction diagnostics and the generated private save.'}
if($Em1ParticleMaterialCandidate -and !$Em1VerifyParticleActivation){throw 'Particle material candidate requires particle activation.'}
if($Em1VerifyParticleActivation -and !$Em1EnterCourtyard){throw 'Particle activation requires courtyard entry.'}
if($Em1ValidateParticleAssets -and (!$Em1EnterCourtyard -or $Em1ValidateGearAssets)){throw 'Particle validation requires courtyard entry without the separate gear-asset fixture.'}
# Ordinary courtyard entry uses the verified UV, blend and cutout adapters.
# Explicit partial candidates remain available for diagnostics; the old fallback
# must be requested deliberately so missing switches cannot silently regress grass.
$em1ExplicitMaterials=$Em1BlendCandidate -or $Em1VertexAlphaCandidate -or $Em1DualUvCandidate -or $Em1CutoutCandidate -or $Em1UvProbe -or $Em1UvCheckerProbe
if($Em1LegacyMaterials -and (!$Em1EnterCourtyard -or $em1ExplicitMaterials)){throw 'Em1LegacyMaterials requires courtyard entry without material candidates or UV probes.'}
$em1DefaultMaterials=$Em1EnterCourtyard -and !$Em1LegacyMaterials -and !$em1ExplicitMaterials
if($em1DefaultMaterials){
    $Em1BlendCandidate=$true
    $Em1VertexAlphaCandidate=$true
    $Em1DualUvCandidate=$true
    $Em1CutoutCandidate=$true
}
if(($Em1VerifyNativeTrigger -or $Em1VerifyNativeTriggerActivation) -and !$Em1EnterCourtyard){throw 'Native trigger validation requires courtyard entry.'}
if($Em1VerifyNativeGlobals -and !$Em1EnterCourtyard){throw 'Native global validation requires courtyard entry.'}
if($Em1UvCheckerProbe -and !$Em1UvProbe){throw 'UV checker requires the private UV probe switch.'}
if($Em1UvProbe -and !$Em1CutoutCandidate){throw 'UV probe requires the complete private material candidate.'}
if($Em1CutoutCandidate -and !$Em1VertexAlphaCandidate){throw 'Cutout requires the private vertex alpha candidate.'}
if($Em1DualUvCandidate -and !$Em1VertexAlphaCandidate){throw 'Dual UV requires the private vertex alpha candidate.'}
if($Em1VertexAlphaCandidate -and !$Em1BlendCandidate){throw 'Vertex alpha requires the private blend candidate.'}
if(($Em1ValidateLevelAnimationManagers -or $Em1ValidateLevelKeyframes -or $Em1ValidateLevelModels -or $Em1ValidateAnimations -or $Em1ValidateLevel -or $Em1ValidateModel -or $Em1ValidateLegacyModel -or $Em1ValidateStaticModels -or $Em1ValidateStaticPhysicsResource -or $Em1ValidateBehaviorResources -or $Em1NativeStaticResource -or $Em1PreviewCourtyard -or $Em1EnterCourtyard) -and !$Em1SourceRvz){throw 'Em1ValidateLevel requires Em1SourceRvz.'}
$em1SourceMetadata=$null
if($Em1SourceRvz){
 $em1Source=Get-Item -LiteralPath $Em1SourceRvz
 if($em1Source.PSIsContainer -or $em1Source.Extension -ine '.rvz'){throw 'Em1SourceRvz must identify an existing RVZ file.'}
 $Em1SourceRvz=$em1Source.FullName
 $em1SourceMetadata=@{path=$Em1SourceRvz;length=$em1Source.Length;last_write_utc=$em1Source.LastWriteTimeUtc.ToString('o')}
}
if($StencilClearProbe){$StencilSurvey=$true;if(!$PSBoundParameters.ContainsKey('StencilSurveyFrames')){$StencilSurveyFrames=120}}
if(($NativePhysics60 -or $NativePhysicsVariable) -and $GameplayTiming -ne 1){throw 'Native physics diagnostics require GameplayTiming 1.'}
if($NativePhysics60 -and $NativePhysicsVariable){throw 'Choose one native physics candidate per capture.'}
$benchmarkCount=[int][bool]$BenchmarkTransfers+[int][bool]$BenchmarkZeroStencil+[int][bool]$BenchmarkStencilValue+[int][bool]$BenchmarkStencilClear+[int][bool]$BenchmarkStencilBarriers+[int][bool]$BenchmarkStencilCopies+[int][bool]$BenchmarkBoundedDivision+[int][bool]$BenchmarkFusedDepthStencil+[int][bool]$BenchmarkDrawExtent+[int][bool]$BenchmarkDirectColorResolve
if($benchmarkCount -gt 1){throw 'Select only one benchmark target.'}
if($StencilSurvey -and $benchmarkCount){throw 'Capture stencil usage separately from FPS comparisons.'}
$workspace=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$em1ShaderArchive=$null
if($Em1BlendCandidate){
    if(!$Em1EnterCourtyard){throw 'EM1 blend candidate requires courtyard entry.'}
    $candidate=if($em1DefaultMaterials){'build/runtime23/pc-assets/packfiles/shaders.pak'}elseif($Em1UvCheckerProbe){'analysis/em1-uv-checker-fetch-order-probe/shaders.pak'}elseif($Em1UvProbe){'analysis/em1-uv-fraction-fetch-order-probe/shaders.pak'}elseif($Em1CutoutCandidate -or $Em1DualUvCandidate -or $Em1VertexAlphaCandidate){'analysis/em1-vertex-fetch-fixed/shaders.pak'}else{'analysis/em1-blend-candidate/shaders.pak'}
    $em1ShaderArchive=Join-Path $workspace $candidate
    if(!(Test-Path -LiteralPath $em1ShaderArchive -PathType Leaf)){throw "Required EM1 shader archive is missing: $em1ShaderArchive. Prepare the EM1 materials before launching."}
}
$runtime=Join-Path $workspace 'build/runtime23'
$executable=Join-Path $runtime 'beetleworx.exe'
if($FinalDemoAssets -and $Em1BlendCandidate){throw 'Final demo assets cannot overlap a shader candidate.'}
$demoPacks=@('UIOptions.pak','UIPauseOptions.pak','UIhud.pak','UIStartGame.pak','UIMainMenu.pak','_Dynamic.pak','shaders.pak')
$demoArchiveRoot=Join-Path $workspace 'private/pc-assets/packfiles'
if($FinalDemoAssets){foreach($name in $demoPacks){if(!(Test-Path -LiteralPath (Join-Path $demoArchiveRoot $name) -PathType Leaf)){throw "Missing final demo archive: $name"}}}
if($OnlineRoomRole){
    if(!$OnlineRoomInvitationFile -or $StartLevel -or $BootLevel -or $Em1SourceRvz -or $OnlineMovementProbe -or $ExtraPlayerInstances){throw 'Room probe requires an invitation path and cannot overlap other world/input probes.'}
    $OnlineRoomInvitationFile=[IO.Path]::GetFullPath($OnlineRoomInvitationFile)
    $analysisRoot=[IO.Path]::GetFullPath((Join-Path $workspace 'analysis'))+[IO.Path]::DirectorySeparatorChar
    if(!$OnlineRoomInvitationFile.StartsWith($analysisRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Room invitation must be inside the workspace analysis directory.'}
}
if($OnlineRoomReplayInvitation -and $OnlineRoomRole -ne 'host'){throw 'Only a private Room host may select a replay invitation.'}
if($TraceRoomEntities -and !$OnlineRoomRole){throw 'Entity capture requires an explicit private Room.'}
if($OnlineRoomGameplay -and (!$OnlineRoomRole -or $OnlineRoomExpectedPlayers -ne 2)){throw 'Native retail gameplay requires an explicit private two-participant Room.'}
if($TraceRoomInitialization -and !$OnlineRoomRole){throw 'Native initialization tracing is restricted to a private Room probe.'}
if($AllowConcurrentRoomProbe -and !$OnlineRoomRole){throw 'Concurrent launch is restricted to the explicit Room probe.'}
$existingGames=@(Get-CimInstance Win32_Process -Filter "Name='beetleworx.exe' OR Name='EM2Recomp.exe'")
if($existingGames.Count){
    if(!$AllowConcurrentRoomProbe){throw 'A game is running; private profile did not launch.'}
    foreach($existingGame in $existingGames){
        if($existingGame.ExecutablePath -ine $executable -or !$existingGame.CommandLine.Contains('--em2_private_profile=true') -or
           !$existingGame.CommandLine.Contains('--em2_room_probe_role=') -or !$existingGame.CommandLine.Contains($OnlineRoomInvitationFile.Replace('\','/'))){
            throw 'Concurrent Room probe refused: another game is not a verified private instance of this same pair.'
        }
    }
}
$root=Join-Path $workspace ('analysis/private-gpu-profile-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
$content=Join-Path $root 'pc-assets';$user=Join-Path $root 'user_data'
New-Item -ItemType Directory -Path $content,$user | Out-Null
$symbolMap=Join-Path $runtime 'beetleworx.map'
if(Test-Path -LiteralPath $symbolMap -PathType Leaf){Copy-Item -LiteralPath $symbolMap -Destination (Join-Path $root 'beetleworx.map')}
$baseline=@{}
foreach($file in Get-ChildItem -LiteralPath (Join-Path $runtime 'user_data') -Recurse -File){$baseline[$file.FullName]=(Get-FileHash -LiteralPath $file.FullName).Hash}
foreach($entry in Get-ChildItem -LiteralPath (Join-Path $runtime 'pc-assets') -Force){
    $target=Join-Path $content $entry.Name
    if(($Em1BlendCandidate -or $FinalDemoAssets) -and $entry.Name -eq 'packfiles'){
        if($Em1BlendCandidate -and !$Em1EnterCourtyard){throw 'EM1 blend candidate requires the private courtyard preview.'}
        New-Item -ItemType Directory -Path $target | Out-Null
        foreach($pack in Get-ChildItem -LiteralPath $entry.FullName -File){
            $packTarget=Join-Path $target $pack.Name
            if($FinalDemoAssets -and $pack.Name -in $demoPacks){
                Copy-Item -LiteralPath (Join-Path $demoArchiveRoot $pack.Name) -Destination $packTarget
            }elseif($Em1BlendCandidate -and $pack.Name -eq 'shaders.pak'){
                Copy-Item -LiteralPath $em1ShaderArchive -Destination $packTarget
            }else{New-Item -ItemType HardLink -Path $packTarget -Target $pack.FullName | Out-Null}
        }
    }
    elseif($entry.PSIsContainer){New-Item -ItemType Junction -Path $target -Target $entry.FullName | Out-Null}
    elseif($entry.Name -eq 'ConfigFiles.ini'){Copy-Item -LiteralPath $entry.FullName -Destination $target}
    else{New-Item -ItemType HardLink -Path $target -Target $entry.FullName | Out-Null}
}
# Copy campaign and root preference files. Cache records remain private; no
# writable save, preferences or renderer-cache files are hard-linked.
if(!$Em1PublicDemo){Copy-Item -LiteralPath (Join-Path $runtime 'user_data/42560816') -Destination $user -Recurse}
foreach($file in Get-ChildItem -LiteralPath (Join-Path $runtime 'user_data') -File){Copy-Item -LiteralPath $file.FullName -Destination $user}
$settings=Join-Path $user 'pc-settings.ini'
$lines=Get-Content -LiteralPath $settings
if($DistanceShadows){$lines=@($lines | Where-Object {$_ -notmatch '^distance_shadow_resolution='})+'distance_shadow_resolution=2'}
$lines=$lines -replace '^render_scale=.*$',('render_scale='+($Scale-1)) -replace '^frame_limit=.*$',('frame_limit='+[Array]::IndexOf(@(30,40,50,60,90,120,144,165,240,0),$FrameCap))
if($OriginalLighting){$lines=$lines -replace '^hdr_output=.*$','hdr_output=0' -replace '^material_ambient_occlusion=.*$','material_ambient_occlusion=0'}
if($EnhancedLighting){if($OriginalLighting -or $RenderPath -ne 'rtv'){throw 'Enhanced lighting requires RTV without OriginalLighting.'};$lines=$lines -replace '^hdr_output=.*$','hdr_output=1' -replace '^material_ambient_occlusion=.*$','material_ambient_occlusion=2'}
if($GameplayTiming -ge 0){$lines=$lines -replace '^gameplay_timing=.*$',('gameplay_timing='+$GameplayTiming)}
if($OnlineRoomRole){$lines=$lines -replace '^window_mode=.*$','window_mode=0' -replace '^resolution=.*$','resolution=0'}
[IO.File]::WriteAllText($settings,($lines -join "`n")+"`n",[Text.UTF8Encoding]::new($false))
$info=[Diagnostics.ProcessStartInfo]::new($executable)
$info.UseShellExecute=$false;$info.WorkingDirectory=$runtime;$info.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
$info.ArgumentList.Add("--em2_private_profile=true")
if($FinalDemoAssets){$info.ArgumentList.Add('--em2_demo_build=true')}
if($Em1PublicDemo){$info.ArgumentList.Add("--em2_demo_build=true");$info.ArgumentList.Add("--em2_demo_em1=true");$info.ArgumentList.Add("--em2_trace_auto_continue=true")}
if($TraceMickeyEars){$info.ArgumentList.Add("--em2_trace_mickey_ears=true")}
if($InstanceMickeyEars){$info.ArgumentList.Add("--em2_instance_mickey_ears=true")}
if($RejectOwnedPlayer){$info.ArgumentList.Add("--em2_probe_owned_native_player_reject=true")}
if($OwnedPlayerReference){$info.ArgumentList.Add("--em2_probe_owned_native_player=true")}
if($OnlineMovementProbe){$info.ArgumentList.Add("--em2_probe_online_local_movement=true")}
if($OnlineRoomRole){
    $info.ArgumentList.Add('--em2_room_probe_role='+$OnlineRoomRole)
    $info.ArgumentList.Add('--em2_room_probe_invitation_file='+$OnlineRoomInvitationFile.Replace('\','/'))
    $info.ArgumentList.Add('--em2_room_probe_expected_players='+$OnlineRoomExpectedPlayers)
    $info.ArgumentList.Add('--em2_trace_online_observations=true')
    $info.ArgumentList.Add('--em2_trace_players=true')
    if($TraceRoomInitialization){$info.ArgumentList.Add('--em2_trace_room_initialization=true')}
    if($TraceRoomEntities){
        $info.ArgumentList.Add('--em2_trace_native_entities=true')
        $info.ArgumentList.Add('--em2_native_entity_directory='+$root.Replace('\','/')+'/entities')
    }
    if($OnlineRoomGameplay){$info.ArgumentList.Add('--em2_room_probe_gameplay=true')}
    $info.ArgumentList.Add('--em2_behavior_trace_start_tick='+$OnlineRoomBehaviorTraceStart)
    $info.ArgumentList.Add('--em2_behavior_trace_dump_tick='+$OnlineRoomBehaviorDumpTick)
    $info.ArgumentList.Add('--em2_room_probe_verification_window='+$OnlineRoomVerificationWindow)
    if($OnlineRoomReplayInvitation){$replay=(Resolve-Path -LiteralPath $OnlineRoomReplayInvitation).Path;$info.ArgumentList.Add('--em2_room_probe_replay_invitation_file='+$replay.Replace('\','/'))}
}
if($ExtraPlayerInstances){$info.ArgumentList.Add("--em2_probe_extra_native_players="+$ExtraPlayerInstances)}
if($ExtraPlayerHoldSeconds){if(!$ExtraPlayerInstances){throw "ExtraPlayerHoldSeconds requires ExtraPlayerInstances."};$info.ArgumentList.Add("--em2_probe_extra_native_players_hold_seconds="+$ExtraPlayerHoldSeconds);$info.ArgumentList.Add("--em2_trace_players=true")}
if($Em1ValidateInventoryWrites){$info.ArgumentList.Add("--em1_validate_inventory_writes=true")}
$profileDraws=if($NoGpuProfile -or $StencilSurvey -or $BenchmarkTransfers -or $BenchmarkZeroStencil -or $BenchmarkStencilValue -or $BenchmarkStencilClear -or $BenchmarkStencilBarriers -or $BenchmarkStencilCopies -or $BenchmarkBoundedDivision -or $BenchmarkFusedDepthStencil -or $BenchmarkDrawExtent){'false'}else{'true'}
foreach($argument in @(('--em2_profile_gpu_draws='+$profileDraws),('--em2_gpu_draw_profile_directory='+$root.Replace('\','/')+'/shaders'),('--em2_user_data_directory='+$user.Replace('\','/')),('--em2_content_directory='+$content.Replace('\','/')),('--log_file='+$root.Replace('\','/')+'/game.log'))){$info.ArgumentList.Add($argument)}
if($StencilSurvey){$info.ArgumentList.Add('--em2_stencil_consumer_survey=true');$info.ArgumentList.Add('--em2_stencil_consumer_survey_frames='+$StencilSurveyFrames)}
if($StencilClearProbe){$info.ArgumentList.Add('--em2_stencil_clear_probe_directory='+(Join-Path $root 'clear-probe'))}
if($OriginalInkCycle){$info.ArgumentList.Add('--em1_qa_ink_cycle=true')}
if($EmrClipProbeDirectory){$info.ArgumentList.Add('--em2_qa_emr_clip_directory='+[IO.Path]::GetFullPath($EmrClipProbeDirectory))}
if($PSBoundParameters.ContainsKey('CpuDrawExtents')){$info.ArgumentList.Add('--execute_unclipped_draw_vs_on_cpu='+([bool]$CpuDrawExtents).ToString().ToLowerInvariant());$info.ArgumentList.Add('--execute_unclipped_draw_vs_on_cpu_with_scissor=false')}
$info.ArgumentList.Add('--em2_gpu_draw_report_limit='+$GpuReportGroups)
if($TraceShadowPreparation){$info.ArgumentList.Add('--em2_trace_shadow_preparation=true')}
if($TraceShadowGpuState){if($profileDraws -ne 'true'){throw 'TraceShadowGpuState requires GPU draw profiling.'};$info.ArgumentList.Add('--em2_trace_shadow_gpu_state=true')}
if($OriginalRenderBindings){
    foreach($name in @('em2_native_binding_cache','em2_native_material_binding','em2_predicted_material_reads','em2_native_matrix_stores','em2_native_visibility_scan','em2_deduplicate_gpu_constants','em2_batch_gpu_constant_reads','em2_inline_gpu_constant_writes')){$info.ArgumentList.Add('--'+$name+'=false')}
}
if($DistanceShadows){$info.ArgumentList.Add('--em2_distance_shadows=true')}
if($BenchmarkDistanceShadows){$info.ArgumentList.Add('--em2_benchmark_distance_shadows=true')}
if($ShadowCastMode -ne -1){$info.ArgumentList.Add('--em2_probe_shadow_mode='+$ShadowCastMode)}
if($OpaqueShadowCasters){$info.ArgumentList.Add('--em2_probe_opaque_shadow_casters=true')}
if($ShadowPixels){$info.ArgumentList.Add('--em2_probe_shadow_pixels=true')}
if($ShadowDepthPixels){$info.ArgumentList.Add('--em2_probe_shadow_depth_pixels=true')}
if($ShadowPixels -and $BenchmarkDistanceShadows){$info.ArgumentList.Add('--em2_probe_shadow_pixel_transitions=true')}
if($TraceTiming){
    $info.ArgumentList.Add('--em2_trace_camera=true')
    # Per-mesh ancestry logging can emit thousands of lines per second. Keep
    # human demo verification free of that observer cost unless requested.
    $motionTrace=$DetailedMotionTrace -or !$FinalDemoAssets
    $info.ArgumentList.Add('--em2_trace_physics='+([bool]$motionTrace).ToString().ToLowerInvariant())
    $info.ArgumentList.Add('--em2_trace_motion='+([bool]$motionTrace).ToString().ToLowerInvariant())
}
$info.ArgumentList.Add('--em2_present_camera='+([bool]$PresentCamera).ToString().ToLowerInvariant())
$info.ArgumentList.Add('--em2_trace_camera_background='+([bool]$TraceCameraBackground).ToString().ToLowerInvariant())
if($NativePhysics60){if($GameplayTiming -ne 1){throw 'NativePhysics60 requires GameplayTiming 1.'};$info.ArgumentList.Add('--em2_native_physics_60hz=true')}
if($PSBoundParameters.ContainsKey('NativePhysicsVariable') -or $NativePhysics60){
 $info.ArgumentList.Add('--em2_native_physics_variable='+([bool]$NativePhysicsVariable).ToString().ToLowerInvariant())
}
if($StartLevel){$info.ArgumentList.Add('--em2_qa_start_level='+$StartLevel)}
if($BootLevel){if(!$ManualContinue){throw 'Direct test boot requires ManualContinue.'};$info.ArgumentList.Add('--em2_qa_boot_level='+$BootLevel);$info.ArgumentList.Add('--em2_trace_auto_continue=true')}
if($Em1RememberSource){
    if(!$Em1SourceRvz){throw 'Remembered-source validation requires Em1SourceRvz.'}
    [IO.File]::WriteAllText((Join-Path $user 'em1-source.txt'),(Resolve-Path -LiteralPath $Em1SourceRvz).Path+"`n",[Text.UTF8Encoding]::new($false))
}elseif($Em1SourceRvz){$info.ArgumentList.Add('--em1_source_rvz='+$Em1SourceRvz)}

if($Em1VerifyNativeTrigger){$info.ArgumentList.Add('--em1_verify_native_trigger=true')}
if($Em1VerifyNativeTriggerActivation){$info.ArgumentList.Add('--em1_verify_native_trigger_activation=true')}
if($Em1ParticleMaterialCandidate){$info.ArgumentList.Add('--em1_particle_material_candidate=true')}
if($Em1VerifyParticleActivation){$info.ArgumentList.Add('--em1_verify_particle_activation=true')}
if($Em1ValidateParticleAssets){$info.ArgumentList.Add('--em1_validate_particle_assets=true')}
if($Em1ValidateGearAssets){$info.ArgumentList.Add('--em1_validate_gear_assets=true')}
if($Em1VerifyGearRotation){if(!$Em1EnterCourtyard -or $Em1ValidateGearAssets){throw 'Gear rotation test requires persistent courtyard gears.'};$info.ArgumentList.Add('--em1_verify_gear_rotation=true')}
if($Em1VerifyGearUnload){if(!$Em1EnterCourtyard -or $Em1ValidateGearAssets){throw 'Gear unload test requires persistent courtyard gears, without the asset probe.'};$info.ArgumentList.Add('--em1_verify_gear_unload=true')}
if($Em1VerifyNativeGearActivation){if(!$Em1EnterCourtyard -or !$Em1ValidateGearAssets){throw 'Gear activation requires courtyard entry and asset validation.'};$info.ArgumentList.Add('--em1_verify_native_gear_activation=true')}
if($Em1VerifyNativeGlobals){$info.ArgumentList.Add('--em1_verify_native_globals=true')}
if($Em1TraceDrawState){$info.ArgumentList.Add('--em1_trace_draw_state=true')}
if($Em1BlendCandidate){$info.ArgumentList.Add('--em1_masked_material_candidate=true')}
if($Em1DualUvCandidate){$info.ArgumentList.Add('--em1_dual_uv_material_candidate=true')}
if($Em1CutoutCandidate){$info.ArgumentList.Add('--em1_cutout_material_candidate=true')}
if($Em1RegisterLevel){$info.ArgumentList.Add('--em1_register_level='+$Em1RegisterLevel)}
if($Em1ProbeLevelEntry){if(!$Em1RegisterLevel){throw 'Normal level-entry experiment requires Em1RegisterLevel.'};$info.ArgumentList.Add('--em1_probe_level_entry=true');$info.ArgumentList.Add('--em1_trace_loader_pending='+(!$QuietEm1Loader).ToString().ToLowerInvariant())}
if($Em1AccelerateCollision){if(!$BootLevel -or !$Em1ProbeLevelEntry){throw 'Collision acceleration validation requires a private registered direct-boot level.'};$info.ArgumentList.Add('--em1_accelerate_registered_collision=true')}
if($Em1ValidateCollisionActivation){if(!$BootLevel -or !$Em1ProbeLevelEntry){throw 'Collision activation validation requires a private registered direct-boot level.'};$info.ArgumentList.Add('--em1_validate_collision_activation=true')}
if($Em1ValidateCollisionReload){if(!$BootLevel -or !$Em1ProbeLevelEntry){throw 'Collision lifecycle validation requires a private registered direct-boot level.'};$info.ArgumentList.Add('--em1_validate_collision_reload=true')}
if($Em1DiskArchiveProbe){if(!$Em1ProbeLevelEntry){throw 'Disk archive experiment requires normal level-entry probe.'};$info.ArgumentList.Add('--em1_probe_disk_archives=true')}
if($Em1ValidateLevel){$info.ArgumentList.Add('--em1_validate_level='+$Em1ValidateLevel)}
if($Em1ValidateSceneConstruction){if(!$Em1ValidateLevel){throw 'Scene construction requires Em1ValidateLevel.'};$info.ArgumentList.Add('--em1_validate_scene_construction=true')}
if($Em1ValidateLevelModels){$info.ArgumentList.Add('--em1_validate_level_models='+$Em1ValidateLevelModels)}
if($Em1ValidateLevelKeyframes){$info.ArgumentList.Add('--em1_validate_level_keyframes='+$Em1ValidateLevelKeyframes)}
if($Em1ValidateLevelAnimationManagers){$info.ArgumentList.Add('--em1_validate_level_animation_managers='+$Em1ValidateLevelAnimationManagers)}
if($Em1ValidateAnimationPlayback){if(!$Em1ValidateLevelAnimationManagers){throw 'Animation playback requires a level manager inventory.'};$info.ArgumentList.Add('--em1_validate_animation_playback=true')}
if($Em1ValidateJigsawRays){if(!$Em1ValidateLevelModels){throw 'Jigsaw ray diagnostics require a complete level-model inventory.'};$info.ArgumentList.Add('--em1_validate_jigsaw_rays=true')}
if($Em1ValidateModel){$info.ArgumentList.Add('--em1_validate_model=true')}
if($Em1ValidateLegacyModel){$info.ArgumentList.Add('--em1_validate_legacy_model=true')}
if($Em1ValidateStaticModels){$info.ArgumentList.Add('--em1_validate_static_models=true')}
if($Em1ValidateStaticPhysicsResource){$info.ArgumentList.Add('--em1_validate_static_physics_resource=true')}
if($Em1ValidateBehaviorResources){$info.ArgumentList.Add('--em1_validate_behavior_resources=true')}
if($DebugLevelRequest){$info.ArgumentList.Add("--em2_qa_debug_level="+$DebugLevelRequest)}
if($Em1NativeStaticResource){$info.ArgumentList.Add('--em1_native_static_resource='+$Em1NativeStaticResource)}
if($Em1PreviewCourtyard){$info.ArgumentList.Add('--em1_preview_courtyard=true')}
if($Em1EnterCourtyard){$info.ArgumentList.Add('--em1_enter_courtyard=true');$info.ArgumentList.Add('--em1_trace_loader_pending=true');$info.ArgumentList.Add('--em1_trace_camera_snapshot=true');$info.ArgumentList.Add('--em1_trace_draw_state=true')}
if($Em1CapturePreview){
    if(!$Em1EnterCourtyard -or !$OriginalLighting){throw 'EM1 preview capture requires Em1EnterCourtyard and OriginalLighting for supported SDR readback.'}
    $info.ArgumentList.Add('--em1_preview_capture_file='+(Join-Path $root 'em1-courtyard.bmp'))
}
if($Em1RegistryRoundtrip){$info.ArgumentList.Add('--em1_registry_roundtrip=true')}
if($Em1ValidateAnimations){$info.ArgumentList.Add('--em1_validate_animations=true')}
if($DirectResolveSurvey){$info.ArgumentList.Add('--em2_direct_color_resolve_survey=true');$info.ArgumentList.Add('--em2_direct_color_resolve_survey_frames=600')}
if(!$ManualContinue){$info.ArgumentList.Add('--em2_auto_continue=true');$info.ArgumentList.Add('--em2_trace_auto_continue=true')}
$info.ArgumentList.Add('--render_target_path_d3d12='+$RenderPath)
if($BenchmarkTransfers){$info.ArgumentList.Add('--em2_benchmark_transfer_scissors=true')}
if($BenchmarkZeroStencil){$info.ArgumentList.Add('--em2_benchmark_zero_stencil=true')}
if($BenchmarkStencilValue){$info.ArgumentList.Add('--em2_benchmark_stencil_value=true')}
if($BenchmarkStencilClear){$info.ArgumentList.Add('--em2_benchmark_stencil_clear=true');$info.ArgumentList.Add('--em2_stencil_plane_copy=true')}
if($BenchmarkStencilBarriers){$info.ArgumentList.Add('--em2_benchmark_stencil_barriers=true');$info.ArgumentList.Add('--em2_stencil_plane_copy=true')}
if($BenchmarkStencilCopies){$info.ArgumentList.Add('--em2_benchmark_stencil_copies=true');$info.ArgumentList.Add('--em2_stencil_plane_copy=true')}
if($BenchmarkDirectColorResolve){$info.ArgumentList.Add('--em2_benchmark_direct_color_resolve=true')}
if($BenchmarkDrawExtent){$info.ArgumentList.Add('--em2_benchmark_draw_extent=true');$info.ArgumentList.Add('--execute_unclipped_draw_vs_on_cpu_with_scissor=false')}
if($BenchmarkFusedDepthStencil){$info.ArgumentList.Add('--em2_benchmark_fused_depth_stencil=true')}
if($BenchmarkBoundedDivision){$info.ArgumentList.Add('--em2_benchmark_bounded_division=true');$info.ArgumentList.Add('--em2_stencil_plane_copy=true')}
$game=[Diagnostics.Process]::Start($info)
$record=[ordered]@{pid=$game.Id;started_utc=$game.StartTime.ToUniversalTime().ToString('o');root=$root;exe_sha256=(Get-FileHash -LiteralPath $executable).Hash;scale=$Scale;benchmark_transfers=[bool]$BenchmarkTransfers;benchmark_zero_stencil=[bool]$BenchmarkZeroStencil;benchmark_stencil_value=[bool]$BenchmarkStencilValue;benchmark_stencil_clear=[bool]$BenchmarkStencilClear;benchmark_stencil_barriers=[bool]$BenchmarkStencilBarriers;benchmark_stencil_copies=[bool]$BenchmarkStencilCopies;benchmark_bounded_division=[bool]$BenchmarkBoundedDivision;render_path=$RenderPath;original_lighting=[bool]$OriginalLighting;enhanced_lighting=[bool]$EnhancedLighting;trace_timing=[bool]$TraceTiming;gameplay_timing=$GameplayTiming;automatic_continue=(!$ManualContinue);normal_user_hashes=$baseline;gameplay_input_sent=$false;screenshots_taken=$false;scope='Interactive private campaign copy, selected FPS cap and GPU work or transfer A/B diagnostics; no automated gameplay input'}
$record.benchmark_draw_extent=[bool]$BenchmarkDrawExtent
$record.benchmark_direct_color_resolve=[bool]$BenchmarkDirectColorResolve
$record.benchmark_fused_depth_stencil=[bool]$BenchmarkFusedDepthStencil
$record.present_camera=[bool]$PresentCamera
$record.trace_camera_background=[bool]$TraceCameraBackground
$record.detailed_motion_trace=[bool]($TraceTiming -and ($DetailedMotionTrace -or !$FinalDemoAssets))
$record.native_physics_60hz=[bool]$NativePhysics60
$record.native_physics_variable=if($PSBoundParameters.ContainsKey('NativePhysicsVariable') -or $NativePhysics60){[bool]$NativePhysicsVariable}else{$null}
$record.frame_cap=$FrameCap
$record.em1_source=$em1SourceMetadata
$record.em1_registry_roundtrip=[bool]$Em1RegistryRoundtrip
$record.em1_validate_level=$Em1ValidateLevel
$record.em1_validate_level_models=$Em1ValidateLevelModels
$record.em1_validate_level_keyframes=$Em1ValidateLevelKeyframes
$record.em1_validate_level_animation_managers=$Em1ValidateLevelAnimationManagers
$record.em1_validate_jigsaw_rays=[bool]$Em1ValidateJigsawRays
$record.em1_validate_model=[bool]$Em1ValidateModel
$record.em1_validate_legacy_model=[bool]$Em1ValidateLegacyModel
$record.em1_validate_static_models=[bool]$Em1ValidateStaticModels
$record.em1_preview_courtyard=[bool]$Em1PreviewCourtyard
$record.em1_default_materials=[bool]$em1DefaultMaterials
$record.em1_legacy_materials=[bool]$Em1LegacyMaterials
$record.em1_shader_archive=$em1ShaderArchive
$record.em1_vertex_alpha_candidate=[bool]$Em1VertexAlphaCandidate
$record.em1_dual_uv_candidate=[bool]$Em1DualUvCandidate
$record.em1_cutout_candidate=[bool]$Em1CutoutCandidate
$record.em1_uv_probe=[bool]$Em1UvProbe
$record.em1_uv_checker_probe=[bool]$Em1UvCheckerProbe
$record.em1_particle_material_candidate=[bool]$Em1ParticleMaterialCandidate
$record.em1_verify_particle_activation=[bool]$Em1VerifyParticleActivation
$record.em1_validate_particle_assets=[bool]$Em1ValidateParticleAssets
$record.em1_enter_courtyard=[bool]$Em1EnterCourtyard
$record.direct_resolve_survey=[bool]$DirectResolveSurvey
$record.stencil_survey=[bool]$StencilSurvey
$record.stencil_survey_frames=$StencilSurveyFrames
$record.stencil_clear_probe=[bool]$StencilClearProbe
$record.cpu_draw_extents=if($PSBoundParameters.ContainsKey('CpuDrawExtents')){[bool]$CpuDrawExtents}else{$null}
$record.em1_register_level=$Em1RegisterLevel
$record.boot_level=$BootLevel
$record.start_level=$StartLevel
$record.em1_public_demo=[bool]$Em1PublicDemo
$record.final_demo_assets=[bool]$FinalDemoAssets
$record.extra_player_instances=$ExtraPlayerInstances
$record.online_movement_probe=[bool]$OnlineMovementProbe
$record.online_room_role=$OnlineRoomRole
$record.online_room_gameplay=[bool]$OnlineRoomGameplay
$record.online_room_replay_invitation=$OnlineRoomReplayInvitation
$record.online_room_behavior_trace_start=$OnlineRoomBehaviorTraceStart
$record.online_room_behavior_dump_tick=$OnlineRoomBehaviorDumpTick
$record.online_room_verification_window=$OnlineRoomVerificationWindow
$record.online_room_trace_entities=[bool]$TraceRoomEntities
$record.online_room_invitation_file=$OnlineRoomInvitationFile
$record.online_room_expected_players=$OnlineRoomExpectedPlayers
$record.owned_player_reference=[bool]$OwnedPlayerReference
$record.reject_owned_player=[bool]$RejectOwnedPlayer
$record.extra_player_hold_seconds=$ExtraPlayerHoldSeconds
$record.trace_mickey_ears=[bool]$TraceMickeyEars
$record.instance_mickey_ears_requested=[bool]$InstanceMickeyEars
$record.symbol_map_sha256=if(Test-Path -LiteralPath (Join-Path $root 'beetleworx.map') -PathType Leaf){(Get-FileHash -LiteralPath (Join-Path $root 'beetleworx.map')).Hash}else{$null}
$record.exe_path=$executable
$record.symbol_map_path=Join-Path $root 'beetleworx.map'
$record.em1_probe_level_entry=[bool]$Em1ProbeLevelEntry
$record.em1_accelerate_collision_override_requested=[bool]$Em1AccelerateCollision
$record.em1_validate_collision_activation=[bool]$Em1ValidateCollisionActivation
$record.em1_validate_collision_reload=[bool]$Em1ValidateCollisionReload
$record.em1_compressed_archives=[bool]$Em1RegisterLevel
$record.em1_quiet_loader=[bool]$QuietEm1Loader
$record.em1_probe_disk_archives=[bool]$Em1DiskArchiveProbe
$record.em1_validate_inventory_writes=[bool]$Em1ValidateInventoryWrites
$record.distance_shadows_requested=[bool]$DistanceShadows
$record.benchmark_distance_shadows=[bool]$BenchmarkDistanceShadows
$record.shadow_pixels=[bool]$ShadowPixels
$record.shadow_depth_pixels=[bool]$ShadowDepthPixels
$record.shadow_pixel_transitions=([bool]$ShadowPixels -and [bool]$BenchmarkDistanceShadows)
$record.opaque_shadow_casters=[bool]$OpaqueShadowCasters
$record.shadow_cast_probe_mode=$ShadowCastMode
$record.trace_shadow_preparation=[bool]$TraceShadowPreparation
$record.original_render_bindings=[bool]$OriginalRenderBindings
$record.trace_shadow_gpu_state=[bool]$TraceShadowGpuState
$record.gpu_report_limit=$GpuReportGroups
$record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $root 'process.json')
$record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $workspace 'analysis/current-private-gpu-profile.json')
Write-Output ('Owned private GPU profile PID '+$game.Id+'; '+$root)
