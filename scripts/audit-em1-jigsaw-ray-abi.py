"""Pin retail original MOPP traversal ABI and its outer shape-key frame."""
import argparse,struct,json,hashlib
from pathlib import Path
import capstone
p=argparse.ArgumentParser();p.add_argument('image',type=Path);args=p.parse_args();data=args.image.read_bytes();base=0x82000000
c=capstone.Cs(capstone.CS_ARCH_PPC,capstone.CS_MODE_32|capstone.CS_MODE_BIG_ENDIAN)
instructions={
    0x82451988:('lwz','r11, 0x40(r31)'),0x82451994:('addi','r8, r11, 1'),
    0x8245199c:('stw','r8, 0x40(r31)'),0x824519a4:('lwz','r3, 0x1c(r4)'),
    0x824519a8:('li','r30, 0'),0x824519ac:('lwz','r11, 0x34(r4)'),
    0x824519b4:('lwz','r10, 0x18(r4)'),0x824519c8:('stw','r10, 0x20(r6)'),
    0x824519d0:('stw','r3, 0x24(r6)'),0x824519fc:('bl','0x824b2378'),
    0x82451a00:('lwz','r11, 0x40(r31)'),0x82451a04:('addi','r11, r11, -1'),
    0x82451a08:('stw','r11, 0x40(r31)'),0x82451a18:('addi','r11, r11, 8'),
    0x82451a1c:('slwi','r10, r11, 2'),0x82451a20:('stwx','r30, r10, r31'),
}
for address,expected in instructions.items():
    row=next(c.disasm(data[address-base:address-base+4],address));assert (row.mnemonic,row.op_str)==expected,(hex(address),row.mnemonic,row.op_str)
assert struct.unpack_from('>I',data,0x25c28+28)[0]==0x82451938
assert struct.unpack_from('>I',data,0x27abc+28)[0]==0x8245ebc8
result=dict(image_sha256=hashlib.sha256(data).hexdigest(),mopp_vtable='82025C28',mopp_cast='82451938',collection_vtable='82027ABC',collection_cast='8245EBC8',mopp_query='824B2378',outer_shape_key=0,
    instructions=[f'{a:08X} {op} {operand}' for a,(op,operand) in instructions.items()],
    scope='Retail original scalar ABI and key-stack instructions. Source geometry/ray equivalence and world/component behavior require separate live evidence.')
Path('analysis/em1-compatibility/jigsaw-native-ray-abi.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS:16original MOPP ABI/key-frame instructions and both virtual ray entries')
