"""Build changed-only distribution patches from owned source and staged archives.

No player-side Python or patch executable is needed. The game validates both
SHA-256 identities and reconstructs the archives during normal content setup.
"""
from pathlib import Path
import argparse
import hashlib
import json
import struct
import zlib
import importlib.util

ARCHIVES = ('UIOptions.pak', 'UIPauseOptions.pak', 'UIhud.pak', 'UIStartGame.pak',
            'UIMainMenu.pak', '_Dynamic.pak', 'shaders.pak')
MAGIC = b'EM2DP01\0'
MAX_FILE = 64 * 1024 * 1024


def preserve_unchanged_resources(source_path, target_path):
    spec = importlib.util.spec_from_file_location('game_pak', Path(__file__).with_name('game-pak.py'))
    pak = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pak)

    def stored(path):
        raw = path.read_bytes()
        files = pak.read_pak(path)  # Validate all directory/extents/decoded sizes.
        endian = '>' if raw[:4] == b' KAP' else '<'
        header, relative = struct.unpack_from(endian+'II', raw, 12)
        offset = header+relative
        result = {}
        for i, (name, blob, kind) in enumerate(files):
            _, size, aligned = struct.unpack_from(endian+'III', raw, header+4+i*24)
            if name in result:
                raise ValueError('Duplicate archive resource: '+name)
            result[name] = (blob, kind, raw[offset:offset+size])
            offset += aligned
        return files, result

    _, originals = stored(source_path)
    files, modified = stored(target_path)
    strings, entries, payloads = bytearray(), [], []
    for name, blob, kind in files:
        folder, _, filename = name.replace('\\', '/').rpartition('/')
        folder_offset = len(strings)
        strings.extend(folder.encode()+b'\0')
        name_offset = len(strings)
        strings.extend(filename.encode()+b'\0')
        original = originals.get(name)
        packed = original[2] if original and original[0] == blob else modified[name][2]
        aligned = (len(packed)+31)&~31
        entries.append(struct.pack('>IIII', len(blob),len(packed),aligned,folder_offset)+kind+struct.pack('>I',name_offset))
        payloads.append(packed+bytes(aligned-len(packed)))
    directory = struct.pack('>I',len(files))+b''.join(entries)+strings
    offset = (32+len(directory)+31)&~31
    header = b' KAP'+struct.pack('>IIII',2,0,32,offset-32)+bytes(12)
    return header+directory+bytes(offset-32-len(directory))+b''.join(payloads), files, pak


def encode(source, target):
    if not source or not target or max(len(source), len(target)) > MAX_FILE:
        raise ValueError('Unsupported archive size')
    # PAK payloads are aligned to 32 bytes. Matching their stored bytes avoids
    # shipping unchanged compressed assets when directory/payload offsets move.
    index = {}
    for offset in range(0, len(source) - 31, 32):
        index.setdefault(source[offset:offset+32], offset)
    commands = bytearray()
    cursor = literal = 0
    copied = 0

    def emit_literal(end):
        if end > literal:
            commands.extend(b'\1' + struct.pack('<Q', end-literal) + target[literal:end])

    while cursor + 64 <= len(target):
        start = index.get(target[cursor:cursor+32])
        if start is None:
            cursor += 32
            continue
        maximum = min(len(source)-start, len(target)-cursor)
        length = 32
        while length + 4096 <= maximum and source[start+length:start+length+4096] == target[cursor+length:cursor+length+4096]:
            length += 4096
        low, high = length, min(length+4096, maximum)
        while low < high:
            mid = (low+high+1)//2
            if source[start+length:start+mid] == target[cursor+length:cursor+mid]:
                low = mid
            else:
                high = mid-1
        length = low & ~31
        if length < 64:
            cursor += 32
            continue
        emit_literal(cursor)
        commands.extend(b'\0' + struct.pack('<QQ', start, length))
        copied += length
        cursor += length
        literal = cursor
    emit_literal(len(target))
    header = MAGIC + struct.pack('<QQQ', len(source), len(target), len(commands))
    header += hashlib.sha256(source).digest() + hashlib.sha256(target).digest()
    patch = header + zlib.compress(commands, 9)
    if len(patch) > MAX_FILE:
        raise ValueError('Patch exceeds runtime bound')
    assert decode(source, patch) == target
    return patch, copied


def decode(source, patch):
    if patch[:8] == b'EM2DP02\0':
        packed, resources = struct.unpack_from('<QQ', patch, 96)
        if not packed or not resources or 112+packed >= len(patch):
            raise ValueError('Invalid regional patch envelope')
        return decode(source, MAGIC+patch[8:96]+patch[112:112+packed])
    if patch[:8] != MAGIC or len(patch) < 97:
        raise ValueError('Invalid patch')
    source_size, target_size, command_size = struct.unpack_from('<QQQ', patch, 8)
    if len(source) != source_size or hashlib.sha256(source).digest() != patch[32:64]:
        raise ValueError('Source mismatch')
    inflater = zlib.decompressobj()
    commands = inflater.decompress(patch[96:], command_size+1)
    if len(commands) != command_size or not inflater.eof or inflater.unused_data or inflater.unconsumed_tail:
        raise ValueError('Invalid command stream')
    output = bytearray()
    cursor = 0
    while cursor < len(commands):
        tag = commands[cursor]
        cursor += 1
        if tag == 0:
            offset, length = struct.unpack_from('<QQ', commands, cursor)
            cursor += 16
            if offset > len(source) or length > len(source)-offset:
                raise ValueError('Invalid copy')
            output.extend(source[offset:offset+length])
        elif tag == 1:
            length, = struct.unpack_from('<Q', commands, cursor)
            cursor += 8
            if length > len(commands)-cursor:
                raise ValueError('Invalid literal')
            output.extend(commands[cursor:cursor+length])
            cursor += length
        else:
            raise ValueError('Invalid command')
        if not length or len(output) > target_size:
            raise ValueError('Invalid length')
    if len(output) != target_size or hashlib.sha256(output).digest() != patch[64:96]:
        raise ValueError('Output mismatch')
    return bytes(output)


def regional_patch(source_path, target, expected_files, pak):
    original = {name.lower(): (blob, kind) for name, blob, kind in pak.read_pak(source_path)}
    endian = '>' if target[:4] == b' KAP' else '<'
    header, relative = struct.unpack_from(endian+'II', target, 12)
    offset = header+relative
    stored = []
    for i, (name, blob, kind) in enumerate(expected_files):
        _, packed, aligned = struct.unpack_from(endian+'III', target, header+4+i*24)
        stored.append((name, blob, kind, target[offset:offset+packed]))
        offset += aligned
    records = []
    for i, (name, blob, kind, packed) in enumerate(stored):
        key=name.lower()
        if original.get(key)==(blob,kind): continue
        mode=0 if key in original else 1
        anchor='' if not mode else next((n.lower() for n, *_ in stored[i+1:] if n.lower() in original),'*')
        records.append((mode,name,anchor,len(blob),kind,packed))
    target_names={name.lower() for name, *_ in stored}
    for name in original.keys()-target_names:
        records.append((2,name,'',0,b'\0'*4,b''))
    result=bytearray(b'EM2RP01\0'+struct.pack('<I',len(records)))
    for mode,name,anchor,size,kind,packed in records:
        result.extend(struct.pack('<I',mode))
        for value in (name,anchor):
            encoded=value.encode('utf-8');result.extend(struct.pack('<I',len(encoded))+encoded)
        result.extend(struct.pack('<II',size,len(packed))+kind+packed)
    if len(result)>MAX_FILE: raise ValueError('Regional patch exceeds native budget')
    return bytes(result),len(records)


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-dir', type=Path, default=Path('D:/Modding/Builds/EM2X360'))
    parser.add_argument('--runtime-dir', type=Path, default=root/'build/runtime23')
    parser.add_argument('--output', type=Path, default=root/'private/content-patches')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    report = {'format': 1, 'archives': []}
    for name in ARCHIVES:
        source_path = args.game_dir/'packfiles'/name
        input_path = args.runtime_dir/'pc-assets/packfiles'/name
        source = source_path.read_bytes()
        target, expected_files, pak = preserve_unchanged_resources(source_path, input_path)
        patch, copied = encode(source, target)
        resources, edits = regional_patch(source_path,target,expected_files,pak)
        packed=patch[96:]
        patch=b'EM2DP02\0'+patch[8:96]+struct.pack('<QQ',len(packed),len(resources))+packed+zlib.compress(resources,9)
        destination = args.output/(name+'.em2patch')
        temporary = destination.with_suffix('.tmp')
        temporary.write_bytes(patch)
        temporary.replace(destination)
        reference = args.output/'reconstructed-reference'/name
        reference.parent.mkdir(parents=True, exist_ok=True)
        reference.write_bytes(decode(source, patch))
        if pak.read_pak(reference) != expected_files:
            raise ValueError('Reconstructed archive changed resource bytes: '+name)
        report['archives'].append(dict(name=name, source_sha256=hashlib.sha256(source).hexdigest(),
            input_sha256=hashlib.sha256(input_path.read_bytes()).hexdigest(),
            output_sha256=hashlib.sha256(target).hexdigest(), patch_sha256=hashlib.sha256(patch).hexdigest(),
            original_bytes=len(source), output_bytes=len(target), patch_bytes=len(patch), copied_original_bytes=copied))
        report['archives'][-1]['resource_edits']=edits
        report['archives'][-1]['regional_patch_format']='EM2RP01'
        print(f'{name}: {len(target):,} output bytes -> {len(patch):,} patch bytes; {copied:,} reused from original', flush=True)
    (args.output/'manifest.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')


if __name__ == '__main__':
    main()
