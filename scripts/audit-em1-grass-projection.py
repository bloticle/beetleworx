"""Project source grass triangles with captured native vertex constants.

Computes perspective-correct UVs at selected image positions, independently
of the pixel program. Does not model other meshes, clipping, or postprocessing.
"""
import argparse
import json
from pathlib import Path
import re
import runpy
import struct

h = runpy.run_path(str(Path(__file__).with_name('audit-em1-live-dual-uv.py')))
C = h['Cursor']


def audit(source, log):
    n = h['NIF'].read(source.read_bytes())
    c = C(n.strings, n.endian); count = c.number(); c.number()
    strings = [c.take(c.number()).decode() for _ in range(count)]
    version = struct.unpack_from('<I', n.prefix, len(n.prefix) - 8)[0]
    _, refs = h['mesh_refs'](n, 809, version)
    attrs, commands = [], None
    for index, selected, semantics in refs:
        regions, formats, widths, data = h['stream'](n, index)
        start, count = regions[selected[0] if selected else 0]
        for component, (semantic, semantic_index) in enumerate(semantics):
            name = strings[semantic]
            if name == 'DISPLAYLIST':
                commands = data[start:start + count]
            elif name != 'INDEX':
                attrs.append((name, semantic_index, start, count, formats[component],
                              sum(widths), sum(widths[:component]), data))
    attrs.sort(key=lambda a: (['POSITION', 'NORMAL', 'COLOR', 'TEXCOORD'].index(a[0]), a[1]))
    width, vertices = h['gx_tuples'](commands, [a[3] for a in attrs])
    lookup = {v: i for i, v in enumerate(vertices)}
    triangles = []; c = C(commands, '>')
    while c.at < len(commands):
        op = c.number('B')
        if not op:
            continue
        size = c.number('H')
        indices = [lookup[tuple(c.number('B' if width == 1 else 'H') for _ in attrs)] for _ in range(size)]
        kind = op & 0xf8
        if kind == 0x90:
            triangles.extend(tuple(indices[i:i + 3]) for i in range(0, size, 3))
        elif kind == 0x80:
            for i in range(0, size, 4):
                triangles.extend([(indices[i], indices[i+1], indices[i+2]), (indices[i], indices[i+2], indices[i+3])])
        elif kind == 0x98:
            triangles.extend((indices[i-1], indices[i-2], indices[i]) if i & 1 else (indices[i-2], indices[i-1], indices[i]) for i in range(2, size))
        elif kind == 0xa0:
            triangles.extend((indices[0], indices[i-1], indices[i]) for i in range(2, size))
        else:
            raise ValueError('Unknown primitive')
    triangles = [t for t in triangles if len(set(t)) == 3]
    positions, uvs = [], []
    colors = b''
    for column, a in enumerate(attrs):
        if a[:2] == ('POSITION', 0):
            if a[4] != 0x30437:
                raise ValueError('Unaudited position format')
            positions = [struct.unpack_from(n.endian+'3f', a[7], (a[2]+v[column])*a[5]+a[6]) for v in vertices]
        if a[:2] == ('COLOR', 0):
            if a[4] != 0x40110:
                raise ValueError('Unaudited color format')
            colors = b''.join(a[7][(a[2]+v[column])*a[5]+a[6]:(a[2]+v[column])*a[5]+a[6]+4] for v in vertices)
        if a[:2] == ('TEXCOORD', 0):
            if a[4] != 0x20240:
                raise ValueError('Unaudited UV format')
            uvs = [tuple(x/1024 for x in struct.unpack_from(n.endian+'2h', a[7], (a[2]+v[column])*a[5]+a[6])) for v in vertices]
    expected_buffers = {
        95: b''.join(struct.pack('>3f', *v) for v in positions),
        94: colors,
        93: b''.join(struct.pack('>2f', *v) for v in uvs),
        0: b''.join(struct.pack('>3H', *t) for t in triangles),
    }
    hashes = {}
    for slot, data in expected_buffers.items():
        digest = 14695981039346656037
        for byte in data:
            digest = ((digest ^ byte)*1099511628211) & ((1 << 64)-1)
        hashes[slot] = {'bytes':len(data), 'fnv':f'{digest:016X}'}
    geometry = []
    for slot, address, size, endian, digest in re.findall(
            r'grass geometry slot=(\d+) address=(\w+) bytes=(\d+) endian=(\d+) fnv=(\w+)', log.read_text()):
        expected = hashes.get(int(slot))
        geometry.append({'slot':int(slot),'address':address,'bytes':int(size),'endian':int(endian),'fnv':digest,
                         'expected':expected,'matches':expected == {'bytes':int(size),'fnv':digest}})
    frames = {}
    # Use the latest matrix before the screenshot, not later camera settling.
    trace = log.read_text().split("[em1] unmodified guest preview captured:", 1)[0]
    for frame, row, raw in re.findall(r'grass projection frame=(\d+) row=(\d+) raw=([A-F0-9/]+)', trace):
        frames.setdefault(int(frame), {})[int(row)] = [struct.unpack('>f', bytes.fromhex(x))[0] for x in raw.split('/')]
    if not frames or len(positions) != len(uvs) or len(triangles)*3 != 7155:
        raise ValueError(f'Missing projection data or source topology changed: frames={len(frames)}, positions={len(positions)}, UVs={len(uvs)}, indices={len(triangles)*3}')
    frame = max(frames); rows = frames[frame]
    if set(rows) != set(range(5)):
        raise ValueError('Incomplete matrix capture')
    sx, ox, sy, oy = rows[4]
    projected = []
    for pos in positions:
        p = (*pos, 1)
        clip = [sum(a*b for a, b in zip(rows[i], p)) for i in range(4)]
        projected.append((clip[0]/clip[3]*sx+ox, clip[1]/clip[3]*sy+oy, clip[2]/clip[3], clip[3]))
    samples = []
    for x, y in [(550,350),(650,350),(750,350),(850,350),(550,400),(650,400),(750,400),(850,400),(640,550)]:
        hits = []
        for tri in triangles:
            a,b,c = [projected[i] for i in tri]
            if min(a[3],b[3],c[3]) <= 0:
                continue
            den = (b[1]-c[1])*(a[0]-c[0])+(c[0]-b[0])*(a[1]-c[1])
            if abs(den) < 1e-10:
                continue
            wa = ((b[1]-c[1])*(x-c[0])+(c[0]-b[0])*(y-c[1]))/den
            wb = ((c[1]-a[1])*(x-c[0])+(a[0]-c[0])*(y-c[1]))/den
            weights = (wa,wb,1-wa-wb)
            if min(weights) < -1e-6:
                continue
            reciprocal = sum(t/v[3] for t,v in zip(weights,(a,b,c)))
            uv = [sum(t*uvs[i][j]/projected[i][3] for t,i in zip(weights,tri))/reciprocal for j in range(2)]
            hits.append({'uv': uv, 'depth': sum(t*v[2] for t,v in zip(weights,(a,b,c))), 'triangle': tri})
        samples.append({'pixel': [x,y], 'hits': sorted(hits,key=lambda p:p['depth'])})
    return {'source': str(source), 'log': str(log), 'mesh':809,'matrix_frame':frame,
            'matrix_samples':len(frames),'vertices':len(vertices),'triangles':len(triangles),
            'samples':samples,'geometry':geometry,'expected_buffers':hashes,'scope':__doc__}


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('source',type=Path);p.add_argument('log',type=Path);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();r=audit(a.source,a.log);a.output.write_text(json.dumps(r,indent=2)+'\n')
    print(json.dumps(r['samples'],indent=2))
