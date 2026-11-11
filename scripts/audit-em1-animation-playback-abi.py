"""Pin the retail actor selection/time update/model update call sequence."""
import argparse
from pathlib import Path
import capstone
p=argparse.ArgumentParser();p.add_argument('image',type=Path);a=p.parse_args();data=a.image.read_bytes();base=0x82000000
c=capstone.Cs(capstone.CS_ARCH_PPC,capstone.CS_MODE_32|capstone.CS_MODE_BIG_ENDIAN)
expected={0x8253000c:('lwz','r11, 0x20(r3)'),0x8253003c:('addi','r3, r3, 0x7c'),0x82530040:('bl','0x8252ff78'),0x82530064:('stw','r31, 0x20(r30)'),
  0x8300df30:('stfs','f1, 0x24(r3)'),0x8252f850:('lwz','r3, 0x28(r25)'),0x8252f854:('bl','0x8252fff8'),
  0x8252f864:('lwz','r3, 0x28(r25)'),0x8252f868:('fmr','f1, f30'),0x8252f86c:('bl','0x8300df18'),
  0x8252f874:('fmr','f1, f30'),0x8252f880:('lwz','r3, 0x14(r11)'),0x8252f888:('li','r5, 1'),0x8252f88c:('bl','0x82552f00'),
  0x82552f10:('stfs','f1, 0x10(r10)'),0x82552f18:('stb','r5, 0x14(r10)'),0x82552f28:('b','0x82552838')}
for address,want in expected.items():
    row=next(c.disasm(data[address-base:address-base+4],address));assert (row.mnemonic,row.op_str)==want,(hex(address),row.mnemonic,row.op_str)
print('PASS: original sequence-ID selection, absolute-time actor update and model update in retail call order')
