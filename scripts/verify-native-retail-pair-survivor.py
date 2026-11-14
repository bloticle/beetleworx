"""Require a real extra survivor to keep a retained native world playable.

Both retail humans receive original fatal Damage; the extra human receives
none. Health alone and reaching CutScene are insufficient for acceptance.
"""
import argparse
import json
import math
from pathlib import Path
import re
import runpy


def verify(root):
    result = json.loads((root/'result.json').read_text(encoding='utf-8-sig'))
    assert result.get('damage_player') == 3 and result['players'] == 3
    assert result['checkpoint_reload_tick'] == 700 and result['checkpoint_reload_count'] == 1
    assert not result.get('damage_extra_peer') and not result.get('native_level_target') and not result.get('native_level_targets')
    helpers = Path(__file__).parent
    menu = runpy.run_path(str(helpers/'verify-native-room-menu-roster.py'))['verify'](root,allow_scripted=True)
    tools = runpy.run_path(str(helpers/'verify-private-room-input.py'))
    tools['verify'](root)
    sample = runpy.run_path(str(helpers/'verify-native-player-respawn.py'))['SAMPLE']
    histories, poses, captures = [], [], []
    for process in result['processes']:
        text = Path(process['log']).read_text(encoding='utf-8',errors='replace')
        text = '\n'.join(line for line in text.splitlines() if not line.startswith('[') or line[1:24] < result['close_started_local'])
        assert text.count('Native lobby QA pair damage health: before 5/5, after 0/0; original Damage and GetHealth, extra humans untouched') == 1
        assert text.count('Native Room gameplay first frame consumed:') == 1
        assert 'Native Room loader agreed:' not in text and 'Room native world rebootstrap:' not in text
        assert 'Private retail-pair survivor health unavailable:' not in text
        assert 'Private Room actor trajectory unavailable:' not in text
        profile = 1 if result.get('extra_oswald') else 0
        assert re.search(rf'Native Room owned look admitted: peer 3 mode [0-9A-F]+ action [0-9A-F]+ profile {profile}; original action math and original OnLook',text)
        assert re.search(rf'Native Room owned button callback: tick \d+ peer 3 profile {profile} action ',text)
        counts = [tuple(int(v,16) if i==3 else int(v) for i,v in enumerate(row)) for row in re.findall(r'Native online death participant count: tick (\d+) participants (\d+) native dead (\d+) caller ([0-9A-F]+); actual admitted humans, original death state',text)]
        assert len(counts) == 4 and all(row[1] == 3 and 1 <= row[2] <= 2 for row in counts)
        assert sorted((t,d) for t,n,d,c in counts if c==0x82B2C8A0) == [(700,1),(700,2)]
        assert len([row for row in counts if row[3]==0x82B2C494 and row[0]>700]) == 2
        assert not re.search(r'Native Room owned camera input context: peer 3 fixed true;',text), 'Party death still restricted the living survivor camera'
        origins = re.findall(r"Private Room SDK input: peer (\d+) tick (\d+) elapsed 0 phase '",text)
        assert len(origins) == 1
        peer, origin = map(int,origins[0]); end = origin+1200
        health = {}
        identity = set()
        for at, generation, entity, life in re.findall(r'Private retail-pair survivor health: tick (\d+) peer 3 generation (\d+) entity ([0-9A-F]+) life (\d+); original native property query, no damage dispatched to survivor',text):
            tick, life = int(at), int(life)
            assert tick not in health or health[tick] == life
            health[tick] = life; identity.add((generation,entity))
        assert len(identity) == 1 and all(health.get(t) == 5 for t in range(700,end+1))
        retail = {int(row[0]):tuple(map(int,row[2:])) for row in sample.findall(text)}
        assert retail[699] == (5,5,5,5) and retail[700] == (0,5,0,5)
        recovered = next(t for t in range(701,end+1) if retail[t][0] > 0 and retail[t][2] > 0)
        assert recovered < origin+1000
        assert all(retail[t][0] == retail[t][2] == 0 for t in range(700,recovered))
        rows = {}
        for match in tools['TRAJECTORY'].findall(text):
            if match[0] != '3': continue
            row = tuple(map(float,match[2:8]))+(int(match[8]),)
            assert all(math.isfinite(v) for v in row)
            assert match[9:] == ('true','true')
            tick = int(match[1]); assert tick not in rows or rows[tick] == row
            rows[tick] = row
        assert all(t in rows for t in range(700,end+1))
        # A valid final session must regain controls; a globally blocked death
        # cinematic is not proof that the surviving human can keep playing.
        if peer == 3:
            for elapsed, phase in ((720,'survivor forward'),(760,'survivor backward'),(800,'survivor jump')):
                assert re.search(rf"SDK input: peer 3 tick {origin+elapsed} elapsed {elapsed} phase '{phase}'",text)
        def delta(a,b): return tuple(rows[origin+b][i]-rows[origin+a][i] for i in (0,2))
        forward, backward = delta(721,733), delta(761,773)
        assert math.hypot(*forward) > .03 and math.hypot(*backward) > .03
        assert sum(a*b for a,b in zip(forward,backward)) < 0
        floor = rows[origin+799][1]
        jump = [rows[t] for t in range(origin+802,origin+883)]
        height = max(r[1] for r in jump)-floor
        assert height > 1 and max(r[4] for r in jump) > 5
        assert rows[origin+940][6] == 0 and abs(rows[origin+940][1]-floor) < .4
        histories.append({t:(health[t],retail[t]) for t in range(700,end+1)})
        poses.append({t:rows[t] for t in range(700,end+1)})
        captures.append({'role':process['role'],'survivor_profile':profile,'retail_recovery_tick':recovered,'survivor_jump_height':height})
    assert all(h == histories[0] for h in histories) and all(p == poses[0] for p in poses)
    return {'passed':True,'binary_sha256':menu['binary_sha256'],'extra_oswald':bool(result.get('extra_oswald')),'captures':captures,'scope':__doc__.strip()}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__);parser.add_argument('root',type=Path)
    root = parser.parse_args().root;result = verify(root)
    (root/'native-retail-pair-survivor-validation.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,indent=2))
