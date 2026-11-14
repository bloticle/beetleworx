"""Check host-relayed chat alongside real native-menu gameplay.

Requires the active movement/action verifier and identical, once-only chat
receipts in every world. Does not establish visual typing, voice, Internet
connectivity, sixteen native worlds, or campaign completion.
"""
import argparse
import json
import math
from pathlib import Path
import re
import runpy

RECEIPT = re.compile(r"Native Room private chat received: sequence (\d+) peer (\d+) color (\d+) text 'Private chat peer (\d+)'; host-authorized identity")
SENT = re.compile(r'Native Room private chat sent: peer (\d+);')


def verify(root, native_hud=False):
    active = runpy.run_path(str(Path(__file__).with_name('verify-native-menu-active-input.py')))['verify'](root)
    record = json.loads((root / 'result.json').read_text(encoding='utf-8-sig'))
    if not record.get('chat_probe'):
        raise AssertionError('Chat probe was not enabled')
    count = record['players']
    captures = []
    authoritative = None
    senders = set()
    for process in record['processes']:
        text = Path(process['log']).read_text(encoding='utf-8', errors='replace')
        sent = list(map(int, SENT.findall(text)))
        if len(sent) != 1 or sent[0] in senders:
            raise AssertionError('Each actual peer must send exactly once')
        senders.add(sent[0])
        if native_hud:
            if 'Native HUD chat callback registered on current movie;' not in text or 'Native HUD chat page initialized by authored GFx;' not in text:
                raise AssertionError('Native HUD did not register and execute its authored chat page')
            size = len('Private chat peer ' + str(sent[0]))
            editor = f'Native HUD chat editor readback accepted: {size} UTF-8 bytes, cursor {size}; authored draft field, gameplay capture true'
            history = f'Native HUD chat history readback accepted: {count} messages; authored colored name and plain message fields'
            if editor not in text or history not in text:
                raise AssertionError('Native HUD did not consume the typed draft and complete colored chat history')
            if sent[0] >= 3:
                source = re.search(r'Private Room SDK input: peer \d+ tick (\d+) elapsed 0 ', text)
                origin = int(source[1])
                if "phase 'move after chat'" not in text:
                    raise AssertionError('No physical SDK movement after chat closed')
                pattern = runpy.run_path(str(Path(__file__).with_name('verify-private-room-input.py')))['TRAJECTORY']
                rows = {int(row[1]): tuple(map(float, row[2:5])) for row in pattern.findall(text) if int(row[0]) == sent[0]}
                a, b = rows[origin+479+2], rows[origin+491+2]
                if math.hypot(b[0]-a[0], b[2]-a[2]) <= .03:
                    raise AssertionError('Native actor did not resume movement after chat')
        rows = [tuple(map(int, row)) for row in RECEIPT.findall(text)]
        if len(rows) != count or [row[0] for row in rows] != list(range(1, count + 1)):
            raise AssertionError('Missing, duplicated, or reordered chat receipts')
        if {row[1] for row in rows} != set(range(1, count + 1)):
            raise AssertionError('Not every sender reached this native world')
        if any(color != peer - 1 or body_peer != peer for _, peer, color, body_peer in rows):
            raise AssertionError('Chat identity/color does not match authoritative roster')
        if authoritative is not None and rows != authoritative:
            raise AssertionError('Native clients disagree on host chat order or identity')
        authoritative = rows
        captures.append({'role': process['role'], 'local_peer': sent[0], 'receipts': len(rows)})
    if senders != set(range(1, count + 1)):
        raise AssertionError('Actual senders do not cover the roster')
    return {'passed': True, 'players': count, 'native_hud_readbacks': native_hud, 'binary_sha256': active['binary_sha256'],
            'captures': captures, 'active_gameplay': active, 'scope': __doc__.strip()}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--native-hud', action='store_true', help='Require actual authored GFx editor/history readbacks in every native world')
    args = parser.parse_args()
    root = args.root
    result = verify(root, native_hud=args.native_hud)
    (root / 'native-room-chat-validation.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: result[k] for k in ('passed', 'players', 'binary_sha256', 'captures')}, indent=2))
