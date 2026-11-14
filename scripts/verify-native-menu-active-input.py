"""Verify active native-menu Room movement, jump, camera and action dispatch.

Combines native-menu/save/view/normal-shutdown acceptance with the existing
independent real actor trajectories and native button callback checks.
Not a claim of paint target effects, sixteen clients or campaign completion.
"""
import argparse
import json
from pathlib import Path
import runpy

def verify(root):
    scripts = Path(__file__).parent
    menu = runpy.run_path(str(scripts / 'verify-native-room-menu-roster.py'))['verify'](root, allow_scripted=True)
    active = runpy.run_path(str(scripts / 'verify-private-room-multi-actors.py'))['verify'](root)
    if menu['binary_sha256'] != active['binary_sha256']:
        raise AssertionError('Native menu and active gameplay evidence use different executables')
    return {'passed': True, 'players': menu['players'], 'binary_sha256': menu['binary_sha256'], 'menu': menu, 'active': active, 'scope': __doc__.strip()}

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    root = parser.parse_args().root
    result = verify(root)
    (root / 'native-menu-active-input-validation.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'passed': result['passed'], 'players': result['players'], 'binary_sha256': result['binary_sha256'], 'active_captures': result['active']['captures']}, indent=2))
