"""Validate scheduled original native loads across four independent worlds.

This does not establish arbitrary level transitions, campaign completion,
rollback/state correction, sixteen native players or Internet multiplayer.
"""
import argparse
import json
import math
from pathlib import Path
import re
import runpy

OBS = runpy.run_path(str(Path(__file__).with_name('verify-native-room-menu-roster.py')))['OBS']
HISTORY = runpy.run_path(str(Path(__file__).with_name('verify-native-input-history.py')))['HISTORY']
TRAJECTORY = runpy.run_path(str(Path(__file__).with_name('verify-private-room-input.py')))['TRAJECTORY']
CALL = runpy.run_path(str(Path(__file__).with_name('verify-private-room-multi-actors.py')))['CALL']


def verify(root, expect_default=False):
    result = json.loads((root / 'result.json').read_text(encoding='utf-8-sig'))
    assert result['players'] == 4 and result['checkpoint_reload_tick'] == 700
    count = result.get('checkpoint_reload_count', 1)
    interval = result.get('checkpoint_reload_interval', 700)
    assert 1 <= count <= 4 and interval >= 600
    reloads = [700+i*interval for i in range(count)]
    epochs = count+1
    target = result.get('native_level_target', '')
    targets = result.get('native_level_targets') or [target]*count
    assert len(targets) == count and all(not value or re.fullmatch(r'[a-z0-9_.]{1,63}', value) for value in targets)
    assert not result.get('native_level_targets') or not target
    destinations = [value or 'tcy_autotopia_visit1.sectionc_first' for value in targets]
    method = result.get('native_level_method', 'original-level') if any(targets) else 'checkpoint-memory'
    assert method in ('original-level', 'original-transition', 'checkpoint-memory')
    assert result['native_reload'] == (not expect_default), 'Unexpected command-line reload override'
    assert result['startup_complete'] and result['observation_complete'] and not result.get('error')
    assert all(result[k] for k in ('normal_user_files_unchanged', 'normal_config_unchanged', 'normal_crash_unchanged'))
    assert result['scripted_sdk_input'] and result['gameplay_input_sent'] and not result['screenshots_taken']
    firsts = None
    histories = None
    policy_checks = []
    worlds = []
    native_bodies = {}
    captures = []
    for process in result['processes']:
        assert process['process_exited'] and process['exit_code'] == 0
        assert not process.get('forced_close') and not process.get('identity_rejected')
        text = Path(process['log']).read_text(encoding='utf-8', errors='replace')
        text = '\n'.join(line for line in text.splitlines() if not line.startswith('[') or line[1:24] < result['close_started_local'])
        for failure in ('Native Room gameplay stopped:', 'Retained native Room stopped:', 'Native room initialization stopped:'):
            assert failure not in text, failure
        requests = re.findall(r'Native lobby QA restart requested: tick (\d+), attempted true, status 0, stack true', text)
        assert list(map(int, requests)) == reloads, 'Original scheduled reloads did not all succeed'
        assert text.count(f'Native lobby QA reload method: {method}') == count
        target_calls = re.findall(r'Native lobby QA level target: ([a-z0-9_.]+)', text)
        assert target_calls == [value for value in targets if value], 'Original level itinerary differs from requested route'
        exits = re.findall(r'Native reload camera mode deactivated: peer (\d+) mode [0-9A-F]+ method ([0-9A-F]+)', text)
        assert exits == [('3', '82A5DEC0'), ('4', '82A5DEC0')]*count, exits
        for index, tick in enumerate(reloads):
            destination = destinations[index]
            for sequence, phase in ((0, 0), (1, 1)):
                assert text.count(f"Native Room loader agreed: tick {tick}, sequence {sequence}, phase {phase}, destination '{destination}'") == 1
            assert text.count(f'Room native world rebootstrap: cancelled unverified tick {tick}, raw first {tick+1}, load sequence 1') == 1
            assert text.count(f'Native Room reload entered fresh admission: raw first {tick+1}, new controls generation {index+2}') == 1
        starts = list(map(int, re.findall(r'Native Room gameplay first frame consumed: tick (\d+)', text)))
        assert len(starts) == epochs and starts[0] < reloads[0], (
            f'{process["role"]}: expected {epochs} separately admitted gameplay worlds, '
            f'observed {len(starts)} at ticks {starts}; loader success alone is insufficient')
        assert all(starts[index+1] > tick+1 for index, tick in enumerate(reloads))
        assert all(start < tick for start, tick in zip(starts, reloads))
        assert firsts is None or firsts == starts
        firsts = starts
        assert text.count("phase 'complete' reads") == epochs, 'Every native input program must complete'
        admissions = re.findall(r'Native Room extra humans admitted: count (\d+) mask ([0-9A-F]+);', text)
        assert admissions == [('2', '000C')]*epochs, admissions
        frames = HISTORY.findall(text)
        assert len(frames) == epochs
        frame_scopes = [(int(first), int(last), int(mask, 16), digest) for _, first, last, mask, digest in frames]
        assert all(last-first+1 == 224 and mask == 15 for first, last, mask, _ in frame_scopes)
        assert [(row[0], row[1]) for row in frame_scopes] == [(start+288, start+511) for start in starts]
        assert len({row[3] for row in frame_scopes}) == epochs
        assert histories is None or histories == frame_scopes
        histories = frame_scopes
        updates = {int(tick): digest for tick, digest in OBS.findall(text)}
        entered = re.findall(r"Native gameplay observation v5: level '([^']+)' state '([^']+)'[^\r\n]*?tick (\d+),", text)
        for index, destination in enumerate(targets):
            if not destination:
                continue
            end = reloads[index+1] if index+1 < count else 2**64
            observed = [(level, state) for level, state, tick in entered if starts[index+1] <= int(tick) < end]
            assert observed and all(level == destination.split('.', 1)[0] and state == destination for level, state in observed), 'Native observations did not enter the requested itinerary level/state'
        assert not set(reloads) & updates.keys(), 'Cancelled transition tick was published as a completed world'
        scoped_worlds = []
        activity = []
        all_rows, all_calls = TRAJECTORY.findall(text), CALL.findall(text)
        for epoch, origin in enumerate(starts):
            end = reloads[epoch] if epoch < count else float('inf')
            scoped_worlds.append({tick: digest for tick, digest in updates.items() if origin <= tick < end})
            trajectories, movement = {}, []
            for peer in (3, 4):
                rows = {int(row[1]): tuple(map(float, row[2:8]))+(int(row[8]),)
                        for row in all_rows if int(row[0]) == peer and origin <= int(row[1]) < end
                        and row[9:] == ('true', 'true')}
                assert len(rows) >= 512 and all(all(map(math.isfinite, row[:6])) for row in rows.values())
                deltas = [(rows[origin+b][0]-rows[origin+a][0], rows[origin+b][2]-rows[origin+a][2])
                          for a, b in ((61, 73), (91, 103))]
                assert all(math.hypot(*delta) > .03 for delta in deltas) and sum(a*b for a, b in zip(*deltas)) < 0, f'Epoch {epoch}: actor did not move and reverse'
                jump = [row for tick, row in rows.items() if origin+132 <= tick <= origin+212]
                height = max(row[1] for row in jump)-rows[origin+131][1]
                assert height > 1 and max(row[4] for row in jump) > 5 and rows[max(rows)][6] == 0, f'Epoch {epoch}: actor did not jump and land'
                profile = 0 if peer & 1 else 1  # Matches the harness's mixed-character roster.
                names = ('SpinAttack', 'Start Paint Brush', 'End Paint Brush') if profile == 0 else ('SpinAttack2', 'Start Electric Stream2', 'End Electric Stream2')
                calls = [row for row in all_calls if origin <= int(row[0]) < end and int(row[1]) == peer]
                assert all(row[2] == str(profile) and row[4] == '0' for row in calls), 'Native action targeted another character profile'
                for name in names:
                    selected = [row for row in calls if row[3] == name]
                    assert len(selected) == 1 and selected[0][4] == '0', f'Epoch {epoch}: native tool dispatch failed'
                    if result.get('button_task_probe') and profile == 1 and 'Electric Stream' in name:
                        at = int(selected[0][0])
                        snapshots = re.findall(r'Private button task snapshot: (extra|retail) peer (\d+) tick (\d+) ordinal (\d+) table ([0-9A-F]+) handler ([0-9A-F]+) name ([0-9A-F]+) flags ([0-9A-F]+)', text)
                        tasks = {}
                        for source in ('extra', 'retail'):
                            rows_at = [row for row in snapshots if row[0] == source and int(row[1]) == peer and int(row[2]) == at]
                            assert len(rows_at) == 15 and len({row[4] for row in rows_at}) == 15, 'Incomplete or duplicate original Oswald task snapshot'
                            tasks[source] = {row[4]: row for row in rows_at}
                        assert tasks['extra'].keys() == tasks['retail'].keys(), 'Extra Oswald is missing native tasks'
                        for table, row in tasks['extra'].items():
                            retail_task = tasks['retail'][table]
                            assert row[5:7] == retail_task[5:7], 'Extra task has a different native input handler/type'
                            assert int(row[7], 16) & 14 == int(retail_task[7], 16) & 14, f'Epoch {epoch}: extra human missed authored task policy update'
                        electric = tasks['retail']['82164EA8']
                        assert electric[5] == '82B473D0' and electric[6] == '82B4AA60', 'Unexpected electric-stream native task ABI'
                        # Original 82853BA8 skips tasks with any low-four flag;
                        # 82B473D0 accepts start/end UIDs only when dispatched.
                        expected = '0' if int(electric[7], 16) & 15 else '1'
                        assert selected[0][5] == expected, 'Native tool result disagrees with retail authored permission'
                        policy_checks.append({'role': process['role'], 'epoch': epoch, 'tick': at, 'action': name, 'allowed': expected == '1'})
                    else:
                        assert selected[0][5] == '1', f'Epoch {epoch}: native tool callback failed'
                trajectories[peer] = rows
                movement.append({'peer': peer, 'jump_height': height, 'post_reload_samples': len(rows)})
            if epoch in native_bodies:
                for peer in (3, 4):
                    common = native_bodies[epoch][peer].keys() & trajectories[peer].keys()
                    assert len(common) >= 512 and all(native_bodies[epoch][peer][tick] == trajectories[peer][tick] for tick in common), f'Epoch {epoch}: body trajectories differ between worlds'
            else:
                native_bodies[epoch] = trajectories
            activity.append({'epoch': epoch, 'first': origin, 'actors': movement})
        worlds.append(scoped_worlds)
        crash_path = Path(process['user']) / 'diagnostics/crash.log'
        if crash_path.exists():
            assert f'process_id={process["pid"]}\n' not in crash_path.read_text()
        captures.append({'role': process['role'], 'gameplay_starts': starts, 'world_admissions': len(admissions),
                         'history_scopes': frame_scopes, 'post_reload_observations': len(scoped_worlds[-1]), 'active_actors': activity[-1]['actors'], 'active_epochs': activity})
    matching = []
    for epoch in range(epochs):
        selected = [scopes[epoch] for scopes in worlds]
        ticks = sorted(set.intersection(*(set(world) for world in selected)))
        longest = run = 0
        previous = None
        for tick in ticks:
            assert len({world[tick] for world in selected}) == 1, f'Epoch {epoch}: worlds differ at tick {tick}'
            run = run+1 if previous is not None and tick == previous+1 else 1
            longest = max(longest, run)
            previous = tick
        assert longest >= 512, f'Epoch {epoch}: only {longest} consecutive matching native updates'
        matching.append(longest)
    if result.get('button_task_probe'):
        assert policy_checks and any(row['allowed'] for row in policy_checks), 'No unrestricted native electric-stream acceptance'
        assert any(not row['allowed'] for row in policy_checks), 'No authored tool restriction exercised'
    return {'passed': True, 'native_policy_checks': policy_checks, 'binary_sha256': result['exe_sha256'], 'reload_ticks': reloads,
            'native_level_target': target, 'native_level_targets': targets,
            'matching_updates_by_epoch': matching, 'post_reload_matching_updates': matching[-1],
            'captures': captures, 'scope': __doc__.strip()}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--expect-default', action='store_true', help='Require no command-line reload override; native re-entry must execute from the compiled default')
    args = parser.parse_args()
    root = args.root
    result = verify(root, args.expect_default)
    (root / 'native-world-reentry-validation.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(result, indent=2))
