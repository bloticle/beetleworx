"""Verify native sequential PAK loading sees clips before HKW initialization."""
from pathlib import Path
import runpy
import tempfile

root = Path(__file__).resolve().parents[1]
module = runpy.run_path(str(root / 'scripts/build-pc-menu.py'))
insert = module['insert_ability_resources']
pak = module['pak']

retail = [
    ('characters/mickey/MickeyBehaviorProject.hkx', b'project', b'HKP\0'),
    ('characters/mickey/animations/walk.hkx', b'walk', b'HKB\0'),
    ('characters/MICKEY/mickey_default.hkw', b'initialize Mickey', b'HKW\0'),
    ('characters/oswald/oswald_default.hkw', b'initialize Oswald', b'HKW\0'),
]
clips = [('characters/mickey/animations/pc/emr_dash.hkx', b'dash', b'HKB\0'),
         ('characters/mickey/animations/pc/emr_sprint.hkx', b'sprint', b'HKB\0')]
expected_names = {item[0].lower() for item in clips}

def initialize_sequentially(resources):
    available = set()
    for name, data, kind in resources:
        if name.lower() == 'characters/mickey/mickey_default.hkw':
            return expected_names <= available
        available.add(name.lower())
    return False

assert not initialize_sequentially(retail + clips), 'Old append order unexpectedly passes'
ordered = insert(retail, clips)
assert initialize_sequentially(ordered), 'HKW sees missing animation dependencies'
assert [item for item in ordered if item not in clips] == retail, 'Retail bytes/order changed'
assert [item for item in ordered if item in clips] == clips, 'Imported bytes/order changed'
assert insert(retail, []) == retail
with tempfile.TemporaryDirectory(prefix='beetleworx-ability-order-') as temporary:
    archive = Path(temporary) / '_Dynamic.pak'
    pak.write_pak(ordered, archive)
    loaded = pak.read_pak(archive)
    assert loaded == ordered and initialize_sequentially(loaded), 'PAK roundtrip lost dependency order'

def rejected(resources, additions):
    try:
        insert(resources, additions)
    except ValueError:
        return
    raise AssertionError('Ambiguous or replacing asset order admitted')

rejected(retail[:2], clips)
rejected(retail + [retail[2]], clips)
rejected(retail, clips + [clips[0]])
rejected(retail, [retail[1]])
print('Ability PAK dependency order passed: sequential initialization, real archive roundtrip, '
      'unchanged retail resources, case handling and ambiguous/replacing input rejection')
