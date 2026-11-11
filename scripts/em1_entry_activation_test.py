"""Entry activation flags against retail active/transition states, not gameplay proof."""
import argparse,ast,ctypes as c,hashlib,json,runpy,struct
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--library',type=Path,required=True);p.add_argument('--archive',type=Path,required=True);a=p.parse_args()
ns={'struct':struct};tree=ast.parse(Path('scripts/em1_level_registry_test.py').read_text())
exec(compile(ast.Module(body=[n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='decode'],type_ignores=[]),'decode','exec'),ns);decode=ns['decode']
raw=next(b for n,b,k in runpy.run_path('scripts/game-pak.py')['read_pak'](a.archive) if n=='levels/master.lvlc')
original=decode(raw);states={row[0]:row for row in original[1]}
active={k for k,z,f in states['mainmenu.start'][2]};assert len(active)==1;active_kind=active.pop();assert active_kind==1
transition=states['rbc_falls.cavernstogulch_angel'][2]
assert any(k==2 and zone.startswith('ftl_gulch.') for k,zone,flag in transition)
d=c.CDLL(str(a.library.resolve()));U=c.POINTER(c.c_ubyte)
d.entry_links.argtypes=[U,c.c_size_t,c.c_int,c.POINTER(c.c_char_p),c.POINTER(c.c_int),c.c_size_t,c.POINTER(U),c.POINTER(c.c_size_t)];d.entry_links.restype=c.c_char_p
buf=(c.c_ubyte*len(raw)).from_buffer_copy(raw);catalog=json.loads(Path('analysis/em1-compatibility/level-catalog-audit.json').read_text());cases=[]
for level in ['gv_zonea_start.level','dbc_courtyard_1st.level','dbc_start_1st.level','toontown.level','dbc_start_zonec.level','dbc_start_zoneg.level']:
 row=next(v for v in catalog['levels'] if v['id']==level);fields=row['fields'];partitions=[(fields[key],key=='interior') for key in ['initial','interior'] if fields.get(key)]
 names=(c.c_char_p*len(partitions))(*(name.lower().removesuffix('.part').encode() for name,mode in partitions));modes=(c.c_int*len(partitions))(*(mode for name,mode in partitions))
 out,size=U(),c.c_size_t();shared=bool(fields.get('shared'))
 error=d.entry_links(buf,len(raw),shared,names,modes,len(partitions),c.byref(out),c.byref(size));assert not error,(level,error)
 zones,states,targets=decode(c.string_at(out,size.value));assert zones[:len(original[0])]==original[0] and states[:-1]==original[1] and targets[:-1]==original[2]
 links=states[-1][2];loads=[v for v in links if v[0]==active_kind]
 assert len(loads)==int(shared)+len(partitions) and all(k!=2 for k,z,f in links)
 assert len([v for v in links if v[0]==5])==sum(mode for name,mode in partitions)
 cases.append(dict(level=level,links=links))
result=dict(active_kind_from_retail_mainmenu=active_kind,neighbor_preload_kind_from_retail_transition=2,
    cases=cases,retail_records_unchanged=True,library_sha256=hashlib.sha256(a.library.read_bytes()).hexdigest(),
    scope='Exact runtime entry compiler flags and codec; fixture zones have no converted assets; not native activation or gameplay proof.')
Path('analysis/em1-compatibility/entry-activation-result.json').write_text(json.dumps(result,indent=2)+'\n')
print('PASS:',len(cases),'source entry plans use retail active-load kind1, preserve retail records, never use neighbor-preload kind2')
