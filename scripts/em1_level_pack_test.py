"""Native full-scene preparation and archive checks against independent readers. Run from repository root."""
import argparse,ctypes as c,sys,json,hashlib,importlib.util,tempfile,copy
from pathlib import Path
sys.path.insert(0,str(Path('scripts').resolve()))
from em1_rvz import RVZ
from em1_scene_upgrade import upgrade,inspect
spec=importlib.util.spec_from_file_location('pak','scripts/game-pak.py');pak=importlib.util.module_from_spec(spec);spec.loader.exec_module(pak)
parser=argparse.ArgumentParser();parser.add_argument('--library',type=Path,required=True);parser.add_argument('--image',type=Path,required=True);args=parser.parse_args()
dll=c.CDLL(str(args.library.resolve()));U=c.POINTER(c.c_ubyte)
dll.prepare.argtypes=[c.c_wchar_p,c.c_char_p,c.POINTER(U),c.POINTER(c.c_size_t)];dll.prepare.restype=c.c_char_p
dll.scene_archive.argtypes=[c.c_size_t,c.POINTER(c.c_char_p),c.POINTER(c.c_char_p),c.POINTER(U),c.POINTER(c.c_size_t)];dll.scene_archive.restype=c.c_char_p
dll.bind_scene.argtypes=[c.c_size_t,c.c_int,c.POINTER(U),c.POINTER(c.c_size_t)];dll.bind_scene.restype=c.c_char_p

dll.scene_files.argtypes=[c.c_size_t,c.POINTER(c.c_size_t)];dll.scene_files.restype=c.c_char_p
dll.scene_file.argtypes=[c.c_size_t,c.POINTER(c.c_uint32),c.POINTER(c.c_uint32),c.POINTER(c.c_char_p),c.POINTER(c.c_char_p),c.POINTER(c.c_size_t)];dll.scene_file.restype=c.c_char_p

dll.partition_resource.argtypes=[c.c_size_t,c.c_size_t,c.POINTER(c.c_int),c.POINTER(c.c_char_p)];dll.partition_resource.restype=c.c_char_p
dll.entry_partition.argtypes=[c.c_size_t,c.POINTER(c.c_int)];dll.entry_partition.restype=c.c_char_p
dll.build.argtypes=[c.POINTER(c.c_char_p),c.POINTER(U),c.POINTER(c.c_size_t),c.c_size_t,c.POINTER(U),c.POINTER(c.c_size_t)];dll.build.restype=c.c_int
image=str(args.image)
rvz=RVZ(image);files={n.lower():v for n,v in rvz.files(1).items()}
catalog=json.loads(Path('analysis/em1-compatibility/level-catalog-audit.json').read_text());results=[]
try:
 with tempfile.TemporaryDirectory() as temp:
  temp=Path(temp);archive_cache={};resource_checks=0
  level_pak=temp/'levels.pak';level_pak.write_bytes(rvz.read_partition(1,*files['packfiles/levels.pak']))
  descriptors={name.lower():data for name,data,_ in pak.read_pak(level_pak)}
  for level in ['dbc_courtyard_1st.level','dbc_start_1st.level','toontown.level','dbc_start_zonec.level','dbc_start_zoneg.level']:
   row=next(x for x in catalog['levels'] if x['id']==level);assert not row['issues'],row['issues']
   pointer=U();length=c.c_size_t();error=dll.prepare(image,level.encode(),c.byref(pointer),c.byref(length));assert not error,error
   expected_parts=[(row['fields'][key].lower().removesuffix('.part'),mode) for key,mode in [('initial',0),('interior',1)] if row['fields'].get(key)]
   for index,(name,mode) in enumerate(expected_parts):
    actual_mode=c.c_int();actual_name=dll.entry_partition(index,c.byref(actual_mode))
    assert actual_name.decode()==name and actual_mode.value==mode,(level,name,mode)
   assert not dll.entry_partition(len(expected_parts),c.byref(c.c_int()))
   for entry,part_name in enumerate(row['entry_partitions']):
    expected=[]
    for line in descriptors[part_name].decode('latin1').splitlines():
     line=line.strip()
     if not line or line.startswith('//'):continue
     path,separator,flags=line.partition(' -');path=path.strip().replace(chr(92),'/').lower()
     expected.append((path,1 if path.endswith('.bnk') else 0,('-'+flags).strip() if separator else ''))
    for index,(path,kind,flags) in enumerate(expected):
     actual_kind=c.c_int();actual_flags=c.c_char_p()
     actual_name=dll.partition_resource(entry,index,c.byref(actual_kind),c.byref(actual_flags))
     assert actual_name.decode()==path and actual_kind.value==kind and actual_flags.value.decode()==flags,(level,part_name,index)
     resource_checks+=1
    assert not dll.partition_resource(entry,len(expected),c.byref(c.c_int()),c.byref(c.c_char_p()))

   encoded=c.string_at(pointer,length.value);target=temp/'candidate.pak';target.write_bytes(encoded);actual=pak.read_pak(target)
   assert len(actual)==len(row['entry_scenes'])
   reports=[]
   for index,((name,data,kind),request) in enumerate(zip(actual,row['entry_scenes'])):
    archive=request['archive']
    if archive not in archive_cache:
     original=rvz.read_partition(1,*files[archive]);target=temp/'original.pak';target.write_bytes(original)
     archive_cache[archive]={n.replace(chr(92),'/').lower():d for n,d,k in pak.read_pak(target)}
    original=archive_cache[archive][request['gsa'][:-4]+'.bin'];expected,report=upgrade(original)
    assert data==expected,request['gsa']
    file_count=c.c_size_t();error=dll.scene_files(index,c.byref(file_count));assert not error,error
    expected_files=[]
    for entity in inspect(original)['entities']:
     for component in entity[3]:
      for property_name,typ,mode,amount,values in component[3]:
       if typ=='String':
        for path in values:
         if path.lower().endswith(('.nif','.nif_wii','.hkx','.hkx_wii','.kfm','.kfm_wii','.kf','.kf_wii','.hkw','.hkw_wii','.hkb','.hkb_wii','.hkp','.hkp_wii','.lua','.bnk','.gsa','.lit_cooked','.bsq')):
          expected_files.append((int(entity[1][:8],16),int(component[2][:8],16),component[0],property_name,path))
    assert file_count.value==len(expected_files)
    for file_index,wanted in enumerate(expected_files):
     entity_id=c.c_uint32();component_id=c.c_uint32();component_name=c.c_char_p();property_name=c.c_char_p();offset=c.c_size_t()
     path=dll.scene_file(file_index,c.byref(entity_id),c.byref(component_id),c.byref(component_name),c.byref(property_name),c.byref(offset))
     assert (entity_id.value,component_id.value,component_name.value.decode(),property_name.value.decode(),path.decode())==wanted
     pointer_offset=int.from_bytes(data[offset.value:offset.value+4],'big')+6
     assert data[pointer_offset:data.index(0,pointer_offset)].decode()==path.decode()
    bound_pointer=U();bound_length=c.c_size_t();error=dll.bind_scene(index,0,c.byref(bound_pointer),c.byref(bound_length));assert not error,error
    bound=c.string_at(bound_pointer,bound_length.value);before=inspect(data);after=inspect(bound)
    mapping={}
    for _,_,_,_,path in expected_files:
     normalized=path.lower().replace(chr(92),'/')
     if normalized not in mapping:
      suffix=normalized[normalized.rfind('.'):].removesuffix('_wii')
      mapping[normalized]='em1/runtime/bound_'+str(len(mapping))+suffix
    rows=copy.deepcopy(before['entities'])
    for entity in rows:
     for component in entity[3]:
      for property_name,typ,mode,amount,values in component[3]:
       if typ=='String':
        for n,path in enumerate(values):
         normalized=path.lower().replace(chr(92),'/')
         if normalized in mapping:values[n]=mapping[normalized]
    assert after['entities']==rows and after['refs']==before['refs']
    assert bound[8:before['string_end']]==data[8:before['string_end']]
    assert len(bound)-len(data)==after['string_end']-before['string_end']
    if expected_files:
     for invalid in [1,2,3,4]:
      assert dll.bind_scene(index,invalid,c.byref(bound_pointer),c.byref(bound_length)), 'Invalid file binding accepted: '+str(invalid)
    gsa=c.c_char_p();scene=c.c_char_p();archive_pointer=U();archive_length=c.c_size_t()
    basename=dll.scene_archive(index,c.byref(gsa),c.byref(scene),c.byref(archive_pointer),c.byref(archive_length)).decode()
    assert basename=='em1_level_probe_s'+str(index)
    assert gsa.value.decode()=='em1/runtime/'+basename+'.gsa' and scene.value.decode()=='em1/runtime/'+basename+'.bin'
    target=temp/'bound-scene.pak';target.write_bytes(c.string_at(archive_pointer,archive_length.value))
    assert pak.read_pak(target)==[(scene.value.decode(),expected,bytes(4))]

    reports.append(dict(source=request['gsa'],bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),file_references=file_count.value))
   results.append(dict(level=level,scenes=reports,archive_bytes=len(encoded)))
  # Preserve the old single-scene layout, checked against the independent
  # archive writer using an incompressible short payload.
  sample=bytes(range(32));storage=(c.c_ubyte*len(sample)).from_buffer_copy(sample)
  one_name=b'em1/runtime/em1_courtyard_probe.bin';args=(c.c_char_p*1)(one_name)
  data=(U*1)(c.cast(storage,U));sizes=(c.c_size_t*1)(len(sample))
  pointer=U();length=c.c_size_t()
  assert dll.build(args,data,sizes,1,c.byref(pointer),c.byref(length))==1
  target=temp/'single-oracle.pak';pak.write_pak([(one_name.decode(),sample,bytes(4))],target)
  assert c.string_at(pointer,length.value)==target.read_bytes()
  # Empty archives and path aliases must reject before publication.
  payload=(c.c_ubyte*1)(42);pointer=U();length=c.c_size_t();rejected=0
  for names in [[],['em1/runtime/../bad.bin'],['em1/runtime/x.bin','EM1/runtime/X.bin'],['em1/runtime/x.bin','em1/runtime/X.bin'],['em1/runtime/x.bin'+chr(92)+'bad']]:
   count=len(names);args=(c.c_char_p*count)(*[n.encode() for n in names]);data=(U*count)(*[c.cast(payload,U)]*count);sizes=(c.c_size_t*count)(*[1]*count)
   assert dll.build(args,data,sizes,count,c.byref(pointer),c.byref(length))==0,names;rejected+=1
 result=dict(levels=results,ordered_partition_resources=resource_checks,individual_scene_archives_verified=True,complete_scene_path_rebinding_verified=True,rejected_inputs=rejected,scope='Native full entry-scene preparation and multi-entry archive match independent RVZ/PAK/scene converter. Does not prove world activation or component/resource compatibility.')
 Path('analysis/em1-compatibility/full-level-entry-pak-result.json').write_text(json.dumps(result,indent=2)+'\n')
 print('PASS:',len(results),'levels;',sum(len(x['scenes']) for x in results),'complete scenes;',resource_checks,'ordered resources; malformed archives rejected:',rejected)
finally:rvz.close()
