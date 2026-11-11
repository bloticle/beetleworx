"""Actual GV scenes: declared model rebinding and native RAW/NIF PAK wire format."""
import argparse,ctypes as c,hashlib,json,runpy,tempfile
from pathlib import Path
from em1_scene_upgrade import inspect
from em1_nif import NIF
p=argparse.ArgumentParser();p.add_argument('--library',type=Path,required=True)
p.add_argument('--source-library',type=Path,required=True);p.add_argument('--image',type=Path,required=True)
args=p.parse_args();U=c.POINTER(c.c_ubyte);S=c.POINTER(c.c_char_p)
d=c.CDLL(str(args.library.resolve()));source=c.CDLL(str(args.source_library.resolve()))
d.build_typed.argtypes=[S,c.POINTER(U),c.POINTER(c.c_size_t),c.POINTER(c.c_uint32),c.c_size_t,c.POINTER(U),c.POINTER(c.c_size_t)];d.build_typed.restype=c.c_char_p
d.bind_models.argtypes=[U,c.c_size_t,c.c_uint32,c.c_uint32,S,S,c.c_size_t,c.POINTER(U),c.POINTER(c.c_size_t)];d.bind_models.restype=c.c_char_p
source.prepare.argtypes=[c.c_wchar_p,c.c_char_p,c.POINTER(U),c.POINTER(c.c_size_t)];source.prepare.restype=c.c_char_p
source.level_model_asset.argtypes=[c.c_wchar_p,c.c_char_p,c.c_char_p,c.c_char_p,c.POINTER(c.c_char_p),c.POINTER(c.c_char_p),c.POINTER(U),c.POINTER(c.c_size_t)];source.level_model_asset.restype=c.c_char_p
pointer,size=U(),c.c_size_t();assert not source.prepare(str(args.image),b'gv_zonea_start.level',c.byref(pointer),c.byref(size))
original_pak=c.string_at(pointer,size.value);read_pak=runpy.run_path('scripts/game-pak.py')['read_pak']
normalize=lambda value:value.lower().replace('\\','/')
scenes=[];references=0
with tempfile.TemporaryDirectory() as folder:
 path=Path(folder)/'scenes.pak';path.write_bytes(original_pak)
 for scene_name,original,kind in read_pak(path):
  before=inspect(original);bindings={}
  for entity in before['entities']:
   for component in entity[3]:
    for name,typ,mode,count,values in component[3]:
     if typ=='String' and mode in [2,3]:
      for value in values:
       key=normalize(value)
       if key.endswith(('.nif','.nif_wii')):bindings.setdefault(key,f'em1/runtime/test_model_{len(bindings)}.nif')
  pairs=list(bindings.items());from_names=(c.c_char_p*len(pairs))(*(key.encode() for key,value in pairs));to_names=(c.c_char_p*len(pairs))(*(value.encode() for key,value in pairs))
  buffer=(c.c_ubyte*len(original)).from_buffer_copy(original)
  entity_count=len(before['entities']);component_count=sum(len(entity[3]) for entity in before['entities'])
  error=d.bind_models(buffer,len(original),entity_count,component_count,from_names,to_names,len(pairs),c.byref(pointer),c.byref(size))
  assert not error,(scene_name,error,pairs[:3])
  rewritten=c.string_at(pointer,size.value);after=inspect(rewritten)
  expected=[]
  for entity in before['entities']:
   components=[]
   for component in entity[3]:
    properties=[]
    for name,typ,mode,count,values in component[3]:
     if typ=='String' and mode in [2,3]:
      values=[bindings.get(normalize(value),value) for value in values]
     properties.append((name,typ,mode,count,values))
    components.append((*component[:3],properties))
   expected.append((*entity[:3],components))
  assert after['entities']==expected,'Rebinding changed data outside declared NIF values'
  assert before['refs']==after['refs'],'Placed entities changed'
  if pairs:assert d.bind_models(buffer,len(original),entity_count,component_count,from_names,to_names,len(pairs)-1,c.byref(pointer),c.byref(size)), 'Missing model binding accepted'
  references+=len(pairs);scenes.append((scene_name,rewritten))
 assert references>0
 # This texture needs a sibling archive from the authored level. No global search.
 origin,textures=c.c_char_p(),c.c_char_p()
 error=source.level_model_asset(str(args.image),b'gv_zonea_start.level',b'packfiles/gv_zonea_airlock.pak',b'environments/thepark/fantasyland/smallworld/zones/gv_zonea_airlock_thinner01.nif_wii',c.byref(origin),c.byref(textures),c.byref(pointer),c.byref(size))
 assert not error,error
 model=c.string_at(pointer,size.value);NIF.read(model)
 payloads=[scenes[0][1],model];buffers=[(c.c_ubyte*len(value)).from_buffer_copy(value) for value in payloads]
 names=(c.c_char_p*2)(b'em1/runtime/test_scene.bin',b'em1/runtime/test_model.nif');data=(U*2)(*(c.cast(value,U) for value in buffers))
 lengths=(c.c_size_t*2)(*map(len,payloads));types=(c.c_uint32*2)(0,0x4E494600)
 assert not d.build_typed(names,data,lengths,types,2,c.byref(pointer),c.byref(size))
 path=Path(folder)/'typed.pak';path.write_bytes(c.string_at(pointer,size.value));entries=read_pak(path)
 assert entries==[('em1/runtime/test_scene.bin',payloads[0],b'\0'*4),('em1/runtime/test_model.nif',model,b'NIF\0')]
 for invalid in [0,0x484B5800]:
  types[1]=invalid;assert d.build_typed(names,data,lengths,types,2,c.byref(pointer),c.byref(size)), 'Invalid NIF type accepted'
result=dict(scenes=len(scenes),declared_model_bindings=references,all_other_values_and_placements_preserved=True,
    native_raw_nif_tags_and_payloads_match=True,missing_bindings_and_wrong_types_rejected=True,
    level_scoped_dependency_origin=origin.value.decode(),texture_origins=textures.value.decode().splitlines(),
    library_sha256=hashlib.sha256(args.library.read_bytes()).hexdigest(),world_activated=False)
Path('analysis/em1-compatibility/native-model-pak-result.json').write_text(json.dumps(result,indent=2)+'\n')
print('PASS:',len(scenes),'complete scenes,',references,'declared model aliases, native RAW/NIF tags, original payloads and scoped texture dependency')
