"""Verify cooperative guest exit before native title teardown in real games.

Requires native menu/world verification and normal process exits. Does not
establish shutdown during every loader/save/debugger state or a full campaign.
"""
import argparse
from datetime import datetime
import json
from pathlib import Path
import re
import runpy


def verify(root):
    result = json.loads((root/'result.json').read_text(encoding='utf-8-sig'))
    menu = runpy.run_path(str(Path(__file__).with_name('verify-native-room-menu-roster.py')))['verify'](root, allow_scripted=bool(result.get('scripted_sdk_input')))
    phases = ['requesting cooperative guest exit', 'guest threads exited before module teardown', 'releasing stopped audio clients', 'guest workers stopped']
    captures = []
    for process in result['processes']:
        text = Path(process['log']).read_text(encoding='utf-8', errors='replace')
        assert process.get('close_messages', 0) > 0 and not process.get('slow_shutdown')
        rows = []
        for phase in phases:
            matches = re.findall(r'^\[([^\]]+)\][^\r\n]*?\[t(\d+)\] Title shutdown: '+re.escape(phase)+r'\s*$', text, re.M)
            assert len(matches) == 1, f'{process["role"]}: missing/duplicate shutdown phase {phase}'
            rows.append((datetime.fromisoformat(matches[0][0]), int(matches[0][1])))
        assert len({row[1] for row in rows}) == 1, 'Shutdown changed owner thread'
        assert all(a[0] <= b[0] for a,b in zip(rows,rows[1:])), 'Native title cleanup ran before cooperative join'
        assert text.count('Execution complete') == 1, 'Native main-thread watcher did not finish'
        assert re.search(r'Beetleworx update shutdown complete:', text), 'Final host teardown did not complete'
        captures.append({'role':process['role'], 'shutdown_owner':rows[0][1], 'guest_join_ms':(rows[1][0]-rows[0][0]).total_seconds()*1000, 'exit_code':process['exit_code']})
    return {'passed':True, 'players':result['players'], 'binary_sha256':menu['binary_sha256'], 'matching_updates':menu['longest_matching_updates'], 'captures':captures, 'scope':__doc__.strip()}


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);root=p.parse_args().root
    result=verify(root)
    (root/'native-guest-shutdown-validation.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,indent=2))
