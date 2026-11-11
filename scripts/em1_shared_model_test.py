"""Verify explicit shared archive fallback and local model provenance."""
import argparse,ctypes as c,hashlib,json
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--library',required=True);p.add_argument('--image',required=True);args=p.parse_args()
d=c.CDLL(str(Path(args.library).resolve()));U=c.POINTER(c.c_ubyte)
d.base_model_source.argtypes=[c.c_wchar_p,c.c_char_p,c.c_char_p,c.POINTER(c.c_char_p),c.POINTER(U),c.POINTER(c.c_size_t)];d.base_model_source.restype=c.c_char_p

def read(archive,resource):
 origin=c.c_char_p();output=U();size=c.c_size_t();error=d.base_model_source(args.image,archive.encode(),resource.encode(),c.byref(origin),c.byref(output),c.byref(size))
 return (error.decode(),None,None) if error else (None,origin.value.decode(),hashlib.sha256(c.string_at(output,size.value)).hexdigest())
results=[]
for resource in ['effects/_shared/fx_guardian_paint.nif_wii','igcs/mickey/camera_film_trans_idle_cam.nif_wii']:
 fallback=read('packfiles/dbc_1st_zonec.pak',resource);direct=read('packfiles/_dynamic.pak',resource)
 assert not fallback[0] and not direct[0],(fallback,direct)
 assert fallback[1:]==direct[1:] and 'packfiles/_dynamic.pak' in fallback[1]
 results.append(dict(resource=resource,origin=fallback[1],sha256=fallback[2]))
local=read('packfiles/nos_hm_zonel_dynamic.pak','igcs/mickey/camera_film_trans_idle_cam.nif_wii')
assert not local[0] and 'packfiles/nos_hm_zonel_dynamic.pak' in local[1],local
assert read('packfiles/dbc_1st_zonec.pak','environments/_test/props/pressureplate.nif_wii')[0]=='EM1 resource is absent from this archive'
assert 'skin/morph' in read('packfiles/dbc_1st_zonec.pak','characters/mickey/mickey.nif_wii')[0]
Path('analysis/em1-compatibility/shared-model-result.json').write_text(json.dumps(dict(shared=results,local_precedence_origin=local[1],missing_and_unsupported_preserved=True,scope='Original base model source/bytes; no scene/native model publication.'),indent=2)+'\n')
print('PASS: shared/direct model bytes and origins identical; local provenance wins; absent/skin failures retained')
