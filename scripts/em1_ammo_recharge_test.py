"""Entire retail recharge Update differential test with explicit delegate fixtures.

Executes original PPC control flow; property/inventory/timer delegates are supplied
identically to both implementations. Does NOT prove a live world/owner binding.
"""
import argparse,ctypes as c,hashlib,json,random,struct,sys
from pathlib import Path
from unicorn import Uc,UC_ARCH_PPC,UC_MODE_32,UC_MODE_BIG_ENDIAN,UC_HOOK_CODE,UC_HOOK_MEM_WRITE
from unicorn.ppc_const import *
from em1_rel_reference import inspect

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--library',type=Path,required=True);p.add_argument('--module',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
a=p.parse_args();raw=a.module.read_bytes();digest=hashlib.sha256(raw).hexdigest()
assert digest=='ac25d520f329a3d0507d656f0ab1cf1c34025bb36ff866187ad082ae3e0a4ebf'
r=inspect(raw,[],True);sites,sections=r['relocation_sites'],r['sections']
begin,end=0x2e42b4,0x2e4508;code=bytearray(raw[begin:end]);bss=0x800000
u=Uc(UC_ARCH_PPC,UC_MODE_32|UC_MODE_BIG_ENDIAN);mapped=set()
def map_page(at):
    page=at&~4095
    if page not in mapped:u.mem_map(page,4096);mapped.add(page)
def target(t):
    module,section,addend,kind=t;assert module==1
    return (sections[section]['offset'] if sections[section]['offset'] else bss)+addend
for site,t in sites.items():
    if not begin<=site<end:continue
    at=target(t);kind=t[3];assert kind in (4,6)
    struct.pack_into('>H',code,site-begin,((at+0x8000)>>16 if kind==6 else at&65535)&65535)
    map_page(at)
    if sections[t[1]]['offset']:
        page=at&~4095;u.mem_write(page,raw[page:page+4096])
for at in range(0x10000,0x20000,4096):map_page(at)
map_page(0x30000);map_page(0x344bf4);map_page(begin);u.mem_write(begin,bytes(code))
# Execute genuine GetAmmo too, including its source cheat value and shared-pool
# selection. Only its owner/property lookup delegates are supplied by fixtures.
helper_begin,helper_end=0x2e47e4,0x2e4890
helper=bytearray(raw[helper_begin:helper_end])
for site,t in sites.items():
    if not helper_begin<=site<helper_end:continue
    at=target(t);kind=t[3];assert kind in (4,6)
    struct.pack_into('>H',helper,site-helper_begin,((at+0x8000)>>16 if kind==6 else at&65535)&65535)
    map_page(at)
    if sections[t[1]]['offset']:
        page=at&~4095;u.mem_write(page,raw[page:page+4096])
u.mem_write(helper_begin,bytes(helper))
infinite_at=sections[2]['offset']+108160
assert struct.unpack_from('>f',raw,infinite_at)[0]==100.0
for off in (65464,159384,180488):map_page(bss+off)
def word(at,n):u.mem_write(at,struct.pack('>I',n))
def f32(n):return struct.unpack('>f',struct.pack('>f',n))[0]
def bits(n):return struct.pack('>f',n)
def fp_set(reg,n):u.reg_write(reg,struct.unpack('>Q',struct.pack('>d',n))[0])
def fp_get(reg):return struct.unpack('>d',struct.pack('>Q',u.reg_read(reg)))[0]
ammo,inventory,core=0x12000,0x13000,0x14000
word(bss+65464,core);word(ammo,0x15000);word(inventory,0x16000)
word(0x15000+0x270,0x30000);word(0x16000+0x270,0x30000)
for channel in range(2):word(ammo+0x40+channel*16,0x19000)
word(0x19008,0x30004);word(0x1900c,0x30008)
for i in range(14):word(bss+159384+i*4,0x17000+i*16)
for i in range(17):word(bss+180488+i*4,0x18000+i*16)
state={}
def ret():u.reg_write(UC_PPC_REG_PC,u.reg_read(UC_PPC_REG_LR))
def hook(uc,at,size,user):
    f,s=state['f'],state['s'];channel=u.reg_read(UC_PPC_REG_4)
    if at==0x13ea8:u.reg_write(UC_PPC_REG_3,inventory if s[0] else 0);ret()
    elif at==0x30000:
        prop=u.reg_read(UC_PPC_REG_4);obj=u.reg_read(UC_PPC_REG_3)
        if obj==ammo:
            index=(prop-0x17004)//16;assert index in (4,5,6,7);value=f[index-3]
        else:
            assert obj==inventory;index=(prop-0x18004)//16;assert index in (1,4);value=f[5+(index==4)]
        u.mem_write(u.reg_read(UC_PPC_REG_5),bits(value));u.reg_write(UC_PPC_REG_3,1);ret()
    elif at==0x344bf4:
        index=(u.reg_read(UC_PPC_REG_4)-0x18004)//16;assert index in (0,3)
        fp_set(UC_PPC_REG_FPR1,f[7+(index==3)]);ret()
    elif at==0x2e4890:
        delta=f32(fp_get(UC_PPC_REG_FPR1));f[13+channel]=delta;s[9+channel]+=1
        if s[0] and s[11]:
            target_channel=0 if s[2] else channel;total=f32(f[7+target_channel]+delta)
            f[7+target_channel]=total if total<f[5+target_channel] else f[5+target_channel]
        ret()
    elif at in (0x30004,0x30008):
        channel=(u.reg_read(UC_PPC_REG_3)-ammo-0x40)//16;assert channel in (0,1)
        if at==0x30004:
            assert fp_get(UC_PPC_REG_FPR1)==0;s[5+channel]+=1;word(ammo+0x48+channel*16,1)
        else:fp_set(UC_PPC_REG_FPR1,f[11+channel])
        ret()
def writes(uc,access,at,size,value,user):
    if at in (ammo+0x48,ammo+0x58) and value==0:state['s'][7+(at-ammo-0x48)//16]+=1
u.hook_add(UC_HOOK_CODE,hook);u.hook_add(UC_HOOK_MEM_WRITE,writes)
lib=c.CDLL(str(a.library.resolve()));lib.ammo_recharge.argtypes=[c.POINTER(c.c_float),c.POINTER(c.c_int)];lib.ammo_recharge.restype=c.c_int
count=0
def compare(frame,flags):
    global count
    f=list(map(f32,frame));s=list(flags);state['f'],state['s']=f,s
    word(ammo+0x1c,inventory if s[0] else 0);u.mem_write(core+0x48,bits(f[0]))
    u.mem_write(ammo+0x31,bytes([int(bool(s[1]))]));word(ammo+0x3c,0x1a000);word(0x1a018,s[2])
    for ch in range(2):word(ammo+0x48+ch*16,int(bool(s[3+ch])));word(ammo+0x4c+ch*16,0)
    for ch in range(2):u.mem_write(ammo+0x34+ch*4,bits(f[9+ch]))
    u.reg_write(UC_PPC_REG_1,0x1f000);u.reg_write(UC_PPC_REG_3,ammo);u.reg_write(UC_PPC_REG_MSR,0x2000)
    try:u.emu_start(begin,end,count=2000)
    except Exception as failure:raise AssertionError((hex(u.reg_read(UC_PPC_REG_PC)),frame,flags)) from failure
    assert u.reg_read(UC_PPC_REG_PC)==end
    for ch in range(2):
        s[3+ch]=int(any(u.mem_read(ammo+0x48+ch*16,8)))
        f[9+ch]=struct.unpack('>f',u.mem_read(ammo+0x34+ch*4,4))[0] if frame[9+ch]>0 else f[9+ch]
    native_f=(c.c_float*15)(*frame);native_s=(c.c_int*12)(*flags)
    assert lib.ammo_recharge(native_f,native_s)==1
    assert [bits(v) for v in native_f]==[bits(v) for v in f],(frame,flags,list(native_f),f)
    assert list(native_s)==s,(frame,flags,list(native_s),s)
    count+=1

rng=random.Random(213924)
for owner in (0,1):
 for infinite in (0,1):
  for shared in (0,1):
   for active in ((0,0),(1,0),(0,1),(1,1)):
    for elapsed in (0,.49,.5,struct.unpack('>f',bytes.fromhex('3f000001'))[0],.75):
     for wait in (0,1/30):
      compare([1/60,2,3,10,10,30,30,0,0,wait,wait,elapsed,elapsed,0,0],
              [owner,infinite,shared,*active,0,0,0,0,0,0,1])
for owner in (0,1):
 for infinite in (0,1):
  compare([1/60,2,3,120,120,300,300,0,0,0,0,.75,.75,0,0],
          [owner,infinite,0,1,1,0,0,0,0,0,0,1])
for i in range(1500):
    compare([rng.choice([0,1/30,1/60,.25]),*([rng.uniform(0,10) for _ in range(2)]),
             *([rng.uniform(0,15) for _ in range(2)]),*([rng.uniform(0,30) for _ in range(2)]),
             *([rng.uniform(0,20) for _ in range(2)]),*([rng.choice([-.01,0,.01,.75]) for _ in range(2)]),
             *([rng.uniform(0,2) for _ in range(2)]),0,0],
            [rng.randrange(2),rng.randrange(2),rng.randrange(2),rng.randrange(2),rng.randrange(2),0,0,0,0,0,0,rng.randrange(2)])
a.output.write_text(json.dumps(dict(scope=__doc__,retail_sha256=digest,native_library_sha256=hashlib.sha256(a.library.read_bytes()).hexdigest(),
    differential_cases=count,retail_region=[hex(begin),hex(end)],retail_get_ammo_region=[hex(helper_begin),hex(helper_end)],
    retail_infinite_ammo_value=100.0,live_owner_or_world_binding=False),indent=2)+'\n')
print('PASS:',count,'complete retail Update control-flow comparisons; exact float bits and timer/delegate effects')
