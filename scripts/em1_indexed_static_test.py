"""Original GV indexed door: byte-preserving geometry and exact UV promotion.

Also checks a private synthetic index subregion/U8 variant, never a game asset.
"""
import argparse
import ctypes as c
import json
import runpy
import struct
import tempfile
from pathlib import Path
from em1_nif import NIF
from em1_rvz import RVZ

h = runpy.run_path(str(Path(__file__).with_name('audit-em1-live-dual-uv.py')))
Cursor, stream, mesh_refs = (h[k] for k in ('Cursor', 'stream', 'mesh_refs'))
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--library', type=Path, required=True); p.add_argument('--image', type=Path, required=True)
a = p.parse_args(); d = c.CDLL(str(a.library.resolve())); U = c.POINTER(c.c_ubyte)
d.mesh_buffer.argtypes = [U, c.c_size_t, c.POINTER(U), c.POINTER(c.c_size_t), c.POINTER(c.c_uint32)]
d.mesh_buffer.restype = c.c_char_p
r = RVZ(a.image)
try:
    files = {name.lower(): span for name, span in r.files(1).items()}
    with tempfile.TemporaryDirectory() as temp:
        path = Path(temp)/'original.pak'; path.write_bytes(r.read_partition(1, *files['packfiles/gv_zonea_airlock.pak']))
        rows = runpy.run_path(str(Path(__file__).with_name('game-pak.py')))['read_pak'](path)
        original = next(data for name, data, kind in rows if name.lower().endswith('/gv_zonea_door02.nif_wii'))
finally: r.close()


def convert(data):
    raw = (c.c_ubyte*len(data)).from_buffer_copy(data); output, size, counts = U(), c.c_size_t(), (c.c_uint32*3)()
    error = d.mesh_buffer(raw, len(data), c.byref(output), c.byref(size), counts)
    assert not error, error
    return NIF.read(c.string_at(output, size.value))


def attributes(nif, block):
    r = Cursor(nif.strings, nif.endian); count = r.number(); r.number()
    names = [r.take(r.number()).decode() for _ in range(count)]; result = {}
    version = struct.unpack_from('<I', nif.prefix, len(nif.prefix)-8)[0]
    for index, regions, semantics in mesh_refs(nif, block, version)[1]:
        table, formats, widths, data = stream(nif, index); start, count = table[regions[0]]
        stride, offset = sum(widths), 0
        for component, (name, channel) in enumerate(semantics):
            result[(names[name], channel)] = (index, formats[component],
                [data[(start+i)*stride+offset:(start+i)*stride+offset+widths[component]] for i in range(count)])
            offset += widths[component]
    return result


source = NIF.read(original); target = convert(original)
meshes = [i for i, t in enumerate(source.indices) if source.types[t] == 'NiMesh']; assert len(meshes) == 2
for i, block in enumerate(source.blocks):
    if i not in meshes: assert block == target.blocks[i], i
fixed = packed = 0
for block in meshes:
    before, after = attributes(source, block), attributes(target, block); assert before.keys() == after.keys()
    for semantic, (_, fmt, values) in before.items():
        _, output_fmt, output_values = after[semantic]
        if semantic[0] == 'TEXCOORD' and fmt == 0x00020240:
            assert output_fmt == 0x00020436
            assert output_values == [struct.pack('>2f', *(x/1024 for x in struct.unpack('>2h', row))) for row in values]
            fixed += 1
        else:
            assert (fmt, values) == (output_fmt, output_values), semantic
        packed += semantic[0] == 'NORMAL' and fmt == 0x0004010c
assert fixed == packed == 2

# Controlled private fixture: verify subregion offset and unsigned widening.
fixture = NIF.read(original); index, _, _ = attributes(fixture, meshes[0])[('INDEX', 0)]
_, _, widths, _ = stream(fixture, index)
clone = struct.unpack_from('>I', fixture.blocks[index], 4)[0]
fixture.blocks[index] = struct.pack('>7I', 6, clone, 1, 3, 3, 1, 0x00010105)+bytes([255, 254, 253, 0, 1, 2, 1])
widened = convert(fixture.write())
_, fmt, values = attributes(widened, meshes[0])[('INDEX', 0)]
assert fmt == 0x00010215 and values == [b'\0\0', b'\0\1', b'\0\2']
result = dict(scope='CPU geometry preparation; no render/world proof', original_meshes=2,
              indices_and_attributes_retained=True, packed_normal_streams_retained=packed,
              exact_fixed_uv_streams=fixed, original_nonmesh_blocks_unchanged=True,
              private_u8_subregion_widening_verified=True)
Path('analysis/em1-compatibility/indexed-static-result.json').write_text(json.dumps(result, indent=2)+'\n')
print('PASS: original indexed door geometry, packed normals, UVs and nonmesh blocks; private U8/subregion widening')
