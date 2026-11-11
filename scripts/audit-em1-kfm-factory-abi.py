"""Pin stock KFM factory, real actor handle and original dependency binding."""
import argparse
import struct
from pathlib import Path
import capstone
p=argparse.ArgumentParser();p.add_argument('image',type=Path);a=p.parse_args();data=a.image.read_bytes();base=0x82000000
c=capstone.Cs(capstone.CS_ARCH_PPC,capstone.CS_MODE_32|capstone.CS_MODE_BIG_ENDIAN)
expected={0x825127d0:('lis','r11, 0x4b46'),0x825127d4:('ori','r11, r11, 0x4d00'),0x8308a0f0:('lwz','r3, 0x4e9c(r11)'),
  0x8274c740:('mr','r8, r29'),0x8274c758:('lwz','r10, 0x48(r11)'),0x8322266c:('bl','0x83018bb0'),0x83222734:('bl','0x825128a0'),
  0x8322277c:('bl','0x8300e730'),0x8322292c:('lwz','r10, 0x54(r11)'),0x83221f8c:('bl','0x82512f98'),
  0x82513050:('bl','0x8300b6f8'),0x8251310c:('bl','0x8300c148'),0x8253a020:('addi','r9, r11, -0x2f4c'),0x8253a02c:('stw','r10, 0x10(r31)'),
  0x8253a030:('stb','r30, 0xc(r31)'),0x8253a070:('lwz','r3, 0x10(r3)'),0x8253a0d0:('lwz','r3, 0x10(r3)'),
  0x8300ba04:('addi','r3, r10, 0x4bf8'),0x8300ba18:('stw','r3, 0(r31)'),
  0x8300b814:('addi','r3, r27, 0x14'),0x8300b820:('mr','r3, r29'),0x8300b7a8:('addi','r29, r27, 0x18'),
  0x82808954:('addi','r3, r3, 0x7c'),0x82808958:('bl','0x8252ff78')}
for address,want in expected.items():
    row=next(c.disasm(data[address-base:address-base+4],address));assert (row.mnemonic,row.op_str)==want,(hex(address),row.mnemonic,row.op_str)
assert struct.unpack_from('>I',data,0x82227bd0-base+72)[0]==0x83222500
assert struct.unpack_from('>I',data,0x82227bd0-base+84)[0]==0x83221d30
print('PASS: stock KFM factory/resolver, distinct actor-handle ABI, native actor class, model/controller binding and original sequence-ID lookup')
