"""Verify real three-human GameOver, native checkpoint loading and restored controls.

This fixture does not establish all campaign checkpoints, Internet play,
sixteen native clients, arbitrary cutscenes or rollback/state correction.
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
    assert result['players']==3 and result['damage_player']==4 and result['damage_extra_peer']==3
    assert result['checkpoint_reload_tick']==700 and result['checkpoint_reload_count']==1
    assert result['startup_complete'] and result['observation_complete'] and not result.get('error')
    assert not result.get('native_level_target') and not result.get('native_level_targets')
    assert result['scripted_sdk_input'] and result['gameplay_input_sent'] and not result['screenshots_taken']
    assert all(result[k] for k in ('normal_user_files_unchanged','normal_config_unchanged','normal_crash_unchanged'))
    helpers=Path(__file__).parent
    sample=runpy.run_path(str(helpers/'verify-native-player-respawn.py'))['SAMPLE']
    trajectory=runpy.run_path(str(helpers/'verify-private-room-input.py'))['TRAJECTORY']
    obs=runpy.run_path(str(helpers/'verify-native-room-menu-roster.py'))['OBS']
    peers=set(); histories=[]; reports=[]
    for process in result['processes']:
        assert process['process_exited'] and process['exit_code']==0 and not process.get('forced_close') and not process.get('slow_shutdown')
        assert process['scripted_sdk_program_completed'] and process['scripted_sdk_input_observed']
        full=Path(process['log']).read_text(encoding='utf-8',errors='replace')
        text='\n'.join(line for line in full.splitlines() if not line.startswith('[') or line[1:24]<result['close_started_local'])
        assert not re.search(r'Native Room gameplay stopped:|Retained native Room stopped:|Native lobby QA rejected:|Native Room actor admission failed:',text)
        assert 'Native lobby completed authored save reload and accepted native Continue' in text
        assert text.count('Native Room terminal GameOver admitted: entry tick 700, participants 3;')==1
        assert text.count('Private whole-party native decision: tick 700 state 7 native dead 3 extra life 0;')==1
        assert text.count('Native lobby QA pair damage health: before 5/5, after 0/0;')==1
        assert text.count('Private extra damage dispatched: tick 700 peer 3 generation 1 ')==1
        assert text.count('Private whole-party damage target retired: original load 2 current load 3;')==1
        counts=re.findall(r'Native online death participant count: tick 700 participants 3 native dead ([123]) caller 82B2C8A0;',text)
        assert counts==['1','2','3']
        loads=re.findall(r"Native Room loader agreed: tick (\d+), sequence (\d+), phase (\d+), destination '([^']+)'",text)
        assert len(loads)==2 and loads[0][1:3]==('0','0') and loads[1][1:3]==('1','1')
        assert loads[0][0]==loads[1][0] and loads[0][3]==loads[1][3]
        load_tick=int(loads[0][0]);assert load_tick>700
        assert text.count(f'Room native world rebootstrap: cancelled unverified tick {load_tick}, raw first {load_tick+1}, load sequence 1')==1
        origins=[tuple(map(int,r)) for r in re.findall(r"Private Room SDK input: peer (\d+) tick (\d+) elapsed 0 phase '",text)]
        assert len(origins)==2 and origins[0][0]==origins[1][0]
        peer,origin=origins[1];assert peer not in peers;peers.add(peer)
        assert origin==load_tick+2
        assert len(re.findall(r'Native Room gameplay first frame consumed:',text))==2
        assert len(re.findall(r'Native Room extra humans admitted: count 1 mask 0004;',text))==2
        assert f"SDK input: peer {peer} tick {origin+1200} elapsed 1200 phase 'complete'" in text
        for elapsed,phase in ((1000,'forward after respawn'),(1040,'backward after respawn'),(1080,'jump after respawn')):
            assert f"SDK input: peer {peer} tick {origin+elapsed} elapsed {elapsed} phase '{phase}'" in text
        terminal=[int(t) for t in re.findall(r'Native Room GameOver native update completed: tick (\d+);',text)]
        assert terminal and terminal[0]==701 and all(700<t<load_tick for t in terminal)
        retail={}
        for row in sample.findall(text):
            pose=tuple(struct.unpack('>f',bytes.fromhex(v))[0] for v in row[1].split('/'))
            assert len(pose)==6 and all(math.isfinite(v) for v in pose)
            retail[int(row[0])]=(pose,tuple(map(int,row[2:])))
        extra={}
        for row in trajectory.findall(text):
            if row[0]!='3':continue
            at=int(row[1]); value=tuple(map(float,row[2:8]))+(int(row[8]),row[9],row[10])
            assert all(math.isfinite(v) for v in value[:7])
            if origin<=at<=origin+1200: assert value[-2:]==('true','true')
            extra[at]=value
        heights=[]
        for player in range(3):
            def position(at):
                tick=origin+at
                return retail[tick][0][player*3:player*3+3] if player<2 else extra[tick][:3]
            def delta(a,b):
                x,y=position(a),position(b);return tuple(y[i]-x[i] for i in (0,2))
            # The six source ticks arrive two ticks later. Measure at the
            # final delivered command, before slope/contact settling can
            # move the actor back after release.
            f,b=delta(1001,1007),delta(1041,1047)
            assert math.hypot(*f)>.03 and math.hypot(*b)>.03 and sum(x*y for x,y in zip(f,b))<0
            floor=position(1079)[1]
            height=max(position(t)[1] for t in range(1082,1161))-floor
            assert height>1 and abs(position(1190)[1]-floor)<.4
            heights.append(height)
        assert retail[699][1]==(5,5,5,5) and retail[700][1]==(0,5,0,5)
        assert all(retail[t][1]==(5,5,5,5) for t in range(origin,origin+1201))
        agreed={int(t):digest for t,digest in obs.findall(text)}
        assert all(t in agreed for t in range(origin,origin+1201))
        histories.append({t:(agreed[t],retail[t],extra[t]) for t in range(origin,origin+1201)})
        phases=['requesting cooperative guest exit','guest threads exited before module teardown','releasing stopped audio clients','guest workers stopped']
        times=[]
        for phase in phases:
            rows=re.findall(r'^\[([^\]]+)\][^\r\n]*?\[t(\d+)\] Title shutdown: '+re.escape(phase)+r'\s*$',full,re.M)
            assert len(rows)==1;times.append((datetime.fromisoformat(rows[0][0]),rows[0][1]))
        assert len({r[1] for r in times})==1 and all(a[0]<=b[0] for a,b in zip(times,times[1:]))
        assert full.count('Execution complete')==1 and 'Beetleworx update shutdown complete:' in full
        reports.append({'role':process['role'],'peer':peer,'native_reload_tick':load_tick,'fresh_input_origin':origin,'jump_heights':heights})
    assert peers=={1,2,3} and len(histories)==3 and all(h==histories[0] for h in histories)
    return {'passed':True,'binary_sha256':result['exe_sha256'],'matching_post_reload_updates':len(histories[0]),'captures':reports,'scope':__doc__.strip()}


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);root=p.parse_args().root
    result=verify(root);(root/'native-party-gameover-validation.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(result,indent=2))
