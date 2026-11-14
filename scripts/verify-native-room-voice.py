"""Verify generated Opus over UDP alongside actual native-menu gameplay.

Matches each source's encoded payload digest at every other game process,
requires actual decoded non-silent PCM and the existing native movement/action
checks. Does not establish microphone/speaker hardware, voice UI, Internet
connectivity, sixteen native game processes or campaign completion.
"""
import argparse
import json
import math
from pathlib import Path
import re
import runpy

SENT = re.compile(r'Native Room private voice sent: peer (\d+) sequence (\d+) bytes (\d+) digest ([0-9a-f]{64}); real Opus over UDP')
RECEIVED = re.compile(r'Native Room private voice received: peer (\d+) sequence (\d+) bytes (\d+) digest ([0-9a-f]{64}) decoder (true|false); host-authorized UDP sender')
DECODED = re.compile(r'Native Room private voice decoded: local (\d+) speakers (\d+) frames (\d+) energy ([\d.eE+-]+) finite (true|false); generated PCM only, no device playback')


def verify(root):
    record = json.loads((root / 'result.json').read_text(encoding='utf-8-sig'))
    if not record.get('voice_probe'):
        raise AssertionError('Voice probe was not enabled')
    scripts = Path(__file__).parent
    if not all(record.get(k) for k in ('native_room_menu', 'normal_user_files_unchanged', 'normal_config_unchanged', 'normal_crash_unchanged')):
        raise AssertionError('Native capture identity or normal-file integrity failed')
    if any(not p.get('process_exited') or p.get('exit_code') != 0 or p.get('forced_close') for p in record['processes']):
        raise AssertionError('Owned native processes did not exit normally')
    try:
        if record.get('chat_probe'):
            gameplay = runpy.run_path(str(scripts / 'verify-native-room-chat.py'))['verify'](root, native_hud=True)
        else:
            gameplay = runpy.run_path(str(scripts / 'verify-native-menu-active-input.py'))['verify'](root)
    except (AssertionError, ValueError, KeyError) as error:
        # Keep media evidence, but never turn an unsuccessful gameplay capture
        # into overall acceptance. The command must still return nonzero.
        gameplay = {'passed': False, 'error': str(error)}
    count = record['players']
    sources, destinations, captures = {}, {}, []
    for process in record['processes']:
        text = Path(process['log']).read_text(encoding='utf-8', errors='replace')
        sent = SENT.findall(text)
        if len(sent) != 3 or len({row[0] for row in sent}) != 1:
            raise AssertionError('Expected three real encoded source frames per native game')
        local = int(sent[0][0])
        if local in destinations:
            raise AssertionError('Duplicate native voice source identity')
        if f'Native Room private voice ready: peer {local}; separate encrypted UDP' not in text:
            raise AssertionError('Native Room did not complete media negotiation')
        for peer, sequence, size, digest in sent:
            key = int(peer), int(sequence)
            if key in sources or not 0 < int(size) <= 400:
                raise AssertionError('Duplicate or oversized source frame')
            sources[key] = int(size), digest
        if {int(row[1]) for row in sent} != {100000+local*100+i for i in range(3)}:
            raise AssertionError('Source sequence does not match generated frame series')
        received = RECEIVED.findall(text)
        actual = {}
        for peer, sequence, size, digest, accepted in received:
            key = int(peer), int(sequence)
            if key in actual or key[0] == local or accepted != 'true':
                raise AssertionError('Duplicate, self-echo, or rejected decoder frame')
            actual[key] = int(size), digest
        if len(actual) != (count-1)*3:
            raise AssertionError('Missing relayed frames in native game')
        decoded = DECODED.findall(text)
        if len(decoded) != 1:
            raise AssertionError('Expected one complete native decoded-audio result')
        peer, speakers, frames, energy, finite = decoded[0]
        energy = float(energy)
        if (int(peer), int(speakers), int(frames), finite) != (local, count-1, (count-1)*3, 'true') or not math.isfinite(energy) or energy <= .0001:
            raise AssertionError('Native network audio did not decode to bounded non-silent PCM')
        destinations[local] = actual
        captures.append({'role': process['role'], 'local_peer': local, 'sent': 3, 'received': len(actual), 'decoded_energy': energy})
    if set(destinations) != set(range(1, count+1)) or len(sources) != count*3:
        raise AssertionError('Actual native worlds do not cover every voice participant')
    for local, received in destinations.items():
        expected = {key: value for key, value in sources.items() if key[0] != local}
        if received != expected:
            raise AssertionError('Relayed source identity or encoded payload changed between native worlds')
    if gameplay.get('passed') and gameplay['binary_sha256'] != record['exe_sha256']:
        raise AssertionError('Gameplay/media executable identities differ')
    return {'passed': bool(gameplay.get('passed')), 'media_passed': True, 'players': count, 'binary_sha256': record['exe_sha256'],
            'captures': captures, 'native_gameplay': gameplay, 'scope': __doc__.strip()}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    root = parser.parse_args().root
    result = verify(root)
    (root / 'native-room-voice-validation.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({k: result[k] for k in ('passed', 'media_passed', 'players', 'binary_sha256', 'captures')}, indent=2))
    raise SystemExit(0 if result['passed'] else 1)
