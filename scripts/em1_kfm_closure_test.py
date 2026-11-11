"""Resolve complete original KFM model/clip dependencies, never placeholders."""
import argparse
import ctypes as c
import json
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--library',type=Path,required=True);p.add_argument('--image',type=Path,required=True)
a=p.parse_args();d=c.CDLL(str(a.library.resolve()));d.kfm_closure.argtypes=[c.c_wchar_p,c.c_char_p,c.c_char_p,c.POINTER(c.c_uint32)];d.kfm_closure.restype=c.c_char_p
managers=json.loads(Path('analysis/em1-compatibility/kfm-path-bindings-result.json').read_text())['files']
clips={(f['archive'],f['resource'].lower()):f for f in json.loads(Path('analysis/em1-compatibility/keyframe-source-result.json').read_text())['files']};results=[]
for manager in managers:
    if manager['archive'] is None:continue
    deps=manager['dependencies'];models={(f['archive'],f['resource']) for f in deps if f['kind']=='4e494600'};keyframes={(f['archive'],f['resource']) for f in deps if f['kind']=='4b460000'}
    expected=[len(models),len(keyframes),sum(clips[f]['roots'] for f in keyframes)];counts=(c.c_uint32*3)(91,92,93)
    error=d.kfm_closure(str(a.image),manager['archive'].encode(),manager['resource'].encode(),counts);assert not error,(manager['resource'],error)
    assert list(counts)==expected,(manager['resource'],list(counts),expected)
    bad=(c.c_uint32*3)(91,92,93);assert d.kfm_closure(str(a.image),b'packfiles/bsq.pak',manager['resource'].encode(),bad) and list(bad)==[91,92,93]
    results.append(dict(archive=manager['archive'],resource=manager['resource'],models=expected[0],keyframes=expected[1],sequences=expected[2]))
assert len(results)==16
Path('analysis/em1-compatibility/kfm-closure-result.json').write_text(json.dumps(dict(scope='Source complete dependency preparation; actual native manager binding tested separately',files=results),indent=2)+'\n')
print('PASS:16complete original KFM dependencies; actual model/texture/KF preparation; wrong owner rejected without modifying output')
