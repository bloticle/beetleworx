"""Correlate sampled cutout UV buffers with original GX meshes and CPU names.

Read-only diagnostic. Source prefixes identify geometry; native stream addresses
identify the observed CPU owner. This is not a full-buffer/framebuffer comparison.
"""
import argparse
import json
from pathlib import Path
import re
import runpy
import struct

helpers = runpy.run_path(str(Path(__file__).with_name('audit-em1-live-dual-uv.py')))
NIF, Cursor = helpers['NIF'], helpers['Cursor']


def audit(source, log):
    nif = NIF.read(source.read_bytes())
    c = Cursor(nif.strings, nif.endian); count = c.number(); c.number()
    strings = [c.take(c.number()).decode() for _ in range(count)]
    version = struct.unpack_from('<I', nif.prefix, len(nif.prefix) - 8)[0]
    meshes = []
    for block, kind in enumerate(nif.indices):
        if nif.types[kind] != 'NiMesh': continue
        name, refs = helpers['mesh_refs'](nif, block, version)
        attrs = []; commands = None
        for index, selected, semantics in refs:
            regions, formats, widths, data = helpers['stream'](nif, index)
            start, count = regions[selected[0] if selected else 0]
            for component, (semantic, semantic_index) in enumerate(semantics):
                text = strings[semantic]
                if text == 'DISPLAYLIST': commands = data[start:start + count]
                elif text != 'INDEX':
                    attrs.append((text, semantic_index, start, count, formats[component],
                                  sum(widths), sum(widths[:component]), data))
        if commands is None: continue
        attrs.sort(key=lambda a: (['POSITION', 'NORMAL', 'COLOR', 'TEXCOORD'].index(a[0]), a[1]))
        _, vertices = helpers['gx_tuples'](commands, [a[3] for a in attrs])
        position_column = next(i for i, a in enumerate(attrs) if a[:2] == ('POSITION', 0))
        position = attrs[position_column]
        if position[4] != 0x30437: raise ValueError('Unaudited source position encoding')
        positions = [struct.unpack_from(nif.endian + '3f', position[7],
                     (position[2] + v[position_column]) * position[5] + position[6]) for v in vertices[:2]]
        for column, a in enumerate(attrs):
            if a[:2] != ('TEXCOORD', 0): continue
            if a[4] != 0x20240: raise ValueError('Unaudited source UV encoding')
            uv = [tuple(x / 1024 for x in struct.unpack_from(nif.endian + '2h', a[7],
                  (a[2] + v[column]) * a[5] + a[6])) for v in vertices[:3]]
            meshes.append({'mesh': block, 'name': strings[name], 'vertices': len(vertices), 'uv': uv, 'positions': positions})
    text = log.read_text()
    cpu = re.findall(r'cull mesh identity mesh=(\w+) name=(.*?) tracked=(true|false) caller=(\w+) requested=(\d+) stencil=(\w+) flags=(\w+)', text)
    streams = []
    for mesh, slot, reference, address, raw in re.findall(
            r'cutout CPU stream mesh=(\w+) slot=(\d+) ref=(\w+) stream=(\w+) words=([A-F0-9/]+)', text):
        words = [int(word, 16) for word in raw.strip('/').split('/')]
        if len(words) < 15 or words[0] != 0x82057768: raise ValueError('Unaudited native stream class')
        if not 0xA0000000 <= words[14] <= 0xBFFFFFFF: raise ValueError('Native stream data is not in physical guest memory')
        streams.append((mesh, words[10], words[14] & 0x1FFFFFFF))
    captures = re.findall(r'cutout vertex fetch(93|95) address=(\w+) peer=(\w+) bytes=(\d+) endian=2 copied=true count=(\d+) raster=(\w+) raw=([A-F0-9/]+)', text)
    position_buffers = {address: (int(size), [struct.unpack('<f', bytes.fromhex(word))[0] for word in raw.split('/')])
                        for slot, address, peer, size, count, raster, raw in captures if slot == '95'}
    observations = []
    for slot, address, peer, size, count, raster, raw in captures:
        if slot != '93': continue
        position_size, position_values = position_buffers[peer]
        positions = [tuple(position_values[:3]), tuple(position_values[3:])]
        values = [struct.unpack('<f', bytes.fromhex(word))[0] for word in raw.split('/')]
        uv = list(zip(values[::2], values[1::2]))
        matches = []
        uv_owners = {mesh for mesh, stride, data in streams if stride == 8 and data == int(address, 16)}
        position_owners = {mesh for mesh, stride, data in streams if stride == 12 and data == int(peer, 16)}
        owners = uv_owners & position_owners
        for mesh in meshes:
            if int(size) != mesh['vertices'] * 8 or mesh['uv'] != uv: continue
            if position_size != mesh['vertices'] * 12 or mesh['positions'] != positions: continue
            matches.append({**mesh, 'cpu_instances': [
                {'address': m, 'tracked': t == 'true', 'caller': caller, 'requested': int(value), 'stencil': stencil, 'flags': flags}
                for m, name, t, caller, value, stencil, flags in cpu if name == mesh['name']]})
        observations.append({'uv_buffer': address, 'position_buffer': peer, 'vertices': int(size)//8, 'draw_indices': int(count),
                             'raster': raster, 'native_cpu_owners': sorted(owners), 'source_matches': matches,
                             'resolved_meshes': [m['mesh'] for m in matches if any(c['address'] in owners for c in m['cpu_instances'])]})
    if not observations: raise ValueError('No captured cutout UV buffers')
    return {'source': str(source), 'log': str(log), 'observations': observations,
            'all_source_matches_unique': all(len(o['source_matches']) == 1 for o in observations),
            'all_captured_geometry_matched': all(o['source_matches'] for o in observations),
            'all_draw_owners_unique': all(len(o['native_cpu_owners']) == 1 and len(o['resolved_meshes']) == 1 for o in observations),
            'scope': 'Three expanded GX UV pairs, two position triples, both buffer extents, and vertex count. CPU ownership joins both physical buffer addresses and stream strides, then source names resolve identical geometry. Covers captured draws and observed CPU stream owners only.'}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path); p.add_argument('log', type=Path)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args(); result = audit(a.source, a.log)
    a.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result['all_captured_geometry_matched'] and result['all_draw_owners_unique'] else 1)
