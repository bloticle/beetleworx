"""Compare diagnostic entity fingerprints and periodic native binary snapshots.

This covers IDs/component presence/observed positions, not complete NPC state.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


def snapshots(folder):
    result = {}
    for path in folder.glob('*.bin'):
        data = path.read_bytes()
        if data[:8] != b'EM2ENT1\0' or len(data) < 12:
            raise ValueError(f'Invalid entity snapshot header: {path}')
        count, = struct.unpack_from('>I', data, 8)
        if count > 65536 or len(data) != 12 + count * 72:
            raise ValueError(f'Invalid entity snapshot length: {path}')
        records = {}
        for index in range(count):
            values = struct.unpack_from('>6I32s4I', data, 12 + index * 72)
            identity = ''.join(f'{word:08X}' for word in values[:4])
            if identity in records:
                raise ValueError(f'Duplicate entity ID: {path}')
            records[identity] = dict(handle=f'{values[4]:08X}', vtable=f'{values[5]:08X}',
                                     components=values[6].hex(), flags=values[7],
                                     position_bits=[f'{word:08X}' for word in values[8:]])
        result[int(path.stem)] = dict(digest=hashlib.sha256(data).hexdigest(), records=records)
    return result


def log_observations(folder):
    text = (folder / 'startup.log').read_text(errors='replace')
    pattern = (r'Native entities v1: tick (\d+), count (\d+), declared (\d+), '
               r'positioned (\d+), buckets (\d+), us (\d+), digest ([0-9a-f]{64})')
    observations = {int(m[0]): dict(count=int(m[1]), declared=int(m[2]), positioned=int(m[3]),
                                  buckets=int(m[4]), microseconds=int(m[5]), digest=m[6])
                    for m in re.findall(pattern, text)}
    newer = (r'Native entities v1: tick (\d+), count (\d+), header12 ([0-9A-Fa-f]+), components (\d+), '
             r'positioned (\d+), buckets (\d+), us (\d+), digest ([0-9a-f]{64})')
    for m in re.findall(newer,text):
        observations[int(m[0])] = dict(count=int(m[1]), header_word_12=int(m[2],16), component_count=int(m[3]),
                                       positioned=int(m[4]), buckets=int(m[5]), microseconds=int(m[6]), digest=m[7])
    invalid = re.findall(r"Native entities v1: tick (\d+), invalid '([^']+)' at ([0-9A-Fa-f]+)", text)
    return observations, invalid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    host, host_invalid = log_observations(args.root / 'host')
    client, client_invalid = log_observations(args.root / 'client')
    ticks = sorted(host.keys() & client.keys())
    different = [tick for tick in ticks if host[tick]['digest'] != client[tick]['digest']]
    hfiles, cfiles = snapshots(args.root / 'host/entities'), snapshots(args.root / 'client/entities')
    import csv
    def names(role):
        path=args.root/role/'entities/names.tsv'
        if not path.exists():return {}
        with path.open(newline='') as stream:return {row['id']:row.get('name','') for row in csv.DictReader(stream,delimiter='\t')}
    hnames,cnames=names('host'),names('client')
    comparisons = []
    for tick in sorted(hfiles.keys() & cfiles.keys()):
        h, c = hfiles[tick]['records'], cfiles[tick]['records']
        fields = []
        for identity in sorted(h.keys() & c.keys()):
            changed = {key: [h[identity][key], c[identity][key]] for key in h[identity]
                       if h[identity][key] != c[identity][key]}
            if changed:
                fields.append(dict(identity=identity, changed=changed))
        by_handle=[]
        hh={value['handle']:(identity,value) for identity,value in h.items()}
        ch={value['handle']:(identity,value) for identity,value in c.items()}
        for handle in sorted(hh.keys()&ch.keys()):
            hi,hv=hh[handle];ci,cv=ch[handle]
            delta={key:[hv[key],cv[key]] for key in hv if hv[key]!=cv[key]}
            if hi!=ci:delta['identity']=[hi,ci]
            if delta:by_handle.append(dict(handle=handle,host_name=hnames.get(hi,''),client_name=cnames.get(ci,''),changed=delta))
        comparisons.append(dict(by_native_handle_diagnostic_only=by_handle,tick=tick, host_count=len(h), client_count=len(c),
                                host_only=sorted(h.keys() - c.keys()),
                                client_only=sorted(c.keys() - h.keys()), differences=fields))
    file_hashes_valid = all(tick in host and value['digest'] == host[tick]['digest'] for tick, value in hfiles.items()) and all(tick in client and value['digest'] == client[tick]['digest'] for tick, value in cfiles.items())
    report = dict(file_hashes_match_logs=file_hashes_valid, root=str(args.root), common_ticks=len(ticks), first_tick=ticks[0] if ticks else None,
                  last_tick=ticks[-1] if ticks else None, different_ticks=different,
                  host_invalid=host_invalid, client_invalid=client_invalid,
                  first_observations=dict(host=host[ticks[0]], client=client[ticks[0]]) if ticks else {},
                  periodic_snapshots=comparisons,
                  scope='Diagnostic IDs, native class/component presence and available position fields; not full NPC/world state')
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(common_ticks=len(ticks), different_ticks=len(different), host_invalid=len(host_invalid), client_invalid=len(client_invalid), file_hashes_match_logs=file_hashes_valid, periodic_pairs=len(comparisons)),indent=2))


if __name__ == '__main__':
    main()
