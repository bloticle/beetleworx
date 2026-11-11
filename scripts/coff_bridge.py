"""Redirect a verified COFF caller while retaining original code/unwind data."""
import struct


def guard_pipeline_destruction(data: bytes):
    """Keep native shutdown ordering, but permit the documented failed-PSO state.

    Replace only Release + sized delete with a null-safe C gateway. Reuse the
    delete relocation, retaining section sizes, branches and unwind offsets.
    """
    machine, count, _, symbol_offset, symbol_count, optional, _ = struct.unpack_from('<HHIIIHH', data)
    if machine != 0x8664 or optional:
        raise RuntimeError('Expected normal AMD64 COFF for pipeline retirement')
    strings_at = symbol_offset + symbol_count * 18
    strings_size = struct.unpack_from('<I', data, strings_at)[0]
    strings = data[strings_at:strings_at + strings_size]
    symbols = {}
    index = 0
    while index < symbol_count:
        at = symbol_offset + index * 18
        raw = data[at:at + 8]
        name = (strings[struct.unpack_from('<I', raw, 4)[0]:] if raw[:4] == bytes(4) else raw).split(b'\0', 1)[0].decode()
        symbols[name] = (index, *struct.unpack_from('<IhHBB', data, at + 8))
        index += 1 + data[at + 17]
    method = '?Shutdown@PipelineCache@d3d12@graphics@rex@@QEAAXXZ'
    bridge = 'EM2DestroyNativePipeline'
    delete = '??3@YAXPEAX_K@Z'
    if method not in symbols or delete not in symbols or bridge in symbols:
        raise RuntimeError('Unexpected pipeline retirement symbol set')
    _, value, section, _, storage, auxiliary = symbols[method]
    if value != 0x14A0 or section <= 0 or storage != 2 or auxiliary:
        raise RuntimeError('Native pipeline shutdown layout changed')
    at = 20 + (section - 1) * 40
    size, raw, relocs = struct.unpack_from('<III', data, at + 16)
    relocation_count = struct.unpack_from('<H', data, at + 32)[0]
    span = value + 0x144
    expected = bytes.fromhex('488b0f488b01ff5010ba680000004889f9e800000000')
    if size < span + len(expected) or relocation_count == 0xFFFF or data[raw + span:raw + span + len(expected)] != expected:
        raise RuntimeError('Native pipeline release/delete sequence changed')
    selected = []
    for i in range(relocation_count):
        entry = relocs + i * 10
        address, target, kind = struct.unpack_from('<IIH', data, entry)
        if span <= address < span + len(expected):
            if (address, target, kind) != (span + 18, symbols[delete][0], 4):
                raise RuntimeError('Unexpected pipeline retirement relocation')
            selected.append(entry)
    if len(selected) != 1:
        raise RuntimeError('Expected one pipeline sized-delete relocation')
    prefix = bytearray(data[:symbol_offset])
    prefix[raw + span:raw + span + len(expected)] = bytes.fromhex('4889f9e800000000') + b'\x90' * (len(expected) - 8)
    struct.pack_into('<II', prefix, selected[0], span + 4, symbol_count)
    struct.pack_into('<I', prefix, 12, symbol_count + 1)
    table = data[symbol_offset:strings_at] + struct.pack('<IIIhHBB', 0, strings_size, 0, 0, 0x20, 2, 0)
    new_strings = strings[4:] + bridge.encode() + b'\0'
    result = bytes(prefix) + table + struct.pack('<I', len(new_strings) + 4) + new_strings + data[strings_at + strings_size:]
    allowed = set(range(12, 16)) | set(range(raw + span, raw + span + len(expected))) | set(range(selected[0], selected[0] + 8))
    if any(a != b and i not in allowed for i, (a, b) in enumerate(zip(data[:symbol_offset], result[:symbol_offset]))):
        raise RuntimeError('Unexpected pipeline retirement mutation')
    return result, {'method': method, 'bridge': bridge, 'patched_calls': 1, 'section_size_unchanged': True,
                    'native_worker_join_and_storage_shutdown_unchanged': True}


def redirect_pso_creation(data: bytes):
    """Replace one pinned COM creation call, reusing its now-unused IID relocation.

    No section grows and no branch/unwind offsets move. vzeroupper stays at
    the SDK site; the C gateway supplies the exact ID3D12PipelineState IID.
    """
    machine, count, _, symbol_offset, symbol_count, optional, _ = struct.unpack_from('<HHIIIHH', data)
    if machine != 0x8664 or optional:
        raise RuntimeError('Expected normal AMD64 COFF for PSO bridge')
    strings_at = symbol_offset + symbol_count * 18
    strings_size = struct.unpack_from('<I', data, strings_at)[0]
    strings = data[strings_at:strings_at + strings_size]
    symbols = {}
    i = 0
    while i < symbol_count:
        at = symbol_offset + i * 18
        raw = data[at:at + 8]
        name = (strings[struct.unpack_from('<I', raw, 4)[0]:] if raw[:4] == bytes(4) else raw).split(b'\0', 1)[0].decode()
        symbols[name] = (i, *struct.unpack_from('<IhHBB', data, at + 8))
        i += 1 + data[at + 17]
    method = '?CreateD3D12Pipeline@PipelineCache@d3d12@graphics@rex@@AEAAPEAUID3D12PipelineState@@AEBUPipelineRuntimeDescription@1234@@Z'
    iid = '_GUID_765a30f3_f624_4c6f_a828_ace948622445'
    bridge = 'EM2CreateGraphicsPipelineStateCached'
    if method not in symbols or iid not in symbols or bridge in symbols:
        raise RuntimeError('Unexpected native PSO symbol set')
    _, value, section, _, storage, auxiliary = symbols[method]
    if value != 0x5AC0 or section <= 0 or storage != 2 or auxiliary:
        raise RuntimeError('Native PSO method layout changed')
    at = 20 + (section - 1) * 40
    size, raw, relocs = struct.unpack_from('<III', data, at + 16)
    relocation_count = struct.unpack_from('<H', data, at + 32)[0]
    if size < 0x6168 or relocation_count == 0xFFFF:
        raise RuntimeError('Unsupported native PSO section layout')
    if data[raw + 0x6150:raw + 0x616A] != bytes.fromhex('4c8d0500000000488d55b84c8d8d68020000c5f877ff505085c0'):
        raise RuntimeError('Native PSO IID/arguments/vzeroupper/call/test sequence changed')
    selected = []
    for i in range(relocation_count):
        entry = relocs + i * 10
        address, target, kind = struct.unpack_from('<IIH', data, entry)
        if 0x6150 <= address < 0x6168:
            if (address, target, kind) != (0x6153, symbols[iid][0], 4):
                raise RuntimeError('Unexpected relocation inside PSO patch span')
            selected.append(entry)
    if len(selected) != 1:
        raise RuntimeError('Expected one native PSO IID relocation')
    prefix = bytearray(data[:symbol_offset])
    prefix[raw + 0x6150:raw + 0x6157] = bytes.fromhex('c5f8774531c090')
    prefix[raw + 0x6162:raw + 0x6168] = bytes.fromhex('e80000000090')
    struct.pack_into('<II', prefix, selected[0], 0x6163, symbol_count)
    struct.pack_into('<I', prefix, 12, symbol_count + 1)
    table = data[symbol_offset:strings_at] + struct.pack('<IIIhHBB', 0, strings_size, 0, 0, 0x20, 2, 0)
    new_strings = strings[4:] + bridge.encode() + b'\0'
    result = bytes(prefix) + table + struct.pack('<I', len(new_strings) + 4) + new_strings + data[strings_at + strings_size:]
    allowed = set(range(12, 16)) | set(range(raw + 0x6150, raw + 0x6157)) | set(range(raw + 0x6162, raw + 0x6168)) | set(range(selected[0], selected[0] + 8))
    if any(a != b and i not in allowed for i, (a, b) in enumerate(zip(data[:symbol_offset], result[:symbol_offset]))):
        raise RuntimeError('Unexpected PSO bridge mutation')
    return result, {'method': method, 'bridge': bridge, 'call_offset': 0x6162, 'reused_iid_relocation': 0x6153,
                    'new_call_relocation': 0x6163, 'patched_calls': 1, 'section_size_unchanged': True}


def redirect_vtable_definition(data: bytes, symbol: str, bridge: str, vtable: str):
    return _redirect_definition(data,symbol,bridge,vtable)


def redirect_direct_call_definition(data: bytes, symbol: str, bridge: str, expected_calls=1):
    return _redirect_definition(data,symbol,bridge,None,expected_calls)


def _redirect_definition(data: bytes, symbol: str, bridge: str, vtable, expected_calls=1):
    machine, count, _, symbol_offset, symbol_count, optional_size, _ = struct.unpack_from('<HHIIIHH', data)
    if machine != 0x8664 or optional_size:
        raise RuntimeError('Expected a normal AMD64 COFF object')
    strings_at = symbol_offset + symbol_count * 18
    strings_size = struct.unpack_from('<I', data, strings_at)[0]
    strings = data[strings_at:strings_at + strings_size]
    if len(strings) != strings_size:
        raise RuntimeError('Truncated COFF string table')

    def name(raw):
        if raw[:4] == bytes(4):
            return strings[struct.unpack_from('<I', raw, 4)[0]:].split(b'\0', 1)[0].decode()
        return raw.split(b'\0', 1)[0].decode()

    symbols = {}
    index = 0
    while index < symbol_count:
        at = symbol_offset + index * 18
        symbols[name(data[at:at + 8])] = (index, struct.unpack_from('<IhHBB', data, at + 8))
        index += 1 + data[at + 17]
    if symbol not in symbols or (vtable is not None and vtable not in symbols) or bridge in symbols:
        raise RuntimeError('Unsupported bridged method/caller symbol set')
    target, (_, target_section, symbol_type, storage, auxiliary) = symbols[symbol]
    vtable_section=symbols[vtable][1][1] if vtable is not None else 0
    if target_section <= 0 or storage != 2 or auxiliary or (vtable is not None and vtable_section <= 0):
        raise RuntimeError('Expected a defined external method and, when requested, a defined vtable')
    prefix = bytearray(data[:symbol_offset])
    changed = []
    metadata_references = 0
    for section in range(count):
        at = 20 + section * 40
        raw_name = data[at:at + 8].split(b'\0', 1)[0].decode()
        section_name = strings[int(raw_name[1:]):].split(b'\0', 1)[0].decode() if raw_name.startswith('/') else raw_name
        if section_name == '.llvm_addrsig':
            raise RuntimeError('Remove address-significance metadata before bridging')
        relocations = struct.unpack_from('<I', data, at + 24)[0]
        relocation_count = struct.unpack_from('<H', data, at + 32)[0]
        if relocation_count == 0xFFFF:
            raise RuntimeError('Extended COFF relocations require separate review')
        for relocation in range(relocation_count):
            entry = relocations + relocation * 10
            address, referenced, kind = struct.unpack_from('<IIH', data, entry)
            if referenced != target:
                continue
            if section + 1 == vtable_section:
                if kind != 1:
                    raise RuntimeError('Expected an ADDR64 vtable slot')
                struct.pack_into('<I', prefix, entry + 4, symbol_count)
                changed.append((entry + 4, address))
            elif vtable is None and section_name == '.text':
                size,raw=struct.unpack_from('<II',data,at+16)
                executable=struct.unpack_from('<I',data,at+36)[0]&0x20000000
                if not executable or kind!=4 or address<1 or address+4>size or data[raw+address-1]!=0xE8:
                    raise RuntimeError('Expected a direct REL32 call instruction')
                struct.pack_into('<I',prefix,entry+4,symbol_count)
                changed.append((entry+4,address))
            elif section_name.startswith(('.debug', '.pdata', '.xdata')):
                # These describe the retained original function, not its caller.
                metadata_references += 1
            else:
                raise RuntimeError(f'Unexpected bridged method reference in {section_name}')
    if len(changed) != expected_calls:
        raise RuntimeError(f'Expected exactly {expected_calls} selected caller references, found {len(changed)}')

    table = bytearray(data[symbol_offset:strings_at])
    original_name = bytes(table[target * 18:target * 18 + 8])
    table[target * 18:target * 18 + 8] = struct.pack('<II', 0, strings_size)
    table += original_name + struct.pack('<IhHBB', 0, 0, symbol_type, 2, 0)
    new_strings = strings[4:] + bridge.encode() + b'\0'
    struct.pack_into('<I', prefix, 12, symbol_count + 1)
    result = bytes(prefix) + bytes(table) + struct.pack('<I', len(new_strings) + 4) + new_strings + data[strings_at + strings_size:]
    # All code/data/unwind/debug bytes and every unrelated relocation stay
    # identical. Only symbol metadata and the selected caller relocation change.
    permitted = set(range(12, 16))
    for offset,_ in changed:permitted.update(range(offset,offset+4))
    if any(a != b and i not in permitted for i, (a, b) in enumerate(zip(data[:symbol_offset], result[:symbol_offset]))):
        raise RuntimeError('Unexpected COFF section mutation')
    if vtable is not None:
        metadata={'factory_symbol':symbol,'bridge':bridge,'vtable_symbol':vtable,'redirected_slots':len(changed)}
    else:
        metadata={'method_symbol':symbol,'bridge':bridge,'redirected_calls':len(changed)}
    return result, dict(metadata,retained_metadata_references=metadata_references)
