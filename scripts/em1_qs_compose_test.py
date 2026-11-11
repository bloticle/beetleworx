"""Compare Qs composition with original DOL instructions in a bounded PS interpreter.
Ordinary finite floats only; FPSCR, exceptions and quantized GQR modes excluded.
"""
import argparse,ctypes as c,capstone,struct,random,math,json,hashlib,re
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--library',required=True);p.add_argument('--dol',type=Path,required=True);args=p.parse_args()
b=args.dol.read_bytes();start,end=0x801e6bc4,0x801e6d4c;raw=None
for i in range(7):
 off,base,size=(struct.unpack_from('>I',b,x+4*i)[0] for x in [0,0x48,0x90])
 if base<=start<end<=base+size:raw=b[off+start-base:off+end-base];break
assert raw is not None
cs=capstone.Cs(capstone.CS_ARCH_PPC,capstone.CS_MODE_32|capstone.CS_MODE_BIG_ENDIAN|capstone.CS_MODE_PS);code=list(cs.disasm(raw,start));assert len(code)*4==len(raw)
f32=lambda value:struct.unpack('f',struct.pack('f',value))[0]
fma=c.CDLL('ucrtbase').fmaf;fma.argtypes=[c.c_float,c.c_float,c.c_float];fma.restype=c.c_float

def original(parent,local):
 mem=bytearray(0x10000);g=[0]*32;f=[[0.,0.] for _ in range(32)]
 def word(at,value):struct.pack_into('>I',mem,at,value)
 def pair(at,values):struct.pack_into('>2f',mem,at,*values)
 g[3]=0x1000;word(0x1000,0x2000);word(0x1004,0x4000);word(0x1010,0x5000);word(0x200c,0x3000)
 g[5]=48;g[7]=2;g[9]=40;g[10]=32;g[11]=8
 for reg,at in [(26,0x6000),(27,0x6100),(28,0x6200),(30,0x6300)]:g[reg]=at
 struct.pack_into('>12f',mem,0x5000,*parent);struct.pack_into('>12f',mem,0x4030,*local);f[5]=[.5,.5]
 def r(x):return int(x[1:])
 def address(x):offset,reg=re.fullmatch(r'(-?(?:0x[0-9a-f]+|[0-9]+))\((r[0-9]+)\)',x).groups();return g[r(reg)]+int(offset,0)
 for instruction in code:
  op=instruction.mnemonic;a=instruction.op_str.split(', ');dest=r(a[0])
  if op=='lwz':g[dest]=struct.unpack_from('>I',mem,address(a[1]))[0]
  elif op=='add':g[dest]=g[r(a[1])]+g[r(a[2])]
  elif op=='addi':g[dest]=g[r(a[1])]+int(a[2],0)
  elif op=='mulli':g[dest]=g[r(a[1])]*int(a[2],0)
  elif op=='lhax':g[dest]=struct.unpack_from('>h',mem,g[r(a[1])]+g[r(a[2])])[0]
  elif op=='lfs':f[dest][0]=struct.unpack_from('>f',mem,address(a[1]))[0]
  elif op=='stfs':struct.pack_into('>f',mem,address(a[1]),f[dest][0])
  elif op in ['psq_lx','psq_stx','psq_l','psq_st']:
   assert a[-2:]==['0','0']
   at=(g[r(a[1])]+g[r(a[2])]) if op.endswith('x') else address(a[1])
   if op.startswith('psq_l'):f[dest]=list(struct.unpack_from('>2f',mem,at))
   else:pair(at,f[dest])
  elif op=='ps_mr':f[dest]=f[r(a[1])].copy()
  elif op=='ps_neg':f[dest]=[-v for v in f[r(a[1])]]
  elif op.startswith('ps_merge'):
   lanes=op[-2:];f[dest]=[f[r(a[1])][int(lanes[0])],f[r(a[2])][int(lanes[1])]]
  elif op=='ps_sum0':f[dest]=[f32(f[r(a[1])][0]+f[r(a[2])][1]),f[r(a[3])][1]]
  elif op in ['ps_add','ps_mul','ps_muls0']:
   left=f[r(a[1])];right=f[r(a[2])]
   f[dest]=[f32(left[i]+right[i]) if op=='ps_add' else f32(left[i]*right[0 if op=='ps_muls0' else i]) for i in range(2)]
  elif op in ['ps_madd','ps_msub']:
   left,right,add=[f[r(v)].copy() for v in a[1:]]
   f[dest]=[fma(left[i],right[i],-add[i] if op=='ps_msub' else add[i]) for i in range(2)]
  elif op=='fmsubs':f[dest][0]=fma(f[r(a[1])][0],f[r(a[2])][0],-f[r(a[3])][0])
  elif op=='frsp':f[dest][0]=f32(f[r(a[1])][0])
  else:raise AssertionError((hex(instruction.address),op,instruction.op_str))
 return list(struct.unpack_from('>12f',mem,0x5030))

d=c.CDLL(str(Path(args.library).resolve()));F=c.POINTER(c.c_float);d.qs_compose.argtypes=[F,F,F];d.qs_compose.restype=None
identity=[0,0,0,0,0,0,0,1,1,1,1,1];cases=[(identity,identity),([10,20,30,0,0,0,1,0,2,3,4,5],[1,2,3,0,0,0,0,1,6,7,8,9])]
rng=random.Random(4701)
def pose():
 q=[rng.uniform(-1,1) for _ in range(4)];length=math.sqrt(sum(v*v for v in q));return [f32(rng.uniform(-100,100)) for _ in range(3)]+[0]+[f32(v/length) for v in q]+[f32(rng.uniform(.1,5)) for _ in range(3)]+[1]
for _ in range(256):cases.append((pose(),pose()))
for parent,local in cases:
 expected=original(parent,local);a=(c.c_float*12)(*parent);v=(c.c_float*12)(*local);out=(c.c_float*12)();d.qs_compose(a,v,out)
 assert struct.pack('12f',*out)==struct.pack('12f',*expected),(parent,local,list(out),expected)
result=dict(cases=len(cases),instructions=len(code),dol_sha256=hashlib.sha256(b).hexdigest(),code_sha256=hashlib.sha256(raw).hexdigest(),scope='Original Qs instructions interpreted at float32 with host fmaf; finite inputs/default GQR float32. No FPSCR/exception/denormal proof, live rig parsing or native animated rendering.')
Path('analysis/em1-compatibility/qs-compose-result.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS:',result)

d.model_poses.argtypes=[c.POINTER(c.c_int32),F,c.c_size_t,F];d.model_poses.restype=c.c_char_p
parents=(c.c_int32*3)(-1,0,1);locals=[*cases[1][0],*cases[1][1],*identity];local_buffer=(c.c_float*36)(*locals);output=(c.c_float*36)()
assert not d.model_poses(parents,local_buffer,3,output)
expected=[*cases[1][0],*original(*cases[1]),*original(original(*cases[1]),identity)]
assert struct.pack('36f',*output)==struct.pack('36f',*expected)
for bad in [(-1,1,1),(-1,2,0),(-1,-2,0)]:assert d.model_poses((c.c_int32*3)(*bad),local_buffer,3,output)
local_buffer[0]=float('nan');assert d.model_poses(parents,local_buffer,3,output)
result['hierarchy_and_invalid_parents_verified']=True
Path('analysis/em1-compatibility/qs-compose-result.json').write_text(json.dumps(result,indent=2)+'\n')
print('PASS: hierarchical Qs resolution; unordered/cyclic/invalid parents and nonfinite local pose reject')
