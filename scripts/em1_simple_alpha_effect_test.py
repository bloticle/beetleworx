"""Read-only donor/one-map shader transformation regression (no game launch)."""
import argparse
from pathlib import Path
import re
import runpy
import struct


def check(source,pixel_test=None):
    root=Path(__file__).resolve().parents[1]
    prep=runpy.run_path(str(root/'scripts/prepare-em1-blend-effects.py'))
    extract=runpy.run_path(str(root/'scripts/inspect-scene-shaders.py'))['extract']
    entries=runpy.run_path(str(root/'scripts/game-pak.py'))['read_pak'](source)
    donor=next(data for name,data,_ in entries if name=='cookedshaders/360/Gen3_StaticEnv_Simple.fxlo')
    name,alias=prep['clone_simple_alpha_effect'](donor)
    assert name=='EM1S_StaticEnv_Simple' and len(alias)==len(donor)
    assert alias.count((name+'\0').encode())==1
    a,b=extract(donor)[0],extract(alias)[0]
    assert a['constants']==b['constants'], 'Sampler0/minimum-color ownership changed'
    assert a['code_offset']==b['code_offset'] and a['code_bytes']==b['code_bytes']==72
    wa,wb=a['words'],b['words']
    assert [i for i,(x,y) in enumerate(zip(wa,wb)) if x!=y]==[7,12,13]
    assert wb[7]==0x1F1FF688 and wb[12]==0xC80FC000
    assert (wb[12]>>16)&15==15 and (wb[12]>>20)&15==0, 'Alpha still forced to constant1'
    # Canonical GPU test must exercise these exact bytes, including unchanged
    # native RGB arithmetic, scheduling and sampler0 rather than a lookalike.
    cpp=(root/'runtime/em1_blend_microcode.h').read_text()
    body=cpp.split('BuildSimpleAlphaPixelProgram() {',1)[1].split('}',1)[0]
    assert tuple(int(x,16) for x in re.findall(r'0x[0-9A-Fa-f]+',body))==wb
    alpha=prep['preserve_vertex_alpha'](donor)
    allowed=[(0x82C,0x830),(0x83C,0x840),(2140,2144),(2160,2168)]
    at=donor.index(b'Gen3_StaticEnv_Simple\0');allowed.append((at,at+22))
    assert all(x==y or any(lo<=i<hi for lo,hi in allowed) for i,(x,y) in enumerate(zip(alpha,alias)))
    assert alias[1160+0x174:1160+0x180]==donor[1160+0x174:1160+0x180], 'Fetch scheduling changed'
    blend=next(data for name,data,_ in entries if name=='cookedshaders/360/Gen3_StaticEnv_Blend.fxlo')
    body=cpp.split('BuildSimplePropertyCutoutPixelProgram(bool greater) {',1)[1].split('}',1)[0]
    for greater in (False,True):
        expression=body.replace('greater?0x7AFC0200u:0x79FC0200u','0x7AFC0200' if greater else '0x79FC0200')
        program=tuple(int(x,16) for x in re.findall(r'0x[0-9A-Fa-f]+',expression))
        raw=struct.pack('>'+str(len(program))+'I',*program)
        name,effect=prep['clone_effect'](blend,donor,raw,False,True,property_cutout='greater127' if greater else 'gequal127')
        ps=extract(effect)[0]
        assert ps['words'][:len(program)]==program
        assert all(word==0 for word in ps['words'][len(program):]), 'Unaudited instructions after end'
        assert struct.unpack_from('>f',effect,3892)[0]==struct.unpack('>f',struct.pack('>f',127/255))[0]
        assert effect[4532:4616]==donor[2244:2328], 'Cutout changed scalar render state'
        assert effect[1736+0x174:1736+0x180]==donor[1160+0x174:1160+0x180]
        assert program[6:9]==(0x10182001,0x1F1FF688,0x00004000), 'Not one source map'
        assert program[14]==0xE1020100 and program[13]==0, 'RGBA multiply differs'
        assert program[17]==(0x7AFC0200 if greater else 0x79FC0200), 'Comparison differs'
    damaged=bytearray(donor);damaged[2112]^=1
    try:prep['clone_simple_alpha_effect'](bytes(damaged))
    except ValueError:pass
    else:raise AssertionError('Unknown donor accepted')
    if pixel_test:
        baseline=(pixel_test/'simple-masked.ucode.txt.bin').read_bytes()
        pixel=prep['masked_property_cutout_program'](baseline)
        name,effect=prep['clone_effect'](blend,donor,pixel,True,True,property_cutout='maskedgreater127')
        _,ordinary=prep['clone_effect'](blend,donor,baseline,True,True)
        assert name=='EM1H_StaticEnv_Blend'
        allowed=[(0x1504,0x1519),(3956,4472),(3892,3896)]
        assert all(a==b or any(lo<=i<hi for lo,hi in allowed) for i,(a,b) in enumerate(zip(ordinary,effect))), 'Masked RGB/interface/state/samplers changed'
        assert effect[4532:4616]==ordinary[4532:4616]
        assert struct.unpack_from('>f',effect,3892)[0]==struct.unpack('>f',struct.pack('>f',127/255))[0]
        assert extract(effect)[0]['words'][:48]==struct.unpack('>48I',pixel)
        result=pixel_test/'masked-greater127.ucode.txt.bin'
        if result.exists():assert result.read_bytes()==pixel, 'Canonical GPU output differs from pinned final-alpha edit'
        damaged=bytearray(baseline);damaged[-4]^=1
        try:prep['masked_property_cutout_program'](damaged)
        except ValueError:pass
        else:raise AssertionError('Changed default masked program accepted')
    print('PASS: one-map alpha effect has exact GPU-test bytes, unchanged RGB/sampler/state, RGBA VS and bounded edits; no archive written')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('source_pak',type=Path)
    p.add_argument('--pixel-test',type=Path)
    args=p.parse_args();check(args.source_pak,args.pixel_test)
