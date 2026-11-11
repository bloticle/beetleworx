"""Attempt actual native model/texture conversion for complete authored entry scenes."""
import argparse,ctypes as c,json,runpy,tempfile,time,collections
from pathlib import Path
from em1_rvz import RVZ
from em1_scene_upgrade import inspect
p=argparse.ArgumentParser();p.add_argument('--library',required=True);p.add_argument('--image',type=Path,required=True)
p.add_argument('--level',action='append',help='Original .level descriptor; repeat to select multiple levels')
p.add_argument('--output',type=Path,default=Path('analysis/em1-compatibility/full-level-model-conversion.json'))
args=p.parse_args()
levels=args.level or ['dbc_courtyard_1st.level','dbc_start_1st.level','toontown.level','dbc_start_zonec.level','dbc_start_zoneg.level']
d=c.CDLL(str(Path(args.library).resolve()));d.model_asset.argtypes=[c.c_wchar_p,c.c_char_p,c.c_char_p,c.POINTER(c.c_size_t),c.POINTER(c.c_uint32),c.POINTER(c.c_uint32)];d.model_asset.restype=c.c_char_p
readpak=runpy.run_path(str(Path(__file__).with_name('game-pak.py')))['read_pak'];catalog=json.loads(Path('analysis/em1-compatibility/level-catalog-audit.json').read_text());requests={};r=RVZ(args.image)
try:
 files={n.lower():v for n,v in r.files(1).items()};cache={}
 with tempfile.TemporaryDirectory() as temp:
  for level in levels:
   row=next(x for x in catalog['levels'] if x['id']==level)
   for scene in row['entry_scenes']:
    archive=scene['archive']
    if archive not in cache:
     path=Path(temp)/'source.pak';path.write_bytes(r.read_partition(1,*files[archive]));cache[archive]={n.lower().replace(chr(92),'/'):b for n,b,k in readpak(path)}
    original=cache[archive][scene['gsa'][:-4]+'.bin']
    for entity in inspect(original)['entities']:
     for component in entity[3]:
      for name,typ,mode,amount,values in component[3]:
       if typ=='String':
        for value in values:
         value=value.lower().replace(chr(92),'/')
         if value.endswith(('.nif','.nif_wii')):
          resource=value if value.endswith('_wii') else value+'_wii'
          requests.setdefault((archive,resource),set()).add(level)
finally:r.close()
print('Original unique model requests:',len(requests),flush=True);results=[]
for i,((archive,resource),levels) in enumerate(requests.items()):
 length=c.c_size_t();meshes=c.c_uint32();textures=c.c_uint32();start=time.monotonic()
 error=d.model_asset(str(args.image),archive.encode(),resource.encode(),c.byref(length),c.byref(meshes),c.byref(textures))
 result=dict(archive=archive,resource=resource,levels=sorted(levels),error=error.decode() if error else None,bytes=length.value,meshes=meshes.value,textures=textures.value,seconds=round(time.monotonic()-start,3));results.append(result)
 if i%10==0:print('Converted/scanned',i+1,'of',len(requests),flush=True)
 args.output.write_text(json.dumps(dict(complete=len(results)==len(requests),total=len(requests),results=results,scope='Actual model/texture converter attempts; no native scene/model publication, baking or full component activation.'),indent=2)+'\n')
print('CONVERTED',sum(not x['error'] for x in results),'UNSUPPORTED',sum(bool(x['error']) for x in results),flush=True)
print(dict(collections.Counter(x['error'] for x in results if x['error'])),flush=True)
