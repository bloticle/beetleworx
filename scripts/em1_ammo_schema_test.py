"""Verify native ammo declarations against original REL registration/default code.

This validates the property schema, not component lifecycle or gameplay.
"""
import argparse
import ctypes as c
import hashlib
import json
import struct
from pathlib import Path
from unicorn import Uc, UC_ARCH_PPC, UC_MODE_32, UC_MODE_BIG_ENDIAN
from unicorn.ppc_const import UC_PPC_REG_4, UC_PPC_REG_MSR
from em1_rel_reference import inspect

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--module',type=Path,required=True)
p.add_argument('--library',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();data=a.module.read_bytes()
assert hashlib.sha256(data).hexdigest()=='ac25d520f329a3d0507d656f0ab1cf1c34025bb36ff866187ad082ae3e0a4ebf'
r=inspect(data,[],True);sites,sections=r['relocation_sites'],r['sections']
def word(at):return struct.unpack_from('>I',data,at)[0]
def address(module,section,offset,kind):
    assert module==1 and sections[section]['offset'] and offset<sections[section]['size']
    return sections[section]['offset']+offset
u=Uc(UC_ARCH_PPC,UC_MODE_32|UC_MODE_BIG_ENDIAN);u.reg_write(UC_PPC_REG_MSR,0x2000)
u.mem_map(0x1000000,4096);mapped=set();output=[]
library=c.CDLL(str(a.library.resolve()));library.ammo_property.argtypes=[c.c_size_t,c.POINTER(c.c_float)];library.ammo_property.restype=c.c_char_p
count_method=address(*sites[0x4f3000+0x308])
assert count_method==0x2e3d34 and word(count_method)>>16==0x3860 and word(count_method+4)==0x4e800020
count=word(count_method)&65535
assert count==14
for index in range(count):
    table=0x4f3540-index*40
    table_section_offset=table-sections[7]['offset']
    origins=[at&~3 for at,ref in sites.items() if ref==(1,7,table_section_offset,6) and 0x2e3000<=at<0x2e3d34]
    assert len(origins)==1,(index,origins)
    init=origins[0]
    assert word(init+8)==0x907d0000  # Store this original descriptor's vtable.
    assert sites[init+14][3]==6 and sites[init+22][3]==4
    assert sites[init+14][:3]==sites[init+22][:3]
    name_base=address(*sites[init+14]);instruction=word(init+24)
    pool_register=(word(init+20)>>21)&31
    assert instruction>>26==14 and (instruction>>21)&31==3 and (instruction>>16)&31==pool_register
    delta=struct.unpack('>h',struct.pack('>H',instruction&65535))[0]
    name_at=name_base+delta;name=data[name_at:data.index(0,name_at)].decode('ascii')
    callback=address(*sites[table+0x24]);code=bytearray(data[callback:callback+16])
    assert word(callback)>>16==0x3c60 and word(callback+4)>>16==0xc003
    assert word(callback+8)==0xd0040000 and word(callback+12)==0x4e800020
    constant=address(*sites[callback+2]);assert address(*sites[callback+6])==constant
    struct.pack_into('>H',code,2,(constant+0x8000)>>16)
    struct.pack_into('>H',code,6,constant&65535)
    for at in [callback,constant]:
        page=at&~4095
        if page not in mapped:
            u.mem_map(page,4096);u.mem_write(page,data[page:page+4096]);mapped.add(page)
    u.mem_write(callback,bytes(code));u.reg_write(UC_PPC_REG_4,0x1000000)
    u.mem_write(0x1000000,b'BAD!');u.emu_start(callback,callback+12,count=4)
    expected=bytes(u.mem_read(0x1000000,4));native=c.c_float(123)
    native_name=library.ammo_property(index,c.byref(native))
    assert native_name==name.encode() and struct.pack('>f',native.value)==expected,(name,native_name,native.value,expected.hex())
    output.append(dict(index=index,name=name,default_value=struct.unpack('>f',expected)[0],default_bits=expected.hex(),
        original_descriptor_table_file_offset=hex(table),original_name_binding_file_offset=hex(init),original_default_callback_file_offset=hex(callback)))
assert library.ammo_property(14,c.byref(c.c_float())) is None
result=dict(scope='Original compiled class schema/default parity; NOT authored GSA overrides, native component registration, update or world activation',
    original_rel_sha256=r['sha256'],native_library_sha256=hashlib.sha256(a.library.read_bytes()).hexdigest(),
    property_count=14,source_names_verified_from_original_registration=True,
    defaults_executed_as_original_ppc=True,properties=output)
a.output.write_text(json.dumps(result,indent=2)+'\n')
print('PASS:14 native ammo declarations match original REL property registration and executed PPC defaults')
