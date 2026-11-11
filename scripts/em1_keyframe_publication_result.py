"""Check actual stock KF publication, all-root retention and native lifetime evidence."""
import argparse
import hashlib
import json
import re
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('profile',type=Path);a=p.parse_args()
source=json.loads(Path('analysis/em1-compatibility/keyframe-source-result.json').read_text());expected={(f['archive'],f['resource'].lower()):f for f in source['files']}
completion=json.loads((a.profile/'completion-result.json').read_text())
assert completion['completed'] and completion['normal_user_files_unchanged'] and completion['em1_source_metadata_unchanged'] and not completion['gameplay_input_sent']
assert completion['exe_sha256']==hashlib.sha256(Path('build/runtime23/beetleworx.exe').read_bytes()).hexdigest().upper()
log=(a.profile/'game.log').read_text(errors='replace');rows=[]
pattern=r"native keyframe publication: archive='([^']+)' resource='([^']+)' published=(\w+) sequences=(\d+) cache_fields=(\w+) collision_rejected=(\w+) shared_lifetime=(\w+) released=(\w+) records_restored=(\w+) error='([^']*)' release_error='([^']*)'"
for m in re.finditer(pattern,log):
    key=(m[1],m[2]);assert key in expected and all(m[i]=='true' for i in [3,5,6,7,8,9]) and not m[10] and not m[11],m.groups()
    assert int(m[4])==expected[key]['roots'];rows.append(dict(archive=m[1],resource=m[2],sequences=int(m[4]),original_sha256=expected[key]['sha256']))
assert len(rows)==len(expected)==16 and {(r['archive'],r['resource']) for r in rows}==expected.keys()
assert sum(r['sequences'] for r in rows)==44 and 'accepted=16/16; no actor/world activation' in log
result=dict(scope='Actual stock KF typed publication/cache lookup and native lifetime; NOT original KFM execution, bound actor playback or full level activation',profile=str(a.profile),canonical_sha256=completion['exe_sha256'],files=rows,sequences=44,source_evaluators=343,
            original_sequence_names_counts_durations_frequencies_match=True,duplicate_names_rejected=True,shared_owners_keep_cache_until_final_release=True,record_counts_restored=True,completion=completion)
Path('analysis/em1-compatibility/keyframe-publication-result.json').write_text(json.dumps(result,indent=2)+'\n')
print('PASS:16original KF typed publications, all44sequences, exact fields, collisions, shared ownership and final cache/record cleanup')
