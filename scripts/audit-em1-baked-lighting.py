"""Match authored BKLT0001 colors to original NIF hierarchy/color streams.

Format reference: D:/Repos/bluethinner/src/cooked_lighting.rs and
src/editors/baked_preview.rs. Read-only; never changes authored assets.
"""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import struct


def audit(lighting, model, entity):
    data = lighting.read_bytes()
    if len(data) > 128 * 1024 * 1024 or data[:8] != b'BKLT0001':
        raise ValueError('Unsupported cooked lighting')
    lookup, count = struct.unpack_from('>2I', data, 8)
    if not 16 <= lookup <= len(data) or lookup % 4 or count > (len(data)-lookup)//8:
        raise ValueError('Invalid lookup')
    records = sorted((struct.unpack_from('>2I', data, lookup+8*i) for i in range(count)), key=lambda r:r[1])
    if len({i for i, _ in records}) != count:
        raise ValueError('Duplicate lighting entity')
    streams = {}
    for j, (identity, start) in enumerate(records):
        end = records[j+1][1] if j+1<count else lookup
        if not 16 <= start < end <= lookup or start % 4:
            raise ValueError('Overlapping/out-of-range record')
        pos = start
        def word():
            nonlocal pos
            if pos+4 > end:
                raise ValueError('Truncated lighting record')
            value = struct.unpack_from('>I', data, pos)[0]
            pos += 4
            return value
        meshes = word()
        if meshes > (end-pos)//8:
            raise ValueError('Invalid mesh count')
        for _ in range(meshes):
            vertices = word()
            if vertices > (end-pos)//4:
                raise ValueError('Truncated colors')
            colors = data[pos:pos+4*vertices]
            pos += 4*vertices
            groups = word()
            if groups > (end-pos)//4:
                raise ValueError('Invalid group count')
            for _ in range(groups):
                length = word()
                if length > (end-pos)//4:
                    raise ValueError('Invalid hierarchy path')
                path = tuple(word() for _ in range(length))
                if identity == entity:
                    if path in streams:
                        raise ValueError('Ambiguous color mapping')
                    streams[path] = colors
        if pos != end:
            raise ValueError('Unexpected lighting record tail')
    if not streams:
        raise ValueError('Entity has no cooked lighting')

    h = runpy.run_path(str(Path(__file__).with_name('audit-em1-live-dual-uv.py')))
    nif = h['NIF'].read(model.read_bytes())
    Cursor = h['Cursor']
    c = Cursor(nif.strings, nif.endian)
    n = c.number(); c.number()
    strings = [c.take(c.number()).decode() for _ in range(n)]
    version = struct.unpack_from('<I', nif.prefix, len(nif.prefix)-8)[0]
    c = Cursor(nif.footer, nif.endian)
    roots = [c.number() for _ in range(c.number())]
    rows = []
    def visit(block, path, ancestors):
        if block == 0xffffffff:
            return
        if block >= len(nif.blocks) or block in ancestors or len(ancestors)>256:
            raise ValueError('Invalid model hierarchy')
        kind = nif.types[nif.indices[block]]
        if kind in ('NiNode','JPSJigsawNode'):
            c = Cursor(nif.blocks[block], nif.endian)
            c.number(); c.take(c.number()*4); c.number()
            c.take(54); c.take(c.number()*4); c.number()
            children = [c.number() for _ in range(c.number())]
            for slot, child in enumerate(children):
                visit(child, path+(slot,), ancestors|{block})
        elif kind == 'NiMesh':
            if path not in streams:
                return  # Sparse lighting: retain original colors on unlisted meshes.
            name, refs = h['mesh_refs'](nif, block, version)
            matched = []
            for index, selected, semantics in refs:
                regions, formats, widths, raw = h['stream'](nif, index)
                for component, (semantic, semantic_index) in enumerate(semantics):
                    if (strings[semantic],semantic_index) != ('COLOR',0):
                        continue
                    if len(selected)>1 or len(regions)!=1 or formats!=[0x00040110] or widths!=[4]:
                        raise ValueError('Unaudited color stream layout')
                    start, vertices = regions[selected[0] if selected else 0]
                    original = raw[start*4:(start+vertices)*4]
                    baked = streams[path]
                    if len(original)!=len(baked):
                        raise ValueError('Baked/source color counts differ')
                    matched.append(dict(block=block,name=strings[name],path=path,stream=index,
                        vertices=vertices,changed_vertices=sum(original[i:i+4]!=baked[i:i+4] for i in range(0,len(baked),4)),
                        original_sha256=hashlib.sha256(original).hexdigest(),baked_sha256=hashlib.sha256(baked).hexdigest()))
            if len(matched)!=1:
                raise ValueError('Expected one color stream per mesh')
            rows.extend(matched)
        else:
            raise ValueError('Unaudited hierarchy object: '+kind)
    if len(roots)!=1:
        raise ValueError('Only the audited single-root model is supported')
    # BKLT paths index child slots below the NIF root, not the root table.
    visit(roots[0],(),set())
    if {tuple(r['path']) for r in rows} != set(streams):
        raise ValueError('Not every authored baked path was matched')
    return dict(entity=entity, lighting=str(lighting), model=str(model), meshes=rows,
                matched_meshes=len(rows),changed_meshes=sum(r['changed_vertices']>0 for r in rows),
                changed_vertices=sum(r['changed_vertices'] for r in rows),
                scope='Original NIF color arrays versus matching authored cooked colors; no runtime adaptation performed.')


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('lighting',type=Path);p.add_argument('model',type=Path)
    p.add_argument('--entity',type=lambda s:int(s,0),required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();r=audit(a.lighting,a.model,a.entity)
    a.output.write_text(json.dumps(r,indent=2)+'\n')
    print(f"Matched {r['matched_meshes']} meshes; {r['changed_meshes']} meshes / {r['changed_vertices']} colors differ from the baked data")
