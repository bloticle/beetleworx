"""Compare host transition planning with isolated original Xbox PPC instructions."""
import argparse,ctypes as c,hashlib,json,random,struct
from pathlib import Path
from unicorn import Uc,UC_ARCH_PPC,UC_MODE_32,UC_MODE_BIG_ENDIAN
import unicorn.ppc_const as reg
parser=argparse.ArgumentParser();parser.add_argument('--library',type=Path,required=True);parser.add_argument('--image',type=Path,required=True);args=parser.parse_args()
image=args.image.read_bytes();start,end=0x82794EA8,0x82794FE0
code=image[start-0x82000000:end-0x82000000]
u=Uc(UC_ARCH_PPC,UC_MODE_32|UC_MODE_BIG_ENDIAN);u.mem_map(0x82794000,0x2000);u.mem_write(start,code);u.mem_map(0x20000,0x10000)
def native(current,previous,next):
    u.mem_write(0x20000,bytes(0x10000))
    def word(at,value):u.mem_write(at,struct.pack('>I',value))
    for n,value in enumerate(current):word(0x23000+n*0x80+0x2c,value)
    for state,table,links in [(0x20000,0x25000,previous),(0x21000,0x24000,next)]:
        word(state+0x18,table);word(state+0x1c,len(links))
        for i,(zone,kind) in enumerate(links):
            word(table+i*12,0x23000+zone*0x80);word(table+i*12+4,kind)
    for number,value in [(6,0x20000),(27,0),(28,0x21000),(29,0x22000),(31,0x28000)]:u.reg_write(getattr(reg,'UC_PPC_REG_'+str(number)),value)
    u.emu_start(start,end,count=10000)
    assert u.reg_read(reg.UC_PPC_REG_PC)==end
    word=lambda at:struct.unpack('>I',u.mem_read(at,4))[0]
    desired=[word(0x23000+n*0x80+0x2c) for n in range(len(current))]
    interior=word(0x22000+0xa8)
    return desired,-1 if not interior else (interior-0x23000)//0x80
class Link(c.Structure):_fields_=[('zone',c.c_uint32),('kind',c.c_uint32)]
dll=c.CDLL(str(args.library.resolve()));dll.targets.restype=c.c_int
dll.targets.argtypes=[c.POINTER(Link),c.c_size_t,c.POINTER(Link),c.c_size_t,c.POINTER(c.c_uint32),c.c_size_t,c.POINTER(c.c_uint32),c.POINTER(c.c_int64)]
def host(current,previous,next):
    values=(c.c_uint32*len(current))(*current);out=(c.c_uint32*len(current))();interior=c.c_int64()
    old=(Link*len(previous))(*[Link(*p) for p in previous]);new=(Link*len(next))(*[Link(*p) for p in next])
    assert dll.targets(old,len(old),new,len(new),values,len(values),out,c.byref(interior))
    return list(out),interior.value
rng=random.Random(794);cases=[([0,1,2],[],[(0,2),(1,5),(2,5)]),([2],[],[(0,5),(0,5)]),([2],[(0,2)],[])]
for _ in range(1000):
    zones=rng.randrange(1,9);current=[rng.randrange(3) for _ in range(zones)]
    links=lambda:[(rng.randrange(zones),rng.choice([0,1,2,3,4,5,6,0xffffffff])) for _ in range(rng.randrange(17))]
    cases.append((current,links(),links()))
for current,old,new in cases:
    expected=native(current,old,new);actual=host(current,old,new)
    assert actual==expected,(current,old,new,actual,expected)
result=dict(cases=len(cases),ppc_start=f'{start:08X}',ppc_end_exclusive=f'{end:08X}',code_sha256=hashlib.sha256(code).hexdigest(),
    scope='Isolated original target-update instructions vs host planner; excludes manager side effects, publication, streaming completion and EM1 semantic mapping.')
Path('analysis/em1-compatibility/level-targets-native-result.json').write_text(json.dumps(result,indent=2)+'\n')
print(f'PASS: {len(cases)} host transition plans match original Xbox instructions')
