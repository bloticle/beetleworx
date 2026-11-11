"""Compare native ammo arithmetic against original retail PPC instructions.

Unicorn is developer-only. Execute only the retail arithmetic region, with
property inputs supplied; no world/lifecycle/inventory bridge is asserted.
"""
import argparse
import ctypes as c
import hashlib
import json
import random
import struct
from pathlib import Path
from unicorn import Uc, UC_ARCH_PPC, UC_MODE_32, UC_MODE_BIG_ENDIAN, UC_HOOK_MEM_WRITE
from unicorn.ppc_const import *
from em1_rel_reference import inspect

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--library',type=Path,required=True)
p.add_argument('--module',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
raw=a.module.read_bytes()
assert hashlib.sha256(raw).hexdigest()=='ac25d520f329a3d0507d656f0ab1cf1c34025bb36ff866187ad082ae3e0a4ebf'
report=inspect(raw,[],include_sites=True)
sites,sections=report['relocation_sites'],report['sections']
begin,end=0x2e4a90,0x2e4b14
code=bytearray(raw[begin:end])

def local_target(site):
    module,section,addend,kind=sites[site]
    assert module==1 and sections[section]['offset'] and addend<sections[section]['size']
    return sections[section]['offset']+addend,kind

# Apply actual linker HA/LO relocations to the isolated code's own constants.
constants=[]
for site in sites:
    if not begin<=site<end:continue
    target,kind=local_target(site)
    assert kind in (4,6)
    value=(target+0x8000)>>16 if kind==6 else target&65535
    struct.pack_into('>H',code,site-begin,value)
    constants.append(target)
negative,kind=local_target(0x2e4ac6)
assert len(constants)==3 and struct.unpack_from('>f',raw,negative)[0]==-1.0
zero,kind=local_target(0x2e4a6e)
assert struct.unpack_from('>f',raw,zero)[0]==0.0
u=Uc(UC_ARCH_PPC,UC_MODE_32|UC_MODE_BIG_ENDIAN)
u.mem_map(0x10000,0x10000)
mapped=set()
for target in [begin,*constants,zero]:
    page=target&~4095
    if page not in mapped:
        u.mem_map(page,4096);u.mem_write(page,raw[page:page+4096]);mapped.add(page)
u.mem_write(begin,bytes(code))
u.reg_write(UC_PPC_REG_MSR,0x2000)  # Enable original PPC floating-point instructions.
written=[]
u.hook_add(UC_HOOK_MEM_WRITE,lambda uc,access,address,size,value,user:written.append(address))
library=c.CDLL(str(a.library.resolve()))
F=c.c_float;I=c.c_int
library.ammo_change.argtypes=[F,F,I,F,F,F,F,c.POINTER(F),c.POINTER(F),c.POINTER(I),c.POINTER(F)]
library.ammo_change.restype=I

def f32(v):return struct.unpack('>f',struct.pack('>f',v))[0]
def bits(v):return struct.pack('>f',v)
def fp_write(reg,v):u.reg_write(reg,struct.unpack('>Q',struct.pack('>d',v))[0])
def fp_read(reg):return struct.unpack('>d',struct.pack('>Q',u.reg_read(reg)))[0]

def compare(current,delta,limit,normal,penalty,threshold,channel):
    current,delta,normal,penalty,threshold=map(f32,(current,delta,normal,penalty,threshold))
    limit=None if limit is None else f32(limit)
    u.reg_write(UC_PPC_REG_1,0x11000);u.reg_write(UC_PPC_REG_27,(zero+0x8000)&~65535)
    u.reg_write(UC_PPC_REG_28,int(limit is not None));u.reg_write(UC_PPC_REG_29,0x13000)
    u.reg_write(UC_PPC_REG_30,channel)
    for offset,v in [(0x10,limit or 0),(0x18,normal),(0x1c,penalty),(0x20,threshold)]:
        u.mem_write(0x11000+offset,bits(v))
    fp_write(UC_PPC_REG_FPR1,current);fp_write(UC_PPC_REG_FPR31,delta)
    written.clear();u.emu_start(begin,end,count=80)
    assert u.reg_read(UC_PPC_REG_PC)==end
    expected_amount,expected_applied=fp_read(UC_PPC_REG_FPR3),fp_read(UC_PPC_REG_FPR31)
    wait_at=0x13034+channel*4
    expected_delay=struct.unpack('>f',u.mem_read(wait_at,4))[0] if wait_at in written else None
    amount,applied,changes,wait=F(91),F(92),I(93),F(94)
    assert library.ammo_change(current,delta,limit is not None,limit or 0,normal,penalty,threshold,c.byref(amount),c.byref(applied),c.byref(changes),c.byref(wait))==1
    actual=(bits(amount.value),bits(applied.value),changes.value,bits(wait.value) if changes.value else None)
    expected=(bits(expected_amount),bits(expected_applied),int(expected_delay is not None),bits(expected_delay) if expected_delay is not None else None)
    assert actual==expected,((current,delta,limit,normal,penalty,threshold,channel),actual,expected)

count=0
for current in [0.,-0.,0.5,1.,5.,10.,12.]:
    for delta in [-20.,-10.,-5.,-1.,-0.5,-0.,0.,0.5,1.,5.,10.,20.]:
        for limit in [None,0.,5.,10.]:
            for channel in [0,1]:
                compare(current,delta,limit,.75,2.,1.,channel);count+=1
rng=random.Random(213924)
boundary=[struct.unpack('>f',struct.pack('>I',word))[0] for word in
    [0x00800000,0x3f7fffff,0x3f800000,0x3f800001,0x7f7fffff]]
for current in boundary:
    for delta in boundary:
        for signed_delta in [delta,-delta]:
            for limit in [None,1.,current]:
                compare(current,signed_delta,limit,.75,2.,1.,0);count+=1
for _ in range(3000):
    compare(rng.uniform(-2,30),rng.uniform(-20,20),None if rng.randrange(2) else rng.uniform(-2,25),
        rng.uniform(0,3),rng.uniform(0,5),rng.uniform(0,8),rng.randrange(2));count+=1
for field in range(7):
    for invalid in [float('nan'),float('inf'),-float('inf')]:
        values=[5.,1.,1,10.,.75,2.,1.]
        if field==2:continue  # Presence flag, not a floating-point property.
        values[field]=invalid
        amount,applied,changes,wait=F(91),F(92),I(93),F(94)
        assert library.ammo_change(*values,c.byref(amount),c.byref(applied),c.byref(changes),c.byref(wait))==0
        assert (amount.value,applied.value,changes.value,wait.value)==(91,92,93,94)
result=dict(scope='Finite-input AddAmmo arithmetic parity only; NOT complete recharge Update, component registration, inventory access or gameplay',
    original_rel_sha256=report['sha256'],original_file_region=[hex(begin),hex(end)],
    differential_cases=count,reference='Original PPC instructions executed by developer-only Unicorn with original constant relocations',
    native_library_sha256=hashlib.sha256(a.library.read_bytes()).hexdigest(),
    exact_float_bits_and_signed_zero=True,invalid_float_inputs_preserve_output=True)
a.output.write_text(json.dumps(result,indent=2)+'\n')
print(f'PASS:{count} native ammo changes match original PPC arithmetic, both channels; invalid inputs rejected transactionally')
