"""Check larger native-menu Room startup in real game processes.

Neutral gameplay capture: does not prove active inputs, sixteen clients,
visual menu layout, Internet services, transitions or campaign completion.
"""
import argparse
import json
from pathlib import Path
import re

OBS = re.compile(r'Native gameplay observation v5:[^\r\n]*?tick (\d+),[^\r\n]*?digest ([0-9a-f]{64}), agreed true(?=[,\s]|$)')
LOCAL = re.compile(r'Native Room menu QA appearance accepted: peer (\d+) color (\d+);')
CAMERA = re.compile(r'Native Room independent camera: room peer (\d+) controller ([0-9A-F]+) output ([0-9A-F]+) tracker ([0-9A-F]+) own target ([0-9A-F]+) node ([0-9A-F]+);[^\r\n]*actor entity ([0-9A-F]+)')
STAMP = re.compile(r'^\[([^\]]+)\]')


def require(value, why):
    if not value:
        raise AssertionError(why)


def verify(root, allow_scripted=False):
    result = json.loads((root / 'result.json').read_text(encoding='utf-8-sig'))
    count = result['players']
    require(3 <= count <= 16 and result['native_room_menu'], 'Not a larger native-menu Room capture')
    require(result['startup_complete'] and result['observation_complete'] and not result.get('error'), 'Native startup/observation did not finish')
    require(all(result[k] for k in ('normal_user_files_unchanged', 'normal_config_unchanged', 'normal_crash_unchanged')), 'Normal files changed')
    require(not result['screenshots_taken'], 'Unexpected screenshots')
    if allow_scripted:
        require(result.get('scripted_sdk_input') and result['gameplay_input_sent'] and all(p.get('scripted_sdk_input_observed') and p.get('scripted_sdk_program_completed') for p in result['processes']), 'Authorized SDK programs did not all complete')
    else:
        require(not result['gameplay_input_sent'], 'Unexpected automated gameplay input')
    all_ticks = []
    captures = []
    peers = set()
    for process in result['processes']:
        require(process['process_exited'] and process['exit_code'] == 0 and not process.get('forced_close'), 'Native test did not shut down normally')
        text = Path(process['log']).read_text(encoding='utf-8', errors='replace')
        # Disconnection after the test requests all closes is expected teardown,
        # and cannot retroactively invalidate the completed shared interval.
        text = '\n'.join(line for line in text.splitlines() if not (stamp := STAMP.match(line)) or stamp[1] < result['close_started_local'])
        require(not re.search(r'Native online lobby handoff failed:|Native online menu preparation failed:|Native lobby QA rejected:|Retained native Room stopped:|Native Room gameplay stopped:|Native Room clothing .*failed', text), 'Native owner/handoff/render failed before shutdown')
        local = LOCAL.findall(text)
        require(len(local) == 1, 'Local peer appearance was not confirmed exactly once')
        peer, color = map(int, local[0])
        require(1 <= peer <= count and color == peer-1 and peer not in peers, 'Duplicate/wrong local peer or assigned color')
        peers.add(peer)
        require('Authored native Online Multiplayer page opened' in text and 'Native FrontMenu online SetOption bridge registered' in text, 'Native lobby page/bridge not used')
        require('Native lobby completed authored save reload and accepted native Continue' in text, 'Original reload/Continue did not complete')
        require(re.search(r'Native lobby load completion: accepted true, (?:reload headers|save exists) true, result 11, archive state \d+->6, decoded true, retained true, completed true', text), 'Original shared-save full decode missing')
        request = 'Native GFx online requested Host' if peer == 1 else 'Native GFx online requested Join through private loopback invitation'
        require(request in text, 'Connection bypassed the native menu')
        admissions = re.findall(r'Native Room extra humans admitted: count (\d+) mask ([0-9A-F]+);', text)
        require(admissions == [(str(count-2), f'{((1 << count)-1)&~3:04X}')], 'Not every actual extra human was admitted once')
        cameras = {int(row[0]): row[1:] for row in CAMERA.findall(text)}
        require(set(cameras) == set(range(3, count+1)), 'Extra player camera/actor ownership is incomplete')
        require(all(int(address, 16) for row in cameras.values() for address in row), 'Native camera/actor identity is null')
        require(all(len({row[index] for row in cameras.values()}) == count-2 for index in (0, 4, 5)), 'Extra cameras or actor entities alias')
        require(re.search(r'Online single-view layout: local [01], split 0,', text), 'Native full-window layout missing')
        if peer > 2:
            controller, output, tracker, handle, node, entity = cameras[peer]
            require(re.search(r'Native Room local view rebound: controller '+controller+r' view [0-9A-F]+ previous [0-9A-F]+ retained '+node+r' matches true split 0', text), 'Extra client did not bind its own full-window native camera')
            require(re.search(r'Native Room HUD health target: profile \d+ retail [0-9A-F]+ local extra '+entity+r';', text), 'Extra client HUD did not target its own actor')
        start = text.index('Native Room gameplay first frame consumed:')
        ticks = {}
        for tick, digest in OBS.findall(text[start:]):
            tick = int(tick)
            require(tick not in ticks or ticks[tick] == digest, 'Conflicting repeated agreed observation')
            ticks[tick] = digest
        require(ticks, 'No agreed native updates after gameplay started')
        all_ticks.append(ticks)
        captures.append({'role': process['role'], 'local_peer': peer, 'color': color, 'extra_actors': count-2, 'agreed_updates': len(ticks)})
    require(peers == set(range(1, count+1)), 'Actual native clients do not cover the roster')
    common = sorted(set.intersection(*(set(t) for t in all_ticks)))
    require(all(len({t[tick] for t in all_ticks}) == 1 for tick in common), 'Native worlds diverged')
    longest = run = 0
    last = None
    for tick in common:
        run = run+1 if last is not None and tick == last+1 else 1
        longest = max(longest, run)
        last = tick
    require(longest >= 300, 'Fewer than 300 consecutive shared agreed native updates')
    return {'passed': True, 'players': count, 'binary_sha256': result['exe_sha256'], 'longest_matching_updates': longest, 'captures': captures, 'scope': __doc__.strip()}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    root = parser.parse_args().root
    result = verify(root)
    (root / 'native-menu-roster-validation.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))
