"""Version46 positioning and virtual-policy native-reference checks."""
import argparse,ctypes as c,json,struct
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--library',required=True);p.add_argument('--reference',type=Path,required=True);args=p.parse_args()
d=c.CDLL(str(Path(args.library).resolve()));U=c.POINTER(c.c_uint32);B=c.POINTER(c.c_ubyte)
d.positioning.argtypes=[B,c.c_size_t,c.POINTER(U),c.POINTER(c.c_size_t)];d.positioning.restype=c.c_char_p
d.virtual_policy.argtypes=[U,c.c_size_t,U];d.virtual_policy.restype=c.c_char_p
fixture=json.loads(args.reference.read_text())
for case in fixture['cases']:
 raw=bytes.fromhex(case['encoded']);b=(c.c_ubyte*len(raw)).from_buffer_copy(raw);pointer=U();size=c.c_size_t();error=d.positioning(b,len(raw),c.byref(pointer),c.byref(size));assert not error,error
 state=case['state'];v=case['voices'];services=case['path_service'];vertex=[];paths=[]
 # Native path service can omit unused vertex arrays when there are no paths.
 # Preserve/check those authored arrays against the encoded source as well.
 if state.get('kind')==2:
  vertices=struct.unpack_from('>I',raw,33)[0];vertex=list(struct.unpack_from('>'+str(vertices*4)+'I',raw,37))
  offset=37+vertices*16;n=struct.unpack_from('>I',raw,offset)[0];offset+=4
  pairs=list(struct.unpack_from('>'+str(n*2)+'I',raw,offset));offset+=n*8
  ranges=list(struct.unpack_from('>'+str(n*2)+'I',raw,offset))
  for i in range(n):paths.extend(pairs[i*2:i*2+2]+ranges[i*2:i*2+2])
 expected=[int(state['override']),state.get('center_bits',0),*state.get('pan_bits',[0,0]),int('kind' in state),int(state.get('panner',False)),state.get('kind',0),state.get('attenuation',0),int(state.get('spatialized',False)),int(state.get('dynamic',False)),state.get('mode',0),int(state.get('looping',False)),state.get('transition',0),int(state.get('follow',False)),len(vertex)//4,len(paths)//4,case['position_bytes']+7,v['virtual_queue'],v['maximum_instances'],v['below_threshold'],int(v['kill_newest']),int(v['override_maximum']),int(v['override_virtual']),*vertex,*paths]
 assert list(pointer[:size.value])==expected,(case,list(pointer[:size.value]),expected)
for case in fixture['virtual_inheritance']:
 nodes=case['nodes'];flat=[v for n in nodes for v in [n['virtual_queue'],n['below_threshold'],int(n['override_virtual'])]];values=(c.c_uint32*len(flat))(*flat);out=(c.c_uint32*2)()
 assert not d.virtual_policy(values,len(nodes),out)
 assert list(out)==[case['virtual_queue'],case['below_threshold']]
result=dict(positioning_cases=len(fixture['cases']),virtual_policy_cases=len(fixture['virtual_inheritance']),scope='Native-reference metadata decode/inheritance. Spatial mixing/path movement/voice stealing not tested.')
Path('analysis/em1-compatibility/audio-positioning-result.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS:',result)
