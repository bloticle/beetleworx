"""Resolve original NIF bindings; reject standalone unbound physics."""
import argparse,ctypes as c,json
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--library',required=True,type=Path);p.add_argument('--image',required=True,type=Path);args=p.parse_args()
d=c.CDLL(str(args.library.resolve()));d.model_asset.argtypes=[c.c_wchar_p,c.c_char_p,c.c_char_p,c.POINTER(c.c_size_t),c.POINTER(c.c_uint32),c.POINTER(c.c_uint32)];d.model_asset.restype=c.c_char_p
d.jigsaw_rejects_unbound.argtypes=[c.POINTER(c.c_ubyte),c.c_size_t];d.jigsaw_rejects_unbound.restype=c.c_char_p
results=[]
for part in ['inert01','toon01','toon02']:
    path='environments/thepark/fantasyland/smallworld/zones/gv_zonea_'+part+'.nif_wii'
    size=c.c_size_t();meshes=c.c_uint32();textures=c.c_uint32();error=d.model_asset(str(args.image),b'packfiles/gremlinvillage_zonea.pak',path.encode(),c.byref(size),c.byref(meshes),c.byref(textures))
    assert not error and meshes.value=={'inert01':60,'toon01':56,'toon02':95}[part],(error,meshes.value)
    results.append(dict(resource=path,error=error.decode() if error else None,meshes=meshes.value))
size=c.c_size_t();meshes=c.c_uint32();textures=c.c_uint32()
error=d.model_asset(str(args.image),b'packfiles/dbc_1st_zonec.pak',b'environments/thepark/darkbeautycastle/zones/dbc_scaletest2.nif_wii',c.byref(size),c.byref(meshes),c.byref(textures));assert not error and meshes.value==127,(error,meshes.value)
original=Path('analysis/em1-compatibility/original-jigsaw-aggregate.hkx').read_bytes();raw=(c.c_ubyte*len(original)).from_buffer_copy(original);error=d.jigsaw_rejects_unbound(raw,len(raw));assert not error,error
result=dict(gv=results,courtyard_meshes=meshes.value,unbound_original_aggregate_rejects_without_output=True,scope='Host model preparation resolves original NIF physics stream bindings. Standalone unbound HKX still rejects. No native world/ray proof or complete-level activation.')
Path('analysis/em1-compatibility/jigsaw-model-guard-result.json').write_text(json.dumps(result,indent=2)+'\n')
print('PASS: GV60/56/95 and courtyard127; standalone unbound aggregate still rejects without native output')
