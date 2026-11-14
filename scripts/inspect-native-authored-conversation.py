"""Compare original authored conversation progress across three native worlds.

The fixed Autotopia sequence runs on its original bound owner. Sparse native
event-cursor samples supplement the ordinary world digests. Neither these
samples nor matching world digests prove cinematic input or skip delivery.
"""
import argparse
import json
from pathlib import Path
import re
import runpy


def inspect(root, require_finished=False):
    result = json.loads((root / 'result.json').read_text(encoding='utf-8-sig'))
    assert result['players'] == 3 and result['authored_conversation']
    assert result['checkpoint_reload_tick'] in (300, 700)
    assert result['checkpoint_reload_count'] == 1 and not result['cutscene_cycle']
    base = runpy.run_path(str(Path(__file__).with_name('verify-native-room-menu-roster.py')))['verify'](root, allow_scripted=True)
    pattern = re.compile(r'Private authored conversation state: tick (\d+) active (\d+) resource ([0-9A-F]+) flags ([0-9A-F]+) cursor (\d+) current (\d+) queued (\d+) ui ([0-9A-F]+)')
    reports = []
    histories = []
    for process in result['processes']:
        full = Path(process['log']).read_text(encoding='utf-8', errors='replace')
        text = '\n'.join(line for line in full.splitlines() if not line.startswith('[') or line[1:24] < result['close_started_local'])
        start = result['checkpoint_reload_tick']
        assert text.count(f'Private native authored conversation: tick {start} sequence AUT_v1_AreaIntro attempted true status 0 stack true;') == 1
        assert text.count('Private native authored conversation result: true') == 1
        rows = pattern.findall(text)
        assert len(rows) >= 8 and len({int(r[0]) for r in rows}) == len(rows)
        assert all(int(resource, 16) and int(ui, 16) for tick, active, resource, flags, cursor, current, entries, ui in rows if int(active))
        history = [(int(t), int(active), int(flags, 16), int(cursor), int(current), int(entries)) for t, active, resource, flags, cursor, current, entries, ui in rows]
        active = [r for r in history if r[0] >= start and r[1]]
        assert active and all(r[2] == 0x419B for r in active)
        assert len({r[3] for r in active}) >= 3, 'Original event cursor did not advance'
        finished = history[-1][0] >= 1900 and history[-1][1] == 0
        if require_finished:
            assert finished, 'Conversation completion was not observed'
        histories.append(history)
        reports.append({'role': process['role'], 'samples': history, 'finished_observed': finished,
                        'cinematic_ui_callbacks': text.count('Native Room cutscene UI callback delivered:')})
    assert all(h == histories[0] for h in histories), 'Original conversation samples differ between clients'
    return {'native_world_validation': base, 'conversation_samples_match': True,
            'finished_observed': all(r['finished_observed'] for r in reports),
            'cinematic_skip_verified': False, 'worlds': reports, 'scope': __doc__.strip()}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--require-finished', action='store_true')
    args = parser.parse_args()
    report = inspect(args.root, args.require_finished)
    (args.root / 'native-authored-conversation-validation.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))
