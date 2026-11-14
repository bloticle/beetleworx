"""Verify original native cutscene restriction, input-epoch return and real controls.

Requires three actual native worlds without a reload or actor replacement.
Not a full campaign, every cutscene/held-button combination, Internet or
sixteen-native-client gameplay assertion.
"""
import argparse
from datetime import datetime
import json
import math
from pathlib import Path
import re
import runpy
import struct


def verify(root):
    result=json.loads((root/'result.json').read_text(encoding='utf-8-sig'))
    assert result['players']==3 and result['cutscene_cycle'] and not result['damage_player'] and not result['damage_extra_peer']
    assert result['checkpoint_reload_tick']==700 and result['checkpoint_reload_count']==2 and result['checkpoint_reload_interval']==600
    helpers=Path(__file__).parent
    menu=runpy.run_path(str(helpers/'verify-native-room-menu-roster.py'))['verify'](root,allow_scripted=True)
    sample=runpy.run_path(str(helpers/'verify-native-player-respawn.py'))['SAMPLE']
    trajectory=runpy.run_path(str(helpers/'verify-private-room-input.py'))['TRAJECTORY']
    obs=runpy.run_path(str(helpers/'verify-native-room-menu-roster.py'))['OBS']
    histories=[];peers=set();reports=[]
    for process in result['processes']:
        full=Path(process['log']).read_text(encoding='utf-8',errors='replace')
        text='\n'.join(line for line in full.splitlines() if not line.startswith('[') or line[1:24]<result['close_started_local'])
        assert 'Native Room loader agreed:' not in text and 'Room native world rebootstrap:' not in text
        assert 'Private extra damage dispatched:' not in text and 'Native lobby QA damage requested:' not in text
        assert text.count('Private native cutscene cycle: tick 700 phase restricted attempted true status 0 stack true;')==1
        assert text.count('Private native cutscene cycle: tick 1300 phase released attempted true status 0 stack true;')==1
        assert text.count('Native Room cutscene input suspended: entry tick 700, input epoch 2;')==1
        returns=re.findall(r'Native Room cutscene bindings re-admitted: source tick (\d+), first delivery (\d+), input epoch 3;',text)
        assert len(returns)==1
        source_return,first_delivery=map(int,returns[0]);assert 1301<=source_return<=1420 and first_delivery==source_return+2
        origins=re.findall(r"Private Room SDK input: peer (\d+) tick (\d+) elapsed 0 phase '",text)
        assert len(origins)==1;peer,origin=map(int,origins[0]);assert peer not in peers;peers.add(peer)
        for elapsed,phase in ((200,'jump before cutscene'),(660,'move and jump across cutscene' if result.get('cutscene_boundary_movement') else 'held jump across cutscene'),(1400,'forward after cutscene'),(1440,'backward after cutscene'),(1480,'jump after cutscene'),(1600,'complete')):
            assert f"SDK input: peer {peer} tick {origin+elapsed} elapsed {elapsed} phase '{phase}'" in text
        retail={}
        for row in sample.findall(text):
            pose=tuple(struct.unpack('>f',bytes.fromhex(v))[0] for v in row[1].split('/'))
            assert len(pose)==6 and all(math.isfinite(v) for v in pose)
            retail[int(row[0])]=(pose,tuple(map(int,row[2:])))
        extra={}
        for row in trajectory.findall(text):
            if row[0]!='3':continue
            tick=int(row[1]);value=tuple(map(float,row[2:8]))+(int(row[8]),row[9],row[10])
            assert all(math.isfinite(v) for v in value[:7])
            assert value[-1]==('false' if 701<=tick<first_delivery else 'true')
            extra[tick]=value
        # The Press queued by source tick origin+660 belongs to the retired
        # context; it must never reach original character callbacks in pause.
        jump_ticks=[int(v) for v in re.findall(r'Native Room owned Jump returned: peer \d+ tick (\d+) edge',text)]
        assert origin+202 in jump_ticks and origin+206 in jump_ticks
        assert not any(701<=t<first_delivery for t in jump_ticks)
        assert origin+1482 in jump_ticks and origin+1486 in jump_ticks
        heights=[]
        if result.get('cutscene_boundary_movement'):
            assert f"SDK input: peer {peer} tick {origin+657} elapsed 657 phase 'movement across cutscene'" in text
            for player in range(3):
                def frozen_position(t):return retail[t][0][player*3:player*3+3] if player<2 else extra[t][:3]
                start=frozen_position(760)
                assert max(math.hypot(frozen_position(t)[0]-start[0],frozen_position(t)[2]-start[2]) for t in range(760,1301))<.25, 'Actor kept walking during native restriction'
        for player in range(3):
            def position(at):
                tick=origin+at
                return retail[tick][0][player*3:player*3+3] if player<2 else extra[tick][:3]
            def delta(a,b):
                x,y=position(a),position(b);return tuple(y[i]-x[i] for i in (0,2))
            f,b=delta(1401,1407),delta(1441,1447)
            assert math.hypot(*f)>.03 and math.hypot(*b)>.03 and sum(x*y for x,y in zip(f,b))<0
            floor=position(1479)[1];height=max(position(t)[1] for t in range(1482,1561))-floor
            assert height>1 and abs(position(1590)[1]-floor)<.4;heights.append(height)
        agreed={int(t):digest for t,digest in obs.findall(text)}
        end=origin+1600
        assert all(t in agreed and t in retail and t in extra for t in range(origin,end+1))
        assert all(retail[t][1]==(5,5,5,5) for t in range(origin,end+1))
        histories.append({t:(agreed[t],retail[t],extra[t]) for t in range(origin,end+1)})
        phases=['requesting cooperative guest exit','guest threads exited before module teardown','releasing stopped audio clients','guest workers stopped'];times=[]
        for phase in phases:
            rows=re.findall(r'^\[([^\]]+)\][^\r\n]*?\[t(\d+)\] Title shutdown: '+re.escape(phase)+r'\s*$',full,re.M)
            assert len(rows)==1;times.append((datetime.fromisoformat(rows[0][0]),rows[0][1]))
        assert len({v[1] for v in times})==1 and all(a[0]<=b[0] for a,b in zip(times,times[1:]))
        assert full.count('Execution complete')==1 and 'Beetleworx update shutdown complete:' in full
        reports.append({'role':process['role'],'peer':peer,'source_origin':origin,'source_return':source_return,'first_delivery':first_delivery,'jump_heights':heights})
    assert peers=={1,2,3} and len(histories)==3 and all(h==histories[0] for h in histories)
    return {'passed':True,'binary_sha256':menu['binary_sha256'],'matching_updates':len(histories[0]),'boundary_movement':bool(result.get('cutscene_boundary_movement')),'captures':reports,'scope':__doc__.strip()}


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);root=p.parse_args().root
    result=verify(root);(root/'native-cutscene-return-validation.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(result,indent=2))
