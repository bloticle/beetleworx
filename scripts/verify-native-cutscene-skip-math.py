"""Verify private original skip math in three real restricted native worlds.

Does not assert UI callback delivery, online skipping or full cinematics.
The existing cutscene verifier also checks native return, controls, health,
matching world updates, unchanged normal user files and cooperative exits.
"""
import argparse
import json
from pathlib import Path
import re
import runpy


def verify(root):
    base=runpy.run_path(str(Path(__file__).with_name('verify-native-cutscene-return.py')))['verify'](root)
    result=json.loads((root/'result.json').read_text(encoding='utf-8-sig'))
    histories=[]
    reports=[]
    for process in result['processes']:
        origin=next(c['source_origin'] for c in base['captures'] if c['role']==process['role'])
        release=origin+680
        text=Path(process['log']).read_text(encoding='utf-8',errors='replace')
        armed=re.findall(r'Private native cutscene skip source armed: tick (\d+) input epoch (\d+) native device (\d+);',text)
        assert len(armed)==1, 'Expected one live skip source admission per world'
        first,epoch,user=map(int,armed[0])
        assert epoch==2 and 0<=user<4 and 701<=first<710
        assert 'Private native cutscene skip source rejected' not in text and 'Private native cutscene skip source failed' not in text
        rows=[(int(t),int(e),p=='true',h=='true',r=='true') for t,e,p,h,r in re.findall(
            r'Private native cutscene skip result: tick (\d+) epoch (\d+) prompt (true|false) hold (true|false) release (true|false);',text)]
        assert len(rows)==3, 'Expected exactly the native prompt, delayed hold and release'
        assert rows[0]==(first,2,True,False,False)
        assert first<rows[1][0]<release and rows[1][1:]==(2,False,True,False)
        assert rows[2]==(release,2,False,False,True)
        histories.append(rows)
        reports.append({'role':process['role'],'native_user':user,'source_start':first,'prompt':rows[0][0],'hold':rows[1][0],'release':rows[2][0]})
    assert len(histories)==3 and all(h==histories[0] for h in histories)
    return {'passed':True,'binary_sha256':base['binary_sha256'],'matching_updates':base['matching_updates'],'worlds':reports,'scope':__doc__.strip()}


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);root=p.parse_args().root
    result=verify(root);(root/'native-cutscene-skip-math-validation.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(result,indent=2))
