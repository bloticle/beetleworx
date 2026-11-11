"""Pin original Wii descriptor entry roles and startup inactive-flag handling."""
import argparse,capstone,hashlib,json,struct
from pathlib import Path
parser=argparse.ArgumentParser();parser.add_argument('module',type=Path);args=parser.parse_args()
b=args.module.read_bytes();word=lambda at:struct.unpack_from('>I',b,at)[0]
assert word(0)==1
sections=[(word(word(16)+8*i)&~3,word(word(16)+8*i+4)) for i in range(word(12))]
assert sections[1][0]==0x144 and sections[2][0]==0x4ac3e0 and sections[7][0]==0x4d5fd8
# REL records retain section-relative addresses; verify the parser's two
# relocation halves really use the string-pool anchor before decoding offsets.
relocs={}
for at in range(word(40),word(40)+word(44),8):
    module,position=word(at),word(at+4);source=offset=0
    while True:
        delta,kind,target,addend=struct.unpack_from('>HBBI',b,position);position+=8
        if kind==203:break
        if kind==202:source=target;offset=0;continue
        offset+=delta
        if kind not in (201,204):relocs[sections[source][0]+offset]=(module,kind,target,addend)
anchor=0x4bd98c
for position,kind in [(0x28fbe6,6),(0x28fbee,4)]:assert relocs[position]==(1,kind,2,anchor-sections[2][0])
keys={'folder':0x892c,'shared':0x8933,'initial':0x893a,'interior':0x8942,'i':0x895d,'inactive':0x895f,'gsa':0x8a09,'bnk':0x8a0d}
for key,offset in keys.items():assert b[anchor+offset:anchor+offset+len(key)+1]==key.encode()+b'\0'
expected={0x28FC04:('addi','r4, r4, -0x76d4'),0x28FC18:('addi','r4, r4, -0x76cd'),
0x28FC34:('bl','0x2955b4'),0x28FCDC:('addi','r4, r4, -0x76c6'),0x28FCF8:('li','r5, 0'),
0x28FCFC:('bl','0x28fde0'),0x28FD5C:('addi','r4, r4, -0x76be'),0x28FD78:('li','r5, 1'),0x28FD7C:('bl','0x28fde0'),
0x28FC90:('li','r0, 1'),0x28FC94:('stb','r0, 0x48(r27)'),0x28FDBC:('li','r0, 0'),0x28FDC0:('stb','r0, 0x48(r27)'),
0x290570:('lbz','r0, 0x48(r29)'),0x290578:('bne','0x2905c4'),0x29058C:('addi','r4, r4, -0x76a3'),0x2905A4:('addi','r4, r4, -0x76a1'),
0x29476C:('addi','r28, r3, 0x898'),0x294774:('addi','r28, r3, 0x890'),
0x294884:('addi','r4, r4, -0x75f7'),0x294894:('stw','r30, 0x18(r1)'),
0x2948A4:('addi','r4, r4, -0x75f3'),0x2948B4:('stw','r24, 0x18(r1)'),
0x2948B8:('lwz','r0, 0x18(r1)'),0x2948BC:('cmpwi','r0, -1'),
0x2948C0:('beq','0x294940'),0x2948CC:('bl','0x294bc0'),0x294954:('addi','r27, r27, 1'),
0x2905B4:('li','r30, 1'),0x2905BC:('rlwimi','r0, r30, 0x1f, 0, 0')}
decoder=capstone.Cs(capstone.CS_ARCH_PPC,capstone.CS_MODE_32|capstone.CS_MODE_BIG_ENDIAN)
for position,kind in [(0x294812,6),(0x294822,4)]:
    assert relocs[position]==(1,kind,2,0x4bd8a8-sections[2][0])
assert b[0x4bd8a8:0x4bd8b4]==bytes.fromhex('0000000000000000ffffffff')
for at,e in expected.items():
    i=next(decoder.disasm(b[at:at+4],at));assert(i.mnemonic,i.op_str)==e,(hex(at),i.mnemonic,i.op_str)
result=dict(module_sha256=hashlib.sha256(b).hexdigest(),addresses='REL file offsets, not live addresses',
    entry_field_sequence='0028FBE4',partition_loader='0028FDE0',partition_line='002904A8',
    default_resource_record='004BD8A8',default_resource_type=-1,unclassified_skip_branch='002948C0',
    instructions=[f'{a:08X} {op} {args}' for a,(op,args) in expected.items()],
    conclusions=['Shared GSA is submitted separately; initial partition mode0 precedes interior mode1.',
      'Startup flag at loader+48 is set before both partition calls and cleared after them; line parser skips i/inactive processing while set.',
      'Partition mode selects separate manager lists at+890 (initial) or+898 (interior); ordered resources are classified by gsa/bnk extension and queued through294BC0.',
      'Default resource type is -1; unmatched extensions/bare tokens bypass294BC0 at2948C0, retaining their source-list records but admitting no resource.',
      'Outside startup, i/inactive sets the high bit in the source resource record. Its downstream activation behavior still needs verification.'])
Path('analysis/em1-compatibility/original-level-parser-audit.json').write_text(json.dumps(result,indent=2)+'\n')
print(f'PASS: {len(expected)} original parser instructions, eight keyword/type strings and four REL pointer halves')
