"""Read-only collision ledger inspection for an owned live private capture.

Requires its exact executable hash and linker map. Native bodies and inline
MSVC container layouts are validated before interpreting ownership fields.
Never suspends threads, writes process memory, or sends gameplay input.
"""
import argparse
import ctypes as c
import hashlib
import json
import re
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('capture', type=Path)
p.add_argument('--include-native', action='store_true', help='Also inspect stock component systems identified by this capture log')
a = p.parse_args()
meta = json.loads((a.capture/'process.json').read_text(encoding='utf-8-sig'))
mapping = (a.capture/'beetleworx.map').read_text(errors='replace')
preferred = int(re.search(r'Preferred load address is ([0-9A-Fa-f]+)', mapping)[1], 16)
k = c.WinDLL('kernel32', use_last_error=True)
k.OpenProcess.argtypes = [c.c_uint32, c.c_bool, c.c_uint32]
k.OpenProcess.restype = c.c_void_p
k.ReadProcessMemory.argtypes = [c.c_void_p, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
k.CloseHandle.argtypes = [c.c_void_p]
ps = c.WinDLL('psapi', use_last_error=True)
ps.EnumProcessModules.argtypes = [c.c_void_p, c.c_void_p, c.c_uint32, c.POINTER(c.c_uint32)]
ps.GetModuleFileNameExW.argtypes = [c.c_void_p, c.c_void_p, c.c_wchar_p, c.c_uint32]
process = k.OpenProcess(0x410, False, meta['pid'])
assert process, 'Capture process is not available'
try:
    modules = (c.c_void_p*2048)()
    size = c.c_uint32()
    assert ps.EnumProcessModules(process, modules, c.sizeof(modules), c.byref(size))
    assert size.value <= c.sizeof(modules)
    main = modules[0]
    name = c.create_unicode_buffer(32768)
    assert ps.GetModuleFileNameExW(process, main, name, len(name))
    executable = Path(name.value)
    assert executable.name.lower() == 'beetleworx.exe'
    with executable.open('rb') as source:
        digest = hashlib.file_digest(source, 'sha256').hexdigest()
    assert digest.lower() == meta['exe_sha256'].lower(), 'Running executable does not match capture'

    def host(address, count):
        buffer = c.create_string_buffer(count)
        got = c.c_size_t()
        assert k.ReadProcessMemory(process, address, buffer, count, c.byref(got)) and got.value == count, hex(address)
        return buffer.raw

    def pointer(address):
        return int.from_bytes(host(address, 8), 'little')

    def guest(address, count):
        assert 4096 <= address and address+count <= 2**32
        return host(0x100000000+address, count)

    def word(address):
        return int.from_bytes(guest(address, 4), 'big')

    def symbol(prefix):
        lines = [line for line in mapping.splitlines() if prefix in line and 'em1_level_collision.cpp.obj' in line]
        assert len(lines) == 1, (prefix, len(lines))
        address = int(re.search(r' ([0-9A-Fa-f]{16}) +em1_level_collision.cpp.obj', lines[0])[1], 16)
        return main+address-preferred

    ledger = symbol('?live@')
    head, count = pointer(ledger), pointer(ledger+8)
    assert 0 <= count <= 256
    seen, rows = set(), []

    def walk(node):
        if node == head:
            return
        assert node and node not in seen and len(seen) < 256
        seen.add(node)
        walk(pointer(node))
        component = int.from_bytes(host(node+32, 4), 'little')
        owner = pointer(node+40)
        assert word(component-12) == 0x820CA608, 'Static component changed type'
        # The interface is complete-object+12. Its+80 byte is Is Painted,
        # not collision enablement. Native828C3C88 admits initial geometry
        # when owner entity virtual352 (827C8B08) reports no exclusion.
        complete_owner = word(component+48)
        entity = complete_owner+8
        assert complete_owner and word(word(entity)+352) == 0x827C8B08, 'Native owner admission method changed'
        excluded = guest(entity+60, 1) != b'\0'
        enabled = not excluded
        painted = guest(component+80, 1) != b'\0'
        real = guest(component+88, 1) != b'\0'
        start, end = pointer(owner), pointer(owner+8)
        assert 0 <= end-start <= 48*256 and (end-start) % 48 == 0
        world = int.from_bytes(host(owner+32, 4), 'little')
        assert host(owner+36, 1) != b'\0', 'Retained world reference missing'
        bodies = []
        for at in range(start, end, 48):
            body = int.from_bytes(host(at, 4), 'little')
            assert word(body) == 0x82030384
            actual = word(body+8)
            assert actual in (0, world)
            bodies.append(dict(body=hex(body), world=hex(actual), filter_info=hex(word(body+44)),
                               references=int.from_bytes(guest(body+6, 2), 'big')))
        rows.append(dict(component=hex(component), owner_entity=hex(entity), owner_excluded=excluded,
                         painted=painted, real=real, enabled=enabled, retained_world=hex(world), bodies=bodies,
                         membership_matches_enabled=all(int(b['world'], 16) == (world if enabled else 0) for b in bodies)))
        walk(pointer(node+16))

    walk(pointer(head+8))
    assert len(rows) == count
    native = []
    if a.include_native:
        observed = {}
        pattern = r'stock static physics owner: source_id=([0-9A-F]{8}) component=([0-9A-F]{8}) system=([0-9A-F]{8}) bodies=(\d+)'
        for match in re.finditer(pattern, (a.capture/'game.log').read_text(errors='replace')):
            source, component, system, number = match.groups()
            observed[int(component, 16)] = (source, int(system, 16), int(number))
        assert len(observed) <= 256
        for component, (source, expected_system, expected_count) in observed.items():
            assert word(component-12) == 0x820CA608, 'Logged native component no longer belongs to this world'
            system = word(component+128)
            assert system == expected_system and word(system) == 0x8202E78C, 'Native system ownership changed'
            number, array = word(system+12), word(system+8)
            assert number == expected_count and 0 < number <= 256
            entity = word(component+48)+8
            bodies = []
            for i in range(number):
                body = word(array+i*4)
                assert word(body) == 0x82030384
                shape, metadata = word(body+16), word(body+12)
                bodies.append(dict(body=hex(body), world=hex(word(body+8)), shape=hex(shape),
                                   shape_vtable=hex(word(shape)), shape_user_data=hex(word(shape+8)), filter_info=hex(word(body+44)),
                                   metadata=hex(metadata), metadata_owner=hex(word(metadata)) if metadata else None,
                                   metadata_matches_owner=bool(metadata and word(metadata) == entity)))
            native.append(dict(source_id=source, component=hex(component), system=hex(system),
                               owner_entity=hex(entity), painted=guest(component+80, 1) != b'\0',
                               real=guest(component+88, 1) != b'\0', bodies=bodies))
    result = dict(pid=meta['pid'], exe_sha256=digest, owners=rows,
                  native_owners=native,
                  pending_activations=pointer(symbol('?activation_pending@')+8),
                  all_membership_matches_enabled=all(r['membership_matches_enabled'] for r in rows),
                  read_only=True, gameplay_input_sent=False)
    (a.capture/'collision-owner-activation-readonly.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(dict(owners=len(rows), disabled_owners=sum(not r['enabled'] for r in rows),
                         native_owners=len(native),
                         all_membership_matches_enabled=result['all_membership_matches_enabled'],
                         pending_activations=result['pending_activations'])))
finally:
    k.CloseHandle(process)
