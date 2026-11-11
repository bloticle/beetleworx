"""Actual EM1 camera footer aliases: preserve all roots and reject lost ownership."""
import argparse
import ctypes as c
import json
import runpy
import struct
import tempfile
from pathlib import Path
from em1_nif import NIF
from em1_rvz import RVZ
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--library',type=Path,required=True);p.add_argument('--image',type=Path,required=True)
a=p.parse_args();d=c.CDLL(str(a.library.resolve()));U=c.POINTER(c.c_ubyte)
d.contained_roots.argtypes=[U,c.c_size_t,c.POINTER(c.c_uint32),c.POINTER(c.c_uint32)];d.contained_roots.restype=c.c_char_p
d.level_model_asset.argtypes=[c.c_wchar_p,c.c_char_p,c.c_char_p,c.c_char_p,c.POINTER(c.c_char_p),c.POINTER(c.c_char_p),c.POINTER(U),c.POINTER(c.c_size_t)];d.level_model_asset.restype=c.c_char_p
h=runpy.run_path(str(Path(__file__).with_name('audit-em1-live-dual-uv.py')))
wanted={'igcs/mickey/camera_film_trans_idle_cam.nif_wii','igcs/mickey/mickey_idle_paint_trans_film_cam.nif_wii'}
r=RVZ(a.image)
try:
    files={name.lower():span for name,span in r.files(1).items()}
    with tempfile.TemporaryDirectory() as temp:
        path=Path(temp)/'original.pak';path.write_bytes(r.read_partition(1,*files['packfiles/_dynamic.pak']))
        fixtures={name.lower():data for name,data,kind in runpy.run_path(str(Path(__file__).with_name('game-pak.py')))['read_pak'](path) if name.lower() in wanted}
finally:r.close()
assert len(fixtures)==2


def plan(data):
    raw=(c.c_ubyte*len(data)).from_buffer_copy(data);roots,depth=c.c_uint32(),c.c_uint32()
    return d.contained_roots(raw,len(data),c.byref(roots),c.byref(depth)),roots.value,depth.value


results={}
for name,data in fixtures.items():
    error,count,depth=plan(data);assert not error and count==2 and depth==7,(name,error,count,depth)
    origin,identities,output,length=c.c_char_p(),c.c_char_p(),U(),c.c_size_t()
    error=d.level_model_asset(str(a.image),b'gv_zonea_start.level',b'packfiles/gremlinvillage_zonea.pak',name.encode(),c.byref(origin),c.byref(identities),c.byref(output),c.byref(length));assert not error,error
    before,after=NIF.read(data),NIF.read(c.string_at(output,length.value))
    assert before.footer==after.footer and before.blocks==after.blocks and before.prefix==after.prefix
    results[name]=dict(roots=count,depth=depth,complete_original_blocks_and_footer_retained=True)

data=next(iter(fixtures.values()));source=NIF.read(data);root=struct.unpack_from(source.endian+'I',source.footer,4)[0]
r=h['Cursor'](source.blocks[root],source.endian);r.number();r.take(r.number()*4);r.number();r.take(54);r.take(r.number()*4);r.number();assert r.number()>0;at=r.at
for label,value in [('disconnected_camera',0xffffffff),('cyclic_child',root)]:
    bad=NIF.read(data);block=bytearray(bad.blocks[root]);struct.pack_into(bad.endian+'I',block,at,value);bad.blocks[root]=bytes(block)
    assert plan(bad.write())[0],label
bad=NIF.read(data);footer=bytearray(bad.footer);struct.pack_into(bad.endian+'I',footer,8,1);bad.footer=bytes(footer)
assert plan(bad.write())[0],'independent_extra_data_root'
result=dict(scope='Original source graph/alias preparation only; native aliases verified separately',assets=results,
            rejected=['disconnected_camera','cyclic_child','independent_extra_data_root'])
Path('analysis/em1-compatibility/contained-roots-result.json').write_text(json.dumps(result,indent=2)+'\n')
print('PASS:2original camera graphs, both footer roots and every block retained; disconnected/cyclic/independent roots rejected')
