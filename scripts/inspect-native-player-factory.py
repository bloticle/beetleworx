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


def inspect(capture, include_components=False, include_input=False):
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

        def controller_tasks(controller, entity):
            collection = controller + 156
            buckets, bucket_count = word(collection + 16), word(collection + 20)
            dispatch, dispatch_count = word(collection + 36), word(collection + 40)
            if bucket_count > 4096 or dispatch_count > 128:
                raise RuntimeError('Unbounded native task collection')
            bucket_bytes = read(buckets, bucket_count * 4) if bucket_count else b''
            dispatch_bytes = read(dispatch, dispatch_count * 4) if dispatch_count else b''
            dispatch_tasks = set(struct.unpack('>' + str(dispatch_count) + 'I', dispatch_bytes))
            visited, named = set(), []
            for node in struct.unpack('>' + str(bucket_count) + 'I', bucket_bytes):
                while node:
                    if node in visited or len(visited) >= 256:
                        raise RuntimeError('Cyclic or oversized native task map')
                    visited.add(node)
                    key, task, following = struct.unpack('>3I', read(node, 12))
                    table, owner, flags = struct.unpack('>3I', read(task, 12))
                    named.append(dict(name=text(key), task=f'{task:08X}', vtable=f'{table:08X}',
                        owner=f'{owner:08X}', owner_matches_entity=owner == entity,
                        flags=f'{flags:08X}', in_dispatch=task in dispatch_tasks,
                        start=f'{word(table + 4):08X}', stop=f'{word(table + 8):08X}',
                        on_action=f'{word(table + 40):08X}'))
                    node = following
            if (word(collection + 16) != buckets or word(collection + 20) != bucket_count
                    or word(collection + 36) != dispatch or word(collection + 40) != dispatch_count
                    or (bucket_count and read(buckets, bucket_count * 4) != bucket_bytes)
                    or (dispatch_count and read(dispatch, dispatch_count * 4) != dispatch_bytes)):
                raise RuntimeError('Native task collection changed during snapshot')
            return dict(collection=f'{collection:08X}', bucket_count=bucket_count,
                        dispatch_count=dispatch_count, named=named)

        registry = word(0x833DA7E4)
        if word(registry + 4) != 0x827B58D0 or word(registry + 8) != 0x82A69948:
            raise RuntimeError('Unknown native entity registry')
        manager = word(0x833DAB60)
        before = [read(manager + slot * 320 + 20, 20) for slot in range(2)]
        buckets, count = word(registry + 16), word(registry + 20)
        if not 0 < count <= 1048576:
            raise RuntimeError('Invalid native entity registry size')
        wanted_handles = {int.from_bytes(record[:4], 'big') for record in before} - {0}
        camera_handles = []
        for record in before:
            controller = int.from_bytes(record[8:12], 'big')
            camera_handles.append(word(controller + 20) if controller else 0)
        wanted_handles.update(set(camera_handles) - {0})
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
                actor['controller_views'] = {}
                for label, global_type, adjustment in [('basic', 0x833DAE58, 8), ('mickey', 0x833E2188, 24)]:
                    type_id = read(global_type, 1)[0]
                    component_slot = read(types + type_id, 1)[0]
                    view = dict(type_id=type_id, slot=component_slot)
                    if component_slot != 255:
                        face = word(components + component_slot * 4)
                        obj = face - adjustment
                        vt = word(obj)
                        view.update(face=f'{face:08X}', face_vtable=f'{word(face):08X}', object=f'{obj:08X}', vtable=f'{vt:08X}',
                                    setter_108=f'{word(vt + 108):08X}')
                        view['retained_reference' if label == 'basic' else 'listener_vtable'] = f'{word(obj + 80):08X}'
                    actor['controller_views'][label] = view
                camera = entities.get(camera_handles[slot], 0)
                actor['camera_entity'] = f'{camera:08X}'
                mickey_view = actor['controller_views']['mickey']
                if include_input and mickey_view['slot'] != 255:
                    actor['input_tasks'] = controller_tasks(int(mickey_view['object'], 16), entity)
                if camera:
                    actor['camera_component_count'] = word(camera + 44)
                    actor['camera_component_vtables'] = [f'{word(word(word(camera + 36) + i * 4)):08X}' for i in range(min(word(camera + 44), 8))]
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
                    # This is the physical-motion interface, not an input route.
                    # Native82786BE8 reads virtual176 to reach the rigid body.
                    physics_component = word(motion + 80)
                    actor['physics_component'] = f'{physics_component:08X}'
                    if physics_component:
                        actor['physics_enabled'] = read(physics_component + 145, 1)[0]
                        actor['physics_interface_at_176'] = f'{word(physics_component + 176):08X}'
            actors.append(actor)
        input_bindings = None
        if include_input:
            action_manager = word(0x833DBAD8)
            event_list, event_count = word(action_manager + 4), word(action_manager + 8)
            if event_count > 128:
                raise RuntimeError('Unbounded native input event list')
            event_bytes = read(event_list, event_count * 4) if event_count else b''
            events = list(struct.unpack('>' + str(event_count) + 'I', event_bytes))
            events.append(action_manager + 24)  # Original827C3D78 default event.
            movement_uid = word(0x833DAEB8)
            captured = []
            snapshots = []
            for event in dict.fromkeys(events):
                header = read(event, 24)
                _, listeners, listener_count, _, actions, action_count = struct.unpack('>6I', header)
                if listener_count > 128 or action_count > 256:
                    raise RuntimeError('Unbounded native input event contents')
                listener_bytes = read(listeners, listener_count * 4) if listener_count else b''
                action_bytes = read(actions, action_count * 4) if action_count else b''
                snapshots.append((event, header, listeners, listener_bytes, actions, action_bytes))
                for action in struct.unpack('>' + str(action_count) + 'I', action_bytes):
                    if word(action) != movement_uid:
                        continue
                    shape = read(action, 436)
                    native_user = word(action + 88)
                    if native_user > 1:
                        raise RuntimeError('Movement binding has an unsupported native device index')
                    input_system = word(action_manager + 72)
                    device = word(input_system + 72 + native_user * 4)
                    history = word(device + 60)
                    captured.append(dict(event=f'{event:08X}', action=f'{action:08X}',
                        event_uid=f'{word(event):08X}',
                        uid=f'{movement_uid:08X}',
                        listener_count=listener_count, action_count=action_count,
                        listeners=[f'{value:08X}' for value in struct.unpack('>' + str(listener_count) + 'I', listener_bytes)],
                        kind=int.from_bytes(shape[80:84], 'big'),
                        binding_count=int.from_bytes(shape[408:412], 'big'),
                        modifier_count=int.from_bytes(shape[412:416], 'big'),
                        axis_records=[dict(user=word(action + offset), key=f'{word(action + offset + 4):08X}',
                            destination=word(action + offset + 8), inverted=read(action + offset + 16, 1)[0])
                            for offset in (88, 120)],
                        response=list(struct.unpack('>4f', shape[416:432])),
                        curve=int.from_bytes(shape[432:436], 'big'),
                        device=dict(input_system=f'{input_system:08X}', object=f'{device:08X}',
                            vtable=f'{word(device):08X}', axis_count=word(device + 56), history=f'{history:08X}',
                            ranges=[list(struct.unpack('>4i', read(history + i * 32 + 16, 16))) for i in range(2)])))
            if (word(0x833DBAD8) != action_manager or word(action_manager + 4) != event_list
                    or word(action_manager + 8) != event_count
                    or (event_count and read(event_list, event_count * 4) != event_bytes)
                    or any(read(event, 24) != header or (lb and read(lp, len(lb)) != lb)
                           or (ab and read(ap, len(ab)) != ab)
                           for event, header, lp, lb, ap, ab in snapshots)):
                raise RuntimeError('Input event ownership changed during snapshot')
            input_bindings = dict(manager=f'{action_manager:08X}', event_count=event_count,
                current_event_uid=f'{word(action_manager + 16):08X}',
                default_event_uid=f'{word(action_manager + 24):08X}', movement=captured)
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
                    factory_contract='8276A828 selects character 0=Mickey/1=Oswald and human/AI mode; it clones a fresh entity without replacing a PlayerManager slot',
                    prefab_names=prefabs, actors=actors, input_bindings=input_bindings, stable=stable,
                    limitation='Two existing actors inspected. No additional actor creation, control, replication or 16-player support proven.')
    finally:
        kernel.CloseHandle(handle)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--include-components', action='store_true')
    parser.add_argument('--include-input', action='store_true')
    args = parser.parse_args()
    result = inspect(args.capture, args.include_components, args.include_input)
    with args.output.open('x', encoding='utf-8') as output:
        output.write(json.dumps(result, indent=2, allow_nan=False) + '\n')
    print(json.dumps(dict(output=str(args.output), stable=result['stable'], prefabs=result['prefab_names'], actors=result['actors'])))
