"""Verify original particle submission gate and cached render-channel policy."""
from pathlib import Path
import capstone,struct,json,hashlib
b=Path('private/image.bin').read_bytes()
c=capstone.Cs(capstone.CS_ARCH_PPC,capstone.CS_MODE_32|capstone.CS_MODE_BIG_ENDIAN)
expected=['830C13C4 mr r27, r4', '830C1428 lwz r6, 0x240(r27)', '830C1430 beq cr6, 0x830c13d8', '830C1434 lbz r11, 0x2dc(r27)', '830C1438 rlwinm r10, r11, 0, 0x1d, 0x1d', '830C1440 bne cr6, 0x830c13d8', '830BB8BC mr r3, r30', '830BB8C0 bl 0x82552f68', '830BB8D0 cmpwi cr6, r31, 5', '830BB8DC blt cr6, 0x830bb8a8', '830BB8F0 rlwimi r10, r8, 2, 0x1d, 0x1d', '830BB8F4 stb r10, 0x2dc(r30)', '82552F70 lwz r4, 0x24(r3)', '82552FD4 addis r11, r9, 1', '82552FD8 addi r11, r11, 0x7792', '82552FE0 lhzx r8, r9, r10', '82552FE4 clrlwi r3, r8, 0x1f', '8256FA24 addi r29, r29, -0x10dc', '8256FA8C lwz r10, 0xc0(r11)', '8256FAC4 or r9, r11, r21', '8256FAD0 sth r8, 0(r29)', '8256FADC andc r9, r10, r21', '8256FAE0 sth r9, 0(r29)']
for text in expected:
 address,operation=text.split(' ',1);a=int(address,16)
 ins=list(c.disasm(b[a-0x82000000:a-0x82000000+4],a))
 assert len(ins)==1 and f'{ins[0].mnemonic} {ins[0].op_str}'==operation,text
assert struct.unpack_from('>5I',b,0x833950AC-0x82000000)==(1,2,4,8,16)
result={"image_sha256":hashlib.sha256(b).hexdigest(),"instructions":expected,"scope":'Native particle simulator submission returns before task dispatch when object+2DC bit4 is set. NiPS update sets bit4 when no cached render-collection channel is accepted. Getter82552F68 reads low channel bits from scene+2EF24+index*2. Scene collection8256F948 sets/clears those bits according to accepted native render submissions. This proves the native policy; live bounds or visible image correctness are separate.'}
Path('analysis/em1-compatibility/native-particle-submission-audit.json').write_text(json.dumps(result,indent=2)+'\n')
print(len(expected),'retail submission/visibility instructions verified')
