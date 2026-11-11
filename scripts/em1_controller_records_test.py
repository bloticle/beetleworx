"""Original animated props/pickups retain complete native controller records.

No replacement interpolation, event execution or live-world behavior is tested.
"""
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

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--library',type=Path,required=True);p.add_argument('--image',type=Path,required=True)
a=p.parse_args();d=c.CDLL(str(a.library.resolve()));U=c.POINTER(c.c_ubyte)
d.controller_records.argtypes=[U,c.c_size_t];d.controller_records.restype=c.c_char_p
d.level_model_asset.argtypes=[c.c_wchar_p,c.c_char_p,c.c_char_p,c.c_char_p,c.POINTER(c.c_char_p),c.POINTER(c.c_char_p),c.POINTER(U),c.POINTER(c.c_size_t)]
d.level_model_asset.restype=c.c_char_p
wanted={'p_barrelb1_inert.nif_wii','gv_zonea_platform01.nif_wii','gv_zonea_door03.nif_wii','pickup_tickets_a.nif_wii','gen_portal_screen_blank.nif_wii','gv_cratescardboard_inert.nif_wii'}
fixtures={};r=RVZ(a.image)
try:
    files={name.lower():span for name,span in r.files(1).items()}
    for archive in ['packfiles/gremlinvillage_zonea.pak','packfiles/_dynamic.pak']:
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'original.pak';path.write_bytes(r.read_partition(1,*files[archive]))
            for name,data,kind in runpy.run_path(str(Path(__file__).with_name('game-pak.py')))['read_pak'](path):
                if name.lower().rsplit('/',1)[-1] in wanted:fixtures[name.lower()]=(archive,data)
finally:r.close()
assert len(fixtures)==6


def check(data):
    raw=(c.c_ubyte*len(data)).from_buffer_copy(data);return d.controller_records(raw,len(data))


types={'NiTransformController','NiTextureTransformController','NiTransformInterpolator','NiTransformData',
       'NiFloatInterpolator','NiFloatData','NiTextKeyExtraData','NiBillboardNode','NiVisController','NiBoolInterpolator','NiBoolData'}
evidence={};records=0
for name,(archive,data) in fixtures.items():
    source=NIF.read(data);assert not check(data)
    origin,identities,output,length=c.c_char_p(),c.c_char_p(),U(),c.c_size_t()
    error=d.level_model_asset(str(a.image),b'gv_zonea_start.level',archive.encode(),name.encode(),c.byref(origin),c.byref(identities),c.byref(output),c.byref(length))
    assert not error,(name,error)
    converted=c.string_at(output,length.value);target=NIF.read(converted);assert not check(converted)
    preserved=[]
    for block,kind in enumerate(source.indices):
        if source.types[kind] not in types:continue
        assert target.blocks[block]==source.blocks[block],(name,block)
        preserved.append(dict(block=block,type=source.types[kind],sha256=hashlib.sha256(source.blocks[block]).hexdigest()))
    records+=len(preserved);evidence[name]=preserved
    if name.endswith('p_barrelb1_inert.nif_wii'):barrel=data
    if name.endswith('gv_zonea_door03.nif_wii'):door=data
    if name.endswith('gv_cratescardboard_inert.nif_wii'):visibility=data

rejected=[]
for label,original,kind,offset,value in [
    ('missing_target',barrel,'NiTransformController',22,0xfffffff0),
    ('nonfinite_frequency',barrel,'NiTransformController',6,0x7fc00000),
    ('absent_pose_data',barrel,'NiTransformInterpolator',32,0xfffffff0),
    ('invalid_text_name',door,'NiTextKeyExtraData',12,0xfffffff0),
    ('invalid_visibility_interpolator',visibility,'NiVisController',26,0xfffffff0),
]:
    bad=NIF.read(original);index=next(i for i,t in enumerate(bad.indices) if bad.types[t]==kind)
    block=bytearray(bad.blocks[index]);struct.pack_into(bad.endian+'I',block,offset,value);bad.blocks[index]=bytes(block)
    assert check(bad.write()),label;rejected.append(label)
private=NIF.read(barrel);i=next(i for i,t in enumerate(private.indices) if private.types[t]=='NiTransformInterpolator')
private.indices[i]=len(private.types);private.types.append('NiBoolInterpolator');private.blocks[i]=b'\x02'+b'\xff'*4
assert not check(private.write()) # Private record-only fixture: keep the invalid-pose sentinel2.
private.blocks[i]=b'\x03'+b'\xff'*4;assert check(private.write());rejected.append('private_invalid_bool_pose')
result=dict(scope='Original record/schema retention and CPU preparation; native sampler/world execution NOT proven',
            assets=evidence,preserved_records=records,rejected=rejected,
            billboard_format_reference='https://raw.githubusercontent.com/niftools/nifxml/develop/nif.xml')
Path('analysis/em1-compatibility/controller-records-result.json').write_text(json.dumps(result,indent=2)+'\n')
print(f'PASS: {len(fixtures)} original props/pickups, {records} complete controller/key/billboard records retained; malformed links/time/keys rejected')
