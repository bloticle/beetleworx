[CmdletBinding()]
param([int]$GameProcessId=0,[string]$Output='',[switch]$IncludeArenaBlocks,[ValidatePattern('^[A-Fa-f0-9]{8}$')][string]$Shader='')
$ErrorActionPreference='Stop'
$workspace=Split-Path $PSScriptRoot -Parent
$executable=Join-Path $workspace 'build/runtime23/beetleworx.exe'
$candidates=@(Get-CimInstance Win32_Process -Filter "Name='beetleworx.exe' OR Name='EM2Recomp.exe'" | Where-Object {
    $_.ExecutablePath -eq $executable -and (!$GameProcessId -or $_.ProcessId -eq $GameProcessId)
})
if($candidates.Count -ne 1){throw 'Expected exactly one matching Beetleworx process'}
$game=$candidates[0]
$guestBaseOutput=& python (Join-Path $PSScriptRoot 'live_guest_base.py') $game.ProcessId (Join-Path (Split-Path $executable -Parent) 'beetleworx.map')
if($LASTEXITCODE -ne 0){throw 'Could not validate the live guest mapping'}
$guestVirtualBase=[uint64]::Parse([string]$guestBaseOutput)
# Only VM_READ is requested. No writes, thread suspension, execution or input.
Add-Type -TypeDefinition @'
using System;using System.ComponentModel;using System.Runtime.InteropServices;
public static class Em2LiveShadowReadOnly {
 [DllImport("kernel32.dll",SetLastError=true)] public static extern IntPtr OpenProcess(uint access,bool inherit,int pid);
 [DllImport("kernel32.dll",SetLastError=true)] static extern bool ReadProcessMemory(IntPtr process,IntPtr address,byte[] data,UIntPtr size,out UIntPtr read);
 [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
 public static byte[] Read(IntPtr process,ulong address,int size){if(size<1||size>4096)throw new ArgumentOutOfRangeException();var data=new byte[size];UIntPtr count;if(!ReadProcessMemory(process,(IntPtr)address,data,(UIntPtr)size,out count)||count.ToUInt64()!=(ulong)size)throw new Win32Exception(Marshal.GetLastWin32Error());return data;}
 public static uint Word(byte[] data,int offset){return ((uint)data[offset]<<24)|((uint)data[offset+1]<<16)|((uint)data[offset+2]<<8)|data[offset+3];}
}
'@
$handle=[Em2LiveShadowReadOnly]::OpenProcess(0x10,$false,$game.ProcessId)
if($handle -eq [IntPtr]::Zero){throw 'Read-only game handle unavailable'}
function Read-Guest([uint64]$Address,[int]$Size) {
    if($Address -gt 0xFFFFFFFFL -or $Address+$Size -gt 0x100000000L){throw 'Guest read exceeds address space'}
    $hostAddress=$guestVirtualBase+$Address
    if($Address -ge 0xE0000000L){$hostAddress+=4096}
    try { return ,([Em2LiveShadowReadOnly]::Read($handle,$hostAddress,$Size)) }
    catch { throw ('Guest read {0:X8} + {1}: {2}' -f $Address,$Size,$_.Exception.Message) }
}
function Read-Word([uint64]$Address){return [Em2LiveShadowReadOnly]::Word((Read-Guest $Address 4),0)}
function Words([uint64]$Address,[int]$Size) {
    $data=Read-Guest $Address $Size
    $result=[ordered]@{}
    for($offset=0;$offset -lt $Size;$offset+=4){$result["+$offset"]='{0:X8}' -f [Em2LiveShadowReadOnly]::Word($data,$offset)}
    return $result
}
try {
    if((Read-Word 0x820FAB5CL) -ne 0x829BDDB0L){throw 'Guest base/image identity did not match; refusing traversal'}
    $owner=Read-Word 0x833E12ACL
    $result=[ordered]@{utc=[DateTime]::UtcNow.ToString('o');process_id=$game.ProcessId;process_creation=$game.CreationDate;
        mode='ReadProcessMemory only; no process writes, suspension, execution or input';owner=('{0:X8}' -f $owner);targets=@();views=@()}
    $renderer=Read-Word 0x833CE200L
    if($renderer) {
        $rendererType=Read-Word $renderer
        $result.renderer=[ordered]@{address=('{0:X8}' -f $renderer);vtable=('{0:X8}' -f $rendererType);
            create_target=('{0:X8}' -f (Read-Word ($rendererType+264)));create_framebuffer=('{0:X8}' -f (Read-Word ($rendererType+312)));
            shadow_mode_method=('{0:X8}' -f (Read-Word ($rendererType+372)));shadow_material_method=('{0:X8}' -f (Read-Word ($rendererType+376)));render_mesh_method=('{0:X8}' -f (Read-Word ($rendererType+424)));
            frame_state=Read-Word ($renderer+212);lock_depth=Read-Word ($renderer+132)}
    }
    if($owner) {
        if((Read-Word $owner) -ne 0x820FAB5CL){throw 'Unknown shadow owner type; refusing traversal'}
        $before=Read-Guest ($owner+164) 24
        $result.source_slots=Words ($owner+152) 12
        for($slot=0;$slot -lt 3;++$slot) {
            $target=Read-Word ($owner+176+4*$slot)
            $entry=[ordered]@{slot=$slot;address=('{0:X8}' -f $target)}
            if($target) {
                $entry.words=Words $target 36
                $entry.native_binding_slot=Read-Word ($target+32)
                $click=Read-Word ($target+20)
                $entry.auxiliary_object=('{0:X8}' -f $click)
                if($click){
                    $clickType=Read-Word $click
                    $entry.auxiliary_object_type=('{0:X8}' -f $clickType)
                    # The known table uses 82599E80, whose deleting destructor
                    # frees 56 bytes. Adjacent RTTI strings do not name a table.
                    if($clickType -eq 0x821DE978L){
                        $entry.auxiliary_object_words=Words $click 56
                        $entry.auxiliary_object_methods=Words $clickType 88
                        # This exact table's GetRTTI implementation, 830AFB10,
                        # returns this object. Read its name pointer, not an
                        # adjacent RTTI string inferred from table placement.
                        $typeInfo=0x833FA49CL
                        $entry.auxiliary_rtti_words=Words $typeInfo 8
                        $nameAddress=Read-Word $typeInfo
                        if($nameAddress -ge 4096){
                            $nameBytes=Read-Guest $nameAddress 96
                            $nameEnd=[Array]::IndexOf($nameBytes,[byte]0)
                            if($nameEnd -gt 0){$entry.auxiliary_rtti_name=[Text.Encoding]::ASCII.GetString($nameBytes,0,$nameEnd)}
                        }
                        # Getter 830AFB30 follows +52 to +12 for width. Preserve
                        # raw storage metadata before assigning a depth role.
                        $auxData=Read-Word ($click+52)
                        if($auxData){
                            $entry.auxiliary_texture_data=('{0:X8}' -f $auxData)
                            $entry.auxiliary_texture_data_words=Words $auxData 24
                            $entry.auxiliary_width=Read-Word ($auxData+12)
                            $entry.auxiliary_height=Read-Word ($auxData+16)
                            $auxNative=Read-Word ($auxData+20)
                            if($auxNative -ge 4096){
                                try{
                                    $entry.auxiliary_native_resource=('{0:X8}' -f $auxNative)
                                    $entry.auxiliary_native_resource_words=Words $auxNative 40
                                    $auxNativeType=Read-Word $auxNative
                                    $auxHeader=0
                                    if($auxNativeType -eq 0x82055A1CL){$auxHeader=Read-Word ($auxNative+24)}
                                    elseif($auxNativeType -eq 0x82055A3CL){$auxHeader=Read-Word ($auxNative+28)}
                                    if($auxHeader){
                                        $entry.auxiliary_native_header=('{0:X8}' -f $auxHeader)
                                        $entry.auxiliary_native_header_words=Words $auxHeader 52
                                    }
                                }catch{$entry.auxiliary_native_inspection_error=$_.Exception.Message}
                            }
                        }
                    }
                }
                $renderClick=Read-Word ($target+24)
                if($renderClick) {
                    $entry.render_click=('{0:X8}' -f $renderClick)
                    $clickType=Read-Word $renderClick
                    $entry.render_click_type=('{0:X8}' -f $clickType)
                    $entry.render_click_words=Words $renderClick $(if($clickType -eq 0x8203EAA8L){152}else{104})
                    if($clickType -eq 0x8203EAA8L){
                        # Native getters 825997E8/F0/F8 expose completed click
                        # object count and cull/render timings. Generation is
                        # at +140 and the last rendered frame at +148.
                        $entry.last_rendered_objects=Read-Word ($renderClick+128)
                        $entry.shadow_generator=('{0:X8}' -f (Read-Word ($renderClick+140)))
                        $entry.last_rendered_frame=Read-Word ($renderClick+148)
                    }
                    # Native render-click list: head at +92, child at node+8.
                    # Inspect only the canonical single-child layout.
                    if((Read-Word ($renderClick+100)) -eq 1) {
                        $node=Read-Word ($renderClick+92)
                        if($node) {
                            $map=Read-Word ($node+8)
                            if($map -and (Read-Word $map) -eq 0x8203EA6CL) {
                                $entry.binding_child=('{0:X8}' -f $map)
                                $entry.binding_child_words=Words $map 72
                                $camera=Read-Word ($map+44)
                                if($camera -and (Read-Word $camera) -eq 0x8203E610L){$entry.shadow_camera=('{0:X8}' -f $camera);$entry.shadow_camera_words=Words $camera 352;$entry.shadow_camera_methods=Words 0x8203E610L 32}
                            }
                        }
                    }
                }
                $surface=Read-Word ($target+16)
                $entry.surface=('{0:X8}' -f $surface)
                if($surface) {
                    $entry.surface_words=Words $surface 56
                    $data=Read-Word ($surface+52)
                    if($data){
                        $entry.width=Read-Word ($data+12);$entry.height=Read-Word ($data+16);$entry.renderer_data_words=Words $data 24
                        # Original 8267C260 attaches native texture data at the
                        # dimensions block +20; native type 82055A1C owns its header at +24.
                        $nativeTexture=Read-Word ($data+20)
                        if($nativeTexture){
                            $entry.native_texture=('{0:X8}' -f $nativeTexture)
                            $entry.native_texture_type=('{0:X8}' -f (Read-Word $nativeTexture))
                            if((Read-Word $nativeTexture) -eq 0x82055A1CL){
                                $entry.native_surface_words=Words $nativeTexture 28
                                $nativeSurface=Read-Word ($nativeTexture+24)
                                if($nativeSurface){$entry.native_surface_header=('{0:X8}' -f $nativeSurface);$entry.native_surface_header_words=Words $nativeSurface 52}
                            }
                            if((Read-Word $nativeTexture) -eq 0x82055A3CL){
                                $entry.native_texture_words=Words $nativeTexture 40
                                $header=Read-Word ($nativeTexture+28)
                                if($header){$entry.native_texture_header=('{0:X8}' -f $header);$entry.native_texture_header_words=Words $header 52}
                            }
                        }
                    }
                    $framebuffer=Read-Word ($surface+40)
                    if($framebuffer){$entry.framebuffer_words=Words $framebuffer 104}
                }
            }
            $result.targets+=,$entry
            $view=Read-Word ($owner+164+4*$slot)
            $viewEntry=[ordered]@{slot=$slot;address=('{0:X8}' -f $view)}
            if($view){
                $viewEntry.words=Words $view 108
                $material=Read-Word ($view+16)
                if($material){$viewEntry.shadow_material=('{0:X8}' -f $material);$viewEntry.shadow_material_words=Words $material 32;$viewEntry.shadow_material_methods=Words (Read-Word $material) 32}
                # The native generator constructor retains its effect at +80;
                # do not confuse it with the main gameplay camera or a cascade.
                $effect=Read-Word ($view+80)
                $viewEntry.effect=('{0:X8}' -f $effect)
                if($effect){
                    $viewEntry.effect_words=Words $effect 456
                    $viewEntry.effect_vtable=Words (Read-Word $effect) 32
                }
            }
            $result.views+=,$viewEntry
        }
        $after=Read-Guest ($owner+164) 24
        $result.owner_slots_unchanged=([Convert]::ToBase64String($before) -eq [Convert]::ToBase64String($after)) -and ((Read-Word 0x833E12ACL) -eq $owner)
        $manager=Read-Word 0x833CDD90L
        if($manager){$result.target_manager=[ordered]@{address=('{0:X8}' -f $manager);used_bytes=Read-Word ($manager+172);budget_bytes=Read-Word ($manager+184);reclaim_callback=('{0:X8}' -f (Read-Word ($manager+176)));binding_source=('{0:X8}' -f (Read-Word ($manager+264)));binding_slots=Words ($manager+268) 12}}
        $source=if($manager){Read-Word ($manager+264)}else{0}
        if($source -and (Read-Word ($source+8)) -eq 0x8203EB5CL){
            # These bounds/offsets are from native 8256F948 and 8256E7E0.
            # This is a moving read-only snapshot, not an atomic frame capture.
            $last=Read-Word ($source+204320)
            if($last -le 6000){
                $entries=[Math]::Min(6000,$last+1)
                $flags=[byte[]]::new($entries*2)
                for($offset=0;$offset -lt $flags.Length;$offset+=4096){
                    $part=Read-Guest ($source+192292+$offset) ([Math]::Min(4096,$flags.Length-$offset))
                    [Array]::Copy($part,0,$flags,$offset,$part.Length)
                }
                $result.visibility_source=[ordered]@{address=('{0:X8}' -f $source);registered=Read-Word ($source+204292);last_index=$last;slots=@()}
                for($slot=0;$slot -lt 5;++$slot){
                    $mask=Read-Word (0x83395098L+$slot*4);$visible=0;$eligible=0;$candidates=@()
                    for($i=0;$i -lt $entries;++$i){
                        $bits=([int]$flags[$i*2] -shl 8) -bor [int]$flags[$i*2+1]
                        if($bits -band $mask){
                            ++$visible
                            if(!($bits -band 1024)){
                                ++$eligible
                                if($slot -ge 2 -and $candidates.Count -lt 32){
                                    $object=Read-Word ($source+36+$i*4)
                                    $candidate=[ordered]@{index=$i;flags=('{0:X4}' -f $bits);object=('{0:X8}' -f $object)}
                                    if($object){
                                        $type=Read-Word $object;$candidate.type=('{0:X8}' -f $type)
                                        if($type -ge 0x82000000L -and $type -lt 0x83400000L){
                                            $predicate=Read-Word ($type+184);$candidate.render_predicate=('{0:X8}' -f $predicate);$candidate.collect=('{0:X8}' -f (Read-Word ($type+192)));$candidate.render_callback=('{0:X8}' -f (Read-Word ($type+208)))
                                            if($predicate -eq 0x825F5258L){
                                                $candidate.disabled=(Read-Guest ($object+414) 1)[0]
                                                $data=Read-Word ($object+284);$candidate.property_state=('{0:X8}' -f $data)
                                                if($data){
                                                    $candidate.property_flags=('{0:X8}' -f (Read-Word ($data+16)))
                                                    $candidate.material_mode=Read-Word ($data+380)
                                                    $candidate.shadow_inputs=[ordered]@{}
                                                    foreach($field in @(116,120,152,156)){$candidate.shadow_inputs["+$field"]=(Read-Guest ($data+$field) 1)[0]}
                                                    $candidate.shadow_inputs['+180']=Read-Word ($data+180)
                                                }
                                            }
                                        }
                                    }
                                    $candidates+=,$candidate
                                }
                            }
                        }
                    }
                    $binding=Read-Word ($source+12+$slot*4)
                    $entry=[ordered]@{slot=$slot;mask=('{0:X4}' -f $mask);visible_flags=$visible;eligible_flags=$eligible;binding=('{0:X8}' -f $binding);dirty=(Read-Guest ($source+204324+$slot) 1)[0]}
                    $entry.candidates=$candidates
                    if($binding){$type=Read-Word $binding;$entry.type=('{0:X8}' -f $type);if($type -ge 0x82000000L -and $type -lt 0x83400000L){$entry.methods=Words $type 60}}
                    $result.visibility_source.slots+=,$entry
                }
            }
        }
        $arena=0x833D2760L+32128
        $result.physical_arena=[ordered]@{address=('{0:X8}' -f $arena);vtable=('{0:X8}' -f (Read-Word $arena));head=('{0:X8}' -f (Read-Word ($arena+8)));used=Read-Word ($arena+12);total=Read-Word ($arena+20)}
        if($IncludeArenaBlocks) {
            $result.physical_arena.blocks_consistent=$false
            try {
                $head=Read-Word ($arena+8);$node=$head;$sum=0L;$free=0L;$large=@();$visited=[Collections.Generic.HashSet[uint32]]::new()
                do {
                    if(!$node -or $visited.Count -ge 65536 -or !$visited.Add($node)){throw 'Invalid or changing arena node chain'}
                    $block=Read-Guest $node 24
                    $offset=[Em2LiveShadowReadOnly]::Word($block,0);$size=[Em2LiveShadowReadOnly]::Word($block,4)
                    if(!$size -or $offset -ne $sum -or $sum+$size -gt $result.physical_arena.total){throw 'Noncontiguous or changing arena blocks'}
                    $isFree=$block[11] -ne 0
                    if($isFree){$free+=$size}
                    if($size -ge 0x1000000L){$large+=,[ordered]@{offset=$offset;bytes=$size;free=$isFree}}
                    $sum+=$size;$node=[Em2LiveShadowReadOnly]::Word($block,20)
                }while($node -ne $head)
                $result.physical_arena.block_count=$visited.Count
                $result.physical_arena.large_blocks=$large
                $result.physical_arena.blocks_consistent=($sum -eq $result.physical_arena.total -and $sum-$free -eq $result.physical_arena.used -and (Read-Word ($arena+12)) -eq $result.physical_arena.used -and (Read-Word ($arena+8)) -eq $head)
            }catch{$result.physical_arena.block_read_error=$_.Exception.Message}
        }
    }
    if($Shader){
        $shaderAddress=[Convert]::ToUInt32($Shader,16)
        $result.diagnostic_shader=[ordered]@{address=$Shader;words=Words $shaderAddress $(if((Read-Word $shaderAddress) -eq 0x821DD178L){156}else{64});methods=Words (Read-Word $shaderAddress) 80}
        if((Read-Word $shaderAddress) -eq 0x821DD178L){
            $name=Read-Word ($shaderAddress+12)
            if($name){$result.diagnostic_shader.name=[Text.Encoding]::UTF8.GetString((Read-Guest $name 128)).Split([char]0)[0]}
        }
    }
    if(!$Output){$Output=Join-Path $workspace ('analysis/live-shadow-readonly-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'.json')}
    if(Test-Path -LiteralPath $Output){throw 'Output already exists; use a new evidence path'}
    $result | ConvertTo-Json -Depth 9 | Set-Content -LiteralPath $Output
    Write-Output $Output
} finally {[void][Em2LiveShadowReadOnly]::CloseHandle($handle)}
