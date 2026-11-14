"""Require real extra-player death, dead-input gating and restored native physics.

One isolated level and extra peer; does not establish whole-party reload,
campaign completion, sixteen players, Internet play or state correction.
"""
import argparse
import json
import math
from pathlib import Path
import re
import runpy


def verify(root):
    result = json.loads((root/'result.json').read_text(encoding='utf-8-sig'))
    target = result.get('damage_extra_peer', 0)
    assert 3 <= target <= result['players'] and not result.get('damage_player')
    assert result['checkpoint_reload_tick'] == 700 and result['checkpoint_reload_count'] == 1
    assert not result.get('native_level_target') and not result.get('native_level_targets')
    assert result.get('extra_dead_input'), 'Dead-input controls were not requested'
    helpers = Path(__file__).parent
    menu = runpy.run_path(str(helpers/'verify-native-room-menu-roster.py'))['verify'](root, allow_scripted=True)
    input_tools = runpy.run_path(str(helpers/'verify-private-room-input.py'))
    input_tools['verify'](root)
    retail = runpy.run_path(str(helpers/'verify-native-player-respawn.py'))['SAMPLE']
    captures, histories, poses = [], [], []
    for process in result['processes']:
        text = Path(process['log']).read_text(encoding='utf-8', errors='replace')
        text = '\n'.join(line for line in text.splitlines() if not line.startswith('[') or line[1:24] < result['close_started_local'])
        assert 'Private extra damage target lost:' not in text
        assert 'Private Room actor trajectory unavailable:' not in text, 'Extra actor physics unavailable during native recovery'
        assert text.count('Native Room gameplay first frame consumed:') == 1
        assert 'Native Room loader agreed:' not in text and 'Room native world rebootstrap:' not in text
        actions = re.findall(r'Private extra damage dispatched: tick (\d+) peer (\d+) generation (\d+) handle ([0-9A-F]+) entity ([0-9A-F]+) health (\d+)->(\d+); original native Damage event, no forced reload or death interception', text)
        assert len(actions) == 1
        tick, peer, generation, handle, entity, before, after = actions[0]
        assert int(tick) == 700 and int(peer) == target and int(before) > 0 and int(after) == 0
        profiles = re.findall(r'Native extra respawn profile resolved: peer (\d+) generation (\d+) entity ([0-9A-F]+) profile (\d+); original checkpoint matrix and recovery tasks', text)
        assert len(profiles) == 1, 'Native extra-player respawn profile was not resolved exactly once'
        expected_profile = 1 if result.get('extra_oswald') or not target % 2 else 0
        assert profiles[0] == (str(0x524f4f4d00000000+target), generation, entity, str(expected_profile))
        placements = re.findall(r'Native extra respawn supported placement: peer (\d+) generation (\d+) position ([^/]+)/([^/]+)/([^;]+); authored checkpoint and native collision query', text)
        assert len(placements) == 1 and placements[0][:2] == profiles[0][:2], 'No unique supported respawn placement'
        assert 'Native extra respawn could not separate' not in text
        health = {}
        for tick, peer, gen, life in re.findall(r'Private extra damage health: tick (\d+) peer (\d+) generation (\d+) life (\d+); native property query', text):
            assert int(peer) == target and gen == generation
            tick, life = int(tick), int(life)
            assert tick not in health or health[tick] == life
            health[tick] = life
        origins = re.findall(r"Private Room SDK input: peer (\d+) tick (\d+) elapsed 0 phase '", text)
        assert len(origins) == 1
        local, origin = map(int, origins[0])
        end = origin+1200
        assert all(t in health for t in range(700, end+1))
        recovered = next(t for t in range(701, end+1) if health[t] > 0)
        assert all(health[t] == 0 for t in range(700, recovered))
        assert recovered < origin+1000 and all(health[t] > 0 for t in range(recovered, end+1))
        rows = {}
        for match in input_tools['TRAJECTORY'].findall(text):
            if int(match[0]) != target:
                continue
            row = (tuple(map(float, match[2:5])), tuple(map(float, match[5:8])), int(match[8]))
            assert all(math.isfinite(v) for values in row[:2] for v in values)
            assert match[9:] == ('true', 'true')
            at = int(match[1]); assert at not in rows or rows[at] == row
            rows[at] = row
        assert all(t in rows for t in range(700, end+1)), 'Recovery trajectory has gaps'
        baseline = rows[origin+721][0]
        for t in range(origin+722, origin+763):
            assert health[t] == 0, 'Dead-control test missed native death interval'
            assert math.dist(rows[t][0], baseline) < .02 and math.dist(rows[t][1], (0, 0, 0)) < .1
        if local == target:
            for elapsed, phase in ((720,'movement while dead'), (740,'jump while dead'), (1000,'forward after respawn'), (1040,'backward after respawn'), (1080,'jump after respawn')):
                assert re.search(rf"SDK input: peer {local} tick {origin+elapsed} elapsed {elapsed} phase '{phase}'", text)
        delta = lambda a,b: tuple(rows[origin+b][0][i]-rows[origin+a][0][i] for i in (0,2))
        forward, backward = delta(1001,1013), delta(1041,1053)
        assert math.hypot(*forward) > .03 and math.hypot(*backward) > .03
        assert sum(a*b for a,b in zip(forward,backward)) < 0
        floor = rows[origin+1079][0][1]
        jump = [rows[t] for t in range(origin+1082, origin+1163)]
        height = max(row[0][1] for row in jump)-floor
        assert height > 1 and max(row[1][1] for row in jump) > 5
        assert abs(rows[end][0][1]-floor) < .4 and rows[end][2] == 0
        assert math.hypot(rows[end][1][0],rows[end][1][2]) < .1
        samples = [row for row in retail.findall(text) if 700 <= int(row[0]) <= end]
        assert len(samples) >= end-699 and len({row[1] for row in samples}) == 1
        assert all(tuple(map(int,row[2:])) == (5,5,5,5) for row in samples)
        histories.append({t:health[t] for t in range(700,end+1)})
        poses.append({t:rows[t] for t in range(700,end+1)})
        captures.append({'role':process['role'], 'target_entity':entity, 'target_handle':handle, 'generation':int(generation), 'recovered_tick':recovered, 'jump_height':height})
    assert all(h == histories[0] for h in histories), 'Native health differs across worlds'
    assert all(p == poses[0] for p in poses), 'Native recovery physics differs across worlds'
    return {'passed':True,'binary_sha256':menu['binary_sha256'],'damaged_extra_peer':target,'extra_oswald':result.get('extra_oswald',False),'captures':captures,'scope':__doc__.strip()}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('root',type=Path)
    root = parser.parse_args().root
    result = verify(root)
    (root/'native-extra-respawn-validation.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,indent=2))
