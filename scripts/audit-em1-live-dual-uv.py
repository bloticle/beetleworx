"""Compare sampled live dual UV pairs with original GX vertex tuples.

Wii display lists index each attribute independently. Adjacent converted
vertices need not use adjacent entries in either original UV array.
This reads source/log files only; it does not validate an entire framebuffer.
"""
import argparse
import json
import re
import struct
from pathlib import Path
from em1_nif import NIF


class Cursor:
    def __init__(self, data, endian):
        self.data, self.endian, self.at = data, endian, 0

    def take(self, count):
        if count < 0 or self.at + count > len(self.data):
            raise ValueError('Truncated NIF field')
        result = self.data[self.at:self.at + count]
        self.at += count
        return result

    def number(self, kind='I'):
        return struct.unpack(self.endian + kind, self.take(struct.calcsize(kind)))[0]


def stream(nif, index):
    if not nif.types[nif.indices[index]].startswith('NiDataStream\x01'):
        raise ValueError('Not a data stream')
    c = Cursor(nif.blocks[index], nif.endian)
    size = c.number(); c.number()
    regions = [(c.number(), c.number()) for _ in range(c.number())]
    formats = [c.number() for _ in range(c.number())]
    widths = [((f >> 16) & 255) * ((f >> 8) & 255) for f in formats]
    data = c.take(size); c.take(1)
    if c.at != len(c.data):
        raise ValueError('Unknown stream tail')
    return regions, formats, widths, data


def mesh_refs(nif, block, version):
    c = Cursor(nif.blocks[block], nif.endian)
    name = c.number(); c.take(c.number() * 4); c.number()
    c.take(54); c.take(c.number() * 4); c.number()
    c.take(c.number() * 8); c.take(5)
    if version >= 4 and c.number(): c.take(8)
    if version >= 15: c.number()
    if version >= 3 and c.number(): c.number()
    c.number(); c.number('H'); c.take(17)
    refs = []
    for _ in range(c.number()):
        index = c.number(); c.take(1)
        regions = [c.number('H') for _ in range(c.number('H'))]
        semantics = [(c.number(), c.number()) for _ in range(c.number())]
        refs.append((index, regions, semantics))
    return name, refs


def gx_tuples(commands, counts):
    candidates = []
    for width in (1, 2):
        c = Cursor(commands, '>'); vertices = []
        try:
            while c.at < len(commands):
                op = c.number('B')
                if not op: continue
                kind = op & 0xf8
                count = c.number('H')
                if kind not in (0x80, 0x90, 0x98, 0xa0) or not count:
                    raise ValueError('Unsupported GX primitive')
                if (kind == 0x80 and count % 4) or (kind == 0x90 and count % 3):
                    raise ValueError('Invalid primitive size')
                for _ in range(count):
                    vertex = tuple(c.number('B' if width == 1 else 'H') for _ in counts)
                    if any(index >= limit for index, limit in zip(vertex, counts)):
                        raise ValueError('GX index exceeds source array')
                    vertices.append(vertex)
            if vertices: candidates.append((width, list(dict.fromkeys(vertices))))
        except (ValueError, struct.error):
            pass
    if len(candidates) != 1:
        raise ValueError('Ambiguous or unsupported GX indices')
    return candidates[0]


def audit(source, log):
    nif = NIF.read(source.read_bytes())
    c = Cursor(nif.strings, nif.endian); count = c.number(); c.number()
    strings = [c.take(c.number()).decode() for _ in range(count)]
    version = struct.unpack_from('<I', nif.prefix, len(nif.prefix) - 8)[0]
    captured = {}
    for slot, size, raw in re.findall(r'dual vertex fetch(92|93) address=\w+ bytes=(\d+) endian=2 copied=true raw=([A-F0-9/]+)', log.read_text()):
        values = [struct.unpack('<f', bytes.fromhex(word))[0] for word in raw.split('/')]
        captured[93 - int(slot)] = (int(size) // 8, [tuple(values[:2]), tuple(values[2:])])
    if len(captured) != 2:
        raise ValueError('Missing live UV0/UV1 observations')
    matches = []
    for block, kind in enumerate(nif.indices):
        if nif.types[kind] != 'NiMesh': continue
        name, refs = mesh_refs(nif, block, version)
        attrs = []; commands = None
        for index, selected, semantics in refs:
            regions, formats, widths, data = stream(nif, index)
            start, count = regions[selected[0] if selected else 0]
            for component, (semantic, semantic_index) in enumerate(semantics):
                text = strings[semantic]
                if text == 'DISPLAYLIST':
                    commands = data[start:start + count]
                elif text != 'INDEX':
                    attrs.append((text, semantic_index, index, start, count, formats[component],
                                  sum(widths), sum(widths[:component]), data))
        attrs.sort(key=lambda a: (['POSITION', 'NORMAL', 'COLOR', 'TEXCOORD'].index(a[0]), a[1]))
        if commands is None or not any(a[:2] == ('TEXCOORD', 1) for a in attrs): continue
        width, vertices = gx_tuples(commands, [a[4] for a in attrs])
        samples = {}; indices = {}
        for column, a in enumerate(attrs):
            if a[0] != 'TEXCOORD' or a[1] not in (0, 1): continue
            if a[5] != 0x20240: raise ValueError('Unaudited source UV encoding')
            indices[a[1]] = [v[column] for v in vertices[:2]]
            samples[a[1]] = [tuple(x / 1024 for x in struct.unpack_from(nif.endian + '2h', a[8],
                (a[3] + v[column]) * a[6] + a[7])) for v in vertices[:2]]
        if all(captured[i] == (len(vertices), samples.get(i)) for i in (0, 1)):
            matches.append({'mesh': block, 'name': strings[name], 'vertices': len(vertices),
                            'gx_index_bytes': width, 'source_indices': indices, 'uv_pairs': samples})
    if len(matches) != 1: raise ValueError(f'Expected one matching source mesh, found {len(matches)}')
    return {'source': str(source), 'log': str(log), 'matches': matches,
            'scope': 'Both live UV streams match the first two expanded GX vertex tuples and vertex count. Not a full-buffer or framebuffer comparison.'}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path); p.add_argument('log', type=Path)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args(); result = audit(a.source, a.log)
    a.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
