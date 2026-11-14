"""Verify carried input cannot cause Jump after native restriction return.

Requires three real native worlds and compares actual callbacks, actor height,
movement while held, fresh post-release Jump/landing and matching updates.
The optional unfenced mode must reproduce the old unsolicited Jump.
"""
import argparse
import json
import math
from pathlib import Path
import re
import runpy
import struct


def verify(root,unfenced=False):
    helpers=Path(__file__).parent
    result=json.loads((root/'result.json').read_text(encoding='utf-8-sig'))
    assert result['cutscene_held_return'] and result['unfenced_cutscene_return']==unfenced
    base=runpy.run_path(str(helpers/'verify-native-cutscene-return.py'))['verify'](root)
    sample=runpy.run_path(str(helpers/'verify-native-player-respawn.py'))['SAMPLE']
    trajectory=runpy.run_path(str(helpers/'verify-private-room-input.py'))['TRAJECTORY']
    reports=[]
    for process in result['processes']:
        full=Path(process['log']).read_text(encoding='utf-8',errors='replace')
        text='\n'.join(line for line in full.splitlines() if not line.startswith('[') or line[1:24]<result['close_started_local'])
        scope=next(c for c in base['captures'] if c['role']==process['role'])
        origin=scope['source_origin'];first=scope['first_delivery'];release=origin+1300+2
        peer=scope['peer']
        for elapsed,phase in ((1250,'held button across return'),(1265,'movement with held return button')):
            assert f"SDK input: peer {peer} tick {origin+elapsed} elapsed {elapsed} phase '{phase}'" in text
        retail={int(t):tuple(struct.unpack('>f',bytes.fromhex(v))[0] for v in pose.split('/')) for t,pose,*_ in sample.findall(text)}
        extra={int(row[1]):tuple(map(float,row[2:5])) for row in trajectory.findall(text) if row[0]=='3'}
        def pos(player,tick):return retail[tick][player*3:player*3+3] if player<2 else extra[tick]
        ticks=[int(t) for t in re.findall(r'Native Room owned Jump returned: peer \d+ tick (\d+) edge',text)]
        unwanted=[t for t in ticks if first<=t<=release+2]
        heights=[max(pos(p,t)[1] for t in range(first,release+3))-pos(p,first-1)[1] for p in range(3)]
        if unfenced:
            assert 'Private native cutscene return fence bypassed:' in text
            assert first in unwanted and all(h>1 for h in heights), 'Old behavior must reproduce unsolicited native Jump for all humans'
        else:
            assert 'Private native cutscene return fence bypassed:' not in text
            assert not unwanted and all(abs(h)<.35 for h in heights), 'A carried press or its release produced an unsolicited Jump'
            for player in range(3):
                a=pos(player,origin+1267);b=pos(player,origin+1273)
                assert math.hypot(b[0]-a[0],b[2]-a[2])>.03, 'Releasing a button must not gate movement axes'
        reports.append({'role':process['role'],'first_delivery':first,'carried_button_jump_ticks':unwanted,'held_interval_heights':heights})
    return {'passed':True,'unfenced_reproduction':unfenced,'binary_sha256':base['binary_sha256'],'matching_updates':base['matching_updates'],'worlds':reports,'scope':__doc__.strip()}


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('--expect-unfenced',action='store_true');args=p.parse_args()
    result=verify(args.root,args.expect_unfenced);(args.root/'native-cutscene-held-return-validation.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(result,indent=2))
