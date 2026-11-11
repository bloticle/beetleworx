"""Pin original KF typed factory, all-sequence cache and release ABI."""
import argparse
import struct
from pathlib import Path
import capstone
p=argparse.ArgumentParser();p.add_argument('image',type=Path);a=p.parse_args();data=a.image.read_bytes();base=0x82000000
c=capstone.Cs(capstone.CS_ARCH_PPC,capstone.CS_MODE_32|capstone.CS_MODE_BIG_ENDIAN)
expected={0x82511150:('lis','r11, 0x4b46'),0x8308a0fc:('lwz','r3, 0x4e98(r11)'),
  0x83222fc4:('addi','r25, r24, 0xc'),0x83222fd4:('bl','0x83009fb8'),0x83223008:('bl','0x82555510'),0x832230b4:('bl','0x83013748'),
  0x83223194:('lhz','r4, 0x7a(r31)'),0x832231b0:('lwz','r11, 0x74(r31)'),0x832231c0:('bl','0x825a9520'),0x832231e8:('bl','0x825d9018'),
  0x825126f0:('mflr','r12'),0x82512710:('stw','r11, 4(r3)'),0x82512720:('sth','r11, 0xa(r3)'),
  0x82511f38:('bl','0x82539f10'),0x82511f54:('bl','0x82511a90'),0x82511aac:('addi','r30, r3, 0xc'),
  0x82511ac0:('bl','0x83009fb8'),0x82511ae0:('bl','0x825a95e0'),0x82511b04:('bl','0x825d9138')}
for address,want in expected.items():
    row=next(c.disasm(data[address-base:address-base+4],address));assert (row.mnemonic,row.op_str)==want,(hex(address),row.mnemonic,row.op_str)
assert struct.unpack_from('>I',data,0x82227d98-base+72)[0]==0x83222f88
assert struct.unpack_from('>I',data,0x82227d98-base+44)[0]==0x82511ef8
print('PASS: stock KF tag/factory, cache +0xc, all-sequence extraction/array and native removal ABI')
