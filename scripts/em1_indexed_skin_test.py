"""Original indexed actor: preserve deformation data and validate native preparation.

Fixtures are extracted read-only from the user's images; nothing is shipped.
This does not claim animated actor/world compatibility.
"""
import argparse
import ctypes as c
import hashlib
import json
import runpy
import struct
from pathlib import Path
from em1_nif import NIF

helpers = runpy.run_path(str(Path(__file__).with_name('audit-em1-live-dual-uv.py')))
Cursor, stream, mesh_refs = (helpers[k] for k in ('Cursor', 'stream', 'mesh_refs'))


def skin_start(nif, block, version):
    r = Cursor(nif.blocks[block], nif.endian)
    r.number(); r.take(r.number() * 4); r.number()
    r.take(54); r.take(r.number() * 4); r.number()
    r.take(r.number() * 8); r.take(5)
    if version >= 4 and r.number(): r.take(8)
    if version >= 15: r.number()
    if version >= 3 and r.number(): r.number()
    r.number(); r.number('H'); r.take(17)
    for _ in range(r.number()):
        r.number(); r.take(1); r.take(r.number('H') * 2); r.take(r.number() * 8)
    assert r.number() == 0
    assert r.number('B') == 1
    return r.at


def skin_layout(tail, endian, version):
    r = Cursor(tail, endian)
    assert r.number() == 1
    transforms = r.number(); r.take(transforms * 4)
    weights = r.number(); r.take(weights * 24)
    vertices = r.number(); r.take(vertices * 4)
    morphs, width = r.number(), r.number(); r.take(morphs * (width * 12 + 2))
    mapped = r.number('H' if version < 17 else 'I'); r.take(mapped)
    partitions = r.number(); partition_start = r.at; r.take(partitions * 28)
    maximum = r.number(); assert r.at == len(tail)
    return dict(transforms=transforms, weights=weights, vertices=vertices,
                morphs=morphs, width=width, mapped=mapped, partitions=partitions,
                maximum=maximum, partition_start=partition_start)


p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--library', type=Path, required=True)
p.add_argument('--source-model', type=Path, required=True)
p.add_argument('--native-model', type=Path, required=True)
a = p.parse_args()
d = c.CDLL(str(a.library.resolve())); U = c.POINTER(c.c_ubyte)
d.mesh_buffer.argtypes = [U, c.c_size_t, c.POINTER(U), c.POINTER(c.c_size_t), c.POINTER(c.c_uint32)]
d.mesh_buffer.restype = c.c_char_p
d.cooked_skin.argtypes = [U, c.c_size_t, c.c_int, c.c_uint32, c.POINTER(U), c.POINTER(c.c_size_t)]
d.cooked_skin.restype = c.c_char_p


def convert(blob):
    raw = (c.c_ubyte * len(blob)).from_buffer_copy(blob)
    output, length, counts = U(), c.c_size_t(), (c.c_uint32 * 3)()
    error = d.mesh_buffer(raw, len(blob), c.byref(output), c.byref(length), counts)
    return error, None if error else c.string_at(output, length.value), list(counts)


evidence = {}
for label, path in [('em1', a.source_model), ('native_em2', a.native_model)]:
    blob = path.read_bytes(); nif = NIF.read(blob)
    version = struct.unpack_from('<I', nif.prefix, len(nif.prefix) - 8)[0]
    blocks = [i for i, t in enumerate(nif.indices) if nif.types[t] == 'NiMesh']
    assert len(blocks) == 1
    block = blocks[0]; start = skin_start(nif, block, version); tail = nif.blocks[block][start:]
    fields = skin_layout(tail, nif.endian, version)
    raw = (c.c_ubyte * len(tail)).from_buffer_copy(tail); output, length = U(), c.c_size_t()
    error = d.cooked_skin(raw, len(tail), nif.endian == '>', version, c.byref(output), c.byref(length))
    assert not error, error
    assert c.string_at(output, length.value) == tail
    evidence[label] = dict(sha256=hashlib.sha256(blob).hexdigest(), user_version=version,
                           skin_bytes=len(tail), **fields)
    if label != 'em1': continue
    original, actor, actor_block, actor_start, actor_fields = blob, nif, block, start, fields

error, converted, counts = convert(original)
assert not error, error
assert counts == [1, 0, 1], counts
target = NIF.read(converted)
assert target.blocks[actor_block][skin_start(target, actor_block, 15):] == actor.blocks[actor_block][actor_start:]
for i in range(len(actor.blocks)):
    if i != actor_block: assert actor.blocks[i] == target.blocks[i], i


def attributes(nif):
    refs = mesh_refs(nif, actor_block, 15)[1]; result = {}
    r = Cursor(nif.strings, nif.endian); count = r.number(); r.number()
    names = [r.take(r.number()).decode() for _ in range(count)]
    for block, regions, semantics in refs:
        table, formats, widths, data = stream(nif, block)
        start, size = table[regions[0]]; stride = sum(widths); offset = 0
        for component, (name, index) in enumerate(semantics):
            result[(names[name], index)] = (formats[component], [data[(start+i)*stride+offset:(start+i)*stride+offset+widths[component]] for i in range(size)])
            offset += widths[component]
    return result


before, after = attributes(actor), attributes(target)
assert before.keys() == after.keys()
for semantic, (fmt, values) in before.items():
    target_fmt, target_values = after[semantic]
    if semantic[0] == 'TEXCOORD' and fmt == 0x00020240:
        assert target_fmt == 0x00020436
        assert target_values == [struct.pack('>2f', *(v / 1024 for v in struct.unpack('>2h', row))) for row in values]
    else: assert (fmt, values) == (target_fmt, target_values), semantic

rejects = []
for name, relative, value in [
    ('palette_weight', actor_fields['partition_start'] + 8, b'\xff\xff'),
    ('partial_triangle', actor_fields['partition_start'] + 4, struct.pack('>I', 5263)),
    ('matrix_nan', 8, struct.pack('>I', 0x7fc00000)),
]:
    bad = NIF.read(original); block = bytearray(bad.blocks[actor_block])
    at = actor_start + relative; block[at:at+len(value)] = value; bad.blocks[actor_block] = bytes(block)
    error, output, _ = convert(bad.write()); assert error and output is None, name
    rejects.append(name)
evidence.update(scope='CPU preparation; native animated actor/world admission NOT proven',
                original_arrays_retained=True, vertex_and_index_order_retained=True,
                exact_fixed_uv_conversion=True, rejected=rejects)
Path('analysis/em1-compatibility/indexed-skin-result.json').write_text(json.dumps(evidence, indent=2)+'\n')
print('PASS: original native/EM1 skin layout, all deformation arrays and indices retained, exact UVs, malformed palettes rejected')
