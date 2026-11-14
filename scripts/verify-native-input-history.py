"""Compare verified consumed-input history across actual native game worlds.

Composes native movement/action/chat/voice/scene validation. Matching input
history is a replay prerequisite; this does not prove native restore or rollback.
"""
import argparse
import json
from pathlib import Path
import re
import runpy

HISTORY = re.compile(r'Native Room verified input history: local peer (\d+) first (\d+) last (\d+) frames 224 mask ([0-9A-F]{4}) sha256 ([0-9a-f]{64}); exact consumed/completed/verified commands, no native replay')


def verify(root):
    native = runpy.run_path(str(Path(__file__).with_name('verify-native-voice-scene.py')))['verify'](root)
    assert native['passed']
    record = json.loads((root / 'result.json').read_text(encoding='utf-8-sig'))
    peers = {row['role']: row['local_peer'] for row in native['captures']}
    same = None
    captures = []
    for process in record['processes']:
        rows = HISTORY.findall(Path(process['log']).read_text(encoding='utf-8', errors='replace'))
        assert len(rows) == 1, 'Expected one native history check per owned game'
        peer, first, last, mask, digest = rows[0]
        peer, first, last, mask = int(peer), int(first), int(last), int(mask, 16)
        assert peer == peers[process['role']] and last-first+1 == 224 and mask == (1 << record['players'])-1
        current = first, last, mask, digest
        assert same is None or current == same, 'Native worlds retained different commands, scope or completed proofs'
        same = current
        captures.append({'role': process['role'], 'local_peer': peer, 'first': first, 'last': last, 'frames': 224, 'digest': digest})
    return {'passed': True, 'binary_sha256': record['exe_sha256'], 'captures': captures,
            'native_gameplay_media_scene': native, 'scope': __doc__.strip()}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    root = parser.parse_args().root
    result = verify(root)
    (root / 'native-input-history-validation.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({'passed': result['passed'], 'binary_sha256': result['binary_sha256'], 'captures': result['captures']}, indent=2))
