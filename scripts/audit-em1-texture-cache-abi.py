"""Pin original NIF texture classification, cache lookup and removal ABI."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
import capstone
p=argparse.ArgumentParser();p.add_argument('image',type=Path);a=p.parse_args()
data=a.image.read_bytes();base=0x82000000
c=capstone.Cs(capstone.CS_ARCH_PPC,capstone.CS_MODE_32|capstone.CS_MODE_BIG_ENDIAN)
expected={
    0x82555e28:('lwz','r3, 0x1d4(r3)'),0x82555e2c:('blr',''),
    0x82585d10:('lis','r11, -0x7cc3'),0x82585d14:('addi','r3, r11, -0x1834'),
    0x82539f28:('addi','r3, r4, 8'),0x82539f34:('lwz','r11, 8(r31)'),
    0x82584a70:('lwz','r11, 0x28(r3)'),0x82584a90:('lwz','r11, 0x70(r11)'),
    0x82584ac0:('lwz','r11, 0x28(r3)'),0x82584ae0:('lwz','r11, 0x74(r11)'),
    0x832187dc:('addi','r27, r26, 0x44'),
    0x83218958:('mr','r4, r21'),0x83218960:('bl','0x8255a280'),
    0x83218970:('mr','r3, r27'),0x83218974:('bl','0x82555e28'),
    0x8321897c:('li','r5, 0'),0x83218980:('mr','r4, r28'),
    0x83218988:('lwz','r10, 0x10(r11)'),
    0x8321899c:('mr','r5, r29'),0x832189ac:('lwz','r10, 0x14(r11)'),
    0x83218554:('addi','r3, r28, 0x44'),0x83218558:('bl','0x82555e28'),
    0x8321856c:('lwz','r9, 0x10(r10)'),0x8321858c:('lwz','r10, 0x18(r11)'),
}
for address,want in expected.items():
    row=next(c.disasm(data[address-base:address-base+4],address))
    assert (row.mnemonic,row.op_str)==want,(hex(address),row.mnemonic,row.op_str)
assert struct.unpack_from('>I',data,0x41158+12)[0]==0x82585d10
assert struct.unpack_from('>I',data,0x2259a8+72)[0]==0x832187a8
assert struct.unpack_from('>I',data,0x2259a8+44)[0]==0x83218520
result=dict(image_sha256=hashlib.sha256(data).hexdigest(),instructions=len(expected),
            original_texture_table='82041158',original_nif_loader_table='822259A8',
            source_texture_rtti='833CE7CC',embedded_stream_offset=0x44,stream_cache_offset=0x1d4,
            cache_lookup_slot=16,cache_register_slot=20,cache_remove_slot=24,
            scope='Static original ABI audit only; native publication/lifetime tested separately')
Path('analysis/em1-compatibility/texture-cache-abi-result.json').write_text(json.dumps(result,indent=2)+'\n')
print('PASS: original texture RTTI, NIF loader, cache lookup/register/remove ABI')
