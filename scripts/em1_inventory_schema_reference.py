"""Inspect EM1's compiled InventoryComponent declarations and execute Float defaults.

Retail reference data only. Executes compiled scalar defaults and shared empty
array initialization. Does not register a class or activate an actor/world.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path
from unicorn import Uc, UC_ARCH_PPC, UC_MODE_32, UC_MODE_BIG_ENDIAN
from unicorn.ppc_const import UC_PPC_REG_4, UC_PPC_REG_28, UC_PPC_REG_MSR
from em1_rel_reference import inspect

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--module',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();data=a.module.read_bytes()
assert hashlib.sha256(data).hexdigest()=='ac25d520f329a3d0507d656f0ab1cf1c34025bb36ff866187ad082ae3e0a4ebf'
r=inspect(data,[],True);sites,sections=r['relocation_sites'],r['sections']
def word(at):return struct.unpack_from('>I',data,at)[0]
def address(ref):
    module,section,offset,kind=ref
    assert module==1 and sections[section]['offset'] and offset<sections[section]['size']
    return sections[section]['offset']+offset
count_method=address(sites[0x504BC0+0x308])
assert count_method==0x3430D0 and word(count_method)==0x38600011 and word(count_method+4)==0x4E800020
origins=[]
for at,ref in sites.items():
    if 0x341EF8<=at<0x342E88 and ref[0:2]==(1,7) and ref[3]==6:
        table=address(ref)
        if table not in [item[1] for item in origins]:origins.append((at&~3,table))
origins.sort();assert len(origins)==17
expected=['Paint','PaintMax','PaintAmmoSegmentAmount','Thinner','ThinnerMax','ThinnerAmmoSegmentAmount',
          'ETicket','ETicketMax','BigBall','SuperSaturated','ThinnerShockwave','LongRange',
          'Sketch','SketchCount','SketchMax','Invisible_Ammo','Indelible_Ammo']
u=Uc(UC_ARCH_PPC,UC_MODE_32|UC_MODE_BIG_ENDIAN);u.reg_write(UC_PPC_REG_MSR,0x2000)
u.mem_map(0x1000000,4096);u.mem_map(0x2000000,0x10000);mapped=set();properties=[]
array_initializers={}
assert word(0x142ADC)==0x3B800000 # Retail initializer establishes r28=0.
for kind,start,end,bss in [('Float',0x142B84,0x142BA4,19952),('Integer',0x142C00,0x142C24,19968),('String',0x142C40,0x142C64,19976)]:
    code=bytearray(data[start:end]);page=start&~4095
    if page not in mapped:u.mem_map(page,4096);u.mem_write(page,data[page:page+4096]);mapped.add(page)
    for at,ref in sites.items():
        if start<=at<end:
            assert ref[0]==1 and ref[3] in (4,6)
            target=0x2000000+ref[2] if ref[1]==8 else address(ref)
            struct.pack_into('>H',code,at-start,((target+0x8000)>>16) if ref[3]==6 else target&65535)
    u.mem_write(start,bytes(code));u.mem_write(0x2000000+bss,b'BAD!BAD!');u.reg_write(UC_PPC_REG_28,0)
    # Stop before __register_global_object; it registers the destructor, it
    # does not populate the vector. Execute all retail vector initialization.
    u.emu_start(start,end,count=32)
    assert bytes(u.mem_read(0x2000000+bss,8))==b'\0'*8
    array_initializers[kind]=dict(initializer_file_offset=hex(start),bss_offset=bss,
        data_pointer=0,size=0,capacity=0,original_initialization_executed=True)
for index,(init,table) in enumerate(origins):
    assert word(init+8) in (0x907E0000,0x907D0000) # Store this exact descriptor's table.
    assert sites[init+14][:3]==sites[init+22][:3]
    base=address(sites[init+14]);insn=word(init+24)
    pool_register=(word(init+20)>>21)&31
    assert insn>>26==14 and (insn>>21)&31==3 and (insn>>16)&31==pool_register
    delta=struct.unpack('>h',struct.pack('>H',insn&65535))[0]
    name_at=base+delta;name=data[name_at:data.index(0,name_at)].decode('ascii')
    assert name in expected and name not in [item['name'] for item in properties],(index,name)
    type_method=address(sites[table+28]);array_method=address(sites[table+32]);default_method=address(sites[table+36])
    array=word(array_method)==0x38600001
    row=dict(index=index,name=name,descriptor_table_file_offset=hex(table),name_binding_file_offset=hex(init),
             type_method_file_offset=hex(type_method),default_method_file_offset=hex(default_method),is_array=array)
    if not array:
        assert type_method==0x124E50 and default_method==0x124E40
        code=bytearray(data[default_method:default_method+16]);constant=address(sites[default_method+2])
        assert constant==address(sites[default_method+6])
        struct.pack_into('>H',code,2,(constant+0x8000)>>16);struct.pack_into('>H',code,6,constant&65535)
        for at in (default_method,constant):
            page=at&~4095
            if page not in mapped:u.mem_map(page,4096);u.mem_write(page,data[page:page+4096]);mapped.add(page)
        u.mem_write(default_method,bytes(code));u.reg_write(UC_PPC_REG_4,0x1000000)
        u.mem_write(0x1000000,b'BAD!');u.emu_start(default_method,default_method+12,count=4)
        bits=bytes(u.mem_read(0x1000000,4));assert bits==b'\0'*4
        row.update(type='Float',default_bits=bits.hex(),default_value=struct.unpack('>f',bits)[0],default_executed=True)
    else:
        kind={0x124FC0:'String',0x125124:'Integer',0x1253EC:'Float'}[type_method]
        assert (name,kind) in [('Sketch','String'),('SketchCount','Integer'),('SketchMax','Float')]
        row.update(type=kind,default_executed=False,shared_default_array_empty_initialization_verified=True,
                   shared_array_initializer=array_initializers[kind])
    properties.append(row)
assert {item['name'] for item in properties}==set(expected)
result=dict(scope=__doc__,original_rel_sha256=r['sha256'],property_count=17,
            original_initializer_file_offset='0x341ef8',scalar_defaults_executed=14,
            properties=properties,shared_array_initializers=array_initializers,
            inventory_component_registered=False,world_activated=False)
a.output.write_text(json.dumps(result,indent=2)+'\n')
print('PASS:17 original InventoryComponent names/types;14 compiled Float defaults zero;3 shared native array initializers execute empty')
