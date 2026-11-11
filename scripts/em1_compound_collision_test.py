"""Independent source/native portal-list and sphere-motion payload checks.

Uses original fixtures extracted read-only from the GV source archive. Checks
conversion semantics; native loading/world ownership need separate evidence.
"""
import ctypes as c
import json
import struct
from pathlib import Path

root = Path(__file__).resolve().parents[1]

class Pack:
    def __init__(self, raw):
        self.raw = raw
        self.word = lambda at: struct.unpack_from('>I', raw, at)[0]
        self.sections = [self.word(84+i*48) for i in range(3)]
        self.base = self.sections[2]
        self.pointers, self.classes = {}, {}
        local, glob, virt, end = [self.word(184+i*4) for i in range(4)]
        for at in range(self.base+local, self.base+glob, 8):
            source, target = self.word(at), self.word(at+4)
            if source != 0xFFFFFFFF:
                self.pointers[source] = target
        for at in range(self.base+glob, self.base+virt, 12):
            source, section, target = [self.word(at+i*4) for i in range(3)]
            if source != 0xFFFFFFFF:
                assert section == 2
                self.pointers[source] = target
        for at in range(self.base+virt, self.base+end, 12):
            obj, section, name = [self.word(at+i*4) for i in range(3)]
            if obj != 0xFFFFFFFF:
                start = self.sections[section]+name
                self.classes[obj] = raw[start:raw.index(0, start)].decode()

    def u(self, at):
        return self.word(self.base+at)

    def data(self, at, size):
        return self.raw[self.base+at:self.base+at+size]

    def one(self, name):
        values = [o for o, n in self.classes.items() if n == name]
        assert len(values) == 1, (name, values)
        return values[0]

lib = c.CDLL(str(root/'analysis/em1-level-collision-probe.dll'))
lib.prepare.argtypes = [c.c_wchar_p, c.c_char_p]+[c.POINTER(c.c_uint32)]*4
lib.prepare.restype = c.c_char_p
lib.hull.argtypes = [c.c_uint32, c.c_uint32, c.POINTER(c.c_size_t), c.POINTER(c.c_uint32)]
lib.hull.restype = c.POINTER(c.c_ubyte)
values = [c.c_uint32() for _ in range(4)]
error = lib.prepare('D:/Roms/Wii/Disney Epic Mickey (USA) (En,Fr,Es).rvz',
                   b'gv_zonea_start.level', *[c.byref(x) for x in values])
assert not error, error
portal = sphere = None
for scene in range(3):
    ordinal = 0
    while True:
        size, kind = c.c_size_t(), c.c_uint32()
        pointer = lib.hull(scene, ordinal, c.byref(size), c.byref(kind))
        if not pointer:
            break
        pack = Pack(c.string_at(pointer, size.value))
        if 'hkpListShape' in pack.classes.values():
            assert portal is None
            portal = pack
        if pack.data(pack.one('hkpRigidBody')+0xE8, 1) == b'\x02':
            assert sphere is None
            sphere = pack
        ordinal += 1
assert portal and sphere and values[3].value == 0
old = Pack((root/'analysis/em1-compatibility/GEN_Portal_Frame_static_hull.hkx_wii').read_bytes())
old_list, new_list = old.one('hkpListShape'), portal.one('hkpListShape')
assert old.u(old_list+0x1C) == portal.u(new_list+0x1C) == 3
assert old.data(old_list+0x24, 0x4C) == portal.data(new_list+0x24, 0x4C)
for source, target in [(old, old.one('hkpMoppCode')), (portal, portal.one('hkpMoppCode'))]:
    assert source.u(target+0x24) > 0
a, b = old.one('hkpMoppCode'), portal.one('hkpMoppCode')
assert old.data(a+0x10, 16) == portal.data(b+0x10, 16)
assert old.data(old.pointers[a+0x20], old.u(a+0x24)) == portal.data(portal.pointers[b+0x20], portal.u(b+0x24))
for i in range(3):
    a, b = old.pointers[old_list+0x18]+i*16, portal.pointers[new_list+0x18]+i*16
    assert old.u(a+4) == portal.u(b+4)
    assert portal.data(b+8, 8) == bytes(8), 'Native ignored cache fields must be initialized'
    sa, sb = old.pointers[a], portal.pointers[b]
    assert old.classes[sa] == portal.classes[sb] == 'hkpConvexVerticesShape'
    assert old.data(sa+0x10, 4) == portal.data(sb+0x10, 4)
    assert old.data(sa+0x20, 32) == portal.data(sb+0x20, 32)
    assert old.u(sa+0x4C) == portal.u(sb+0x4C)
    for field, width in [(0x40, 48), (0x54, 16)]:
        count = old.u(sa+field+4)
        assert count == portal.u(sb+field+4)
        assert old.data(old.pointers[sa+field], count*width) == portal.data(portal.pointers[sb+field], count*width)
old_sphere = Pack((root/'analysis/em1-compatibility/GV_CratesCardboard_Inert_dynamic_hull.hkx_wii').read_bytes())
a, b = old_sphere.one('hkpRigidBody'), sphere.one('hkpRigidBody')
assert old_sphere.data(a+0xE8, 1) == sphere.data(b+0xE8, 1) == b'\x02'
assert old_sphere.data(a+0x1A0, 48) == sphere.data(b+0x1A0, 48)
assert old_sphere.data(a+0xF0, 64) == sphere.data(b+0xF0, 64)
report = dict(portal_children=3, child_geometry_filters_masks_and_mopp_exact=True,
              sphere_motion_inertia_mass_velocities_and_pose_exact=True,
              unsupported_gv_static_resources=values[3].value, native_world_verified=False)
(root/'analysis/em1-compatibility/compound-collision-result.json').write_text(json.dumps(report, indent=2))
print(json.dumps(report))
