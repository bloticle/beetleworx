"""Require native player/camera snapshots alongside gameplay/chat/UDP voice.

Checks the actual native producer in isolated game processes. Audio hardware,
audible proximity and Internet multiplayer remain separate acceptance work.
"""
import argparse
import json
import math
from pathlib import Path
import re
import runpy

SCENE = re.compile(r'Native Room voice scene: local peer (\d+) generation (\d+) mask ([0-9A-F]{4}) listener ([^/\s]+)/([^/\s]+)/([^/\s]+) right ([^/\s]+)/([^/\s]+)/([^;\s]+); copied native actor/camera values, no audio-thread guest reads')


def verify(root):
    accepted = runpy.run_path(str(Path(__file__).with_name('verify-native-room-voice.py')))['verify'](root)
    assert accepted['passed'] and accepted['media_passed']
    record = json.loads((root / 'result.json').read_text(encoding='utf-8-sig'))
    peers = {row['role']: row['local_peer'] for row in accepted['captures']}
    full = (1 << record['players'])-1
    captures = []
    for process in record['processes']:
        rows = SCENE.findall(Path(process['log']).read_text(encoding='utf-8', errors='replace'))
        assert rows, 'Native voice scene producer never published a valid snapshot'
        complete = False
        for peer, generation, mask, *values in rows:
            peer, generation, mask = int(peer), int(generation), int(mask, 16)
            assert peer == peers[process['role']] and generation > 0
            assert mask & (1 << (peer-1)) and not mask & ~full
            values = list(map(float, values))
            assert all(map(math.isfinite, values))
            assert .9 <= sum(value*value for value in values[3:]) <= 1.1
            complete |= mask == full
        assert complete, 'Native producer never captured every admitted player'
        captures.append({'role': process['role'], 'local_peer': peers[process['role']], 'scene_publications': len(rows), 'full_roster_observed': complete})
    return {'passed': True, 'binary_sha256': record['exe_sha256'], 'captures': captures,
            'native_gameplay_and_media': accepted, 'scope': __doc__.strip()}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    root = parser.parse_args().root
    result = verify(root)
    (root / 'native-voice-scene-validation.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({'passed': result['passed'], 'binary_sha256': result['binary_sha256'], 'captures': result['captures']}, indent=2))
