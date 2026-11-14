"""Verify original Bink movie entry, synchronized exit and restored controls.

Three native clients play the retail Havok logo through StartSequence. This
does not prove other movies, skipping, credits, Internet stalls or a campaign.
"""
import argparse
import json
import math
from pathlib import Path
import re
import runpy
import struct


def verify(root):
    result = json.loads((root / 'result.json').read_text(encoding='utf-8-sig'))
    assert result['movie_sequence'] and result['players'] == 3
    assert result['checkpoint_reload_tick'] == 300 and result['checkpoint_reload_count'] == 1
    assert not result['cutscene_cycle'] and not result['authored_conversation']
    helpers = Path(__file__).parent
    menu = runpy.run_path(str(helpers / 'verify-native-room-menu-roster.py'))['verify'](root, allow_scripted=True)
    sample = runpy.run_path(str(helpers / 'verify-native-player-respawn.py'))['SAMPLE']
    trajectory = runpy.run_path(str(helpers / 'verify-private-room-input.py'))['TRAJECTORY']
    obs = runpy.run_path(str(helpers / 'verify-native-room-menu-roster.py'))['OBS']
    histories = []
    reports = []
    returns = []
    for process in result['processes']:
        full = Path(process['log']).read_text(encoding='utf-8', errors='replace')
        text = '\n'.join(line for line in full.splitlines() if not line.startswith('[') or line[1:24] < result['close_started_local'])
        assert text.count('Private native movie sequence: tick 300 LOGO_havok.bik attempted true status 0 stack true;') == 1
        assert text.count('Native Room movie entry interrupted update: tick 300 actual retail Motion mask 00 extra mask 0000;') == 1
        assert text.count('Native Room cutscene input suspended: entry tick 300, input epoch 2; original state 2') == 1
        assert len(re.findall(r'Native Room movie sequence target retained: tick 301 object [0-9A-F]+;', text)) == 1
        assert len(re.findall(r'Native Room movie playback clock admitted: tick 301 Bink [0-9A-F]+;', text)) == 1
        assert 'Native Room movie camera retained: tick 360;' in text
        assert 'Native Room movie camera retained: tick 420;' in text
        assert 'Native Room cutscene UI callback delivered:' not in text
        assert 'Native Room loader agreed:' not in text and 'Room native world rebootstrap:' not in text
        resumed = re.findall(r'Native Room cutscene bindings re-admitted: source tick (\d+), first delivery (\d+), input epoch 3;', text)
        assert len(resumed) == 1
        source, delivery = map(int, resumed[0])
        assert 430 <= source <= 480 and delivery == source + 2
        returns.append((source, delivery))
        origins = re.findall(r"Private Room SDK input: peer (\d+) tick (\d+) elapsed 0 phase '", text)
        assert len(origins) == 1
        peer, origin = map(int, origins[0])
        assert f"SDK input: peer {peer} tick {origin+1600} elapsed 1600 phase 'complete'" in text
        retail = {}
        for row in sample.findall(text):
            pose = tuple(struct.unpack('>f', bytes.fromhex(v))[0] for v in row[1].split('/'))
            assert len(pose) == 6 and all(math.isfinite(v) for v in pose)
            retail[int(row[0])] = (pose, tuple(map(int, row[2:])))
        extra = {}
        for row in trajectory.findall(text):
            if row[0] != '3':
                continue
            value = tuple(map(float, row[2:8])) + (int(row[8]), row[9], row[10])
            assert all(math.isfinite(v) for v in value[:7])
            extra[int(row[1])] = value
        def position(player, at):
            tick = origin + at
            return retail[tick][0][player*3:player*3+3] if player < 2 else extra[tick][:3]
        heights = []
        for player in range(3):
            def delta(a, b):
                x, y = position(player, a), position(player, b)
                return tuple(y[i]-x[i] for i in (0, 2))
            forward, backward = delta(1401, 1407), delta(1441, 1447)
            assert math.hypot(*forward) > .03 and math.hypot(*backward) > .03
            assert sum(x*y for x, y in zip(forward, backward)) < 0
            floor = position(player, 1479)[1]
            height = max(position(player, t)[1] for t in range(1482, 1561)) - floor
            assert height > 1 and abs(position(player, 1590)[1] - floor) < .4
            heights.append(height)
        jumps = [int(t) for t in re.findall(r'Native Room owned Jump returned: peer \d+ tick (\d+) edge', text)]
        assert origin+1482 in jumps and origin+1486 in jumps
        assert not any(301 <= t < delivery for t in jumps)
        agreed = {int(t): digest for t, digest in obs.findall(text)}
        assert all(t in agreed and t in retail and t in extra for t in range(origin, origin+1601))
        assert all(retail[t][1] == (5, 5, 5, 5) for t in range(origin, origin+1601))
        histories.append({t: (agreed[t], retail[t], extra[t]) for t in range(origin, origin+1601)})
        reports.append({'role': process['role'], 'peer': peer, 'source_origin': origin,
                        'source_return': source, 'first_delivery': delivery, 'jump_heights': heights})
    assert len(set(returns)) == 1, 'Movie exit differs between native clients'
    assert all(h == histories[0] for h in histories), 'Actual world or actor trajectories differ'
    assert all(result[k] for k in ('normal_user_files_unchanged', 'normal_config_unchanged', 'normal_crash_unchanged'))
    return {'passed': True, 'binary_sha256': menu['binary_sha256'], 'matching_updates': len(histories[0]),
            'shared_return': returns[0], 'captures': reports, 'scope': __doc__.strip()}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    root = parser.parse_args().root
    report = verify(root)
    (root / 'native-movie-sequence-validation.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))
