"""Verify retail instructions used by the opt-in particle scheduling observers."""
from pathlib import Path
import capstone,json,hashlib
b=Path('private/image.bin').read_bytes()
c=capstone.Cs(capstone.CS_ARCH_PPC,capstone.CS_MODE_32|capstone.CS_MODE_BIG_ENDIAN)
expected=['830BB5B8 lbz r11, 0x15(r27)', '830BB5C0 beq cr6, 0x830bb62c', '830BB5F4 stb r28, 0x305(r31)', '830BB608 stfs f0, 0x238(r31)', '830BA7F0 lbz r11, 0x15(r4)', '830BA7F8 beqlr cr6', '830BA7FC lfs f0, 0x10(r4)', '830BA800 stfs f0, 0x238(r3)', '830BA818 lwz r11, 0(r3)', '830BA820 lwz r10, 0xf8(r11)', '830BA828 bctrl', '830BA850 stb r9, 0x305(r31)', '830BA86C stb r11, 0x305(r31)', '830BA460 lwz r11, 0x240(r3)', '830BA464 addi r11, r11, -1', '830BA468 stw r11, 0x240(r3)']
for text in expected:
 address,operation=text.split(' ',1);a=int(address,16)
 ins=list(c.disasm(b[a-0x82000000:a-0x82000000+4],a))
 assert len(ins)==1 and f'{ins[0].mnemonic} {ins[0].op_str}'.rstrip()==operation,(text,ins)
output={"image_sha256":hashlib.sha256(b).hexdigest(),"instructions":expected,"scope":'Native update context and simulation-time commit plus 830BA808 finalization (vtable F8 and optional quad generator, clears pending flag) and 830BA310 swap/remove (decrements population). Upstream visibility policy is not proven.'}
Path('analysis/em1-compatibility/native-particle-update-audit.json').write_text(json.dumps(output,indent=2)+'\n')
print(len(expected),'retail update/finalization instructions verified')
