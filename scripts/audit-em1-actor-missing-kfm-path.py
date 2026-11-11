"""Audit retail NiActorComponent's failed-KFM update path; no fallback patch."""
import argparse
from pathlib import Path
import capstone
p=argparse.ArgumentParser();p.add_argument('image',type=Path);a=p.parse_args();data=a.image.read_bytes();base=0x82000000
c=capstone.Cs(capstone.CS_ARCH_PPC,capstone.CS_MODE_32|capstone.CS_MODE_BIG_ENDIAN)
expected={0x8252f238:('mflr','r12'),0x8252f43c:('mtctr','r10'),0x8252f440:('bctrl',''),0x8252f444:('clrlwi','r9, r3, 0x18'),
  0x8252f44c:('beq','cr6, 0x8252f894'),0x8252f894:('lbz','r11, 0x20(r28)'),0x8252f898:('ori','r10, r11, 1'),
  0x8252f8a0:('stb','r10, 0x20(r28)'),0x8252f8f8:('addi','r4, r8, -0x24c0'),0x8252f90c:('bctrl',''),
  0x8252f91c:('bl','0x8253a050'),0x8252f924:('bl','0x822f1c10'),0x8252f928:('b','0x8252f930'),
  0x8252f930:('lwz','r3, 0xe0(r1)'),0x8252f934:('bl','0x82b6d6e0'),0x8252f944:('b','0x82b695ac')}
for address,want in expected.items():
    row=next(c.disasm(data[address-base:address-base+4],address));assert (row.mnemonic,row.op_str)==want,(hex(address),row.mnemonic,row.op_str)
assert b'The actor manager could not be created from the specified KFM file.\0' in data
print('PASS: retail actor KFM failure flags/reporter/cleanup/rejoin pinned; derived component and reporter policy NOT verified')
