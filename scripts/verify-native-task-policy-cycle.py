"""Verify balanced native ability blocking/unblocking in four local worlds.

Uses isolated SDK inputs and original native restriction writers. This is not
full campaign, Internet, sixteen-client, or effect/target-damage acceptance.
"""
import argparse
import json
import math
from pathlib import Path
import re
import runpy


def verify(root):
    scripts = Path(__file__).parent
    result = json.loads((root / 'result.json').read_text(encoding='utf-8-sig'))
    assert result.get('policy_cycle') and result.get('button_task_probe') and result['players'] == 4
    assert not result['checkpoint_reload_tick'] and not result.get('reenter_after_return')
    menu = runpy.run_path(str(scripts / 'verify-native-room-menu-roster.py'))['verify'](root, allow_scripted=True)
    movement_tools = runpy.run_path(str(scripts / 'verify-private-room-input.py'))
    movement = movement_tools['verify'](root)
    call_pattern = runpy.run_path(str(scripts / 'verify-private-room-multi-actors.py'))['CALL']
    assert menu['longest_matching_updates'] >= 700, 'Too little synchronized play around the policy cycle'
    captures = []
    reference_calls = None
    reference_body = None
    for process in result['processes']:
        text = Path(process['log']).read_text(encoding='utf-8', errors='replace')
        text = '\n'.join(line for line in text.splitlines() if not line.startswith('[') or line[1:24] < result['close_started_local'])
        assert 'Private native task policy cycle rejected:' not in text
        markers = list(re.finditer(r'Private native task policy cycle: tick (\d+) operation (add|remove) profile 1 task ElectricStream before ([0-9A-F]+) after ([0-9A-F]+); original writer, balanced test only', text))
        assert [(int(m[1]), m[2], int(m[3], 16) & 14, int(m[4], 16) & 14) for m in markers] == [(300, 'add', 0, 2), (500, 'remove', 2, 0)]
        origin = re.search(r'Private Room SDK input: peer (\d+) tick (\d+) elapsed 0 ', text)
        assert origin and int(origin[2]) < 300
        first = int(origin[2])
        assert re.search(r"Private Room SDK input: peer \d+ tick \d+ elapsed 560 phase 'complete'", text), 'Extended SDK program did not complete'
        if int(origin[1]) == 4:
            assert re.search(r"Private Room SDK input: peer 4 tick \d+ elapsed 530 phase 'tool after policy release'", text)
            assert re.search(r"Private Room SDK input: peer 4 tick \d+ elapsed 545 phase 'neutral'", text)
        calls = call_pattern.findall(text)
        snapshots = re.findall(r'Private button task snapshot: (extra|retail) peer (\d+) tick (\d+) ordinal (\d+) table ([0-9A-F]+) handler ([0-9A-F]+) name ([0-9A-F]+) flags ([0-9A-F]+)', text)
        checked = []
        for name in ('Start Electric Stream2', 'End Electric Stream2'):
            selected = [row for row in calls if row[1] == '4' and row[3] == name]
            assert len(selected) == 2 and all(row[2] == '1' and row[4] == '0' for row in selected)
            assert [row[5] for row in selected] == ['0', '1'], 'Extra Oswald did not become usable again after native unblocking'
            assert 300 < int(selected[0][0]) < 500 < int(selected[1][0]) < first+560
            for index, row in enumerate(selected):
                at = int(row[0])
                task_sets = {}
                for source in ('extra', 'retail'):
                    found = [item for item in snapshots if item[0] == source and item[1] == '4' and int(item[2]) == at]
                    assert len(found) == 15 and len({item[4] for item in found}) == 15
                    task_sets[source] = {item[4]: item for item in found}
                assert task_sets['extra'].keys() == task_sets['retail'].keys()
                for table, item in task_sets['extra'].items():
                    retail = task_sets['retail'][table]
                    assert item[5:7] == retail[5:7] and int(item[7], 16) & 14 == int(retail[7], 16) & 14
                for source in ('extra', 'retail'):
                    electric = task_sets[source]['82164EA8']
                    assert electric[5:7] == ('82B473D0', '82B4AA60')
                    assert int(electric[7], 16) & 15 == (2 if index == 0 else 0)
                checked.append({'action': name, 'tick': at, 'allowed': index == 1})
        for peer, names in ((3, ('SpinAttack', 'Start Paint Brush', 'End Paint Brush')), (4, ('SpinAttack2',))):
            for name in names:
                selected = [row for row in calls if int(row[1]) == peer and row[3] == name]
                assert len(selected) == 1 and selected[0][4:] == ('0', '1'), 'Unrelated native action regressed'
        body = {int(row[1]): tuple(map(float, row[2:8]))+(int(row[8]),) for row in movement_tools['TRAJECTORY'].findall(text) if row[0] == '4' and row[9:] == ('true', 'true')}
        assert len(body) >= 700 and all(all(map(math.isfinite, row[:6])) for row in body.values())
        jump = [row for at, row in body.items() if first+132 <= at <= first+212]
        assert max(row[1] for row in jump)-body[first+131][1] > 1 and max(row[4] for row in jump) > 5
        final = body[max(body)]
        assert final[6] == 0 and math.hypot(final[3], final[5]) < .1
        if reference_calls is not None:
            assert calls == reference_calls, 'Native action results/order differ between worlds'
            common = body.keys() & reference_body.keys()
            assert len(common) >= 700 and all(body[t] == reference_body[t] for t in common)
        else:
            reference_calls, reference_body = calls, body
        captures.append({'role': process['role'], 'native_policy_cycle': True, 'electric_stream': checked})
    return {'passed': True, 'binary_sha256': result['exe_sha256'], 'matching_native_updates': menu['longest_matching_updates'], 'captures': captures, 'movement': movement, 'scope': __doc__.strip()}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    root = parser.parse_args().root
    result = verify(root)
    (root / 'native-task-policy-cycle-validation.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(result, indent=2))
