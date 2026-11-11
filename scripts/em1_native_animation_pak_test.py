"""Original complete GV scenes: KFM rebinding isolation and typed PAK routing."""
import argparse, ctypes as c, hashlib, json, re, runpy, tempfile
from pathlib import Path
from em1_scene_upgrade import inspect

p=argparse.ArgumentParser()
p.add_argument('--library',type=Path,required=True)
p.add_argument('--source-library',type=Path,required=True)
p.add_argument('--image',type=Path,required=True)
p.add_argument('--profile',type=Path)
a=p.parse_args(); U=c.POINTER(c.c_ubyte); S=c.POINTER(c.c_char_p)
d=c.CDLL(str(a.library.resolve())); source=c.CDLL(str(a.source_library.resolve()))
d.build_typed.argtypes=[S,c.POINTER(U),c.POINTER(c.c_size_t),c.POINTER(c.c_uint32),c.c_size_t,c.POINTER(U),c.POINTER(c.c_size_t)]
d.build_typed.restype=c.c_char_p
d.bind_animation_paths.argtypes=[U,c.c_size_t,c.c_uint32,c.c_uint32,S,S,c.c_size_t,S,c.c_size_t,c.POINTER(U),c.POINTER(c.c_size_t)]
d.bind_animation_paths.restype=c.c_char_p
source.prepare.argtypes=[c.c_wchar_p,c.c_char_p,c.POINTER(U),c.POINTER(c.c_size_t)];source.prepare.restype=c.c_char_p
out,n=U(),c.c_size_t()
assert not source.prepare(str(a.image),b'gv_zonea_start.level',c.byref(out),c.byref(n))
original=c.string_at(out,n.value); read_pak=runpy.run_path('scripts/game-pak.py')['read_pak']
normalize=lambda s:s.lower().replace('\\','/')
scenes=aliases=0
with tempfile.TemporaryDirectory() as folder:
 path=Path(folder)/'scene.pak';path.write_bytes(original)
 for scene_name,scene,tag in read_pak(path):
  before=inspect(scene); bindings={}
  for entity in before['entities']:
   for component in entity[3]:
    for name,typ,mode,count,values in component[3]:
     if typ=='String' and mode in [2,3]:
      for value in values:
       key=normalize(value)
       if key.endswith(('.kfm','.kfm_wii')):bindings.setdefault(key,f'em1/runtime/animation_{len(bindings)}.kfm')
  pairs=list(bindings.items()); keys=(c.c_char_p*len(pairs))(*(k.encode() for k,v in pairs));values=(c.c_char_p*len(pairs))(*(v.encode() for k,v in pairs))
  buf=(c.c_ubyte*len(scene)).from_buffer_copy(scene)
  entities=len(before['entities']);components=sum(len(e[3]) for e in before['entities'])
  error=d.bind_animation_paths(buf,len(scene),entities,components,keys,values,len(pairs),None,0,c.byref(out),c.byref(n))
  assert not error,(scene_name,error)
  after=inspect(c.string_at(out,n.value));expected=[]
  for entity in before['entities']:
   comps=[]
   for component in entity[3]:
    props=[]
    for name,typ,mode,count,vals in component[3]:
     if typ=='String' and mode in [2,3]:vals=[bindings.get(normalize(v),v) for v in vals]
     props.append((name,typ,mode,count,vals))
    comps.append((*component[:3],props))
   expected.append((*entity[:3],comps))
  assert after['entities']==expected, 'Changed a non-KFM property or component'
  assert before['refs']==after['refs'], 'Changed placed entities'
  if pairs:
   assert d.bind_animation_paths(buf,len(scene),entities,components,keys,values,len(pairs)-1,None,0,c.byref(out),c.byref(n)), 'Missing animation binding accepted'
   retained=(c.c_char_p*1)(pairs[-1][0].encode())
   assert not d.bind_animation_paths(buf,len(scene),entities,components,keys,values,len(pairs)-1,retained,1,c.byref(out),c.byref(n))
   kept=inspect(c.string_at(out,n.value))
   expected_kept=[]
   for entity in before['entities']:
    comps=[]
    for component in entity[3]:
     props=[]
     for name,typ,mode,count,vals in component[3]:
      if typ=='String' and mode in [2,3]:vals=[bindings.get(normalize(v),v) if normalize(v)!=pairs[-1][0] else v for v in vals]
      props.append((name,typ,mode,count,vals))
     comps.append((*component[:3],props))
    expected_kept.append((*entity[:3],comps))
   assert kept['entities']==expected_kept and kept['refs']==before['refs'], 'Explicit unresolved reference was altered'
  scenes+=1;aliases+=len(pairs)
 assert scenes==3 and aliases>0
 # The wire-format test deliberately treats payloads as opaque. Native loader
 # acceptance/playback is checked separately inside Beetleworx, not inferred here.
 payloads=[b'raw-scene',b'native-model',b'native-keyframe',b'native-manager']
 names=(c.c_char_p*4)(b'em1/runtime/a.bin',b'em1/runtime/a.nif',b'em1/runtime/a.kf',b'em1/runtime/a.kfm')
 buffers=[(c.c_ubyte*len(b)).from_buffer_copy(b) for b in payloads];data=(U*4)(*(c.cast(b,U) for b in buffers))
 sizes=(c.c_size_t*4)(*map(len,payloads));types=(c.c_uint32*4)(0,0x4E494600,0x4B460000,0x4B464D00)
 assert not d.build_typed(names,data,sizes,types,4,c.byref(out),c.byref(n))
 path.write_bytes(c.string_at(out,n.value));entries=read_pak(path)
 assert entries==[(name.decode(),b,typ.to_bytes(4,'big')) for name,b,typ in zip(names,payloads,types,strict=True)]
 for i in [2,3]:
  valid=types[i]
  for wrong in [0,0x4E494600,0x484B5800,0x4B464D00 if i==2 else 0x4B460000]:
   types[i]=wrong;assert d.build_typed(names,data,sizes,types,4,c.byref(out),c.byref(n)), 'Mismatched animation factory type accepted'
  types[i]=valid
result=dict(scenes=scenes,declared_animation_aliases=aliases,all_other_properties_components_and_placements_preserved=True,
    missing_bindings_rejected=True,explicit_unresolved_paths_preserved_exactly=True,native_raw_nif_kf_kfm_tags_and_opaque_payloads_match=True,
    wrong_factory_types_rejected=True,library_sha256=hashlib.sha256(a.library.read_bytes()).hexdigest(),world_activated=False)
if a.profile:
 completion=json.loads((a.profile/'completion-result.json').read_text())
 assert completion['completed'] and completion['normal_user_files_unchanged'] and completion['em1_source_metadata_unchanged'] and not completion['gameplay_input_sent']
 assert completion['exe_sha256']==hashlib.sha256(Path('build/runtime23/beetleworx.exe').read_bytes()).hexdigest().upper()
 source.level_resource_scope.argtypes=[c.c_wchar_p,c.c_char_p,c.POINTER(c.c_uint32),c.POINTER(c.c_char_p)];source.level_resource_scope.restype=c.c_char_p
 source.level_scope_item.argtypes=[c.c_size_t,c.POINTER(c.c_uint32),c.POINTER(c.c_uint32),c.POINTER(c.c_int),c.POINTER(c.c_char_p),c.POINTER(c.c_char_p)];source.level_scope_item.restype=c.c_char_p
 counts=(c.c_uint32*3)();bootstrap=c.c_char_p()
 assert not source.level_resource_scope(str(a.image),b'gv_zonea_start.level',counts,c.byref(bootstrap))
 available=set();unresolved=set();presence=[]
 for i in range(sum(counts)):
  scene,entity,state=c.c_uint32(),c.c_uint32(),c.c_int();component,prop=c.c_char_p(),c.c_char_p()
  path=source.level_scope_item(i,c.byref(scene),c.byref(entity),c.byref(state),c.byref(component),c.byref(prop)).decode()
  if normalize(path).endswith(('.kfm','.kfm_wii')):
   item=(scene.value,normalize(path));(available if state.value==0 else unresolved).add(item)
   presence.append(dict(scene=scene.value,path=path,source_presence=state.value))
 assert len(available)+len(unresolved)==aliases
 log=(a.profile/'game.log').read_text(errors='replace')
 row=re.search(r"original level animation archives staged: level='gv_zonea_start.level' managers=(\d+) resources=(\d+) unresolved=(\d+);",log)
 assert row and int(row[1])==len(available) and int(row[3])==len(unresolved) and int(row[2])>=len(available)
 assert 'original level registry extension failed:' not in log
 reported=re.findall(r"original animation reference retained unresolved: '([^']+)'",log)
 assert sorted(reported)==sorted(path for scene,path in unresolved)
 result['normal_level_staging']=dict(profile=str(a.profile),completion=completion,available_managers=len(available),
   unresolved_authored_references=len(unresolved),resource_count=int(row[2]),source_presence=presence,
   source_catalog_and_scene_counts_match=True,normal_factory_consumption_and_world_activation_proven=False)
Path('analysis/em1-compatibility/native-animation-pak-result.json').write_text(json.dumps(result,indent=2)+'\n')
print('PASS:',scenes,'complete scenes,',aliases,'KFM aliases, unchanged other data and placements; exact RAW/NIF/KF/KFM routing')
