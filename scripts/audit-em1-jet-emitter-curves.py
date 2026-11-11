"""Read original jet emitter curves without modifying the NIF or RVZ."""
from pathlib import Path
import json,struct,hashlib
from em1_nif import NIF
path=Path('analysis/em1-compatibility/tutorial-actor-resources/Effects/_Shared/Steam_Jet_01.nif_wii')
raw=path.read_bytes();n=NIF.read(raw)
def u(b,o=0):return struct.unpack_from(n.endian+'I',b,o)[0]
def f(b,o=0):return struct.unpack_from(n.endian+'f',b,o)[0]
p=8;strings=[]
for _ in range(u(n.strings)):
 size=u(n.strings,p);p+=4;strings.append(n.strings[p:p+size].decode());p+=size

def keys(index,boolean):
 typ=n.types[n.indices[index]];b=n.blocks[index]
 expected='NiBoolInterpolator' if boolean else 'NiFloatInterpolator'
 if typ!=expected or len(b)!=(5 if boolean else 8):raise ValueError('Interpolator layout changed')
 value=b[0] if boolean else f(b);data=u(b,1 if boolean else 4)
 if data==0xffffffff:return {'constant':value,'keys':[]}
 k=n.blocks[data];kind=n.types[n.indices[data]]
 if kind!=('NiBoolData' if boolean else 'NiFloatData'):raise ValueError('Unexpected key container')
 count=u(k);stride=5 if boolean else 8
 if not count or u(k,4)!=5 or len(k)!=8+stride*count:raise ValueError('Unsupported authored curve layout')
 return {'default':value,'interpolation_type':5,'keys':[{'time':f(k,8+i*stride),'value':k[12+i*stride] if boolean else f(k,12+i*stride)} for i in range(count)]}

records=[]
for i,b in enumerate(n.blocks):
 if n.types[n.indices[i]]!='NiPSEmitParticlesCtlr':continue
 if len(b)!=38:raise ValueError('Emitter controller layout changed')
 target=u(b,22)
 if n.types[n.indices[target]]!='NiPSParticleSystem':raise ValueError('Unexpected emitter target')
 records.append({'block':i,'system':strings[u(n.blocks[target])],'emitter':strings[u(b,30)],'frequency':f(b,6),'phase':f(b,10),'start':f(b,14),'stop':f(b,18),'rate':keys(u(b,26),False),'enabled':keys(u(b,34),True)})
if len(records)!=4:raise ValueError('Expected four authored jet emitters')
Path('analysis/em1-compatibility/original-jet-emitter-curves.json').write_text(json.dumps({'source_sha256':hashlib.sha256(raw).hexdigest(),'emitters':records},indent=2)+'\n')
for item in records:
 print(item['system'], 'first positive rate:',next((k['time'] for k in item['rate']['keys'] if k['value']>0),item['rate'].get('constant')),'first enable:',next((k['time'] for k in item['enabled']['keys'] if k['value']==1),item['enabled'].get('constant')))
