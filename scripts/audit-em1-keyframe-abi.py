"""Pin original NiSequenceData table, real NiRTTI and named initializer."""
import argparse
import struct
from pathlib import Path
import capstone
p=argparse.ArgumentParser();p.add_argument('image',type=Path);a=p.parse_args();data=a.image.read_bytes();base=0x82000000
c=capstone.Cs(capstone.CS_ARCH_PPC,capstone.CS_MODE_32|capstone.CS_MODE_BIG_ENDIAN)
expected={0x83013bd8:('lis','r11, -0x7cc1'),0x83013bdc:('addi','r3, r11, -0x1530'),0x83013be0:('blr',''),
          0x83013be8:('lis','r11, -0x7cc1'),0x83013bec:('addi','r3, r11, -0x1520'),
          0x833231f0:('lis','r11, -0x7cc3'),0x833231f4:('lis','r10, -0x7de3'),0x833231f8:('lis','r9, -0x7cc1'),
          0x833231fc:('addi','r5, r11, -0x21f4'),0x83323200:('addi','r4, r10, 0x4c0c'),0x83323204:('addi','r3, r9, -0x1530'),0x83323208:('b','0x82551f78'),
          0x830123f0:('addi','r8, r11, 0x4c28'),0x830123fc:('stw','r8, 0(r3)'),
          0x83013370:('addi','r4, r30, 0xc'),0x830135c0:('addi','r27, r30, 0x14'),0x83013650:('addi','r4, r30, 0x20'),0x830136b8:('addi','r4, r30, 0x28')}
for address,want in expected.items():
    row=next(c.disasm(data[address-base:address-base+4],address));assert (row.mnemonic,row.op_str)==want,(hex(address),row.mnemonic,row.op_str)
assert struct.unpack_from('>II',data,0x821d4c28-base+8)==(0x83013bd8,0x83013be8)
assert data[0x821d4c0c-base:0x821d4c0c-base+15]==b'NiSequenceData\0'
print('PASS: original NiSequenceData table, NiRTTI +8, distinct numeric descriptor +12, class-name initializer and destructor table')
