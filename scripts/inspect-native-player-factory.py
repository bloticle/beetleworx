"""Read the live retail player factory and actor bindings in a private capture.

No input, process writes, suspension, injected calls or actor spawning. This
records prerequisites for extra actors, not proof that extra players work.
"""
import argparse
import ctypes as c
import hashlib
import json
import struct
from datetime import datetime, timezone
from pathlib import Path
from live_guest_base import resolve_guest_base


def inspect(capture, include_components=False):
    meta = json.loads((capture / 'process.json').read_text(encoding='utf-8-sig'))
    kernel = c.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [c.c_uint32, c.c_bool, c.c_uint32]
    kernel.OpenProcess.restype = c.c_void_p
    kernel.ReadProcessMemory.argtypes = [c.c_void_p, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
    kernel.CloseHandle.argtypes = [c.c_void_p]
    kernel.GetProcessTimes.argtypes = [c.c_void_p] + [c.c_void_p] * 4
    psapi = c.WinDLL('psapi', use_last_error=True)
    psapi.EnumProcessModules.argtypes = [c.c_void_p, c.c_void_p, c.c_uint32, c.POINTER(c.c_uint32)]
    psapi.GetModuleFileNameExW.argtypes = [c.c_void_p, c.c_void_p, c.c_wchar_p, c.c_uint32]
    handle = kernel.OpenProcess(0x410, False, meta['pid'])
    if not handle:
        raise RuntimeError('Private capture process is no longer readable')
    try:
        times = [c.c_uint64() for _ in range(4)]
        if not kernel.GetProcessTimes(handle, *[c.byref(t) for t in times]):
            raise RuntimeError('Could not verify process creation time')
        created = datetime.fromtimestamp((times[0].value - 116444736000000000) / 10000000, timezone.utc)
        expected = datetime.fromisoformat(meta['started_utc'].replace('Z', '+00:00'))
        if abs((created - expected).total_seconds()) > 2:
            raise RuntimeError('Process ID was reused; refusing stale capture')
        modules = (c.c_void_p * 2048)()
        size = c.c_uint32()
        if not psapi.EnumProcessModules(handle, modules, c.sizeof(modules), c.byref(size)) or not 0 < size.value <= c.sizeof(modules):
            raise RuntimeError('Could not enumerate live module')
        name = c.create_unicode_buffer(32768)
        if not psapi.GetModuleFileNameExW(handle, modules[0], name, len(name)):
            raise RuntimeError('Could not verify executable path')
        executable = Path(name.value).resolve()
        wanted = Path(__file__).resolve().parents[1] / 'build/runtime23/beetleworx.exe'
        if executable != wanted.resolve():
            raise RuntimeError('Capture is not the canonical game executable')
        with executable.open('rb') as stream:
            if hashlib.file_digest(stream, 'sha256').hexdigest().lower() != meta['exe_sha256'].lower():
                raise RuntimeError('Capture and executable hashes differ')

        def host_read(address, count):
            data = c.create_string_buffer(count)
            got = c.c_size_t()
            if not kernel.ReadProcessMemory(handle, address, data, count, c.byref(got)) or got.value != count:
                raise RuntimeError(f'Unreadable memory at {address:X}')
            return data.raw

        base = resolve_guest_base(modules[0], capture / 'beetleworx.map', host_read)

        def read(address, count):
            if not 4096 <= address < address + count <= 2**32:
                raise RuntimeError('Invalid guest memory range')
            return host_read(base + address + (4096 if address >= 0xE0000000 else 0), count)

        def word(address):
            return int.from_bytes(read(address, 4), 'big')

        def text(address):
            return read(address, 96).split(b'\0', 1)[0].decode('ascii')

        registry = word(0x833DA7E4)
        if word(registry + 4) != 0x827B58D0 or word(registry + 8) != 0x82A69948:
            raise RuntimeError('Unknown native entity registry')
        manager = word(0x833DAB60)
        before = [read(manager + slot * 320 + 20, 20) for slot in range(2)]
        buckets, count = word(registry + 16), word(registry + 20)
        if not 0 < count <= 1048576:
            raise RuntimeError('Invalid native entity registry size')
        wanted_handles = {int.from_bytes(record[:4], 'big') for record in before} - {0}
        entities = {}
        visited = set()
        # This mirrors the native registry's node layout; handles are not
        # pointers. Do not read a fabricated third PlayerManager record.
        for node in struct.unpack('>' + str(count) + 'I', read(buckets, count * 4)):
            while node:
                if node in visited or len(visited) >= 65536:
                    raise RuntimeError('Cyclic or unexpectedly large entity registry')
                visited.add(node)
                key, entity, following = struct.unpack('>III', read(node, 12))
                if key in wanted_handles:
                    entities[key] = entity
                node = following
        motion_type = read(0x833DAF54, 1)[0]
        actors = []
        for slot, record in enumerate(before):
            key = int.from_bytes(record[:4], 'big')
            entity = entities.get(key, 0)
            actor = dict(slot=slot, handle=f'{key:08X}', entity=f'{entity:08X}',
                         controller=f'{int.from_bytes(record[8:12], "big"):08X}',
                         mode=int.from_bytes(record[12:16], 'big'))
            if entity:
                table = word(entity)
                actor.update(entity_vtable=f'{table:08X}',
                             int_property_getter=f'{word(table + 204):08X}',
                             int_property_setter=f'{word(table + 268):08X}')
                types, components = word(entity + 88), word(entity + 36)
                # Same audited transformation lookup as native_entity_probe.
                position_slot = read(types + read(0x833DBC3C, 1)[0], 1)[0]
                if position_slot != 255:
                    transform = word(components + position_slot * 4) - 8
                    actor['world_position'] = list(struct.unpack('>fff', read(transform + 172, 12)))
                if include_components:
                    component_count = word(entity + 44)
                    if component_count > 256:
                        raise RuntimeError('Invalid native actor component count')
                    actor['components'] = []
                    for index in range(component_count):
                        face = word(components + index * 4)
                        component_table = word(face)
                        actor['components'].append(dict(index=index, face=f'{face:08X}',
                            vtable=f'{component_table:08X}',
                            int_getter=f'{word(component_table + 204):08X}',
                            int_setter=f'{word(component_table + 268):08X}'))
                motion_slot = read(types + motion_type, 1)[0]
                actor['motion_slot'] = motion_slot
                if motion_slot != 255:
                    motion = word(components + motion_slot * 4) - 8
                    actor.update(motion=f'{motion:08X}', motion_vtable=f'{word(motion):08X}',
                                 motion_state=word(motion + 384), movement_enabled=read(motion + 640, 1)[0],
                                 desired_velocity=list(struct.unpack('>fff', read(motion + 240, 12))))
                    input_component = word(motion + 80)
                    actor['input_component'] = f'{input_component:08X}'
                    if input_component:
                        actor['input_enabled'] = read(input_component + 145, 1)[0]
                        actor['input_route'] = f'{word(input_component + 176):08X}'
            actors.append(actor)
        # Exact string addresses selected by retail 8276A828 (r4 slot, r5
        # human/3D-AI/2D-AI mode), recovered from its lis/addi instructions.
        string_base = ((-32248 << 16) - 30820) & 0xFFFFFFFF
        prefabs = {name: dict(address=f'{string_base + offset:08X}', name=text(string_base + offset))
                   for name, offset in [('mickey', -32), ('oswald_human', -24), ('oswald_ai_3d', -12), ('oswald_ai_2d', 0)]}
        table = word(manager)
        factory = word(table + 72)
        stable = (word(0x833DAB60) == manager and word(0x833DA7E4) == registry
                  and word(registry + 16) == buckets and word(registry + 20) == count
                  and all(read(manager + slot * 320 + 20, 20) == before[slot] for slot in range(2)))
        if not stable:
            raise RuntimeError('World changed during snapshot; rerun after loading')
        return dict(utc=datetime.now(timezone.utc).isoformat(), pid=meta['pid'],
                    exe_sha256=meta['exe_sha256'], created_utc=created.isoformat(),
                    mode='ReadProcessMemory only; no execution, writes, suspension or input',
                    manager=f'{manager:08X}', manager_vtable=f'{table:08X}', factory_method=f'{factory:08X}',
                    factory_contract='8276A828 accepts slot 0/1 only; 82743D18 resolves prefab then 82743AD8 clones it',
                    prefab_names=prefabs, actors=actors, stable=stable,
                    limitation='Two existing actors inspected. No additional actor creation, control, replication or 16-player support proven.')
    finally:
        kernel.CloseHandle(handle)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--include-components', action='store_true')
    args = parser.parse_args()
    result = inspect(args.capture, args.include_components)
    with args.output.open('x', encoding='utf-8') as output:
        output.write(json.dumps(result, indent=2, allow_nan=False) + '\n')
    print(json.dumps(dict(output=str(args.output), stable=result['stable'], prefabs=result['prefab_names'], actors=result['actors'])))
