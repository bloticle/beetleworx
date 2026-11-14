"""Verify native fatal damage, in-place recovery and restored controls.

Requires actual original Damage/GetHealth calls and continued matching worlds.
Does not prove extra-human death, whole-party checkpoint reload, all campaign
situations, sixteen clients, Internet conditions, or rollback/state correction.
"""
import argparse
import json
import math
from pathlib import Path
import re
import runpy
import struct

SAMPLE = re.compile(r'Native gameplay observation v5:[^\r\n]*?tick (\d+),[^\r\n]*?bits ([0-9A-Fa-f/]+), life (\d+)/(\d+)/(\d+)/(\d+),')


def verify(root):
    result = json.loads((root / 'result.json').read_text(encoding='utf-8-sig'))
    target = result.get('damage_player')
    assert target in (1, 2) and result['checkpoint_reload_tick'] == 700
    assert result.get('checkpoint_reload_count', 1) == 1
    assert not result.get('native_level_target') and not result.get('native_level_targets')
    menu = runpy.run_path(str(Path(__file__).with_name('verify-native-room-menu-roster.py')))['verify'](root, allow_scripted=True)
    active = runpy.run_path(str(Path(__file__).with_name('verify-private-room-input.py')))['verify'](root)
    assert active['binary_sha256'] == menu['binary_sha256']
    captures = []
    transition = None
    for process in result['processes']:
        text = Path(process['log']).read_text(encoding='utf-8', errors='replace')
        text = '\n'.join(line for line in text.splitlines() if not line.startswith('[') or line[1:24] < result['close_started_local'])
        origins = re.findall(r"Private Room SDK input: peer (\d+) tick (\d+) elapsed 0 phase '", text)
        assert len(origins) == 1
        peer, origin = map(int, origins[0])
        assert text.count('Native Room gameplay first frame consumed:') == 1
        assert 'Native Room loader agreed:' not in text and 'Room native world rebootstrap:' not in text
        assert text.count('Native lobby QA reload method: original-damage') == 1
        assert text.count(f'Native lobby QA damage requested: tick 700, retail player {target}, amount 1000; original Damage Lua export, no forced reload') == 1
        assert text.count('Native lobby QA restart requested: tick 700, attempted true, status 0, stack true') == 1
        health = re.findall(r'Native lobby QA damage health: player (\d+), before ([^,]+), after ([^;]+); native GetHealth', text)
        assert len(health) == 1 and int(health[0][0]) == target and float(health[0][1]) > 0 and float(health[0][2]) == 0
        assert len(re.findall(r"elapsed 1200 phase 'complete' reads", text)) == 1
        samples = {}
        for row in SAMPLE.findall(text):
            tick = int(row[0]); bits = row[1].split('/')
            assert len(bits) == 6
            pose = tuple(struct.unpack('>f', struct.pack('>I', int(value, 16)))[0] for value in bits)
            assert all(map(math.isfinite, pose))
            value = (pose, tuple(map(int, row[2:6])))
            assert tick not in samples or samples[tick] == value
            samples[tick] = value
        index = 2 * (target-1)
        assert samples[699][1][index] > 0 and samples[700][1][index] == 0
        dead = [tick for tick in sorted(samples) if tick >= 700 and samples[tick][1][index] == 0]
        restored = [tick for tick in sorted(samples) if tick > 700 and samples[tick][1][index] > 0]
        assert dead and restored and max(dead) < min(restored)
        recovered = min(restored)
        assert recovered < origin+1000, 'Recovery must precede fresh SDK controls'
        signature = (max(dead), recovered, samples[recovered][1])
        assert transition is None or transition == signature, 'Peers recovered at different native updates'
        transition = signature
        # All peers simulate the damaged retail actor; only its own process
        # supplies these new physical-source SDK inputs.
        if peer == target:
            for elapsed, phase in ((1000, 'forward after respawn'), (1040, 'backward after respawn'), (1080, 'jump after respawn')):
                assert re.search(rf"SDK input: peer {peer} tick {origin+elapsed} elapsed {elapsed} phase '{phase}'", text)
        def position(elapsed):
            return samples[origin+elapsed][0][3*(target-1):3*target]
        deltas = []
        for begin, end in ((1001, 1013), (1041, 1053)):
            a, b = position(begin), position(end)
            deltas.append((b[0]-a[0], b[2]-a[2]))
        assert all(math.hypot(*delta) > .03 for delta in deltas), 'Restored character did not move'
        assert sum(a*b for a,b in zip(*deltas)) < 0, 'Restored character did not reverse'
        base_y = position(1079)[1]
        height = max(position(at)[1] for at in range(1082, 1163)) - base_y
        assert height > 1, 'Restored character did not jump'
        assert abs(position(1200)[1] - base_y) < .4, 'Restored character did not land near its starting floor'
        captures.append({'role': process['role'], 'local_peer': peer, 'last_dead_tick': max(dead), 'recovered_tick': recovered, 'restored_life': samples[recovered][1][index], 'post_recovery_jump_height': height})
    return {'passed': True, 'players': result['players'], 'damaged_retail_player': target, 'binary_sha256': menu['binary_sha256'], 'matching_updates': menu['longest_matching_updates'], 'captures': captures, 'scope': __doc__.strip()}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    root = parser.parse_args().root
    result = verify(root)
    (root / 'native-player-respawn-validation.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(result, indent=2))
