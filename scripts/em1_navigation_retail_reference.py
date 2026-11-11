"""Execute original EM1 mesh conversion code in a developer-only PPC reference.

This is analysis of original retail instructions, not a production emulator,
world loader or native surface conversion. Original source files stay read-only.
"""
import argparse
import collections
import hashlib
import json
import struct
import ctypes as c
import random
import math
from pathlib import Path
from unicorn import Uc, UcError, UC_ARCH_PPC, UC_MODE_32, UC_MODE_BIG_ENDIAN, UC_HOOK_CODE
from unicorn.ppc_const import UC_PPC_REG_1, UC_PPC_REG_2, UC_PPC_REG_3, UC_PPC_REG_4, UC_PPC_REG_5, UC_PPC_REG_13, UC_PPC_REG_LR, UC_PPC_REG_PC, UC_PPC_REG_MSR
import unicorn.ppc_const as registers
from em1_rel_reference import inspect

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--module',type=Path,required=True)
p.add_argument('--dol',type=Path,required=True)
p.add_argument('--mesh',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--library',type=Path,help='Compare native source-floor containment against original code')
p.add_argument('--portal-steps',action='store_true',help='Also compare original directed intersections and portal steps')
a=p.parse_args()
module=a.module.read_bytes();dol=a.dol.read_bytes();mesh=a.mesh.read_bytes()
assert hashlib.sha256(module).hexdigest()=='ac25d520f329a3d0507d656f0ab1cf1c34025bb36ff866187ad082ae3e0a4ebf'
assert hashlib.sha256(dol).hexdigest()=='994779fa2e276983efdb50f02c406568f285a82558cb5d54bbdf74651a98a920'
assert mesh[:18]==b'KynogonRuntimeMesh'
reference=inspect(module,[],True);sections=reference['sections']
base,bss,data,stack,stop=0x81000000,0x81700000,0x90000000,0x83000000,0x83FFF000
u=Uc(UC_ARCH_PPC,UC_MODE_32|UC_MODE_BIG_ENDIAN)
u.mem_map(0x80000000,0x1800000);u.mem_map(stack,0x100000);u.mem_map(stop,4096);u.mem_map(data,0x1000000)
for i in range(18):
    off,address,size=[struct.unpack_from('>I',dol,x+i*4)[0] for x in (0,0x48,0x90)]
    if size:
        assert 0x80000000<=address and address+size<=0x81800000 and off+size<=len(dol)
        u.mem_write(address,dol[off:off+size])
for section in sections:
    if section['offset']:u.mem_write(base+section['offset'],module[section['offset']:section['offset']+section['size']])
for at,(mod,section,offset,kind) in reference['relocation_sites'].items():
    assert mod in (0,1)
    target=offset if mod==0 else bss+offset if section==8 else base+sections[section]['offset']+offset
    if kind==1:u.mem_write(base+at,struct.pack('>I',target))
    elif kind in (4,6):u.mem_write(base+at,struct.pack('>H',target&65535 if kind==4 else ((target+0x8000)>>16)&65535))
    else:
        assert kind==10
        delta=target-(base+at);assert -(1<<25)<=delta<(1<<25) and delta%4==0
        word=struct.unpack_from('>I',module,at)[0]
        u.mem_write(base+at,struct.pack('>I',(word&0xFC000003)|(delta&0x03FFFFFC)))
u.mem_write(data,mesh)
u.reg_write(UC_PPC_REG_1,stack+0xFF000);u.reg_write(UC_PPC_REG_2,0x80705B60);u.reg_write(UC_PPC_REG_13,0x80704520)
u.reg_write(UC_PPC_REG_3,data);u.reg_write(UC_PPC_REG_LR,stop);u.reg_write(UC_PPC_REG_MSR,0x2000)
history=collections.deque(maxlen=32);steps=0;paired_second={};paired_preservation_instructions=0
gekko_sqrt_instructions=0
# Gekko's documented/table-based reciprocal-square-root estimate; Unicorn's
# generic PPC estimate and single-multiply operand rules differ from Gekko.
estimate_rows=[(0x1a7e800,-0x568),(0x17cb800,-0x4f3),(0x1552800,-0x48d),(0x130c000,-0x435),
 (0x10f2000,-0x3e7),(0x0eff000,-0x3a2),(0x0d2e000,-0x365),(0x0b7c000,-0x32e),
 (0x09e5000,-0x2fc),(0x0867000,-0x2d0),(0x06ff000,-0x2a8),(0x05ab800,-0x283),
 (0x046a000,-0x261),(0x0339800,-0x243),(0x0218800,-0x226),(0x0105800,-0x20b),
 (0x3ffa000,-0x7a4),(0x3c29000,-0x700),(0x38aa000,-0x670),(0x3572000,-0x5f2),
 (0x3279000,-0x584),(0x2fb7000,-0x524),(0x2d26000,-0x4cc),(0x2ac0000,-0x47e),
 (0x2881000,-0x43a),(0x2665000,-0x3fa),(0x2468000,-0x3c2),(0x2287000,-0x38e),
 (0x20c1000,-0x35e),(0x1f12000,-0x332),(0x1d79000,-0x30a),(0x1bf4000,-0x2e6)]
def fpr(index):return struct.unpack('>d',struct.pack('>Q',u.reg_read(getattr(registers,f'UC_PPC_REG_FPR{index}'))))[0]
def set_fpr(index,value):u.reg_write(getattr(registers,f'UC_PPC_REG_FPR{index}'),struct.unpack('>Q',struct.pack('>d',value))[0])
def estimate(value):
    if value==0:return math.inf
    bits=struct.unpack('>Q',struct.pack('>d',value))[0];exponent=((bits>>52)&2047)-1023;fraction=(bits&((1<<52)-1))>>37
    start,slope=estimate_rows[(16 if exponent%2==0 else 0)+(fraction>>11)]
    return struct.unpack('>d',struct.pack('>Q',((1022-exponent//2)<<52)|((start+slope*(fraction&2047))<<26)))[0]
allocation_cursor=data+0x800000;allocations=[];frees=[]
# Original wrapper42C21C calls a configurable allocator's vtable+1C with
# (size, pointer-out). Supply just that memory service, not navigation behavior.
allocator,allocator_vtable,allocate_stub,free_stub=stop+0x100,stop+0x200,stop+0x300,stop+0x304
def target(at):
    mod,section,offset,kind=reference['relocation_sites'][at]
    return offset if mod==0 else bss+offset if section==8 else base+sections[section]['offset']+offset
assert target(0x42C23A)==target(0x42C23E)
u.mem_write(target(0x42C23A),struct.pack('>I',allocator))
u.mem_write(allocator,struct.pack('>I',allocator_vtable))
u.mem_write(allocator_vtable+0x1C,struct.pack('>I',allocate_stub))
u.mem_write(target(0x42C20E),struct.pack('>I',free_stub))
u.mem_write(target(0x42C182),struct.pack('>I',free_stub))
u.mem_write(allocate_stub,bytes.fromhex('4e8000204e800020'))
def trace(uc,address,size,user):
    global steps,allocation_cursor,paired_preservation_instructions,gekko_sqrt_instructions
    steps+=1;history.append(address)
    if address==0x80007204:
        set_fpr(4,estimate(fpr(0)));gekko_sqrt_instructions+=1;uc.reg_write(UC_PPC_REG_PC,address+4);return
    if address in (0x80007208,0x8000720C):
        operand=fpr(4 if address==0x80007208 else 1);bits=struct.unpack('>Q',struct.pack('>d',operand))[0]
        operand=struct.unpack('>d',struct.pack('>Q',((bits>>28)+((bits>>27)&1))<<28))[0]
        value=struct.unpack('>f',struct.pack('>f',fpr(4)*operand))[0]
        set_fpr(2 if address==0x80007208 else 1,value);gekko_sqrt_instructions+=1;uc.reg_write(UC_PPC_REG_PC,address+4);return
    word=struct.unpack('>I',uc.mem_read(address,4))[0]
    if word>>26 in (56,60):
        # Gekko psq_l/st are absent from Unicorn's scalar PPC model. This
        # original function uses them only for ABI register preservation.
        # Execute their unquantized two-float semantics, including PS1.
        assert base+0x42FED8<=address<=base+0x430230 or base+0x42CAE0<=address<=base+0x42D3B4
        fr,ra=(word>>21)&31,(word>>16)&31
        assert ra==1 and (word>>12)&15==0
        displacement=word&4095;displacement-=4096 if displacement&2048 else 0
        at=uc.reg_read(UC_PPC_REG_1)+displacement;reg=getattr(registers,f'UC_PPC_REG_FPR{fr}')
        if word>>26==60:
            first=struct.unpack('>d',struct.pack('>Q',uc.reg_read(reg)))[0]
            uc.mem_write(at,struct.pack('>2f',first,paired_second.get(fr,0.0)))
        else:
            first,second=struct.unpack('>2f',uc.mem_read(at,8));paired_second[fr]=second
            uc.reg_write(reg,struct.unpack('>Q',struct.pack('>d',first))[0])
        paired_preservation_instructions+=1;uc.reg_write(UC_PPC_REG_PC,address+4)
        return
    if address==allocate_stub:
        size=uc.reg_read(UC_PPC_REG_4);out=uc.reg_read(UC_PPC_REG_5)
        assert size<=0x100000 and allocation_cursor+size<data+0x1000000
        at=allocation_cursor;allocation_cursor+=(size+31)&~31;allocations.append(dict(address=hex(at),size=size))
        uc.mem_write(out,struct.pack('>I',at));uc.reg_write(UC_PPC_REG_3,1);uc.reg_write(UC_PPC_REG_PC,uc.reg_read(UC_PPC_REG_LR))
    elif address==free_stub:
        frees.append(hex(uc.reg_read(UC_PPC_REG_3)));uc.reg_write(UC_PPC_REG_PC,uc.reg_read(UC_PPC_REG_LR))
u.hook_add(UC_HOOK_CODE,trace)
error=''
initialization_return=None
try:
    # Original module static initializer for zero/Z/X/Y vector constants.
    u.reg_write(UC_PPC_REG_LR,stop);u.emu_start(base+0x46A908,stop,count=64)
    assert struct.unpack('>3f',u.mem_read(target(0x42CB7E),12))==(0.0,1.0,0.0)
    # Execute the original game's argument setup, then the same SDK initializer
    # it invokes. Do not patch its checks, keys or generated data masks.
    u.emu_start(base+0x1859B0,base+0x1859C8,count=16)
    key,seed=u.reg_read(UC_PPC_REG_4),u.reg_read(UC_PPC_REG_5)
    u.reg_write(UC_PPC_REG_3,key);u.reg_write(UC_PPC_REG_4,seed);u.reg_write(UC_PPC_REG_LR,stop)
    u.emu_start(base+0x42BEE8,stop,count=1000000)
    initialization_return=u.reg_read(UC_PPC_REG_3)
    if u.reg_read(UC_PPC_REG_PC)!=stop or initialization_return!=1:raise RuntimeError('Original SDK initializer did not succeed')
    u.reg_write(UC_PPC_REG_3,data);u.reg_write(UC_PPC_REG_LR,stop)
    u.emu_start(base+0x42ADD4,stop,count=2000000)
except UcError as failure:error=str(failure)
except RuntimeError as failure:error=str(failure)
pc=u.reg_read(UC_PPC_REG_PC);loader_return=u.reg_read(UC_PPC_REG_3);converted=bytes(u.mem_read(data,len(mesh)))
if not error and initialization_return==1 and pc==stop:
    assert loader_return==data+20
    for at in [28,32,36,40,44,48,52,56,60,64,72]:assert struct.unpack_from('>I',converted,at)[0]==struct.unpack_from('<I',mesh,at)[0]
    for at in [68,76]:
        original=struct.unpack_from('<I',mesh,at)[0]
        if original!=0xFFFFFFFF:assert struct.unpack_from('>I',converted,at)[0]==data+original
    assert len(allocations)==len(frees) and {x['address'] for x in allocations}==set(frees)
query_result=None
if not error and pc==stop and initialization_return==1 and a.library:
    d=c.CDLL(str(a.library.resolve()));U=c.POINTER(c.c_ubyte);S=c.c_size_t
    d.navigation_runtime_mesh.argtypes=[U,S,c.POINTER(S)];d.navigation_runtime_mesh.restype=c.c_char_p
    d.navigation_runtime_contains.argtypes=[S,c.POINTER(c.c_float),c.c_float];d.navigation_runtime_contains.restype=c.c_int
    d.navigation_runtime_find.argtypes=[c.POINTER(c.c_float)];d.navigation_runtime_find.restype=c.c_int
    d.navigation_intersection.argtypes=[c.POINTER(c.c_float),c.POINTER(c.c_float),c.POINTER(c.c_float)];d.navigation_intersection.restype=c.c_int
    d.navigation_portal_step.argtypes=[S,c.POINTER(c.c_float),c.c_float,c.POINTER(c.c_uint32),c.POINTER(c.c_int32),c.POINTER(c.c_float)];d.navigation_portal_step.restype=c.c_int
    raw=(c.c_ubyte*len(mesh)).from_buffer_copy(mesh);count=S();assert not d.navigation_runtime_mesh(raw,len(mesh),c.byref(count))
    word=lambda at:struct.unpack_from('<I',mesh,at)[0]
    floors=[]
    for cell in range(word(56)*word(60)):
        at=word(word(68)+cell*4)
        for layer in range(word(at)):floors.append(word(at+4+layer*12))
    assert len(floors)==count.value
    rng=random.Random(701);checks=0;errors=[];digest=hashlib.sha256();inside=outside=0;find_checks=0;find_errors=[]
    for index,at in enumerate(floors):
        assert struct.unpack_from('>4I',converted,at)==struct.unpack_from('<4I',mesh,at)
        edges=[]
        for field,stride in [(40,32),(48,44)]:
            for i in range(word(at+field)):edges.append(struct.unpack_from('<6f',mesh,at+word(at+field+4)+i*stride))
        for i in range(word(at+64)):edges.append(struct.unpack_from('<6f',mesh,at+word(at+word(at+68)+i*4)))
        points=[]
        if edges:
            xs=[edge[j]for edge in edges for j in (0,3)];zs=[edge[j]for edge in edges for j in (2,5)]
            lo,hi=struct.unpack_from('<2f',mesh,at+24)[::-1]
            points=[((min(xs)+max(xs))/2,(lo+hi)/2,(min(zs)+max(zs))/2),(min(xs)-1,lo,max(zs)+1)]
            for _ in range(4):points.append((rng.uniform(min(xs)-1,max(xs)+1),rng.uniform(lo-1,hi+1),rng.uniform(min(zs)-1,max(zs)+1)))
            for edge in edges[:3]:
                points.extend([edge[:3],edge[3:],tuple((edge[j]+edge[j+3])/2 for j in range(3))])
        for point in points:
            point=struct.unpack('>3f',struct.pack('>3f',*point))
            for tolerance in (0.0,0.01,0.1):
                tolerance=c.c_float(tolerance).value
                u.mem_write(data+0x700000,struct.pack('>3f',*point));u.reg_write(UC_PPC_REG_3,data+at);u.reg_write(UC_PPC_REG_4,data+0x700000)
                u.reg_write(registers.UC_PPC_REG_FPR1,struct.unpack('>Q',struct.pack('>d',tolerance))[0]);u.reg_write(UC_PPC_REG_LR,stop)
                try:u.emu_start(base+0x42FED8,stop,count=100000)
                except UcError as failure:error=str(failure);break
                assert u.reg_read(UC_PPC_REG_PC)==stop
                expected=u.reg_read(UC_PPC_REG_3);assert expected in (0,1)
                native=d.navigation_runtime_contains(index,(c.c_float*3)(*point),tolerance)
                digest.update(struct.pack('>I4fII',index,*point,tolerance,expected,native));checks+=1;inside+=expected;outside+=not expected
                if native!=expected and len(errors)<8:errors.append(dict(floor=index,point=point,tolerance=tolerance,original=expected,native=native))
            if error:break
            u.reg_write(UC_PPC_REG_3,data+20);u.reg_write(UC_PPC_REG_4,data+0x700000);u.reg_write(UC_PPC_REG_LR,stop)
            try:u.emu_start(base+0x42F8A8,stop,count=100000)
            except UcError as failure:error=str(failure);break
            assert u.reg_read(UC_PPC_REG_PC)==stop
            original=u.reg_read(UC_PPC_REG_3);expected=floors.index(original-data) if original else -1
            native=d.navigation_runtime_find((c.c_float*3)(*point));find_checks+=1
            digest.update(struct.pack('>3fii',*point,expected,native))
            if native!=expected and len(find_errors)<8:find_errors.append(dict(point=point,original=expected,native=native))
        if error:break
    query_result=dict(checks=checks,inside=inside,outside=outside,digest=digest.hexdigest(),mismatches=errors,
        original_entry_file_offset='0x42FED8',find_checks=find_checks,find_mismatches=find_errors,find_original_entry_file_offset='0x42F8A8',
        library_sha256=hashlib.sha256(a.library.read_bytes()).hexdigest())
    if a.portal_steps and not error:
        # Minimal original SDK world service data: unit scale+0C and the
        # statistics pointer+4C read by4685DC/469940. No query code is stubbed.
        u.mem_write(target(0x4685DE),struct.pack('>I',data+0x700400))
        u.mem_write(data+0x70040C,struct.pack('>f',1.0));u.mem_write(data+0x70044C,struct.pack('>I',data+0x700500))
        portal_cases=[]
        for index,at in enumerate(floors):
            for field,stride in [(40,32),(48,44)]:
                for j in range(min(word(at+field),2)):
                    edge=struct.unpack_from('<6f',mesh,at+word(at+field+4)+j*stride)
                    mx,my,mz=[(edge[k]+edge[k+3])/2 for k in range(3)];dx,dz=edge[3]-edge[0],edge[5]-edge[2];length=math.hypot(dx,dz)
                    if not length:continue
                    nx,nz=dz/length,-dx/length
                    inside=(mx+nx*.05,my,mz+nz*.05);outside=(mx-nx*.05,my,mz-nz*.05)
                    for movement in [inside+outside,outside+inside,(mx,my,mz)+outside,inside+inside]:portal_cases.append((index,edge,movement))
        segment_checks=portal_checks=0;segment_errors=[];portal_errors=[]
        for index,edge,movement in portal_cases:
            edge=struct.unpack('>6f',struct.pack('>6f',*edge));movement=struct.unpack('>6f',struct.pack('>6f',*movement))
            u.mem_write(data+0x700000,struct.pack('>6f',*movement));u.mem_write(data+0x700020,struct.pack('>6f',*edge))
            u.mem_write(data+0x700100,struct.pack('>6f',*([123.0]*6)))
            for reg,value in {3:data+0x700000,4:data+0x70000C,5:data+0x700020,6:data+0x70002C,7:data+0x700100,8:data+0x70010C}.items():u.reg_write(getattr(registers,f'UC_PPC_REG_{reg}'),value)
            u.reg_write(UC_PPC_REG_LR,stop)
            try:u.emu_start(base+0x459CBC,stop,count=100000)
            except UcError as failure:error=str(failure);break
            expected=u.reg_read(UC_PPC_REG_3);values=struct.unpack('>6f',u.mem_read(data+0x700100,24));out=(c.c_float*6)(*([123.0]*6))
            native=d.navigation_intersection((c.c_float*6)(*movement),(c.c_float*6)(*edge),out);segment_checks+=1
            if (expected,values)!=(native,tuple(out)) and len(segment_errors)<8:segment_errors.append(dict(edge=edge,movement=movement,original=[expected,values],native=[native,list(out)]))
            for previous in [[],[index],[-1]*16]:
                u.mem_write(data+0x700040,b'\0'*128)
                for i,floor_index in enumerate(previous):u.mem_write(data+0x700080+i*4,struct.pack('>I',data+floors[floor_index] if floor_index>=0 else 0))
                u.mem_write(data+0x7000C0,struct.pack('>I',len(previous)))
                for reg,value in {3:data+floors[index],4:data+0x700000,5:data+0x70000C,6:data+0x700040,7:data+0x700050,8:data+0x700060,9:data+0x700080,10:data+0x7000C0}.items():u.reg_write(getattr(registers,f'UC_PPC_REG_{reg}'),value)
                u.reg_write(UC_PPC_REG_LR,stop)
                try:u.emu_start(base+0x42CAE0,stop,count=100000)
                except UcError as failure:error=str(failure);break
                expected=u.reg_read(UC_PPC_REG_3);pointer=struct.unpack('>I',u.mem_read(data+0x700040,4))[0]
                expected_target=floors.index(pointer-data) if pointer else -1
                values=struct.unpack('>3f',u.mem_read(data+0x700050,12))+struct.unpack('>3f',u.mem_read(data+0x700060,12))
                n=struct.unpack('>I',u.mem_read(data+0x7000C0,4))[0];assert n<=16
                seen=[]
                for i in range(n):
                    ptr=struct.unpack('>I',u.mem_read(data+0x700080+i*4,4))[0];seen.append(floors.index(ptr-data) if ptr else -1)
                visited=(c.c_uint32*17)(len(previous),*[0xFFFFFFFF if x<0 else x for x in previous]);out=(c.c_float*6)();destination=c.c_int32(-10)
                native=d.navigation_portal_step(index,(c.c_float*6)(*movement),1.0,visited,c.byref(destination),out);portal_checks+=1
                native_seen=[int(x) if x!=0xFFFFFFFF else -1 for x in visited[1:1+visited[0]]]
                if (expected,expected_target,values,seen)!=(native,destination.value,tuple(out),native_seen) and len(portal_errors)<8:portal_errors.append(dict(floor=index,movement=movement,previous=previous,original=[expected,expected_target,values,seen],native=[native,destination.value,list(out),native_seen]))
            if error:break
        query_result['portal_step_comparison']=dict(segment_checks=segment_checks,segment_mismatches=segment_errors,portal_checks=portal_checks,portal_mismatches=portal_errors,
            original_intersection_entry='0x459CBC',original_step_entry='0x42CAE0',scope='Single loaded mesh; authored unbound links unresolved as in original loader; no world/cross-sector routing claim')
result=dict(scope=__doc__,module_sha256=hashlib.sha256(module).hexdigest(),dol_sha256=hashlib.sha256(dol).hexdigest(),
    mesh_sha256=hashlib.sha256(mesh).hexdigest(),entry_file_offset='0x42ADD4',returned=pc==stop,
    return_value=loader_return,pc=hex(pc),steps=steps,error=error,last_addresses=[hex(x) for x in history],
    sdk_initialization_return=initialization_return,
    changed_bytes=sum(x!=y for x,y in zip(mesh,converted,strict=True)),allocations=allocations,frees=frees,
    converted_header_words=[hex(x)for x in struct.unpack_from('>15I',converted,20)],floor_query_comparison=query_result,
    paired_register_preservation_instructions=paired_preservation_instructions,gekko_sqrt_cpu_instructions=gekko_sqrt_instructions,
    reference_emulator_used_in_production=False,world_activated=False)
a.output.write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
if error or pc!=stop or initialization_return!=1 or query_result and (query_result['mismatches'] or query_result['find_mismatches']):raise SystemExit(1)
if query_result and a.portal_steps:
    proof=query_result['portal_step_comparison']
    if proof['segment_mismatches'] or proof['portal_mismatches']:raise SystemExit(1)
