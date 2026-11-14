"""Verify restriction mode cannot dispatch authored cutscene UI callbacks.

Three real native worlds, a carried entry press and a fresh extra-peer hold.
This is a wrong-handler guard test, not proof of skipping an authored scene.
"""
import argparse
import json
from pathlib import Path
import re
import runpy


def verify(root):
    base=runpy.run_path(str(Path(__file__).with_name('verify-native-cutscene-return.py')))['verify'](root)
    result=json.loads((root/'result.json').read_text(encoding='utf-8-sig'))
    assert result['cutscene_skip_input'] and result['cutscene_boundary_movement']
    reports=[]
    for process in result['processes']:
        text=Path(process['log']).read_text(encoding='utf-8',errors='replace')
        origin=next(c['source_origin'] for c in base['captures'] if c['role']==process['role'])
        assert 'Native Room cutscene UI callback delivered:' not in text
        assert 'Native Room cutscene UI receiver armed:' not in text
        assert 'Native cutscene UI could not deliver' not in text
        assert 'original state 3' in text
        rows=[(int(t),int(e),p=='true',h=='true',r=='true') for t,e,p,h,r in re.findall(
            r'Private native cutscene skip result: tick (\d+) epoch (\d+) prompt (true|false) hold (true|false) release (true|false);',text)]
        expected=[(702,2,True,False,False),(710,2,False,True,False),(origin+680,2,False,False,True)]
        if process['role']=='client-3':expected += [(origin+800,2,True,False,False),(origin+808,2,False,True,False),(origin+820,2,False,False,True)]
        assert rows==expected, 'Private original math must observe actual held/fresh SDK samples without granting native UI dispatch'
        reports.append({'role':process['role'],'private_decisions':rows,'native_ui_callbacks':0})
    return {'passed':True,'binary_sha256':base['binary_sha256'],'matching_updates':base['matching_updates'],'worlds':reports,'scope':__doc__.strip()}


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);root=p.parse_args().root
    result=verify(root);(root/'native-cutscene-ui-guards-validation.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(result,indent=2))
