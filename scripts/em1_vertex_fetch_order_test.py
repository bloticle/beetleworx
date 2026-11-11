"""Guard the native input scheduling metadata while enabling vertex alpha."""
import argparse
from pathlib import Path
import runpy
import struct


def check(simple, archive):
    prep=runpy.run_path(str(Path(__file__).with_name('prepare-em1-blend-effects.py')))
    pak=runpy.run_path(str(Path(__file__).with_name('game-pak.py')))
    original=simple if isinstance(simple,bytes) else simple.read_bytes()
    alpha=prep['preserve_vertex_alpha'](original)
    # Input records contain more than a semantic and component count. Keep
    # their scheduling metadata; the explicit fetch swizzle enables alpha.
    assert alpha[1160+0x174:1160+0x180] == original[1160+0x174:1160+0x180], 'Input scheduling metadata changed'
    assert struct.unpack_from('>I',alpha,1560+5*12+4)[0] & 0xfff == 0x688, 'Alpha lane is not fetched'
    count=0
    for name,data,kind in pak['read_pak'](archive):
        if name in {f'cookedshaders/360/EM1{x}_StaticEnv_Blend.fxlo' for x in 'WMDA'}:
            if '/EM1D_' in name:
                records=struct.unpack_from('>4I',data,1736+0x174)
                donor=struct.unpack_from('>3I',original,1160+0x174)
                assert records == (*donor[:2],0x00015005,0x0030A006), name
                code=1736+404
                # The last fetch writes color to r0. UV1 must precede it.
                assert struct.unpack_from('>3I',data,code+5*12)==(0x05F81000,0x0000023F,0), name
                assert struct.unpack_from('>I',data,code+6*12)[0]==0x05F80000, name
            else:
                assert data[1736+0x174:1736+0x180] == original[1160+0x174:1160+0x180], name
            count+=1
    assert count==4, 'Missing staged variants'
    print('PASS: all four variants preserve donor fetch scheduling; alpha fetch remains enabled')


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('simple',type=Path);p.add_argument('archive',type=Path)
    a=p.parse_args();check(a.simple,a.archive)
