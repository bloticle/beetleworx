"""Verify original Bink movie entry, synchronized exit and restored controls.

Three native clients play the retail Havok logo through StartSequence.
--direct-movie instead checks the original PlayMovieInternal state-1 route.
--expect-skip checks native Accept or direct held-skip timing and held-input return.
This does not prove other movies, every skip configuration, credits, Internet
stalls or a campaign.
"""
import argparse
import json
import math
from pathlib import Path
import re
import runpy
import struct


def verify(root, expect_skip=False, direct_movie=False):
    result = json.loads((root / 'result.json').read_text(encoding='utf-8-sig'))
    assert result['players'] == 3
    assert bool(result.get('direct_movie')) == direct_movie
    assert bool(result['movie_sequence']) != direct_movie
    assert result['checkpoint_reload_tick'] == 300 and result['checkpoint_reload_count'] == 1
    assert not result['cutscene_cycle'] and not result['authored_conversation']
    skip_delay = result.get('movie_skip_delay', 0)
    if expect_skip:
        assert result['cutscene_skip_input'] and skip_delay in ((0,) if direct_movie else (0, 2))
    helpers = Path(__file__).parent
    menu = runpy.run_path(str(helpers / 'verify-native-room-menu-roster.py'))['verify'](root, allow_scripted=True)
    sample = runpy.run_path(str(helpers / 'verify-native-player-respawn.py'))['SAMPLE']
    trajectory = runpy.run_path(str(helpers / 'verify-private-room-input.py'))['TRAJECTORY']
    obs = runpy.run_path(str(helpers / 'verify-native-room-menu-roster.py'))['OBS']
    histories = []
    reports = []
    returns = []
    for process in result['processes']:
        full = Path(process['log']).read_text(encoding='utf-8', errors='replace')
        text = '\n'.join(line for line in full.splitlines() if not line.startswith('[') or line[1:24] < result['close_started_local'])
        route = 'direct movie' if direct_movie else 'movie sequence'
        mode = 1 if direct_movie else 2
        assert text.count(f'Private native {route}: tick 300 LOGO_havok.bik attempted true status 0 stack true;') == 1
        assert '[NtCreateFile] FAILED:' not in text[text.index(f'Private native {route}: tick 300'):], 'Movie or later content open failed'
        retail_mask, extra_mask = ('03', '0004') if direct_movie else ('00', '0000')
        assert text.count(f'Native Room movie entry interrupted update: tick 300 actual retail Motion mask {retail_mask} extra mask {extra_mask};') == 1
        assert text.count(f'Native Room cutscene input suspended: entry tick 300, input epoch 2; original state {mode}') == 1
        assert len(re.findall(r'Native Room movie sequence target retained: tick 301 object [0-9A-F]+;', text)) == 1
        clock_start = 300 if direct_movie else 301
        assert len(re.findall(rf'Native Room movie playback clock admitted: tick {clock_start} Bink [0-9A-F]+;', text)) == 1
        assert 'Native Room movie camera retained: tick 360;' in text
        if not expect_skip:
            assert 'Native Room movie camera retained: tick 420;' in text
        if not (direct_movie and expect_skip):
            assert 'Native Room cutscene UI callback delivered:' not in text
        assert 'Native Room loader agreed:' not in text and 'Room native world rebootstrap:' not in text
        resumed = re.findall(r'Native Room cutscene bindings re-admitted: source tick (\d+), first delivery (\d+), input epoch 3;', text)
        assert len(resumed) == 1
        source, delivery = map(int, resumed[0])
        assert delivery == source + 2
        if direct_movie and expect_skip:
            stops = list(re.finditer(r'Native Room direct movie held skip: tick (\d+) object [0-9A-F]+ elapsed ([^;]+);', text))
            assert len(stops) == 1
            # The observation immediately after the native stop belongs to the
            # original update that consumed it, not a guessed wall-clock frame.
            stop_tick = int(stops[0][1])
            assert int(obs.search(text, stops[0].end())[1]) == stop_tick
            callbacks = [(int(t), action, int(stamp)) for t, action, stamp in re.findall(
                r"Native Room cutscene UI callback delivered: tick (\d+) epoch 2 action '([^']+)' native result \d+ timer (\d+);", text)]
            assert [r[1] for r in callbacks] == ['DisplaySkipMovieButton', 'MovieSkipStart']
            assert callbacks[0][0] == 321 and 321 < callbacks[1][0] < 350
            settings = [(int(t), int(allowed), int(held), float(duration), action) for t, allowed, held, duration, action in re.findall(
                r"Native Room direct movie skip settings: tick (\d+) allowed (\d+) held (\d+) duration ([^;]+); native fields after '([^']+)'", text)]
            assert len(settings) == 2 and settings[0][0] == callbacks[0][0] and settings[0][1:3] == (1, 0)
            assert settings[1][0] == callbacks[1][0] and settings[1][1:3] == (1, 1)
            duration = settings[1][3]
            assert math.isfinite(duration) and 0 < duration <= 3600 and settings[0][3] == duration
            assert float(stops[0][2]) >= duration and (stop_tick-callbacks[1][0]-1)/30 < duration
            assert callbacks[1][2] == 100000000 + callbacks[1][0]*1000000
            assert stop_tick == callbacks[1][0] + math.ceil(duration*30-1e-6) and source == stop_tick+2 and source < 430
            assert text.count('Native Room cutscene UI receiver armed: tick 304 epoch 2 handler 82B1EDF8;') == 1
        else:
            assert source == 372 if expect_skip else 430 <= source <= 480
        accepts = [(int(t), int(epoch), int(native), int(delay), bool(int(remaining, 16)), int(done))
                   for t, epoch, native, delay, remaining, done in re.findall(
                       r'Native Room movie Accept delivered: tick (\d+) epoch (\d+) native result (\d+) skip delay (-?\d+) remaining ([0-9A-F]+) completed (\d+);', text)]
        if expect_skip and not direct_movie:
            assert text.count('Native Room movie Accept receiver armed: tick 304 epoch 2 handler 82B1DF70;') == 1
            expected = [(371, 2, 0, skip_delay, False, 1)]
            if skip_delay:
                expected.insert(0, (321, 2, 0, 2, True, 0))
                timers = [(int(t), op, int(stamp), float(elapsed)) for t, op, stamp, elapsed in re.findall(
                    r'Native Room movie skip timer: tick (\d+) operation (start|elapsed) stamp (\d+) elapsed ([^;]+);', text)]
                assert [r[:3] for r in timers] == [(301, 'start', 401000000), (321, 'elapsed', 401000000), (371, 'elapsed', 401000000)]
                assert abs(timers[1][3] - 2/3) < 1e-5 and abs(timers[2][3] - 7/3) < 1e-5
            assert accepts == expected, 'Original handler must retain its skip delay and finish only on the later agreed press'
        else:
            assert not accepts
        returns.append((source, delivery))
        origins = re.findall(r"Private Room SDK input: peer (\d+) tick (\d+) elapsed 0 phase '", text)
        assert len(origins) == 1
        peer, origin = map(int, origins[0])
        assert f"SDK input: peer {peer} tick {origin+1600} elapsed 1600 phase 'complete'" in text
        retail = {}
        for row in sample.findall(text):
            pose = tuple(struct.unpack('>f', bytes.fromhex(v))[0] for v in row[1].split('/'))
            assert len(pose) == 6 and all(math.isfinite(v) for v in pose)
            retail[int(row[0])] = (pose, tuple(map(int, row[2:])))
        extra = {}
        for row in trajectory.findall(text):
            if row[0] != '3':
                continue
            value = tuple(map(float, row[2:8])) + (int(row[8]), row[9], row[10])
            assert all(math.isfinite(v) for v in value[:7])
            extra[int(row[1])] = value
        def position(player, at):
            tick = origin + at
            return retail[tick][0][player*3:player*3+3] if player < 2 else extra[tick][:3]
        heights = []
        for player in range(3):
            def delta(a, b):
                x, y = position(player, a), position(player, b)
                return tuple(y[i]-x[i] for i in (0, 2))
            forward, backward = delta(1401, 1407), delta(1441, 1447)
            assert math.hypot(*forward) > .03 and math.hypot(*backward) > .03
            assert sum(x*y for x, y in zip(forward, backward)) < 0
            floor = position(player, 1479)[1]
            height = max(position(player, t)[1] for t in range(1482, 1561)) - floor
            assert height > 1 and abs(position(player, 1590)[1] - floor) < .4
            heights.append(height)
        jumps = [int(t) for t in re.findall(r'Native Room owned Jump returned: peer \d+ tick (\d+) edge', text)]
        assert origin+1482 in jumps and origin+1486 in jumps
        assert not any(301 <= t < delivery for t in jumps)
        if expect_skip:
            assert not any(delivery <= t < origin+662 for t in jumps), 'Held movie Accept leaked into a gameplay Jump'
        agreed = {int(t): digest for t, digest in obs.findall(text)}
        assert all(t in agreed and t in retail and t in extra for t in range(origin, origin+1601))
        assert all(retail[t][1] == (5, 5, 5, 5) for t in range(origin, origin+1601))
        histories.append({t: (agreed[t], retail[t], extra[t]) for t in range(origin, origin+1601)})
        reports.append({'role': process['role'], 'peer': peer, 'source_origin': origin,
                        'source_return': source, 'first_delivery': delivery, 'jump_heights': heights})
    assert len(set(returns)) == 1, 'Movie exit differs between native clients'
    assert all(h == histories[0] for h in histories), 'Actual world or actor trajectories differ'
    assert all(result[k] for k in ('normal_user_files_unchanged', 'normal_config_unchanged', 'normal_crash_unchanged'))
    return {'passed': True, 'direct_movie': direct_movie, 'skip_verified': expect_skip, 'skip_delay': skip_delay, 'binary_sha256': menu['binary_sha256'], 'matching_updates': len(histories[0]),
            'shared_return': returns[0], 'captures': reports, 'scope': __doc__.strip()}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--expect-skip', action='store_true')
    parser.add_argument('--direct-movie', action='store_true')
    args = parser.parse_args();root = args.root
    report = verify(root, args.expect_skip, args.direct_movie)
    (root / 'native-movie-sequence-validation.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))
