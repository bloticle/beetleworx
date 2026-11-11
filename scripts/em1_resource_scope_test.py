"""Actual EM1 catalog, original bootstrap order and archive ownership checks."""
import argparse
import ctypes as c
import json
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--library',type=Path,required=True);p.add_argument('--image',type=Path,required=True)
a=p.parse_args();d=c.CDLL(str(a.library.resolve()));S=c.POINTER(c.c_char_p)
d.resource_catalog.argtypes=[c.c_wchar_p,c.POINTER(c.c_size_t),c.POINTER(c.c_size_t)];d.resource_catalog.restype=c.c_char_p
d.resource_query.argtypes=[c.c_char_p,S,c.c_size_t,c.POINTER(c.c_int),c.POINTER(c.c_char_p)];d.resource_query.restype=c.c_char_p
d.level_resource_scope.argtypes=[c.c_wchar_p,c.c_char_p,c.POINTER(c.c_uint32),c.POINTER(c.c_char_p)];d.level_resource_scope.restype=c.c_char_p
d.level_scope_item.argtypes=[c.c_size_t,c.POINTER(c.c_uint32),c.POINTER(c.c_uint32),c.POINTER(c.c_int),c.POINTER(c.c_char_p),c.POINTER(c.c_char_p)];d.level_scope_item.restype=c.c_char_p
archives,resources=c.c_size_t(),c.c_size_t();error=d.resource_catalog(str(a.image),c.byref(archives),c.byref(resources));assert not error,error
assert archives.value==379 and resources.value>20000


def query(path,scope):
    names=(c.c_char_p*len(scope))(*[s.encode() for s in scope]);state,rows=c.c_int(91),c.c_char_p()
    error=d.resource_query(path.encode(),names,len(scope),c.byref(state),c.byref(rows))
    if error:assert state.value==91 and rows.value is None;return error,None
    return state.value,rows.value.decode().splitlines()


airlock='packfiles/gv_zonea_airlock.pak';village='packfiles/gremlinvillage_zonea.pak';steam='Effects/_Shared/Steam_Blocker_01'
state,rows=query(steam+'.nif',[airlock,village]);assert state==0 and len(rows)==1 and rows[0].startswith(airlock+'\t') and rows[0].endswith('.nif_wii')
state,rows=query(steam+'.nif',[village,airlock]);assert state==0 and rows[0].startswith(village+'\t')
assert query(steam+'.kfm',[airlock,'packfiles/_dynamic.pak'])[0]==2
state,rows=query('Environments/ThePark/FantasyLand/SmallWorld/Zones/GV_ZoneA_Door03.kfm',[airlock]);assert state==1 and rows and all(not r.startswith(airlock+'\t') for r in rows)
assert query('Environments/ThePark/FantasyLand/SmallWorld/Zones/GV_ZoneA_Door03.kfm',[village])[0]==0
assert query('Apprentice/ApprenticeGlobalState.bsq',[''])[0]==0
assert query('EFFECTS\\_SHARED\\STEAM_BLOCKER_01.NIF_WII',[airlock])[0]==0
for invalid in ['../escape.nif','C:/foreign.nif','/root.nif','a//b.nif','a/./b.nif']:assert isinstance(query(invalid,[airlock])[0],bytes)
counts=(c.c_uint32*3)(91,92,93);bootstrap=c.c_char_p();error=d.level_resource_scope(str(a.image),b'gv_zonea_start.level',counts,c.byref(bootstrap));assert not error,error
expected=['_dynamic','globalscripts','levels','bsq','collectibles','hudeffects','hudtextures','mainmenutextures','pausemenu_base','pausemenu_main','pausemenu_map','pausemenu_pins','pausemenu_quests']
assert bootstrap.value.decode().splitlines()==['packfiles/'+s+'.pak' for s in expected]
assert sum(counts)==394 and counts[0]>0
unavailable=[]
for i in range(sum(counts)):
    scene,entity,state=c.c_uint32(),c.c_uint32(),c.c_int();comp,prop=c.c_char_p(),c.c_char_p()
    path=d.level_scope_item(i,c.byref(scene),c.byref(entity),c.byref(state),c.byref(comp),c.byref(prop));assert path
    if state.value:unavailable.append(dict(scene=scene.value,definition=entity.value,component=comp.value.decode(),property=prop.value.decode(),path=path.decode(),state=state.value))
report=dict(scope='Original source presence/ownership and declared-resource planning; NOT mandatory readiness, optional-component behavior, conversion, runtime binding or playable-level proof',packfiles=archives.value,indexed_names=resources.value,bootstrap=bootstrap.value.decode().splitlines(),level='gv_zonea_start.level',in_scope=counts[0],outside_scope=counts[1],not_found_in_catalog=counts[2],unavailable=unavailable)
Path('analysis/em1-compatibility/resource-scope-result.json').write_text(json.dumps(report,indent=2)+'\n')
print(f'PASS:379original packfiles, {resources.value} catalog names; owner priority/outside-scope/absence distinct; original13archive bootstrap; GV declared fields {list(counts)}')
