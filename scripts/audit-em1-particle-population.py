"""Verify the retail particle type/count and child traversal used by the live probe."""
from pathlib import Path
import capstone,json,hashlib
b=Path('private/image.bin').read_bytes()
c=capstone.Cs(capstone.CS_ARCH_PPC,capstone.CS_MODE_32|capstone.CS_MODE_BIG_ENDIAN)
expected={0x830BE250:('li','r3, 0x320'),0x830BE14C:('stw','r28, 0x23c(r31)'),0x830BA460:('lwz','r11, 0x240(r3)'),0x830BA464:('addi','r11, r11, -1'),0x830BA468:('stw','r11, 0x240(r3)'),0x830BCFF0:('lwz','r11, 0x240(r31)'),0x830BCFF4:('lwz','r4, 0x2a0(r31)'),0x82800528:('lhz','r11, 0xfa(r29)'),0x82800548:('lwz','r11, 0xf4(r29)'),0x82800498:('lwz','r31, 0xd8(r29)'),0x830BEB58:('lis','r11, -0x7cc0'),0x830BEB5C:('addi','r3, r11, -0x5730')}
checks=[]
for a,e in expected.items():
 ins=list(c.disasm(b[a-0x82000000:a-0x82000000+4],a))
 if len(ins)!=1 or (ins[0].mnemonic,ins[0].op_str)!=e:raise ValueError(f'Retail particle ABI mismatch at {a:08X}')
 checks.append(f'{a:08X} {e[0]} {e[1]}')
Path('analysis/em1-compatibility/native-particle-population-audit.json').write_text(json.dumps({'image_sha256':hashlib.sha256(b).hexdigest(),'asserted_instructions':checks,'particle_type':'833FA8D0','capacity_offset':'23C','live_count_offset':'240','scope':'Native allocation, capacity initialization, particle removal/count and clone-array use; live counts still require runtime observations.'},indent=2)+'\n')
print(len(checks),'retail instructions verified')
