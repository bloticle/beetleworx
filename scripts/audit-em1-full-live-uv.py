"""Compare complete captured GPU UV-buffer hashes with expanded source GX UVs.

Read-only. Matching UVs do not prove texture selection, lighting, or final pixels.
Shared UV layouts may match several source meshes; report all candidates.
"""
import argparse
import json
from pathlib import Path
import re
import runpy
import struct

helpers = runpy.run_path(str(Path(__file__).with_name('audit-em1-live-dual-uv.py')))
Cursor, NIF = helpers['Cursor'], helpers['NIF']


def audit(source, log):
    nif = NIF.read(source.read_bytes())
    c = Cursor(nif.strings, nif.endian)
    count = c.number(); c.number()
    strings = [c.take(c.number()).decode() for _ in range(count)]
    version = struct.unpack_from('<I', nif.prefix, len(nif.prefix) - 8)[0]
    expected = {}
    for block, kind in enumerate(nif.indices):
        if nif.types[kind] != 'NiMesh':
            continue
        name, refs = helpers['mesh_refs'](nif, block, version)
        attrs, commands = [], None
        for index, selected, semantics in refs:
            regions, formats, widths, data = helpers['stream'](nif, index)
            start, count = regions[selected[0] if selected else 0]
            for component, (semantic, semantic_index) in enumerate(semantics):
                text = strings[semantic]
                if text == 'DISPLAYLIST':
                    commands = data[start:start + count]
                elif text != 'INDEX':
                    attrs.append((text, semantic_index, start, count,
                                  formats[component], sum(widths),
                                  sum(widths[:component]), data))
        if commands is None:
            continue
        attrs.sort(key=lambda a: (['POSITION', 'NORMAL', 'COLOR', 'TEXCOORD'].index(a[0]), a[1]))
        _, vertices = helpers['gx_tuples'](commands, [a[3] for a in attrs])
        for column, a in enumerate(attrs):
            if a[:2] != ('TEXCOORD', 0):
                continue
            if a[4] not in (0x20240, 0x20436):
                raise ValueError(f'Unaudited UV encoding {a[4]:X}')
            values = [struct.unpack_from(nif.endian + ('2h' if a[4] == 0x20240 else '2f'),
                      a[7], (a[2] + v[column]) * a[5] + a[6]) for v in vertices]
            if a[4] == 0x20240:
                values = [(u / 1024, v / 1024) for u, v in values]
            data = b''.join(struct.pack('>2f', *uv) for uv in values)
            digest = 14695981039346656037
            for byte in data:
                digest = ((digest ^ byte) * 1099511628211) & ((1 << 64) - 1)
            expected.setdefault((len(data), f'{digest:016X}'), []).append(
                {'mesh': block, 'name': strings[name], 'vertices': len(values)})
    observations = []
    for address, size, endian, indices, digest in re.findall(
            r'full UV buffer address=(\w+) bytes=(\d+) endian=(\d+) indices=(\d+) fnv=(\w+)',
            log.read_text()):
        if endian != '2':
            raise ValueError('Unaudited GPU endianness')
        observations.append({'address': address, 'bytes': int(size), 'indices': int(indices),
                             'fnv': digest, 'source_matches': expected.get((int(size), digest), [])})
    if not observations or any(not x['source_matches'] for x in observations):
        raise ValueError('Missing observations or unmatched full UV buffer')
    return {'source': str(source), 'log': str(log), 'observations': observations,
            'scope': 'Complete UV0 buffer byte hashes, independently expanded from source GX tuples; not a framebuffer or texture audit.'}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path); p.add_argument('log', type=Path)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    result = audit(args.source, args.log)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(f"PASS: {len(result['observations'])} complete live UV buffers match source GX expansion")
