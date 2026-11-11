"""Read-only extraction of pinned material lighting programs; bytecode stays private."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import struct
ROOT = Path(__file__).resolve().parents[1]
PINNED = {
    'Gen3_Char_Oswald_LOD0': '8d5231a1f13139422683be1dc4789709bae02602997549691e79ebe35d4e82e7',
    'Gen3_Char_Oswald': '32e944d2fb96c7bab739762ee13d11a2ca0fe7f0a14c54fc25fb3a26ee41dcda',
}
def split_lighting(words,kernel):
    groups=[]
    for pair in range(8):
        a,b,c=words[pair*3:pair*3+3]
        for cf in (a|((b&65535)<<32),(b>>16)|(c<<16)):
            op=cf>>44
            if op==12:
                if (cf>>41)&3!=2 or cf&7:raise ValueError('Unexpected colour allocation')
                groups.append((cf|2,[]))
            elif op in (1,2):
                address,count,sequence=cf&4095,(cf>>12)&7,(cf>>16)&4095
                groups.append((cf&~((1<<28)-1),[((sequence>>(2*i))&3,tuple(words[(address+i)*3:(address+i+1)*3])) for i in range(count)]))
            else:raise ValueError('Unexpected lighting control flow')
    if [w for _,items in groups for _,ins in items for w in ins]!=list(words[24:282]):raise ValueError('Original arithmetic extent changed')
    ambient=kernel['alu'](1,15,2,0,0);ambient=(ambient[0]|0x8000|(8<<20),ambient[1],ambient[2])
    specular=kernel['alu'](2,15,1,5,1,0xC0,0xC0);specular=(specular[0]|0x8000|(8<<20),specular[1],specular[2])
    cf,items=groups[-1];groups[-1]=((cf&~(15<<44))|(1<<44),items+[(0,ambient)])
    groups.extend([((cf&~(15<<44))|(2<<44),[(0,specular)]),(0,[])])
    control=[];body=[]
    for cf,items in groups:
        if items:
            if len(items)>6:raise ValueError('Too many serialized ALUs')
            cf|=len(groups)//2+len(body)//3;cf|=len(items)<<12;cf|=sum(s<<(16+2*i) for i,(s,_) in enumerate(items))
            for _,ins in items:body.extend(ins)
        control.append(cf)
    output=[]
    for a,b in zip(control[::2],control[1::2]):output.extend((a&0xffffffff,(a>>32)|((b&65535)<<16),b>>16))
    if body[:258]!=list(words[24:282]):raise ValueError('Split changed original executable ALUs')
    return output+body
def full_lighting_difference(words,kernel):
    """Diagnostic dual evaluation; exact SH-disabled baseline through nonlinear lighting."""
    if len(words)!=510 or tuple(words[504:507])!=(0xC88FC000,0x00C0B4B4,0xEB020100):raise ValueError('Full material final combine changed')
    groups=[]
    for pair in range(14):
        a,b,c=words[pair*3:pair*3+3]
        for cf in (a|((b&65535)<<32),(b>>16)|(c<<16)):
            op=cf>>44
            if op==0:continue
            if op==12:groups.append((cf,[]));continue
            if op not in (1,2):raise ValueError('Full material has unsupported control flow')
            address,count,sequence=cf&4095,(cf>>12)&7,(cf>>16)&4095
            groups.append((cf&~((1<<28)-1),[((sequence>>(2*i))&3,tuple(words[(address+i)*3:(address+i+1)*3])) for i in range(count)]))
    original=[ins for _,items in groups for _,ins in items]
    if [w for ins in original for w in ins]!=list(words[42:507]):raise ValueError('Full executable extent changed')
    template=(groups[0][0]&~(15<<44))|(1<<44)
    def clauses(items,end=False):
        out=[]
        for start in range(0,len(items),6):out.append((template,[(0,i) for i in items[start:start+6]]))
        if end:out[-1]=((out[-1][0]&~(15<<44))|(2<<44),out[-1][1])
        return out
    def capture_colour(register):
        a,b,c=original[-1]
        # Same original MAD into a private temporary; disable scalar/export masks.
        return ((a&~(0xFFFF|(15<<20)))|register,b,c)
    result=clauses([kernel['copy'](32+i,i) for i in range(8)])
    for cf,items in groups:
        if cf>>44==12:
            if (cf>>41)&3!=2 or cf&7:raise ValueError('Unexpected full colour allocation')
            result.append((cf|2,items))
        else:result.append(((cf&~(15<<44))|(1<<44),items))
    result+=clauses([capture_colour(40)]+[kernel['copy'](i,32+i) for i in range(8)])
    remapped=0
    for cf,items in groups:
        if cf>>44==12:continue
        zero_items=[]
        for sequence,ins in items:
            a,b,c=ins
            if sequence&1:zero_items.append((sequence,ins));continue
            for source in range(3):
                shift=16-8*source;select=31-source;reg=(c>>shift)&255
                if not (c>>select)&1 and 16<=reg<=24:
                    c=(c&~(255<<shift))|(252<<shift);b=(b&~(255<<shift))|(kernel['broadcast'](3)<<shift);remapped+=1
            if a&0x8000:
                if a&63:raise ValueError('Unexpected extra original colour export')
                a=(a&~63)|1
            zero_items.append((sequence,(a,b,c)))
        result.append(((cf&~(15<<44))|(1<<44),zero_items))
    if remapped<9:raise ValueError('SH read remapping incomplete')
    delta=kernel['alu'](2,15,0,40,41,b_negate=True);delta=(delta[0]|0x8000|(8<<20),delta[1],delta[2])
    result+=clauses([capture_colour(41),delta],end=True)
    if len(result)&1:result.append((0,[]))
    control=[];body=[]
    for cf,items in result:
        if items:
            cf|=len(result)//2+len(body)//3;cf|=len(items)<<12;cf|=sum(s<<(16+2*i) for i,(s,_) in enumerate(items))
            for _,ins in items:body.extend(ins)
        control.append(cf)
    code=[]
    for a,b in zip(control[::2],control[1::2]):code.extend((a&0xffffffff,(a>>32)|((b&65535)<<16),b>>16))
    return code+body
def full_suffix_difference(words,kernel):
    """Repeat only the final nonlinear combine, sharing all original texture reads.

    In this pinned program, entry to slot146 consumes SH through r1 and r11.xy.
    r0.xy/w still contain SH scratch, but are overwritten before being read;
    r0.z is the unchanged view/normal term. Keep the whole original path and
    restore its pre-combine inputs before clearing those SH components.
    """
    if len(words)!=510 or tuple(words[504:507])!=(0xC88FC000,0x00C0B4B4,0xEB020100):
        raise ValueError('Full material final combine changed')
    if hashlib.sha256(struct.pack('>510I',*words)).hexdigest()!='9053a8530dbbe152771c1106b1a57bd4b6b1eb58a36ae3e1ac347422b8b935a7':
        raise ValueError('Unrecognized full material checkpoint dataflow')
    groups=[]
    for pair in range(14):
        a,b,c=words[pair*3:pair*3+3]
        for cf in (a|((b&65535)<<32),(b>>16)|(c<<16)):
            op=cf>>44
            if op==0:continue
            if op==12:
                if (cf>>41)&3!=2 or cf&7:raise ValueError('Unexpected colour allocation')
                groups.append((cf|2,[]));continue
            if op not in (1,2):raise ValueError('Unsupported full material control flow')
            address,count,sequence=cf&4095,(cf>>12)&7,(cf>>16)&4095
            for i in range(count):
                index=address+i
                if index==146:
                    for register in range(13):
                        if register!=1:groups.append((1<<44,[(0,kernel['copy'](32+register,register))]))
                groups.append((1<<44,[((sequence>>(2*i))&3,tuple(words[index*3:index*3+3]))]))
    def append(ins):groups.append((1<<44,[(0,ins)]))
    def colour(register):
        a,b,c=words[504:507]
        return ((a&~(0xFFFF|(15<<20)))|register,b,c)
    append(colour(45))
    for register in range(13):
        if register!=1:append(kernel['copy'](register,32+register))
    for register,mask in ((1,15),(11,3)):
        a,b,c=kernel['alu'](register,mask,2,252,252,kernel['broadcast'](3),kernel['broadcast'](3),True)
        append((a,b,c&~(1<<31)))
    for index in range(146,169):
        a,b,c=words[index*3:index*3+3]
        if (a&0x40) or (not(a&0x8000) and a&0x4000) or (b&0xF0000000):raise ValueError('Relative or predicated suffix')
        if (a>>26) in (1,3,4,26) or 51<=(a>>26):raise ValueError('Previous-scalar dependency in suffix')
        if index==168:a=(a&~63)|1
        append((a,b,c))
    append(colour(46))
    a,b,c=kernel['alu'](2,15,0,45,46,b_negate=True)
    append((a|0x8000|(8<<20),b,c))
    # Pack linear clauses, preserving every original ALU/fetch sequence bit.
    packed=[]
    for cf,items in groups:
        if items and packed and packed[-1][0]==cf and len(packed[-1][1])<6:
            packed[-1][1].extend(items)
        else:packed.append((cf,list(items)))
    cf,items=packed[-1];packed[-1]=((2<<44),items)
    if len(packed)&1:packed.append((0,[]))
    control=[];body=[]
    for cf,items in packed:
        if items:
            cf|=len(packed)//2+len(body)//3
            cf|=len(items)<<12
            cf|=sum(seq<<(16+2*i) for i,(seq,_) in enumerate(items))
            for _,ins in items:body.extend(ins)
        control.append(cf)
    code=[]
    for a,b in zip(control[::2],control[1::2]):code.extend((a&0xffffffff,(a>>32)|((b&65535)<<16),b>>16))
    return code+body
def world_baked_difference(words,kernel,suffix=True,base=False,surface=False):
    """Pinned diagnostic: preserve colour0, export black-baked-vertex-colour baseline/delta.

    Entry r5 RGB is removed in the baseline; alpha and all sampled
    directional/light/shadow resources stay intact. This is a baked-light-dependent residual, not a
    decomposition of baked direct versus indirect irradiance.
    """
    expected='331d6281ba39dbaa3f18b108f1c0d0b90e50a8006cbea92be06252b196025b75' if base else 'ccb4ef7bb578b61f2fc0e0f775930dfef0ad9c1c672c3dfc45918dc5fbd17994'
    if len(words)!=(192 if base else 369) or hashlib.sha256(struct.pack(f'>{len(words)}I',*words)).hexdigest()!=expected:raise ValueError('Unrecognized environment shader')
    pairs,first,last,checkpoint_slot=(6,6,63,54) if base else (10,10,122,106)
    zero_channel=0 if base else 1
    if surface and not base:raise ValueError("Combined surface capture is pinned to base environment")
    groups=[];exports=[];fetches=0
    for pair in range(pairs):
        a,b,c=words[pair*3:pair*3+3]
        for cf in (a|((b&65535)<<32),(b>>16)|(c<<16)):
            op=cf>>44
            if op==0:continue
            if op==12:
                if (cf>>41)&3!=2 or cf&7:raise ValueError('Unexpected environment export allocation')
                groups.append((cf|(3 if surface else 2),[]));continue
            if op not in (1,2):raise ValueError('Unsupported environment control flow')
            address,count,sequence=cf&4095,(cf>>12)&7,(cf>>16)&4095
            items=[]
            for i in range(count):
                seq=(sequence>>(2*i))&3;ins=tuple(words[(address+i)*3:(address+i+1)*3]);a,b,c=ins
                if seq&1:
                    if (a&31)!=1 or ((a>>5)&63)>=32 or ((a>>12)&63)>=32:raise ValueError('Unsupported environment fetch')
                    if ((a>>20)&31)==0:
                        if b&4095!=0x688:raise ValueError('Unexpected lightmap fetch swizzle')
                        fetches+=1
                else:
                    if a&0x8000:
                        if a&63:raise ValueError('Extra environment colour export')
                        exports.append(ins)
                    elif a&63>=32 or (a>>8)&63>=32:raise ValueError('Environment temporary overlaps saved inputs')
                    for source in range(3):
                        if (c>>(31-source))&1 and ((c>>(16-8*source))&127)>=32:raise ValueError('Environment source overlaps saved inputs')
                items.append((seq,ins))
            groups.append(((cf&~((1<<28)-1)&~(15<<44))|(1<<44),items))
    if fetches!=1 or len(exports)!=1 or groups[-1][1][-1][1]!=exports[0] or [w for _,items in groups for _,ins in items for w in ins]!=list(words[first*3:last*3]):raise ValueError('Environment executable extent changed')
    result=[]
    def emit(ins,seq=0):result.append((1<<44,[(seq,ins)]))
    def capture(reg):
        a,b,c=exports[0];return ((a&~(0xFFFF|(15<<20)))|reg,b,c)
    if suffix:
        # Before the checkpoint, baked r5 RGB has not entered any lighting ALU.
        # Only live suffix inputs are saved; no previous-scalar read
        # precedes its explicit scalar writes. Keep the original first path.
        saved_regs=(0,1,5,6,7) if base else (0,1,2,3,4,5,6,7,9)
        cursor=first;checkpoint=False
        for cf,items in groups:
            if items and cursor==checkpoint_slot:
                for reg in saved_regs:emit(kernel['copy'](32+reg,reg))
                checkpoint=True
            result.append((cf,items));cursor+=len(items)
            if surface and items and cursor==18:emit(kernel["copy"](54,6))
        if not checkpoint:raise ValueError('Environment suffix checkpoint changed')
        emit(capture(48))
        for reg in saved_regs:emit(kernel['copy'](reg,32+reg))
        a,b,c=kernel['alu'](5,7,2,252,252,kernel['broadcast'](zero_channel),kernel['broadcast'](zero_channel),True)
        emit((a,b,c&~(1<<31)))
        for index in range(checkpoint_slot,last):
            a,b,c=words[index*3:index*3+3]
            if a&0x8000:a=(a&~63)|1
            emit((a,b,c))
        emit(capture(49))
        a,b,c=kernel['alu'](2,15,0,48,49,b_negate=True);emit((a|0x8000|(8<<20),b,c))
    else:
        for reg in range(16):emit(kernel['copy'](32+reg,reg))
        # Both evaluations have the same pre-fetch coordinates. Keep each original
        # returned GPR (including preserved channels) and reuse it in the baseline.
        # The pinned shader has its four fetches together before any lighting ALU.
        saved_fetches=[]
        for cf,items in groups:
            if cf>>44==12:result.append((cf,items));continue
            for seq,ins in items:
                emit(ins,seq)
                if seq&1:
                    saved=50+len(saved_fetches);saved_fetches.append(saved)
                    emit(kernel['copy'](saved,(ins[0]>>12)&63))
                    if surface and (ins[0]>>12)&63==6:emit(kernel["copy"](54,6))
        if len(saved_fetches)!=4:raise ValueError('Environment fetch group changed')
        emit(capture(48))
        for reg in range(16):emit(kernel['copy'](reg,32+reg))
        a,b,c=kernel['alu'](5,7,2,252,252,kernel['broadcast'](zero_channel),kernel['broadcast'](zero_channel),True)
        emit((a,b,c&~(1<<31)))
        fetch_index=0
        for cf,items in groups:
            if cf>>44==12:continue
            for seq,ins in items:
                a,b,c=ins
                if not seq&1 and a&0x8000:a=(a&~63)|1
                if seq&1:
                    emit(kernel['copy']((a>>12)&63,saved_fetches[fetch_index]));fetch_index+=1
                else:emit((a,b,c),seq)
        emit(capture(49))
        a,b,c=kernel['alu'](2,15,0,48,49,b_negate=True);emit((a|0x8000|(8<<20),b,c))
    if surface:
        a,b,c=kernel['alu'](54,8,2,5,5,kernel['broadcast'](3),kernel['broadcast'](3));emit((a,b,c))
        a,b,c=kernel['copy'](3,54);emit((a|0x8000,b,c))
    cf,items=result[-1];result[-1]=((cf&~(15<<44))|(2<<44),items)
    if len(result)&1:result.append((0,[]))
    control=[];body=[]
    for cf,items in result:
        if items:
            cf|=len(result)//2+len(body)//3;cf|=len(items)<<12;cf|=sum(s<<(16+2*i) for i,(s,_) in enumerate(items))
            for _,ins in items:body.extend(ins)
        control.append(cf)
    out=[]
    for a,b in zip(control[::2],control[1::2]):out.extend((a&0xffffffff,(a>>32)|((b&65535)<<16),b>>16))
    return out+body

def world_base_surface(words,kernel):
    # Exact sampled diffuse RGB and native effective exponent, without lighting.
    if len(words)!=192 or hashlib.sha256(struct.pack('>192I',*words)).hexdigest()!='331d6281ba39dbaa3f18b108f1c0d0b90e50a8006cbea92be06252b196025b75':raise ValueError('Unrecognized base surface material')
    groups=[];found=False
    for pair in range(6):
        a,b,c=words[pair*3:pair*3+3]
        for cf in (a|((b&65535)<<32),(b>>16)|(c<<16)):
            op=cf>>44
            if op==0:continue
            if op==12:
                if (cf>>41)&3!=2 or cf&7:raise ValueError('Unexpected surface allocation')
                groups.append((cf|1,[]));continue
            if op not in (1,2):raise ValueError('Unsupported surface control flow')
            address,count,sequence=cf&4095,(cf>>12)&7,(cf>>16)&4095
            items=[((sequence>>(2*i))&3,tuple(words[(address+i)*3:(address+i+1)*3])) for i in range(count)]
            groups.append(((cf&~((1<<28)-1)&~(15<<44))|(1<<44),items))
            if address+count==18:
                if not items[-1][0]&1 or (items[-1][1][0]>>12)&63!=6:raise ValueError('Diffuse fetch changed')
                groups.append((1<<44,[(0,kernel['copy'](54,6))]));found=True
    if not found:raise ValueError('Surface diffuse checkpoint missing')
    # Original slot21 writes specPower * sampled diffuse alpha to r5.w;
    # the pinned program retains this value through its final colour export.
    a,b,c=kernel['alu'](54,8,2,5,5,kernel['broadcast'](3),kernel['broadcast'](3))
    groups.append((1<<44,[(0,(a,b,c))]))
    a,b,c=kernel['copy'](1,54);groups.append((2<<44,[(0,(a|0x8000,b,c))]))
    if len(groups)&1:groups.append((0,[]))
    control=[];body=[]
    for cf,items in groups:
        if items:
            cf|=len(groups)//2+len(body)//3;cf|=len(items)<<12;cf|=sum(seq<<(16+2*i) for i,(seq,_) in enumerate(items))
            for _,ins in items:body.extend(ins)
        control.append(cf)
    out=[]
    for a,b in zip(control[::2],control[1::2]):out.extend((a&0xffffffff,(a>>32)|((b&65535)<<16),b>>16))
    return out+body

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-dir',type=Path,required=True)
    args=parser.parse_args()
    entries=runpy.run_path(str(ROOT/'scripts/game-pak.py'))['read_pak'](args.game_dir/'packfiles/shaders.pak')
    extract=runpy.run_path(str(ROOT/'scripts/inspect-scene-shaders.py'))['extract']
    report={};lines=['#pragma once','#include <array>','#include <cstdint>','namespace em2::material_shader_data {']
    for name,digest in PINNED.items():
        matches=[data for path,data,kind in entries if path==f'cookedshaders/360/{name}.fxlo']
        if len(matches)!=1 or hashlib.sha256(matches[0]).hexdigest()!=digest:raise ValueError(f'Unrecognized material {name}')
        programs=extract(matches[0])
        if len(programs)!=1:raise ValueError('Unexpected material permutation count')
        p=programs[0];words=p.pop('words')
        bindings={c['name']:(c['register_set'],c['index'],c['count']) for c in p['constants']}
        start=4 if name.endswith('LOD0') else 16
        if any(bindings.get(f'SHCoeff{i}')!=(2,start+i,1) for i in range(9)):raise ValueError('SH constant ABI changed')
        if any(bindings.get(n)!=(3,i,1) for i,n in enumerate(('diffuseMapSampler','normalMapSampler','specularMapSampler'))):raise ValueError('Material texture ABI changed')
        lines.append(f'inline constexpr std::array<uint32_t,{len(words)}> {name}={{'+','.join(f'0x{x:08x}u' for x in words)+'};')
        if name.endswith('LOD0'):
            if len(words)!=285 or tuple(words[279:282])!=(0xC88FC000,0x00C0C0C0,0xEB050100):raise ValueError('Native final lighting combine changed')
            kernel=runpy.run_path(str(ROOT/'scripts/shadow-kernel.py'))
            ambient=list(words);export=kernel['alu'](0,15,2,0,0);ambient[279:282]=(export[0]|0x8000|(8<<20),export[1],export[2])
            specular=list(words);specular[280]=(specular[280]&~255)|kernel['broadcast'](2);specular[281]=(specular[281]&~(255|(1<<29)))|252
            for probe,code in (('ambient_probe',ambient),('specular_probe',specular)):
                if code[:279]!=list(words[:279]) or code[282:]!=list(words[282:]):raise ValueError('Probe changed original pre-combine arithmetic')
                lines.append(f'inline constexpr std::array<uint32_t,{len(code)}> {probe}={{'+','.join(f'0x{x:08x}u' for x in code)+'};')
            split=split_lighting(words,kernel)
            lines.append(f'inline constexpr std::array<uint32_t,{len(split)}> split_lighting={{'+','.join(f'0x{x:08x}u' for x in split)+'};')
            literal=struct.unpack_from('>16I',matches[0],p['code_offset']-64)
            if literal[2]!=0:raise ValueError('Specular probe zero literal changed')
            lines.append('inline constexpr std::array<uint32_t,16> lod_literals={'+','.join(f'0x{x:08x}u' for x in literal)+'};')
        else:
            vertices=extract(matches[0],False)
            if len(vertices)!=1 or vertices[0]['code_sha256']!='74e0379ba8a80f728f4783c171b313a7b85088c71b593ea6adcf40da369b0c51':raise ValueError('Original full vertex shader changed')
            vertex=vertices[0];original_vertex=vertex.pop('words');linked=list(original_vertex)
            vertex_bindings={c['name']:(c['register_set'],c['index'],c['count']) for c in vertex['constants']}
            if vertex_bindings.get('EyePosObjectSpace')!=(2,0,1) or vertex_bindings.get('WorldViewProjection')!=(2,15,4):raise ValueError('Vertex float constant ABI changed')
            # Runtime-style declaration linking only: position3, UV2, normal3,
            # tangent4 in a controlled 48-byte stream. Original ALU is untouched.
            for slot,offset,fmt in ((7,0,57),(8,3,37),(9,5,57),(10,8,38)):
                if linked[slot*3+2]!=0:raise ValueError('Vertex fetch already linked')
                linked[slot*3+1]=(linked[slot*3+1]&~0x3F3FF000)|(fmt<<16)|(1<<13)
                linked[slot*3+2]=(linked[slot*3+2]&0x80000000)|12|(offset<<8)
            for label,program in (('full_vertex_original',original_vertex),('full_vertex_linked',linked)):
                lines.append(f'inline constexpr std::array<uint32_t,{len(program)}> {label}={{'+','.join(f'0x{x:08x}u' for x in program)+'};')
            vertex_literals=struct.unpack_from('>4I',matches[0],vertex['code_offset']-16)
            if vertex_literals!=(0x3F800000,0x40000000,0x3E4CCCCD,0x40400000):raise ValueError('Vertex literals changed')
            lines.append('inline constexpr std::array<uint32_t,4> full_vertex_literals={'+','.join(f'0x{x:08x}u' for x in vertex_literals)+'};')
            (ROOT/'analysis/material-full-vertex.json').write_text(json.dumps(vertex,indent=2)+'\n')
            kernel=runpy.run_path(str(ROOT/'scripts/shadow-kernel.py'))
            code=full_lighting_difference(words,kernel)
            lines.append(f'inline constexpr std::array<uint32_t,{len(code)}> full_difference={{'+','.join(f'0x{x:08x}u' for x in code)+'};')
            suffix=full_suffix_difference(words,kernel)
            lines.append(f'inline constexpr std::array<uint32_t,{len(suffix)}> full_suffix_difference={{'+','.join(f'0x{x:08x}u' for x in suffix)+'};')
            literal=struct.unpack_from('>16I',matches[0],p['code_offset']-64)
            if literal[3]!=0:raise ValueError('Full material zero literal changed')
            lines.append('inline constexpr std::array<uint32_t,16> full_literals={'+','.join(f'0x{x:08x}u' for x in literal)+'};')
        report[name]={'effect_sha256':digest,'program':p}
    # The normal Mickey pixel program shares the proven full-material executable
    # extent. Its separate effect/program identities remain pinned in full.
    mickey_name='Gen3_Char_Mickey'
    mickey_digest='7b01975e66f277b559790ad41e5ea3e80493fa155099675e664c43347c9007f0'
    mickey_blob=[data for path,data,kind in entries if path==f'cookedshaders/360/{mickey_name}.fxlo']
    if len(mickey_blob)!=1 or hashlib.sha256(mickey_blob[0]).hexdigest()!=mickey_digest:raise ValueError('Unrecognized full Mickey effect')
    mickey_programs=extract(mickey_blob[0])
    if len(mickey_programs)!=2 or len(mickey_programs[0]['words'])!=9 or mickey_programs[1]['code_sha256']!='1e09de9e9e74853085664ee1a832232cacf351c4fd33a9d8f90abdb6ab25dcba':raise ValueError('Mickey material permutations changed')
    mickey=mickey_programs[1];mickey_words=mickey.pop('words')
    oswald=extract(next(data for path,data,kind in entries if path=='cookedshaders/360/Gen3_Char_Oswald.fxlo'))[0]
    if len(mickey_words)!=510 or mickey_words[:507]!=oswald['words'][:507] or mickey['constants']!=oswald['constants']:raise ValueError('Mickey full executable/constant ABI differs')
    mickey_literals=struct.unpack_from('>16I',mickey_blob[0],mickey['code_offset']-64)
    oswald_blob=next(data for path,data,kind in entries if path=='cookedshaders/360/Gen3_Char_Oswald.fxlo')
    if mickey_literals!=struct.unpack_from('>16I',oswald_blob,oswald['code_offset']-64):raise ValueError('Mickey pixel literals differ')
    mickey_vertices=extract(mickey_blob[0],False)
    if len(mickey_vertices)!=2 or mickey_vertices[1]['code_sha256']!='4edbf654e702722411064af34b0b1ca40151598d57c755068c51e648eaf9f01a':raise ValueError('Mickey full vertex identity changed')
    mickey_vertex=mickey_vertices[1];mickey_vs=mickey_vertex.pop('words')
    for label,code in ((mickey_name,mickey_words),('mickey_full_vertex_original',mickey_vs)):
        lines.append(f'inline constexpr std::array<uint32_t,{len(code)}> {label}={{'+','.join(f'0x{x:08x}u' for x in code)+'};')
    report[mickey_name]={'effect_sha256':mickey_digest,'program':mickey,'vertex':mickey_vertex,'shared_executable_words':507,'scope':'Normal full pixel permutation only; outline and other material variants are not substituted. Full Mickey vertex differs and remains original.'}
    world_name='Gen3_StaticEnv_DL';world_digest='9d7aa1c0949ceb473a268bc7b8c11cde52dab2a851250851c532bc7245987eb1'
    world_blobs=[data for path,data,kind in entries if path==f'cookedshaders/360/{world_name}.fxlo']
    if len(world_blobs)!=1 or hashlib.sha256(world_blobs[0]).hexdigest()!=world_digest:raise ValueError('Unrecognized environment material')
    world_programs=extract(world_blobs[0])
    if len(world_programs)!=1:raise ValueError('Environment permutations changed')
    world=world_programs[0];world_words=world.pop('words')
    world_literals=struct.unpack_from('>20I',world_blobs[0],world['code_offset']-80)
    if world_literals[5]!=0:raise ValueError('Environment c252.y zero literal changed')
    world_diff=world_baked_difference(world_words,kernel)
    world_dual=world_baked_difference(world_words,kernel,False)
    world_vertices=extract(world_blobs[0],False)
    if len(world_vertices)!=1 or world_vertices[0]['code_sha256']!='3554aba3dc714dee4ada3e13e3ab7b33eaaa451250e8ffdfb864bb743d860f52':raise ValueError('Environment vertex identity changed')
    world_vertex=world_vertices[0];world_vs=world_vertex.pop('words')
    for label,code in ((world_name,world_words),('world_baked_difference',world_diff),('world_baked_dual_reference',world_dual),('world_literals',world_literals),('world_vertex_original',world_vs)):
        lines.append(f'inline constexpr std::array<uint32_t,{len(code)}> {label}={{'+','.join(f'0x{x:08x}u' for x in code)+'};')
    report[world_name]={'effect_sha256':world_digest,'program':world,'vertex':world_vertex,'diagnostic_words':len(world_diff),'dual_reference_words':len(world_dual),'scope':'Exact full standard environment material only. Original colour0 and original-minus-black-baked-vertex-colour RGB at colour2. Directional texture/vertex alpha retained. GPU and native registration verification are separate.'}
    base_blob=next(data for path,data,kind in entries if path=='cookedshaders/360/Gen3_StaticEnv.fxlo')
    if hashlib.sha256(base_blob).hexdigest()!='3066bfc590359df1213fb54d028a9ca78103ca2d87374b987cc390824e87c6ec':raise ValueError('Unrecognized base environment effect')
    base_programs=extract(base_blob)
    if len(base_programs)!=1 or base_programs[0]['code_sha256']!='331d6281ba39dbaa3f18b108f1c0d0b90e50a8006cbea92be06252b196025b75':raise ValueError('Base environment pixel identity changed')
    base=base_programs[0];base_words=base.pop('words')
    base_vertices=extract(base_blob,False)
    if len(base_vertices)!=1 or base_vertices[0]['code_sha256']!='0a1fba884bc32510a83b15c74c2d358b2f0a8e32763509ad750eb5f5093ba773':raise ValueError('Base environment vertex changed')
    base_vertex=base_vertices[0];base_vs=base_vertex.pop('words')
    base_literals=struct.unpack_from('>20I',base_blob,base['code_offset']-80)
    if base_literals[4]!=0:raise ValueError('Base environment zero literal changed')
    base_diff=world_baked_difference(base_words,kernel,True,True);base_dual=world_baked_difference(base_words,kernel,False,True)
    for label,code in (('world_base_surface',world_base_surface(base_words,kernel)),('world_base_combined',world_baked_difference(base_words,kernel,True,True,True)),('world_base_combined_dual',world_baked_difference(base_words,kernel,False,True,True)),('Gen3_StaticEnv',base_words),('world_base_baked_difference',base_diff),('world_base_baked_dual_reference',base_dual),('world_base_literals',base_literals),('world_base_vertex_original',base_vs)):
        lines.append(f'inline constexpr std::array<uint32_t,{len(code)}> {label}={{'+','.join(f'0x{x:08x}u' for x in code)+'};')
    report['Gen3_StaticEnv']={'effect_sha256':hashlib.sha256(base_blob).hexdigest(),'program':base,'vertex':base_vertex,'diagnostic_words':len(base_diff),'dual_reference_words':len(base_dual),'surface_probe_words':len(world_base_surface(base_words,kernel)),'combined_words':len(world_baked_difference(base_words,kernel,True,True,True)),'combined_dual_words':len(world_baked_difference(base_words,kernel,False,True,True)),'scope':'Exact base environment shader; r5 RGB baked-colour baseline retains textures, vertex alpha and colour floor. Native registration and GPU checks are separate.'}
    lines.append('}')
    target=ROOT/'private/material_shader_data.h';text='\n'.join(lines)+'\n'
    if not target.exists() or target.read_text()!=text:target.write_text(text)
    (ROOT/'analysis/material-shader-inputs.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Pinned material audit: Oswald LOD/full and Mickey full; Mickey shares 507 executable words, constants and pixel literals with full Oswald. Exact identities only; GPU validation is separate.')
if __name__=='__main__':main()
