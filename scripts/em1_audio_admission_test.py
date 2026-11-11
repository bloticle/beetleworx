"""Original priority victim selection including owner/group/stopping/tie rules."""
import argparse,ctypes as c,json
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--library',required=True);p.add_argument('--reference',type=Path,required=True);args=p.parse_args()
d=c.CDLL(str(Path(args.library).resolve()));Q=c.POINTER(c.c_uint64);F=c.POINTER(c.c_float);U=c.POINTER(c.c_uint32)
d.victim.argtypes=[c.c_float,c.c_int,Q,Q,F,U,c.c_size_t,c.c_uint64,c.c_uint32];d.victim.restype=c.c_uint64
fixture=json.loads(args.reference.read_text())
for case in fixture['cases']:
 voices=case['voices'];n=len(voices);ids=(c.c_uint64*n)(*[v['id'] for v in voices]);owners=(c.c_uint64*n)(*[v['owner'] for v in voices]);priorities=(c.c_float*n)(*[v['priority'] for v in voices]);flags=(c.c_uint32*n)(*[int(v['matches'])|int(v['stopping'])<<1 for v in voices])
 actual=d.victim(case['incoming'],int(case['kill_newest']),ids,owners,priorities,flags,n,case['owner'],case['group'])
 assert actual==(case['selected'] or 0),(case,actual)
result=dict(cases=len(fixture['cases']),scope='Original-DOL reference victim kernel including object/group scope, stopping exclusion and tie ordering. Mixer integration separately tested in audio-output probe; native stop ramps/virtualization remain unfinished.')
Path('analysis/em1-compatibility/audio-admission-result.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS:',result)
