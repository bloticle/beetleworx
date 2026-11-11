"""Read the live retail player factory and actor bindings in a private capture.

No input, process writes, suspension, injected calls or actor spawning. This
records prerequisites for extra actors, not proof that extra players work.
"""
import argparse
import ctypes as c
import hashlib
import json
import math
import struct
from datetime import datetime, timezone
from pathlib import Path
from live_guest_base import resolve_guest_base


def inspect(capture, include_components=False, include_input=False, include_initialization=False, all_input_actions=False):
    if all_input_actions and (not include_input or include_initialization):
        raise ValueError('--all-input-actions requires --include-input and cannot be combined with --include-initialization')
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

        def optional_text(address):
            if not address:
                return dict(text=None, error='null UID')
            try:
                data = read(address, 96)
                if b'\0' not in data:
                    return dict(text=None, error='no terminator within 96 bytes', hex=data.hex())
                return dict(text=data.split(b'\0', 1)[0].decode('ascii'))
            except (RuntimeError, UnicodeError) as exc:
                return dict(text=None, error=str(exc))

        def float_word(data):
            value = struct.unpack('>f', data)[0]
            return dict(bits=data.hex(), finite=math.isfinite(value), value=value if math.isfinite(value) else None)

        if include_initialization:
            before = read(0x833D0098, 0x324)
            count = read(0x833D03B8, 1)[0]
            if count > 32:
                raise RuntimeError('Native initialization module count exceeds its 32-record table')
            modules = []
            for index in range(count):
                record = 0x833D00B8 + index * 24
                name, init, deps = word(record), word(record + 8), word(record + 4)
                modules.append(dict(index=index, name=text(name) if name else None,
                                    initializer=f'{init:08X}', dependencies=text(deps) if deps else None,
                                    record=read(record, 24).hex()))
            return dict(mode='ReadProcessMemory only; native initialization, no execution or input',
                        stable=before == read(0x833D0098, 0x324), prefab_names={}, actors=[],
                        module_count=count, initialized_count=read(0x833D03B9, 1)[0],
                        initialization_enabled=read(0x833D03BA, 1)[0],
                        order=list(read(0x833D0098, count)), modules=modules,
                        component_registry=f'{word(0x833DBAF4):08X}')

        def controller_tasks(basic, entity):
            # ORIGINAL Basic constructor827863D8: action collection+140.
            # Mickey's Basic subobject is full+16, hence older full+156.
            collection = basic + 140
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
            actor['observed_control_mode'] = {0: 'native human mode0', 1: 'native AI mode1',
                                               2: 'native AI mode2'}.get(actor['mode'], 'other/unavailable native mode')
            actor['identity_note'] = 'Retail record slot; control mode/type observations do not prove character identity'
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
                        if label == 'basic':
                            listener = obj + 64
                            reference = word(obj + 80)
                            view.update(listener=f'{listener:08X}', listener_vtable=f'{word(listener):08X}',
                                listener_method=f'{word(word(listener)):08X}',
                                raw_owner=f'{word(obj + 48):08X}', owner_matches_entity=word(obj + 48) == entity - 8,
                                flags_52=read(obj + 52, 1)[0], captured_reference_241=read(obj + 241, 1)[0],
                                alternate_reference_292=f'{word(obj + 292):08X}', scripted_reference_297=read(obj + 297, 1)[0])
                            if reference:
                                ref_count, ref_list = word(reference + 44), word(reference + 36)
                                if ref_count > 128:
                                    raise RuntimeError('Unbounded retained movement reference components')
                                refs = struct.unpack('>' + str(ref_count) + 'I', read(ref_list, ref_count * 4)) if ref_count else []
                                view['reference_snapshot'] = dict(entity=f'{reference:08X}', vtable=f'{word(reference):08X}',
                                    guid=read(reference + 4, 16).hex(), components=[dict(face=f'{face:08X}',
                                    vtable=f'{word(face):08X}', property_getter_172=f'{word(word(face) + 172):08X}') for face in refs])
                    actor['controller_views'][label] = view
                camera = entities.get(camera_handles[slot], 0)
                actor['camera_entity'] = f'{camera:08X}'
                basic_view = actor['controller_views']['basic']
                if include_input and basic_view['slot'] != 255:
                    actor['input_tasks'] = controller_tasks(int(basic_view['object'], 16), entity)
                if all_input_actions and actor['controller_views']['mickey']['slot'] != 255:
                    full = int(actor['controller_views']['mickey']['object'], 16)
                    # ORIGINAL82A3C7A0 reads these fields on the full human
                    # controller. Tool object ownership beyond that list is
                    # deliberately not inferred from the task-object layout.
                    header = read(full + 684, 16)
                    tools, tool_count, _, selected = struct.unpack('>4I', header)
                    if tool_count > 128:
                        raise RuntimeError('Unbounded native human controller tool list')
                    tool_bytes = read(tools, tool_count * 4) if tool_count else b''
                    tool_objects = []
                    for index, obj in enumerate(struct.unpack('>' + str(tool_count) + 'I', tool_bytes)):
                        entry = dict(index=index, object=f'{obj:08X}', selected=index == selected,
                                     ownership='Member of this retail full-controller list; no speculative tool owner field')
                        try:
                            table = word(obj)
                            entry.update(vtable=f'{table:08X}', flags_at_8=f'{word(obj + 8):08X}',
                                         native_name_getter_80=f'{word(table + 80):08X}')
                        except RuntimeError as exc:
                            entry['read_error'] = str(exc)
                        tool_objects.append(entry)
                    actor['native_tools'] = dict(list=f'{tools:08X}', count=tool_count, selected_index=selected,
                                                 uid_fields=[dict(listener_offset=offset, uid=f'{word(full + 80 + offset):08X}',
                                                     **optional_text(word(full + 80 + offset))) for offset in range(280, 304, 4)],
                                                 objects=tool_objects)
                    if read(full + 684, 16) != header or (tool_bytes and read(tools, len(tool_bytes)) != tool_bytes):
                        raise RuntimeError('Native retail tool-list ownership changed during snapshot')
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
            current_uid = word(action_manager + 16)
            default_event = action_manager + 24
            # ORIGINAL827C3D78 compares pointer UIDs, chooses inline default
            # first, otherwise the first matching named event. No guest call.
            current_matches = ([default_event] if word(default_event) == current_uid else
                               [event for event in events[:-1] if word(event) == current_uid])
            current_event = current_matches[0] if current_matches else 0
            selection = dict(current_uid=f'{current_uid:08X}', selected=f'{current_event:08X}',
                             matches=[f'{event:08X}' for event in current_matches],
                             ambiguous=len(current_matches) > 1,
                             rule='Original827C3D78 inline-default first, otherwise first pointer-UID match')
            if all_input_actions:
                events = list(dict.fromkeys([default_event] + ([current_event] if current_event else [])))
            movement_uid = word(0x833DAEB8)
            wanted_uids = {word(address): name for name, address in
                           [('Movement', 0x833DAEB8), ('Jump', 0x833DAEBC), ('ReleaseJump', 0x833DAEC0),
                            ('Movement2', 0x833DAEC4), ('Jump2', 0x833DAEC8), ('ReleaseJump2', 0x833DAECC)]}
            wanted_uids.pop(0, None)
            profile = word(0x833DC87C)
            routes = []
            route_snapshots = [(0x833DC87C, read(0x833DC87C, 4))]
            if profile:
                route_snapshots.append((profile, read(profile, 16)))
            for logical_user in range(2):
                route = dict(logical_user=logical_user, profile=f'{profile:08X}', resolved=False)
                if profile:
                    user_slot, users = word(profile + 4 + logical_user * 4), word(profile + 12)
                    route.update(user_slot=user_slot, users=f'{users:08X}')
                    if user_slot < 4 and users:
                        route_snapshots.append((users + user_slot * 4, read(users + user_slot * 4, 4)))
                        obj = word(users + user_slot * 4)
                        route['user_object'] = f'{obj:08X}'
                        if obj:
                            route_snapshots.append((obj, read(obj, 4)))
                            getter = word(word(obj) + 8)
                            route_snapshots.append((word(obj) + 8, read(word(obj) + 8, 4)))
                            code = read(getter, 8)
                            instruction, terminal = struct.unpack('>2I', code)
                            route.update(getter_8=f'{getter:08X}', getter_code_hex=code.hex(), object_hex=read(obj, 32).hex())
                            # ORIGINAL827C43C0 calls this virtual after profile slot
                            # resolution. Recognize only exact native leaf getters.
                            offset = c.c_int16(instruction & 65535).value
                            if terminal == 0x4E800020 and instruction & 0xFFFF0000 == 0x80630000:
                                route.update(native_device=word(obj + offset), method='native leaf lwz r3,offset(r3); blr')
                                route_snapshots.append((obj + offset, read(obj + offset, 4)))
                            elif terminal == 0x4E800020 and instruction & 0xFFFF0000 == 0x38600000:
                                route.update(native_device=offset, method='native leaf li r3,index; blr')
                            if 0 <= route.get('native_device', -1) < 4:
                                route['resolved'] = True
                routes.append(route)
            captured = []
            snapshots = []
            action_snapshots = []
            contexts = []
            for event in dict.fromkeys(events):
                header = read(event, 24)
                _, listeners, listener_count, _, actions, action_count = struct.unpack('>6I', header)
                if listener_count > 128 or action_count > 256:
                    raise RuntimeError('Unbounded native input event contents')
                listener_bytes = read(listeners, listener_count * 4) if listener_count else b''
                action_bytes = read(actions, action_count * 4) if action_count else b''
                snapshots.append((event, header, listeners, listener_bytes, actions, action_bytes))
                listener_values = struct.unpack('>' + str(listener_count) + 'I', listener_bytes)
                action_values = struct.unpack('>' + str(action_count) + 'I', action_bytes)
                if all_input_actions:
                    listener_details = []
                    for index, listener in enumerate(listener_values):
                        entry = dict(index=index, object=f'{listener:08X}', retail_owners=[])
                        for actor in actors:
                            basic = actor.get('controller_views', {}).get('basic', {})
                            if basic.get('listener') == f'{listener:08X}':
                                entry['retail_owners'].append(dict(slot=actor['slot'], entity=actor['entity'],
                                    control_mode=actor['observed_control_mode'], proof='Exact validated Basic listener subobject'))
                        try:
                            table = word(listener)
                            entry.update(vtable=f'{table:08X}', callback=f'{word(table):08X}')
                        except RuntimeError as exc:
                            entry['read_error'] = str(exc)
                        listener_details.append(entry)
                    contexts.append(dict(event=f'{event:08X}', uid=f'{word(event):08X}', uid_text=optional_text(word(event)),
                        roles=[label for label, address in [('default', default_event), ('current', current_event)] if event == address],
                        listeners=listener_details, action_count=action_count,
                        duplicate_listeners=[f'{value:08X}' for value in sorted(set(listener_values)) if listener_values.count(value) > 1],
                        duplicate_action_pointers=[f'{value:08X}' for value in sorted(set(action_values)) if action_values.count(value) > 1]))
                for ordinal, action in enumerate(action_values):
                    action_uid = word(action)
                    if not all_input_actions and action_uid not in wanted_uids:
                        continue
                    shape = read(action, 436)
                    binding_count = word(action + 408)
                    if binding_count > 10:
                        raise RuntimeError('Unbounded native action binding records')
                    logical_user = word(action + 88)
                    if logical_user > 1 and not all_input_actions:
                        raise RuntimeError('Action binding has an unsupported logical user')
                    route = routes[logical_user] if logical_user < 2 else dict(logical_user=logical_user, resolved=False,
                        reason='Outside audited retail logical-user0/1 mapping; preserved without assuming a device')
                    input_system = word(action_manager + 72)
                    device_snapshot = None
                    if route['resolved']:
                        device = word(input_system + 72 + route['native_device'] * 4)
                        if device:
                            history = word(device + 60)
                            button_count, buttons = word(device + 48), word(device + 52)
                            axis_count = word(device + 56)
                            if button_count > 32 or axis_count > 32:
                                raise RuntimeError('Unbounded native device history')
                            device_snapshot = dict(input_system=f'{input_system:08X}', object=f'{device:08X}',
                                native_user=route['native_device'], vtable=f'{word(device):08X}', axis_count=axis_count, history=f'{history:08X}',
                                header_hex=read(device, 72).hex(), button_count=button_count, buttons=f'{buttons:08X}',
                                button_records_hex=read(buttons, button_count * 20).hex() if button_count else '',
                                ranges=[list(struct.unpack('>4i', read(history + i * 32 + 16, 16))) for i in range(axis_count if all_input_actions else min(2, axis_count))])
                            if all_input_actions:
                                device_snapshot['axis_records_hex'] = read(history, axis_count * 32).hex() if axis_count else ''
                    item = dict(event=f'{event:08X}', action=f'{action:08X}', ordinal=ordinal,
                        action_name=wanted_uids.get(action_uid, optional_text(action_uid)['text']),
                        native_uid_text=optional_text(action_uid)['text'], uid_read=optional_text(action_uid), shape_hex=shape.hex(),
                        event_uid=f'{word(event):08X}',
                        uid=f'{action_uid:08X}',
                        listener_count=listener_count, action_count=action_count,
                        listeners=[f'{value:08X}' for value in struct.unpack('>' + str(listener_count) + 'I', listener_bytes)],
                        kind=int.from_bytes(shape[80:84], 'big'),
                        binding_count=binding_count,
                        modifier_count=int.from_bytes(shape[412:416], 'big'),
                        axis_records=[dict(user=word(action + offset), key=f'{word(action + offset + 4):08X}',
                            destination=word(action + offset + 8), inverted=read(action + offset + 16, 1)[0])
                            for offset in range(88, 88 + binding_count * 32, 32)],
                        response=[float_word(shape[off:off + 4])['value'] for off in range(416, 432, 4)],
                        curve=int.from_bytes(shape[432:436], 'big'),
                        profile_route=route, device=device_snapshot)
                    if all_input_actions:
                        item.update(used_bindings=[dict(index=index, raw_hex=shape[88 + index * 32:120 + index * 32].hex(),
                            user=int.from_bytes(shape[88 + index * 32:92 + index * 32], 'big'),
                            key=f'{int.from_bytes(shape[92 + index * 32:96 + index * 32], "big"):08X}',
                            destination=int.from_bytes(shape[96 + index * 32:100 + index * 32], 'big'),
                            inverted=shape[104 + index * 32],
                            words=[f'{int.from_bytes(shape[off:off + 4], "big"):08X}' for off in range(88 + index * 32, 120 + index * 32, 4)])
                            for index in range(binding_count)],
                            modifiers_raw_412=f'{int.from_bytes(shape[412:416], "big"):08X}',
                            modifiers_note='Raw native +412 value; legacy modifier_count field does not establish modifier semantics',
                            prefix_state_words=[dict(offset=off, **float_word(shape[off:off + 4])) for off in range(4, 80, 4)],
                            prefix_note='Raw pre-kind state/timer words; float interpretations are mechanical, not assigned semantics',
                            response_words=[float_word(shape[off:off + 4]) for off in range(416, 432, 4)])
                        action_snapshots.append((action, shape, item))
                    captured.append(item)
            if (any(read(address, len(data)) != data for address, data in route_snapshots)
                    or word(0x833DBAD8) != action_manager or word(action_manager + 16) != current_uid or word(action_manager + 4) != event_list
                    or word(action_manager + 8) != event_count
                    or (event_count and read(event_list, event_count * 4) != event_bytes)
                    or any(read(event, 24) != header or (lb and read(lp, len(lb)) != lb)
                           or (ab and read(ap, len(ab)) != ab)
                           for event, header, lp, lb, ap, ab in snapshots)):
                raise RuntimeError('Input event ownership changed during snapshot')
            input_bindings = dict(manager=f'{action_manager:08X}', event_count=event_count,
                current_event_uid=f'{word(action_manager + 16):08X}',
                default_event_uid=f'{word(action_manager + 24):08X}', profile_routes=routes, actions=captured,
                movement=[action for action in captured if action['action_name'] == 'Movement'])
            if all_input_actions:
                for address, shape, item in action_snapshots:
                    after = read(address, 436)
                    # Live clocks may move without input or suspension. Pin
                    # identity/schema and report every other changed word.
                    if any(after[off:off + 4] != shape[off:off + 4] for off in (0, 80, 408, 412)):
                        raise RuntimeError('Native action identity/schema changed during snapshot')
                    item['changed_words_during_snapshot'] = [off for off in range(0, 436, 4) if after[off:off + 4] != shape[off:off + 4]]
                    used_count = item['binding_count']
                    item['used_binding_shape_stable'] = after[88:88 + used_count * 32] == shape[88:88 + used_count * 32]
                for context in contexts:
                    members = [item for item in captured if item['event'] == context['event']]
                    uids = set(item['uid'] for item in members)
                    context['duplicate_uids'] = [dict(uid=uid, ordinals=[item['ordinal'] for item in members if item['uid'] == uid])
                                                 for uid in sorted(uids) if sum(item['uid'] == uid for item in members) > 1]
                input_bindings.update(all_input_actions=True, context_selection=selection, contexts=contexts,
                    used_binding_shapes_stable=all(item['used_binding_shape_stable'] for item in captured),
                    snapshot_note='Read-only non-atomic live samples; immutable ownership/schema verified; changed action words explicitly reported')
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
    parser.add_argument('--all-input-actions', action='store_true', help='Bounded full records for default/current contexts; requires --include-input')
    parser.add_argument('--include-initialization', action='store_true')
    args = parser.parse_args()
    if args.all_input_actions and (not args.include_input or args.include_initialization):
        parser.error('--all-input-actions requires --include-input and cannot be combined with --include-initialization')
    result = inspect(args.capture, args.include_components, args.include_input, args.include_initialization, args.all_input_actions)
    with args.output.open('x', encoding='utf-8') as output:
        output.write(json.dumps(result, indent=2, allow_nan=False) + '\n')
    print(json.dumps(dict(output=str(args.output), stable=result['stable'], prefabs=result['prefab_names'], actors=result['actors'])))
