"""Verify genuine original KFM/NIF/KF native graphs, bindings and cleanup."""
import argparse
import hashlib
import json
import re
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('profile',type=Path);a=p.parse_args()
source=json.loads(Path('analysis/em1-compatibility/kfm-closure-result.json').read_text())
expected={(f['archive'],f['resource']):f for f in source['files']};completion=json.loads((a.profile/'completion-result.json').read_text())
assert completion['completed'] and completion['normal_user_files_unchanged'] and completion['em1_source_metadata_unchanged'] and not completion['gameplay_input_sent']
assert completion['exe_sha256']==hashlib.sha256(Path('build/runtime23/beetleworx.exe').read_bytes()).hexdigest().upper()
log=(a.profile/'game.log').read_text(errors='replace');rows=[]
pattern=r"native animation manager result: archive='([^']+)' resource='([^']+)' loaded=(\w+) object=([0-9A-F]+) table=([0-9A-F]+) bound=(\w+) model=([0-9A-F]+) controller=([0-9A-F]+) sequences=(\d+) released=(\w+) records_restored=(\w+) error='([^']*)' cleanup_error='([^']*)'"
for m in re.finditer(pattern,log):
    key=(m[1],m[2]);assert key in expected and all(m[i]=='true' for i in [3,6,10,11]) and not m[12] and not m[13],m.groups()
    assert m[5]=='821D4BF8' and all(int(m[i],16)>4096 for i in [4,7,8]) and int(m[9])==expected[key]['sequences']
    rows.append(dict(archive=m[1],resource=m[2],native_actor=m[4],native_model=m[7],native_controller=m[8],original_sequence_ids_bound=int(m[9])))
assert len(rows)==len(expected)==16 and {(r['archive'],r['resource']) for r in rows}==expected.keys()
assert sum(r['original_sequence_ids_bound'] for r in rows)==44 and 'accepted=16/16; no world activation' in log
report=dict(scope='Actual original KFM/NIF/KF typed loading and native actor-animation graph binding/lifetime. NOT playback/curve-output, entity/world attachment, rendering or playable-level proof.',files=rows,original_sequence_ids_bound=44,profile=str(a.profile),completion=completion)
Path('analysis/em1-compatibility/kfm-native-result.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS:16complete native original animation graphs, all44authored sequence IDs bound, full dependency cleanup and record restoration')
