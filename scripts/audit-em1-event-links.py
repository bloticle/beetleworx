"""Independently inventory placed EM1 I/O connections, including inheritance."""
import argparse, copy, hashlib, json, struct
from pathlib import Path
from em1_scene_upgrade import inspect

def audit(path):
    data=path.read_bytes(); scene=inspect(data)
    entities={int(e[1][:8],16):e for e in scene['entities']}
    placed=[v[0] for v in struct.iter_unpack('>I',bytes.fromhex(scene['refs']))]
    rows=[]
    for identifier in placed:
        chain=[]; seen=set(); current=identifier
        while current:
            assert current not in seen and len(chain)<256
            seen.add(current);entity=entities[current];chain.append(entity);current=int(entity[1][8:16],16)
        merged=[]
        for entity in reversed(chain):
            for klass,version,links,properties in entity[3]:
                cid,master=int(links[:8],16),int(links[8:],16)
                matches=[c for c in merged if cid in c['lineage'] or master and master in c['lineage']]
                assert len(matches)<=1
                if matches:
                    c=matches[0];assert c['class']==klass;c['lineage'].add(cid)
                else:
                    c={'class':klass,'lineage':{cid},'props':{}};merged.append(c)
                for prop in properties:c['props'][prop[0]]=copy.deepcopy(prop)
        ios=[c for c in merged if c['class']=='InputOutputComponent'];assert len(ios)<=1
        if not ios:continue
        props=ios[0]['props']
        def array(key,kind):
            p=props[key];assert p[1]==kind and p[2]==1
            return p[4] if kind=='String' else bytes.fromhex(p[4])
        inputs=array('Input','String');n=len(inputs)
        if not n:continue
        outputs=array('Output','String');targets=array('Target','String');params=array('Parameters','String')
        delays=[v[0] for v in struct.iter_unpack('>f',array('DelayWait','Float'))]
        flags=[v[0] for v in struct.iter_unpack('>H2x',array('IoFlags','Unsigned Short'))]
        groups=array('TargetGroup','String') if 'TargetGroup' in props else []
        assert all(len(v)==n for v in [outputs,targets,params,delays,flags]);assert len(groups) in (0,n)
        for i in range(n):rows.append(dict(source_entity=identifier,source_name=entities[identifier][0],output=outputs[i],input=inputs[i],target=targets[i],parameters=params[i],target_group=groups[i] if groups else '',delay=delays[i],flags=flags[i]))
    signature=14695981039346656037
    for row in rows:
        packed=struct.pack('>I',row['source_entity'])
        for key in ['source_name','output','input','target','parameters','target_group']:packed+=row[key].encode()+b'\0'
        packed+=struct.pack('>fH',row['delay'],row['flags'])
        for byte in packed:signature=((signature^byte)*1099511628211)&((1<<64)-1)
    return dict(source_sha256=hashlib.sha256(data).hexdigest(),connections=len(rows),owners=len({r['source_entity'] for r in rows}),fingerprint=f'{signature:016X}',links=rows)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('source',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    result=audit(a.source);a.output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:v for k,v in result.items() if k!='links'}))
