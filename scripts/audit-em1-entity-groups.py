"""Resolve authored placed-entity groups independently of the native adapter."""
import argparse, copy, hashlib, json, struct
from pathlib import Path
from em1_scene_upgrade import inspect


def audit(path):
    data=path.read_bytes();scene=inspect(data)
    entities={int(e[1][:8],16):e for e in scene['entities']}
    rows=[];actors=[]
    for (identifier,) in struct.iter_unpack('>I',bytes.fromhex(scene['refs'])):
        chain=[];seen=set();current=identifier
        while current:
            if current in seen or len(chain)>=256:raise ValueError('Invalid entity inheritance')
            seen.add(current);entity=entities[current];chain.append(entity);current=int(entity[1][8:16],16)
        merged=[]
        for entity in reversed(chain):
            for klass,version,links,properties in entity[3]:
                cid,master=int(links[:8],16),int(links[8:],16)
                matches=[c for c in merged if cid in c['lineage'] or master and master in c['lineage']]
                if len(matches)>1:raise ValueError('Ambiguous inherited component')
                if matches:
                    c=matches[0]
                    if c['class']!=klass:raise ValueError('Inherited component class changed')
                    c['lineage'].add(cid)
                else:c={'class':klass,'lineage':{cid},'props':{}};merged.append(c)
                for prop in properties:c['props'][prop[0]]=copy.deepcopy(prop)
        actors.append(dict(source_entity=identifier,source_name=entities[identifier][0],components=[c['class'] for c in merged],resources=[dict(component=c['class'],property=p[0],paths=p[4]) for c in merged for p in c['props'].values() if p[1]=='String' and 'File Path' in p[0]]))
        for c in merged:
            if c['class']!='InputOutputComponent':continue
            prop=c['props'].get('EntityGroups')
            if prop is None:continue
            if prop[1:3]!=('String',1) or len(prop[4])!=prop[3]:raise ValueError('Invalid group array')
            for group in prop[4]:rows.append(dict(source_entity=identifier,source_name=entities[identifier][0],group=group))
    signature=14695981039346656037
    for row in rows:
        packed=struct.pack('>I',row['source_entity'])+row['source_name'].encode()+b'\0'+row['group'].encode()+b'\0'
        for byte in packed:signature=((signature^byte)*1099511628211)&((1<<64)-1)
    return dict(source_sha256=hashlib.sha256(data).hexdigest(),memberships=len(rows),groups=len({r['group'] for r in rows}),fingerprint=f'{signature:016X}',members=rows,actors=actors)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('source',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    result=audit(a.source);a.output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:v for k,v in result.items() if k not in ('members','actors')}))
