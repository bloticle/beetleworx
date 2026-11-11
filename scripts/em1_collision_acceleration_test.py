"""Original GV terrain geometry/filter bytes survive acceleration staging.

This checks native pack structure only. Native MOPP-vs-mesh ray parity and
gameplay performance require a separate initialized-engine capture.
"""
import ctypes as c
import json
import struct
from pathlib import Path

root = Path(__file__).resolve().parents[1]
lib = c.CDLL(str(root / 'analysis/em1-level-collision-probe.dll'))
lib.prepare.argtypes = [c.c_wchar_p, c.c_char_p] + [c.POINTER(c.c_uint32)] * 4
lib.prepare.restype = c.c_char_p
lib.accelerate.argtypes = [c.c_uint32, c.POINTER(c.c_uint32), c.POINTER(c.c_uint64)]
lib.accelerate.restype = c.c_char_p
lib.accelerated_shape.argtypes = [c.c_uint32, c.POINTER(c.c_size_t)]
lib.accelerated_shape.restype = c.POINTER(c.c_ubyte)
lib.original_shape.argtypes = [c.c_uint32, c.c_uint32, c.POINTER(c.c_size_t)]
lib.original_shape.restype = c.POINTER(c.c_ubyte)
values = [c.c_uint32() for _ in range(4)]
error = lib.prepare('D:/Roms/Wii/Disney Epic Mickey (USA) (En,Fr,Es).rvz',
                    b'gv_zonea_start.level', *[c.byref(x) for x in values])
assert not error, error
bodies, triangles = c.c_uint32(), c.c_uint64()
error = lib.accelerate(0x20AF5, c.byref(bodies), c.byref(triangles))
assert not error, error

def word(data, at):
    return struct.unpack_from('>I', data, at)[0]

rows = []
for index in range(bodies.value):
    size = c.c_size_t()
    pointer = lib.original_shape(0x20AF5, index, c.byref(size))
    old = c.string_at(pointer, size.value)
    pointer = lib.accelerated_shape(index, c.byref(size))
    new = c.string_at(pointer, size.value)
    section = 64 + 2 * 48
    old_start, new_start = word(old, section + 20), word(new, section + 20)
    length = word(old, section + 24)
    assert old[old_start:old_start+length] == new[new_start:new_start+length]
    assert word(new, 28) == length and word(new, 24) == 2
    local_end = word(old, section + 28)
    bindings = {}
    for at in range(old_start + length, old_start + local_end, 8):
        source, target = word(old, at), word(old, at + 4)
        if source != 0xFFFFFFFF:
            bindings[source] = target
    if word(old, old_start + 0xBC) == 1:
        part = bindings[0xB8]
        assert old[old_start+0x20:old_start+0x90] == old[old_start+part:old_start+part+0x70]
        for field in (8, 0xC, 0x18, 0x20):
            assert bindings[0x20+field] == bindings[part+field]
    rows.append(dict(body=index, old_bytes=len(old), accelerated_bytes=len(new),
                     all_geometry_and_filter_payload_exact=True,
                     inline_subpart_bindings_match=True))
report = dict(bodies=bodies.value, triangles=triangles.value, results=rows,
              native_vm_parity_verified=False)
(root / 'analysis/em1-compatibility/ground-mopp-preparation-result.json').write_text(json.dumps(report, indent=2))
print(json.dumps(report))
