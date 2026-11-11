"""Check that native collision aliases preserve every other authored property."""
import ctypes as c
import json
import runpy
from pathlib import Path
from em1_scene_upgrade import inspect

root = Path(__file__).resolve().parents[1]
lib = c.CDLL(str(root / 'analysis/em1-level-collision-probe.dll'))
lib.prepare.argtypes = [c.c_wchar_p, c.c_char_p] + [c.POINTER(c.c_uint32)] * 4
lib.prepare.restype = c.c_char_p
lib.scene.argtypes = [c.c_uint32, c.POINTER(c.c_size_t)]
lib.scene.restype = c.POINTER(c.c_ubyte)
lib.hull.argtypes = [c.c_uint32, c.c_uint32, c.POINTER(c.c_size_t), c.POINTER(c.c_uint32)]
lib.hull.restype = c.POINTER(c.c_ubyte)
values = [c.c_uint32() for _ in range(4)]
error = lib.prepare('D:/Roms/Wii/Disney Epic Mickey (USA) (En,Fr,Es).rvz',
                    b'gv_zonea_start.level', *[c.byref(x) for x in values])
assert not error, error
read_pak = runpy.run_path(str(root / 'scripts/game-pak.py'))['read_pak']
scenes = read_pak(root / 'analysis/em1-compatibility/gv-original-scenes-movement-audit.pak')
changed = assets = 0
for index, (name, data, tag) in enumerate(scenes):
    size = c.c_size_t()
    result = lib.scene(index, c.byref(size))
    assert result, name
    old, new = inspect(data), inspect(c.string_at(result, size.value))
    assert old['refs'] == new['refs']
    assert len(old['entities']) == len(new['entities'])
    aliases = set()
    for a, b in zip(old['entities'], new['entities'], strict=True):
        assert a[:3] == b[:3]
        for ca, cb in zip(a[3], b[3], strict=True):
            assert ca[:3] == cb[:3]
            for pa, pb in zip(ca[3], cb[3], strict=True):
                if pa == pb:
                    continue
                assert ca[0] == 'PhysicsRigidBodyStaticComponent'
                assert pa[:4] == pb[:4] and pa[0] == 'RB Hull File Path'
                assert len(pb[4]) == 1 and pb[4][0].startswith('em1/runtime/')
                assert pb[4][0].endswith('.hkx')
                aliases.add(pb[4][0])
                changed += 1
    ordinal = 0
    while True:
        kind = c.c_uint32()
        payload = lib.hull(index, ordinal, c.byref(size), c.byref(kind))
        if not payload:
            break
        raw = c.string_at(payload, size.value)
        assert kind.value == 0x484B5800
        assert raw[:4] == bytes.fromhex('57e0e057')
        assert b'hk_2011.2.0-r1' in raw[:80]
        assets += 1
        ordinal += 1
    assert ordinal == len(aliases)
assert changed == lib.native_hulls() and changed > 0
report = dict(scenes=len(scenes), rebound_hull_properties=changed,
              native_hkx_resources=assets, all_other_source_data_preserved=True,
              native_world_ownership_verified=False)
(root / 'analysis/em1-compatibility/native-hull-bindings-result.json').write_text(json.dumps(report, indent=2))
print(json.dumps(report))
