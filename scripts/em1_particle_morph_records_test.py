"""Original GV particle/morph assets: complete native data retention, no simulation claim."""
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
d.level_model_asset.argtypes=[c.c_wchar_p,c.c_char_p,c.c_char_p,c.c_char_p,c.POINTER(c.c_char_p),c.POINTER(c.c_char_p),c.POINTER(U),c.POINTER(c.c_size_t)];d.level_model_asset.restype=c.c_char_p
d.mesh_buffer.argtypes=[U,c.c_size_t,c.POINTER(U),c.POINTER(c.c_size_t),c.POINTER(c.c_uint32)];d.mesh_buffer.restype=c.c_char_p
d.controller_records.argtypes=[U,c.c_size_t];d.controller_records.restype=c.c_char_p
h=runpy.run_path(str(Path(__file__).with_name('audit-em1-live-dual-uv.py')))
wanted={'common_sparks_01.nif_wii','common_steam_01.nif_wii','pickup_pin_generic.nif_wii','steam_jet_02.nif_wii',
        'fx_item_reward.nif_wii','pipe_straight_2m_spawner_01.nif_wii','pipespawner_spatter.nif_wii','gen_portal_screen_distortion.nif_wii'}
r=RVZ(a.image);fixtures={}
try:
    files={name.lower():span for name,span in r.files(1).items()}
    for archive in ['packfiles/_dynamic.pak','packfiles/gv_shared_za.pak','packfiles/gremlinvillage_zonea.pak','packfiles/gv_zonea_airlock.pak']:
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'original.pak';path.write_bytes(r.read_partition(1,*files[archive]))
            for name,data,kind in runpy.run_path(str(Path(__file__).with_name('game-pak.py')))['read_pak'](path):
                if name.lower().rsplit('/',1)[-1] in wanted:fixtures[name.lower()]=(archive,data)
finally:r.close()
assert len(fixtures)==8,len(fixtures)


def morph_refs(nif,block):
    version=struct.unpack_from('<I',nif.prefix,len(nif.prefix)-8)[0]
    r=h['Cursor'](nif.strings,nif.endian);count=r.number();r.number();names=[r.take(r.number()).decode() for _ in range(count)]
    return [(stream,regions,[(names[name],index) for name,index in semantics]) for stream,regions,semantics in h['mesh_refs'](nif,block,version)[1]
            if semantics and names[semantics[0][0]].startswith('MORPH')]


records=0;morph_meshes=0;results={}
for name,(archive,data) in fixtures.items():
    source=NIF.read(data);origin,identities,output,length=c.c_char_p(),c.c_char_p(),U(),c.c_size_t()
    error=d.level_model_asset(str(a.image),b'gv_zonea_start.level',archive.encode(),name.encode(),c.byref(origin),c.byref(identities),c.byref(output),c.byref(length));assert not error,(name,error)
    converted=c.string_at(output,length.value);target=NIF.read(converted)
    raw=(c.c_ubyte*len(converted)).from_buffer_copy(converted);assert not d.controller_records(raw,len(converted)),name
    preserved=[];is_morph='NiMorphMeshModifier' in source.types
    for block,t in enumerate(source.indices):
        kind=source.types[t]
        if kind.startswith(('NiPS','NonUniform','NiMorph')) or kind in {'NiFloatExtraDataController','NiColorExtraDataController','NiMaterialColorController'}:
            assert source.blocks[block]==target.blocks[block],(name,block,kind);preserved.append(kind);records+=1
        if is_morph and kind.startswith('NiDataStream'):
            assert source.blocks[block]==target.blocks[block],(name,block,'original morph/geometry stream changed')
        if is_morph and kind=='NiMesh':
            before,after=morph_refs(source,block),morph_refs(target,block);assert before==after,(name,block)
            assert len(before)==2 and len(before[0][2])==4 and before[1][2]==[('MORPHWEIGHTS',0)]
            morph_meshes+=1
    results[name]=dict(preserved_records=preserved,bytes=len(converted))
    if name.endswith('pipe_straight_2m_spawner_01.nif_wii'):pipe=data
assert morph_meshes>=3


def rejected(nif):
    data=nif.write();raw=(c.c_ubyte*len(data)).from_buffer_copy(data);output,size,counts=U(),c.c_size_t(),(c.c_uint32*3)()
    return d.mesh_buffer(raw,len(data),c.byref(output),c.byref(size),counts)


for label,value in [('unsupported_flags',b'\x81'),('missing_target',b'\x01\x00\x05')]:
    bad=NIF.read(pipe);i=next(i for i,t in enumerate(bad.indices) if bad.types[t]=='NiMorphMeshModifier');block=bytearray(bad.blocks[i])
    r=h['Cursor'](block,bad.endian)
    for _ in range(2):r.take(r.number()*2)
    block[r.at:r.at+len(value)]=value;bad.blocks[i]=bytes(block);assert rejected(bad),label
bad=NIF.read(pipe);mesh=next(i for i,t in enumerate(bad.indices) if bad.types[t]=='NiMesh');weights=morph_refs(bad,mesh)[1][0]
block=bytearray(bad.blocks[weights]);regions=struct.unpack_from('>I',block,8)[0];at=12+regions*8;components=struct.unpack_from('>I',block,at)[0];at+=4+components*4
struct.pack_into('>I',block,at,0x7fc00000);bad.blocks[weights]=bytes(block);assert rejected(bad),'nonfinite_weight'
result=dict(scope='CPU complete-payload retention/preparation; no emission, morph execution or world proof',assets=results,
            preserved_records=records,morph_meshes=morph_meshes,original_morph_streams_and_references_retained=True,
            rejected=['unsupported_flags','missing_target','nonfinite_weight'])
Path('analysis/em1-compatibility/particle-morph-records-result.json').write_text(json.dumps(result,indent=2)+'\n')
print(f'PASS:8original assets; {records} complete particle/morph/controller records; {morph_meshes} morph meshes; malformed morph data rejected')
