"""Verify actual GV cross-archive textures against their original owner data."""
import argparse
import ctypes as c
import hashlib
import json
import runpy
import struct
import tempfile
from pathlib import Path
from em1_nif import NIF
from em1_rvz import RVZ

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--library', type=Path, required=True)
p.add_argument('--image', type=Path, required=True)
a = p.parse_args(); d = c.CDLL(str(a.library.resolve())); U = c.POINTER(c.c_ubyte)
d.model_asset.argtypes = [c.c_wchar_p, c.c_char_p, c.c_char_p, c.POINTER(c.c_size_t), c.POINTER(c.c_uint32), c.POINTER(c.c_uint32)]
d.model_asset.restype = c.c_char_p
d.level_model_asset.argtypes = [c.c_wchar_p, c.c_char_p, c.c_char_p, c.c_char_p, c.POINTER(c.c_char_p), c.POINTER(c.c_char_p), c.POINTER(U), c.POINTER(c.c_size_t)]
d.level_model_asset.restype = c.c_char_p
d.texture_asset.argtypes = [U, c.c_size_t, c.POINTER(U), c.POINTER(c.c_size_t)]
d.texture_asset.restype = c.c_char_p
archive = b'packfiles/gv_zonea_airlock.pak'
model = b'environments/thepark/fantasyland/smallworld/zones/gv_zonea_airlock_thinner01.nif_wii'
size, meshes, textures = c.c_size_t(123), c.c_uint32(456), c.c_uint32(789)
error = d.model_asset(str(a.image), archive, model, c.byref(size), c.byref(meshes), c.byref(textures))
assert error and b'absent' in error
assert (size.value, meshes.value, textures.value) == (123, 456, 789)
origin, identities, output, length = c.c_char_p(), c.c_char_p(), U(), c.c_size_t()
error = d.level_model_asset(str(a.image), b'gv_zonea_start.level', archive, model,
                           c.byref(origin), c.byref(identities), c.byref(output), c.byref(length))
assert not error, error
result = NIF.read(c.string_at(output, length.value))
owners = identities.value.decode().splitlines()
assert len(owners) == 3 and all('packfiles/gremlinvillage_zonea.pak:' in identity for identity in owners)
assert all(identity in origin.value.decode() for identity in owners)


def strings(nif):
    count = struct.unpack_from(nif.endian+'I', nif.strings)[0]; at = 8; result = []
    for _ in range(count):
        n = struct.unpack_from(nif.endian+'I', nif.strings, at)[0]; at += 4
        result.append(nif.strings[at:at+n].decode()); at += n
    return result


def pixels(nif):
    names = strings(nif); values = {}
    for i, block in enumerate(nif.blocks):
        if nif.types[nif.indices[i]] != 'NiSourceTexture': continue
        base = 12 + 4*struct.unpack_from(nif.endian+'I', block, 4)[0]
        assert block[base] == 0
        name, link = struct.unpack_from(nif.endian+'II', block, base+1)
        assert nif.types[nif.indices[link]] == 'NiPixelData'
        values['' if name == 0xffffffff else names[name]] = nif.blocks[link]
    return values


embedded = pixels(result); assert embedded.keys() == set(owners)
r = RVZ(a.image)
try:
    files = {name.lower(): span for name, span in r.files(1).items()}
    with tempfile.TemporaryDirectory() as temp:
        path = Path(temp)/'original.pak'; path.write_bytes(r.read_partition(1, *files['packfiles/gremlinvillage_zonea.pak']))
        rows = runpy.run_path(str(Path(__file__).with_name('game-pak.py')))['read_pak'](path)
        resources = {name.lower(): data for name, data, kind in rows}
finally: r.close()
hashes = {}
for owner in owners:
    name = owner.split('packfiles/gremlinvillage_zonea.pak:', 1)[1]
    raw = resources[name]; blob = (c.c_ubyte*len(raw)).from_buffer_copy(raw)
    decoded, n = U(), c.c_size_t()
    error = d.texture_asset(blob, len(raw), c.byref(decoded), c.byref(n)); assert not error, error
    standalone = pixels(NIF.read(c.string_at(decoded, n.value)))
    assert len(standalone) == 1 and next(iter(standalone.values())) == embedded[owner]
    hashes[name] = hashlib.sha256(embedded[owner]).hexdigest()
evidence = dict(scope='CPU complete-level texture dependency preparation; no world/render proof',
                level='gv_zonea_start.level', model=model.decode(), local_archive=archive.decode(),
                actual_texture_owner='packfiles/gremlinvillage_zonea.pak', pixel_hashes=hashes,
                unscoped_missing_rejected=True, failure_outputs_unchanged=True,
                cache_identity_includes_dependencies=True, bytes=length.value)
Path('analysis/em1-compatibility/level-texture-scope-result.json').write_text(json.dumps(evidence, indent=2)+'\n')
print('PASS: 3 original GV sibling textures; exact decoded pixel payloads and qualified identities; absent dependency remains an error without scope')
