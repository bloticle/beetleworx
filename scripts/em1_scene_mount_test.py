"""Exercise the real SDK VFS batch mount via a development DLL (no game launch)."""
import argparse, ctypes, tempfile, pathlib, json
parser = argparse.ArgumentParser()
parser.add_argument('--library', required=True)
parser.add_argument('--image')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='em1-mount-') as fixture:
    directory = pathlib.Path(fixture) / 'packfiles'
    directory.mkdir()
    original = directory / 'globalscripts.pak'
    original.write_bytes(b'original-pak-probe')
    dll = ctypes.CDLL(str(pathlib.Path(args.library).resolve()))
    dll.run.argtypes = [ctypes.c_wchar_p]
    dll.run.restype = ctypes.c_char_p
    result = dll.run(fixture).decode()
    print(result)
    assert result.startswith('PASS:'), result
    assert original.read_bytes() == b'original-pak-probe'
    levels = []
    if args.image:
        dll.stage_level.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_char_p]
        dll.stage_level.restype = ctypes.c_char_p
        for level in ['dbc_courtyard_1st.level', 'dbc_start_1st.level', 'toontown.level', 'dbc_start_zonec.level', 'dbc_start_zoneg.level']:
            observed = dll.stage_level(fixture, args.image, level.encode()).decode()
            print(observed)
            assert observed.startswith('PASS:'), observed
            levels.append(observed)
    pathlib.Path('analysis/em1-compatibility/scene-batch-mount-result.json').write_text(json.dumps({
        'result': result, 'original_fixture_unchanged': True, 'actual_levels': levels,
        'scope': 'Real SDK VFS, synthetic scene payload. No live engine/native scene activation.'
    }, indent=2) + '\n')
