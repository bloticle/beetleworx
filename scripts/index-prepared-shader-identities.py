"""Map prepared PC shader binaries to exact original effect programs.

Read-only metadata analysis: output contains names and hashes, never game code.
Identical programs can belong to multiple effects; all aliases are retained.
"""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import struct


def program_identity(record):
    if len(record) < 64 or record[:8] != struct.pack('<Q', 0x3143424458324D45):
        raise ValueError('Invalid shader record')
    key_size = struct.unpack_from('<I', record, 8)[0]
    if key_size < 44 or key_size > len(record) - 24:
        raise ValueError('Invalid shader key extent')
    key = record[12:12 + key_size]
    version, stage = struct.unpack_from('<II', key)
    words = struct.unpack_from('<I', key, 40)[0]
    if version != 1 or stage > 1 or key_size != 44 + words * 4:
        raise ValueError('Invalid shader program key')
    binary_size = struct.unpack_from('<I', record, 12 + key_size)[0]
    start = 16 + key_size
    if binary_size < 32 or start + binary_size > len(record) - 8:
        raise ValueError('Invalid PC shader extent')
    binary = record[start:start + binary_size]
    if binary[:4] != b'DXBC':
        raise ValueError('Invalid PC shader header')
    return (stage, hashlib.sha256(key[44:]).digest()), hashlib.sha256(binary).hexdigest()


def index(archive, library, observed=None):
    scripts = Path(__file__).resolve().parent
    read_pak = runpy.run_path(str(scripts / 'game-pak.py'))['read_pak']
    extract = runpy.run_path(str(scripts / 'inspect-scene-shaders.py'))['extract']
    names = {}
    for name, data, _ in read_pak(archive):
        if not name.lower().endswith('.fxlo'):
            continue
        for stage in (0, 1):
            for number, program in enumerate(extract(data, pixel=bool(stage))):
                code = struct.pack('<' + str(len(program['words'])) + 'I', *program['words'])
                key = stage, hashlib.sha256(code).digest()
                names.setdefault(key, set()).add(f'{name}#{"PS" if stage else "VS"}{number}')
    identities = {}
    def add(record):
        program, binary = program_identity(record)
        aliases = names.get(program, set())
        if aliases:
            identities.setdefault(binary, set()).update(aliases)
        return bool(aliases)
    mapped = 0
    with Path(library).open('rb') as file:
        if file.read(8) != b'EM2DXPK1':
            raise ValueError('Invalid prepared bundle header')
        def word():
            data = file.read(4)
            if len(data) != 4:
                raise ValueError('Truncated prepared bundle')
            return struct.unpack('<I', data)[0]
        count = word()
        if not 0 < count <= 16384:
            raise ValueError('Invalid prepared record count')
        for _ in range(count):
            size = word()
            if not 64 <= size <= 8 * 1024 * 1024:
                raise ValueError('Invalid prepared record extent')
            record = file.read(size)
            if len(record) != size:
                raise ValueError('Truncated shader record')
            mapped += add(record)
        if file.read(1):
            raise ValueError('Prepared bundle has trailing bytes')
    observed_count = observed_mapped = 0
    manifest = hashlib.sha256()
    if observed:
        paths = sorted(Path(observed).glob('*.em2dxbc'))
        if len(paths)>16384:
            raise ValueError('Observed record count exceeds native bounds')
        for path in paths:
            if not 64<=path.stat().st_size<=8*1024*1024:
                raise ValueError('Invalid observed record size')
            record = path.read_bytes()
            manifest.update(path.name.encode('utf-8')+b'\0'+hashlib.sha256(record).digest())
            observed_mapped += add(record)
            observed_count += 1
    return dict(archive_sha256=hashlib.sha256(Path(archive).read_bytes()).hexdigest(),
                library_sha256=hashlib.sha256(Path(library).read_bytes()).hexdigest(),
                records=count, mapped_records=mapped,
                observed_records=observed_count, mapped_observed_records=observed_mapped,
                observed_manifest_sha256=manifest.hexdigest() if observed else None,
                shaders={key: sorted(value) for key, value in identities.items()},
                scope='Exact prepared DXBC to original microcode/effect aliases; unmatched runtime patches and newly compiled variants remain unidentified')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('library', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--observed', type=Path)
    args = parser.parse_args()
    report = index(args.archive, args.library, args.observed)
    with args.output.open('x', encoding='utf-8') as file:
        json.dump(report, file, indent=2)
        file.write('\n')
    print(json.dumps({key: value for key, value in report.items() if key != 'shaders'}))
