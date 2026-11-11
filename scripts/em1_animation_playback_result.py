"""Check real native selection/update and observed detached-model motion."""
import argparse
import hashlib
import json
import re
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('profile',type=Path);a=p.parse_args()
completion=json.loads((a.profile/'completion-result.json').read_text());log=(a.profile/'game.log').read_text(errors='replace')
assert completion['completed'] and completion['normal_user_files_unchanged'] and completion['em1_source_metadata_unchanged'] and not completion['gameplay_input_sent']
assert completion['exe_sha256']==hashlib.sha256(Path('build/runtime23/beetleworx.exe').read_bytes()).hexdigest().upper()
source=json.loads(Path('analysis/em1-compatibility/kfm-closure-result.json').read_text());expected=[f['resource'] for f in source['files']];rows=[]
for m in re.finditer(r"native animation playback: resource='([^']+)' selected=(\d+) updates=(\d+) observed_nodes=(\d+) changed_transform_samples=(\d+) finite=(\w+) success=(\w+) error='([^']*)'",log):
    assert m[1] in expected and m[6]==m[7]=='true' and not m[8]
    assert int(m[2])>0 and int(m[3])==int(m[2])*8 and int(m[4])>0
    rows.append(dict(resource=m[1],selected=int(m[2]),updates=int(m[3]),observed_nodes=int(m[4]),changed_transform_samples=int(m[5])))
assert len(rows)==16 and sorted(r['resource'] for r in rows)==sorted(expected)
assert sum(r['selected'] for r in rows)==44 and sum(r['updates'] for r in rows)==352
for suffix in ['mickey_idle_paint_trans_film_cam.kfm_wii','gv_zonea_door03.kfm_wii','pipe_knob_01.kfm_wii','gv_zoneb_entrydoora1.kfm_wii']:
    assert any(r['resource'].endswith(suffix) and r['changed_transform_samples']>0 for r in rows),suffix
assert 'accepted=16/16; no world activation' in log
report=dict(scope='Stock sequence-ID selection and absolute-time actor/model update; original detached graph transform motion and finite outputs observed. NOT source-curve numerical parity, visibility/material/morph channel output, rendered actor playback, entity/world attachment or playable-level proof.',profile=str(a.profile),files=rows,sequences_selected=44,native_updates=352,changing_graphs=sum(r['changed_transform_samples']>0 for r in rows),completion=completion)
Path('analysis/em1-compatibility/animation-playback-result.json').write_text(json.dumps(report,indent=2)+'\n')
print(f'PASS:44original native sequence selections,352updates, finite model transforms; motion observed in {report["changing_graphs"]}/16graphs; world/render output untested')
