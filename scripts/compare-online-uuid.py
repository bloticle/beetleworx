"""Map real native entity identity disagreements to GUID creation callsites.

Addresses and handles are diagnostic correspondence only, never an identity
equivalence rule. Run both copies with em2_trace_online_uuid and the native
entity probe enabled. This does not certify complete world synchronization.
"""
import argparse
from collections import Counter
import importlib.util
import json
from pathlib import Path
import re


PATTERN = re.compile(
    r'Native GUID create serial=(\d+) caller=([0-9A-F]{8}) ordinal=(\d+) '
    r'thread=(\d+) output=([0-9A-F]{8}) id=([0-9A-F]{32})'
    r'(?: parents=([0-9A-F]{8}),([0-9A-F]{8}))?'
    r'(?: source=([0-9A-F]{32}))?'
    r"(?: level='([^']*)')?")


def creations(text):
    events = []
    seen_serials = set()
    caller_ordinals = {}
    source_ordinals = Counter()
    for match in PATTERN.finditer(text):
        serial, caller, ordinal, thread, output, identity, parent, ancestor, source, level = match.groups()
        serial, ordinal = int(serial), int(ordinal)
        if serial in seen_serials or ordinal != caller_ordinals.get(caller, 0) + 1:
            raise ValueError('Duplicate or missing GUID provenance sequence')
        seen_serials.add(serial)
        caller_ordinals[caller] = ordinal
        source = source or '0' * 32
        source_ordinals[(caller, source)] += 1
        events.append(dict(serial=serial, caller=caller, ordinal=ordinal,
                           thread=int(thread), output=output, id=identity,
                           parents=[parent or '00000000', ancestor or '00000000'],
                           source=source,
                           level=level,
                           source_ordinal=source_ordinals[(caller, source)] if source != '0' * 32 else 0))
    return events


def identity_sets(host, client):
    """Compare complete ID sets, without assuming matching native addresses."""
    common = sorted(host.keys() & client.keys())
    rows = []
    for tick in common:
        h = set(host[tick]['records'])
        c = set(client[tick]['records'])
        rows.append(dict(tick=tick, host_count=len(h), client_count=len(c),
                         host_only=sorted(h - c), client_only=sorted(c - h)))
    return dict(common_snapshot_ticks=common,
                host_only_snapshot_ticks=sorted(host.keys() - client.keys()),
                client_only_snapshot_ticks=sorted(client.keys() - host.keys()),
                identity_sets_match=(len(rows) >= 2 and host.keys() == client.keys() and
                                     all(r['host_count'] > 0 and r['client_count'] > 0 and
                                         not r['host_only'] and not r['client_only'] for r in rows)),
                snapshots=rows)


def compare(root):
    module_path = Path(__file__).with_name('compare-native-entities.py')
    spec = importlib.util.spec_from_file_location('native_entities', module_path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    sides = {}
    for side in ('host', 'client'):
        directory = root / side
        events = creations((directory / 'startup.log').read_text(errors='replace'))
        if not events:
            raise ValueError(f'{side}: no GUID creation trace; enable em2_trace_online_uuid')
        by_id = {}
        for event in events:
            by_id.setdefault(event['id'], []).append(event)
        sides[side] = dict(events=events, by_id=by_id,
                           snapshots=module.snapshots(directory / 'entities'))
    common = sorted(set(sides['host']['snapshots']) & set(sides['client']['snapshots']))
    if not common:
        raise ValueError('No common native entity snapshot tick')
    tick = common[0]
    host = sides['host']['snapshots'][tick]['records']
    client = sides['client']['snapshots'][tick]['records']
    host_handles = {value['handle']: (identity, value) for identity, value in host.items()}
    client_handles = {value['handle']: (identity, value) for identity, value in client.items()}
    differences = []
    for handle in sorted(set(host_handles) & set(client_handles)):
        hid, hr = host_handles[handle]
        cid, cr = client_handles[handle]
        if hid == cid:
            continue
        hcreation = sides['host']['by_id'].get(hid, [])
        ccreation = sides['client']['by_id'].get(cid, [])
        same_event = (len(hcreation) == len(ccreation) == 1 and
                      (hcreation[0]['caller'], hcreation[0]['ordinal'], hcreation[0]['parents']) ==
                      (ccreation[0]['caller'], ccreation[0]['ordinal'], ccreation[0]['parents']))
        differences.append(dict(
            diagnostic_native_handle=handle, host_id=hid, client_id=cid,
            host_creation=hcreation, client_creation=ccreation,
            same_creation_callsite_and_ordinal=same_event,
            same_nonzero_authored_source=(len(hcreation) == len(ccreation) == 1 and
                                         hcreation[0]['source'] != '0' * 32 and
                                         hcreation[0]['source'] == ccreation[0]['source']),
            same_vtable=hr['vtable'] == cr['vtable'],
            same_component_presence=hr['components'] == cr['components'],
            same_position_bits=hr['position_bits'] == cr['position_bits']))
    counts = {}
    for side, data in sides.items():
        counts[side] = dict(events=len(data['events']),
                           callsites=dict(sorted(Counter(e['caller'] for e in data['events']).items())),
                           duplicate_generated_ids=sum(len(v) > 1 for v in data['by_id'].values()))
    coverage = identity_sets(sides['host']['snapshots'], sides['client']['snapshots'])
    integrity = True
    for side, data in sides.items():
        observations, invalid = module.log_observations(root / side)
        integrity &= not invalid and all(
            t in observations and snapshot['digest'] == observations[t]['digest']
            for t, snapshot in data['snapshots'].items())
    coverage['snapshot_integrity_valid'] = bool(integrity)
    coverage['native_identity_verified'] = coverage['identity_sets_match'] and bool(integrity)
    return dict(source=str(root), tick=tick, trace=counts, identity_coverage=coverage,
                differing_identity_count=len(differences),
                matched_creation_events=sum(d['same_creation_callsite_and_ordinal'] for d in differences),
                matched_authored_sources=sum(d['same_nonzero_authored_source'] for d in differences),
                disagreements=differences,
                scope='Native GUID provenance at first common entity snapshot, plus complete ID sets at all captured ticks. '
                      'Native handles only associate diagnostic records; no IDs are normalized, '
                      'and complete AI/physics/Lua state is not verified.')


def self_test():
    line = ('Native GUID create serial=1 caller=827A8384 ordinal=1 thread=7 '
            'output=10000000 id=00000001000000020000000300000004')
    events = creations('prefix ' + line + '\n')
    assert len(events) == 1 and events[0]['caller'] == '827A8384'
    assert events[0]['id'] == '00000001000000020000000300000004'
    parented = creations(line + ' parents=82507304,827B9A18')
    assert parented[0]['parents'] == ['82507304', '827B9A18']
    sourced = creations(line + ' parents=82507304,827B9A18 source=' + '1' * 32)
    assert sourced[0]['source'] == '1' * 32
    for invalid in (line + '\n' + line, line.replace('ordinal=1', 'ordinal=2')):
        try:
            creations(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError('Invalid/duplicate provenance accepted')
    assert creations('unrelated log') == []
    def snapshot(*ids):
        return dict(records={identity: {} for identity in ids})
    host = {1: snapshot('a', 'b'), 2: snapshot('a', 'b')}
    assert identity_sets(host, host)['identity_sets_match']
    assert not identity_sets({}, {})['identity_sets_match']
    assert not identity_sets({1: snapshot()}, {1: snapshot()})['identity_sets_match']
    assert not identity_sets(host, {1: snapshot('a', 'b')})['identity_sets_match']
    # A differing actor need not share a native handle to invalidate identity.
    assert not identity_sets(host, {1: snapshot('a', 'b'), 2: snapshot('a', 'c')})['identity_sets_match']
    assert not identity_sets(host, {1: snapshot('a', 'b'), 2: snapshot('a', 'b', 'c')})['identity_sets_match']


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', nargs='?', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
        print('GUID provenance parser tests passed')
    else:
        if not args.root:
            parser.error('root or --self-test is required')
        report = compare(args.root)
        encoded = json.dumps(report, indent=2) + '\n'
        if args.output:
            args.output.write_text(encoded, encoding='utf-8')
            print(f"{report['differing_identity_count']} identity disagreements; "
                  f"{report['matched_creation_events']} matched creation events; {args.output}")
        else:
            print(encoded, end='')
