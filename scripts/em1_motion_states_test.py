"""Original GV scenes retain all data while acquiring native movement states."""
import argparse,ctypes as c,json,runpy
from pathlib import Path
from em1_scene_upgrade import inspect
p=argparse.ArgumentParser();p.add_argument('--library',type=Path,required=True);p.add_argument('--archive',type=Path,required=True);a=p.parse_args()
read=runpy.run_path(str(Path(__file__).with_name('game-pak.py')))['read_pak'];scenes=read(a.archive)
lib=c.CDLL(str(a.library.resolve()));U=c.POINTER(c.c_ubyte);lib.reset()
lib.reserve.argtypes=[U,c.c_size_t,c.c_uint32,c.c_uint32];lib.reserve.restype=c.c_char_p
lib.adapt.argtypes=[U,c.c_size_t,c.c_uint32,c.c_uint32,c.POINTER(U),c.POINTER(c.c_size_t)];lib.adapt.restype=c.c_char_p
states={'OnGroundStateComponent':('3,0,0,0',{'Max Move Speed','GroundControlAccelStop','GroundControlAccel','Ground_FixedSpeeds','Ground_StickValues'}),
 'InAirStateComponent':('2,0,0,0',{'Max Air Speed','AirControlAccel','Character Gravity Down','MinNormalFallSpeed','TerminalSpeed','MinDeathFallSpeed'}),
 'JumpingStateComponent':('2,0,0,0',{'Jump Height','JumpType','JumpHeightMultipliers'}),
 'KnockbackStateComponent':('2,0,0,0',{'KnockbackMaxStim','KnockbackFriction'}),
 'DamagedLeapStateComponent':('2,0,0,0',{'DamagedLeapGravity','DamagedLeapStunTime','DamagedLeapMaxAirSpeed','DamagedLeapAirAccel','DamagedLeapLaunchSpeed'})}
def args(data):
 s=inspect(data);buf=(c.c_ubyte*len(data)).from_buffer_copy(data)
 return buf,len(data),len(s['entities']),sum(len(e[3]) for e in s['entities'])
def adapt(data):
 out=U();length=c.c_size_t();error=lib.adapt(*args(data),c.byref(out),c.byref(length));assert not error,error
 return c.string_at(out,length.value)
all_ids=set()
for name,data,tag in scenes:
 assert not lib.reserve(*args(data))
 for e in inspect(data)['entities']:
  all_ids.update(co[2][:8] for co in e[3])
added=matched=entities=original_components=0;new_ids={}
for name,data,tag in scenes:
 result=adapt(data);before,after=inspect(data),inspect(result)
 assert before['refs']==after['refs'] and len(before['entities'])==len(after['entities'])
 for old,new in zip(before['entities'],after['entities'],strict=True):
  assert old[:3]==new[:3] and old[3]==new[3][:len(old[3])],(name,old[0],'Authored data changed')
  controllers=[co for co in old[3] if co[0] in ['BasicMotionControllerComponent','MickeyPlayerMotionControllerComponent']]
  existing={co[0] for co in old[3]};expected=[s for s in states if controllers and s not in existing]
  extra=new[3][len(old[3]):];assert [co[0] for co in extra]==expected
  for co in extra:
   version,fields=states[co[0]];assert co[1]==version and co[2][8:]=='00000000'
   assert co[2][:8] not in all_ids
   identity=(old[1],co[0]);assert co[2][:8] not in new_ids or new_ids[co[2][:8]]==identity
   new_ids[co[2][:8]]=identity
   expected_props=[v for v in controllers[-1][3] if v[0] in fields];assert co[3]==expected_props
   matched+=len(co[3])
  added+=len(extra);entities+=bool(extra);original_components+=len(old[3])
 assert adapt(result)==result,'Adapter is not idempotent'
assert added>0 and matched>0
report=dict(scenes=len(scenes),original_components=original_components,adapted_entities=entities,added_states=added,copied_properties=matched,all_original_data_preserved=True,idempotent=True,world_activated=False)
Path('analysis/em1-compatibility/motion-states-result.json').write_text(json.dumps(report,indent=2));print(json.dumps(report))
