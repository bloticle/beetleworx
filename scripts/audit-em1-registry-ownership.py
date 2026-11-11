"""Pin retail registry resource bindings and source-buffer ownership instructions."""
from pathlib import Path
import argparse,json,capstone
p=argparse.ArgumentParser();p.add_argument('image',type=Path);args=p.parse_args()
data=args.image.read_bytes()
c=capstone.Cs(capstone.CS_ARCH_PPC,capstone.CS_MODE_32|capstone.CS_MODE_BIG_ENDIAN)
expected={
0x82749E98:('lwz','r11, 0x10(r3)'),
0x82749EA8:('lwz','r11, 8(r3)'),
0x82749EB4:('addi','r11, r11, 0x1f'),
0x82749EC0:('bl','0x826549b8'),
0x82749EC8:('lwz','r5, 0x18(r31)'),
0x82749ED0:('lwz','r9, 4(r31)'),
0x82749EDC:('srwi','r6, r9, 5'),
0x82749EE0:('bl','0x831c0e68'),

0x82797310:('stw','r9, 0x54(r23)'),
0x82797324:('stw','r7, 8(r23)'),
0x82797338:('stw','r5, 0xc(r23)'),
0x8279734C:('stw','r3, 0x10(r23)'),
0x82797360:('lwz','r11, 0x68(r23)'),
0x82797378:('mr','r3, r24'),
0x8279737C:('bl','0x827989e8'),
0x827974D4:('stw','r19, 0x84(r31)'),
0x8279750C:('lbz','r11, -0x4f6e(r14)'),
0x82797514:('addi','r4, r31, 0x58'),
0x8279751C:('bne','cr6, 0x82797524'),
0x82797520:('addi','r4, r31, 0x50'),
0x82797524:('bl','0x8289fda8'),
0x82797F8C:('addi','r6, r1, 0x54'),
0x82797F90:('addi','r5, r1, 0x50'),
0x82797F98:('bl','0x82749e50'),
0x82797FAC:('lwz','r4, 0x50(r1)'),
0x82797FB0:('bl','0x82797910'),
0x82797FB4:('lwz','r11, 0x54(r1)'),
0x82797FB8:('lwz','r10, 4(r11)'),
0x82797FBC:('lwz','r9, 8(r11)'),
0x82797FC0:('srwi','r8, r10, 5'),
0x82797FC4:('cmplw','cr6, r8, r9'),
0x82797FC8:('beq','cr6, 0x82797fd8'),
0x82797FCC:('lwz','r3, 0x50(r1)'),
0x82797FD0:('bl','0x826547c8'),
0x82797FD8:('lwz','r3, -0x57e8(r29)'),
0x82797FDC:('lwz','r4, 0(r11)'),
0x82797FE0:('bl','0x82749df0'),
}
for address,wanted in expected.items():
 offset=address-0x82000000
 actual=next(c.disasm(data[offset:offset+4],address))
 assert (actual.mnemonic,actual.op_str)==wanted,(hex(address),actual.mnemonic,actual.op_str,wanted)
result=dict(instructions=[f'{a:08X} {op} {operand}' for a,(op,operand) in expected.items()],
 findings=[
 'Zone metadata words 0..3 stored at +54,+8,+C,+10; metadata4 controls resource-vector capacity. Semantic defaults remain unverified.',
 'Registry original resource vs alias chosen by global833DB092 before resource name binding8289FDA8. Imported paths must bind identically for both modes or provide both assets.',
 'Initializer82797DB8 passes stack[50] bytes to82797910; cleanup later reloads original stack[50], conditionally frees through826547C8, then releases original resource through82749DF0.',
 'Candidate initial-load hook must substitute parser argument r4 only, retain original initializer stack slots and release temporary candidate bytes after synchronous parser return. Do not rerun parser against an initialized manager.'
 ],scope='Static original Xbox instructions only. No registry hook installed; native zone publication, field defaults, component conversion, live transitions remain unverified.')
Path('analysis/em1-compatibility/native-registry-binding-ownership.json').write_text(json.dumps(result,indent=2)+'\n')
print(f'PASS: {len(expected)} original registry binding/cleanup instructions')
