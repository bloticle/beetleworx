"""Verify native disconnect, offline save recovery, and fresh online gameplay.

The derived log view contains exact second-session log bytes from the original
capture. It lets the unchanged active-input/roster verifiers check that session
independently, without mixing tick numbers from two different Rooms.
This is not reconnect, world-state correction or campaign completion.
"""
import argparse
import base64
import copy
import json
from pathlib import Path
import re
import runpy
import zlib
from datetime import datetime


def private_invitation_nonce(path):
    code = path.read_text(encoding='ascii').strip()
    assert len(code) in (62, 64) and code.startswith('EM2-')
    encoded = code[4:]
    payload = base64.b32decode(encoded + '=' * (-len(encoded) % 8))
    assert len(payload) in (36, 37) and payload[0] in (1, 3)
    assert payload[1:5] == bytes((127, 0, 0, 1))
    assert payload[-4:] == zlib.crc32(payload[:-4]).to_bytes(4, 'big')
    return payload[7:23]


def verify(root):
    result = json.loads((root / 'result.json').read_text(encoding='utf-8-sig'))
    assert result.get('reenter_after_return') and result['return_to_title_probe']
    assert result['players'] == 4 and not result['close_participant']
    assert result['failure_observed'] and result['startup_complete'] and not result.get('error')
    assert not result['observation_complete'] and not result['screenshots_taken']
    assert all(result[k] for k in ('normal_user_files_unchanged', 'normal_config_unchanged', 'normal_crash_unchanged'))
    if result.get('replay_invitation'):
        original_nonce = private_invitation_nonce(Path(result['replay_invitation']))
    else:
        initial = root / 'initial-menu-invitation.txt'
        assert Path(result['initial_invitation']).resolve() == initial.resolve()
        captured = datetime.fromisoformat(result['initial_invitation_captured_utc'])
        failed = datetime.fromisoformat(result['failure_injection_started_utc'])
        started = datetime.fromisoformat(result['processes'][0]['started_utc'])
        assert started <= captured < failed, 'Initial invitation was not preserved before disconnect'
        original_nonce = private_invitation_nonce(initial)
    fresh_nonce = private_invitation_nonce(root / 'native-menu-invitation.txt')
    assert fresh_nonce != original_nonce and any(fresh_nonce), 'Old Room invitation identity was reused'
    view = root / 'second-session-log-view'
    projected = copy.deepcopy(result)
    projected['observation_complete'] = True
    projected['derived_from'] = str(root / 'result.json')
    projected['validation_scope'] = 'Second native session only; exact source log slices.'
    roles, source_peers, saved = [], set(), {}
    for process, output in zip(result['processes'], projected['processes']):
        assert process['process_exited'] and process['exit_code'] == 0
        assert not process.get('forced_close') and not process.get('identity_rejected')
        assert process['offline_campaign_unchanged']
        text = Path(process['log']).read_text(encoding='utf-8', errors='replace')
        stop = text.index('Retained native Room stopped:')
        fresh = text.index('Native online title restore: fresh lobby accepted after original offline save refresh')
        markers = [
            stop,
            text.index('Native Room title return: normal original transition queued;'),
            text.index('Native Room title return: original title load completed'),
            text.index('Native Room title return: retired cameras released;'),
            text.index('Native online title restore complete: saved campaign '),
            text.index('Native Room title return complete:'),
            fresh,
        ]
        assert markers == sorted(markers) and len(set(markers)) == len(markers)
        assert "phase 'complete' reads" in text[:stop]
        assert 'Native gameplay observation v' not in text[stop:fresh]
        assert not re.search(r'Native (?:Room title return|online title restore) (?:failed|rejected):', text)
        report = text.split('Native Room failure context: ', 1)[1].split('\n[', 1)[0]
        assert report.startswith('Beetleworx online failure context v1\n') and len(report) < 2048
        fields = dict(line.split(': ', 1) for line in report.splitlines()[1:] if ': ' in line)
        assert fields['Phase'] == 'running (8)' and fields['Participant mask'] == '15'
        assert int(fields['Last verified tick']) >= 400
        source_peers.add(int(fields['Local peer']))
        summary = re.search(r'Native online title restore complete: saved campaign (true|false);', text)
        assert summary
        saved[process['role']] = summary[1] == 'true'
        bridge = text.rfind('Native FrontMenu online SetOption bridge registered', stop, fresh)
        assert bridge > stop
        begin = text.rfind('\n', 0, bridge) + 1
        folder = view / process['role']
        folder.mkdir(parents=True, exist_ok=True)
        path = folder / 'game.log'
        path.write_text(text[begin:], encoding='utf-8')
        output['root'], output['log'] = str(folder), str(path)
        output['source_log'] = process['log']
        output['source_line'] = text[:begin].count('\n') + 1
        roles.append(process['role'])
    assert source_peers == {1, 2, 3, 4}
    assert saved == {'host': True, 'client': False, 'client-3': False, 'client-4': False}
    host = Path(result['processes'][0]['log']).read_text(encoding='utf-8', errors='replace')
    assert len(re.findall('Private native Room completed disconnect injected:', host)) == 1
    assert 'participant_close_requested_utc' not in result
    for filename in ('result.json', 'roster.json'):
        (view / filename).write_text(json.dumps(projected, indent=2) + '\n', encoding='utf-8')
    active = runpy.run_path(str(Path(__file__).with_name('verify-native-menu-active-input.py')))['verify'](view)
    return {'passed': True, 'binary_sha256': result['exe_sha256'], 'roles': roles,
            'fresh_session_nonce': True,
            'offline_campaigns': saved, 'fresh_session': active,
            'derived_log_view': str(view), 'scope': __doc__.strip()}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    root = parser.parse_args().root
    result = verify(root)
    (root / 'native-title-reentry-validation.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))
