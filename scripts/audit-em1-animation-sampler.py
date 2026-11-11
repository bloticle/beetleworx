"""Pin original EM2 spline ABI used by the private-source animation probe."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
import capstone

p=argparse.ArgumentParser()
p.add_argument('image',type=Path)
args=p.parse_args()
data=args.image.read_bytes()
base=0x82000000
c=capstone.Cs(capstone.CS_ARCH_PPC,capstone.CS_MODE_32|capstone.CS_MODE_BIG_ENDIAN)
expected={
    0x822F2900:('cmplwi','cr6, r3, 0'),
    0x822F2908:('lis','r11, -0x7e00'),
    0x822F290C:('cmpwi','cr6, r4, 0'),
    0x822F2910:('addi','r10, r11, 0x360c'),
    0x822F2914:('stw','r10, 0(r3)'),
    0x822F2918:('beqlr','cr6'),
    0x822F291C:('b','0x822fc398'),
    0x822FC3A4:('lwz','r11, 0x80(r3)'),
    0x822FC3AC:('cmpwi','cr6, r11, 1'),
    0x822FC3B0:('beq','cr6, 0x822fc5c8'),
    0x822FE000:('lwz','r11, 0(r3)'),
    0x822FE004:('mr','r8, r6'),
    0x822FE008:('mr','r6, r5'),
    0x822FE00C:('lwz','r7, 0x14(r3)'),
    0x822FE010:('lwz','r5, 0x10(r3)'),
    0x822FE014:('lwz','r10, 0xc(r11)'),
    0x822FE018:('mtctr','r10'),
    0x822FE01C:('bctr',''),
}
for address,wanted in expected.items():
    offset=address-base
    actual=next(c.disasm(data[offset:offset+4],address))
    assert (actual.mnemonic,actual.op_str)==wanted,(hex(address),actual.mnemonic,actual.op_str,wanted)
typeinfo=struct.unpack_from('>6I',data,0x8338719C-base)
assert typeinfo==(0x8200383C,0x8200385C,0x822F2900,0x822F23A0,0x8200360C,132),typeinfo
assert data[0x383C:0x383C+len(b'hkaSplineCompressedAnimation\0')]==b'hkaSplineCompressedAnimation\0'
assert struct.unpack_from('>I',data,0x360C+8)[0]==0x822FE000
assert struct.unpack_from('>I',data,0x360C+12)[0]==0x822FF778
result=dict(image_sha256=hashlib.sha256(data).hexdigest(),typeinfo_address='8338719C',object_size=132,
    vtable='8200360C',finish='822F2900',sample_tracks='822FE000',sample_partial_tracks='822FF778',
    instructions=[f'{a:08X} {op} {args}' for a,(op,args) in expected.items()],
    scope='Static original EM2 ABI only. Native sampling private fixtures, original-Wii parity, gameplay/actor publication and compressed source compatibility require separate verification.')
Path('analysis/em1-compatibility/native-animation-sampler-audit.json').write_text(json.dumps(result,indent=2)+'\n')
print(f'PASS: {len(expected)} native instructions, spline type-info size132, finish/endian state and sample dispatch')
