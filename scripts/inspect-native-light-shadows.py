"""Inspect native light/shadow ownership in an owned private game capture.

ReadProcessMemory only. No writes, thread suspension, code execution or input.
The component and light offsets are derived from retail 828A8C08/828A9CA8 and
82560348/82564D18, rather than inferred from the three allocated map textures.
"""
import argparse
import ctypes as c
import hashlib
import json
import re
import struct
from datetime import datetime, timezone
from pathlib import Path
from live_guest_base import resolve_guest_base

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('capture', type=Path)
p.add_argument('--output', type=Path, help='New evidence file; existing files are never replaced')
a = p.parse_args()
meta = json.loads((a.capture/'process.json').read_text(encoding='utf-8-sig'))
k = c.WinDLL('kernel32', use_last_error=True)
k.OpenProcess.argtypes = [c.c_uint32,c.c_bool,c.c_uint32]
k.OpenProcess.restype = c.c_void_p
k.ReadProcessMemory.argtypes = [c.c_void_p,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)]
k.CloseHandle.argtypes = [c.c_void_p]
ps = c.WinDLL('psapi', use_last_error=True)
ps.EnumProcessModules.argtypes = [c.c_void_p,c.c_void_p,c.c_uint32,c.POINTER(c.c_uint32)]
ps.GetModuleFileNameExW.argtypes = [c.c_void_p,c.c_void_p,c.c_wchar_p,c.c_uint32]
h = k.OpenProcess(0x410,False,meta['pid'])
assert h, 'Owned capture is no longer live'
try:
    modules = (c.c_void_p*2048)(); size = c.c_uint32()
    assert ps.EnumProcessModules(h,modules,c.sizeof(modules),c.byref(size))
    assert size.value <= c.sizeof(modules)
    path = c.create_unicode_buffer(32768)
    assert ps.GetModuleFileNameExW(h,modules[0],path,len(path))
    executable = Path(path.value)
    assert executable.name.lower() == 'beetleworx.exe'
    with executable.open('rb') as file:
        assert hashlib.file_digest(file,'sha256').hexdigest().lower() == meta['exe_sha256'].lower()

    def read_host(address,count):
        buffer = c.create_string_buffer(count); got = c.c_size_t()
        assert k.ReadProcessMemory(h,address,buffer,count,c.byref(got)) and got.value == count, hex(address)
        return buffer.raw

    guest_base = resolve_guest_base(modules[0],a.capture/'beetleworx.map',read_host)
    def read(address,count):
        assert 4096 <= address and count > 0 and address+count <= 2**32
        return read_host(guest_base+address+(4096 if address >= 0xe0000000 else 0),count)

    def word(address):
        return int.from_bytes(read(address,4),'big')

    def floats(address,count):
        return list(struct.unpack('>'+str(count)+'f',read(address,count*4)))

    registry = word(0x833da7e4)
    assert word(registry+4) == 0x827b58d0 and word(registry+8) == 0x82a69948
    buckets,count = word(registry+16),word(registry+20)
    assert 0 < count <= 1048576
    light_type = read(0x833de684,1)[0]  # Registered by 828A9CA8.
    assert light_type < read(0x833dbaf0,1)[0]
    owner = word(0x833e12ac)
    assert owner and word(owner) == 0x820fab5c
    slots_before = read(owner+152,24)
    result = dict(utc=datetime.now(timezone.utc).isoformat(),pid=meta['pid'],exe_sha256=meta['exe_sha256'],guest_virtual_base=hex(guest_base),mode='ReadProcessMemory only',
                  light_component_type=light_type,shadow_owner=f'{owner:08X}',
                  source_slots=[f'{word(owner+152+i*4):08X}' for i in range(3)],
                  generator_slots=[f'{word(owner+164+i*4):08X}' for i in range(3)],lights=[])
    nodes,handles = set(),set()
    for bucket in range(count):
        node = word(buckets+bucket*4)
        while node:
            assert node not in nodes and len(nodes) < 65536
            nodes.add(node)
            handle,entity = word(node),word(node+4)
            assert handle not in handles
            handles.add(handle)
            types = word(entity+88)
            slot = read(types+light_type,1)[0]
            if slot != 255:
                component = word(word(entity+36)+slot*4)-8
                assert read(word(component+56),64).split(b'\0')[0] in (b'JPS Light',b'JPSLightComponent')
                light = word(component+192)
                record = dict(handle=handle,entity=f'{entity:08X}',component=f'{component:08X}',
                              component_vtable=f'{word(component):08X}',authored_slot=word(component+164),light=f'{light:08X}')
                if light:
                    record.update(light_vtable=f'{word(light):08X}',effect_flags=read(light+240,4).hex(),
                                  world_translation=floats(light+192,3),ambient=floats(light+308,4),
                                  diffuse=floats(light+324,3),generator=f'{word(light+256):08X}')
                    generator = word(light+256)
                    if generator:
                        record['generator_vtable'] = f'{word(generator):08X}'
                        record['generator_effect_matches'] = word(generator+80) == light
                result['lights'].append(record)
            node = word(node+8)
    result['entity_count'] = len(nodes)
    observations = re.findall(r'Native depth-shadow preparation:.*?first_camera=([A-F0-9]{8}) camera_type=8203E610',
                              (a.capture/'game.log').read_text(encoding='utf-8',errors='replace'))
    if observations:
        camera = int(observations[-1],16)
        # Trace addresses are observations, not retained camera ownership.
        # Record the transform only if the exact live camera type still matches.
        if camera and word(camera) == 0x8203e610:
            result['traced_camera'] = dict(address=f'{camera:08X}',world_translation=floats(camera+192,3),
                projection_words={str(i):f'{word(camera+i):08X}' for i in range(304,352,4)})
    manager = word(0x833db1f0)
    selector = word(manager+20)
    result['selector'] = f'{selector:08X}'
    result['selector_records'] = []
    if selector:
        table = word(selector)
        result['selection_function'] = f'{word(table+40):08X}'
        # Audit only the exact retail selector whose linked list layout and
        # acceptance fields are established by 830B7E90.
        if table == 0x821df028 and word(table+40) == 0x830b7e90:
            collection = word(selector+8)
            node = word(collection+32)
            visited = set()
            while node:
                assert node not in visited and len(visited)<4096
                visited.add(node)
                result['selector_records'].append(dict(node=f'{node:08X}',
                    source_kind=word(node+8),native_light=f'{word(node+76):08X}',
                    mode=struct.unpack('>i',read(node+80,4))[0],strength=floats(node+48,1)[0],
                    position=floats(node+12,3),words={str(i):f'{word(node+i):08X}' for i in range(8,96,4)}))
                node = word(node+4)
    result['owner_slots_stable'] = word(0x833e12ac) == owner and read(owner+152,24) == slots_before
    result['registry_stable'] = word(0x833da7e4) == registry and word(registry+16) == buckets and word(registry+20) == count
    output = a.output or a.capture/'native-light-shadows-readonly.json'
    assert not output.exists(), 'Choose a fresh capture; evidence is never overwritten'
    output.write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
    print(json.dumps(dict(lights=len(result['lights']),constructed_lights=sum(x['light'] != '00000000' for x in result['lights']),
                         shadowed_lights=sum(x.get('generator','00000000') != '00000000' for x in result['lights']),
                         stable=result['owner_slots_stable'] and result['registry_stable'],output=str(output))))
finally:
    k.CloseHandle(h)
