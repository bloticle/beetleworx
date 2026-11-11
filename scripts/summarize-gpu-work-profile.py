"""Summarize sampled native GPU work; estimates are diagnostic, not frame times."""
import argparse
import hashlib
import json
import re
from pathlib import Path


def summarize(log, shaders, root, game_identities=None):
    known = {}
    native = root / 'build/runtime23/render-target/shader-manifest.json'
    if native.exists():
        for name, data in json.loads(native.read_text()).items():
            known[data['sha256']] = 'SDK/' + name
    built = root / 'build/runtime23/built_shaders_data.json'
    if built.exists():
        for data in json.loads(built.read_text()):
            known[data['dxbc_sha256']] = 'PC/' + data['id']
    effects = json.loads(game_identities.read_text())['shaders'] if game_identities else {}
    identities = {}
    for path in shaders.glob('shader-*.dxbc'):
        sha = hashlib.sha256(path.read_bytes()).hexdigest()
        aliases = effects.get(sha, [])
        name = known.get(sha)
        if not name and aliases:
            name = 'Game/' + ', '.join(aliases[:3]) + (f' (+{len(aliases)-3} aliases)' if len(aliases)>3 else '')
        identities[path.stem.removeprefix('shader-')] = dict(path=str(path), sha256=sha, name=name, effects=aliases)
    pattern = re.compile(r'\[([^]]+)\].*GPU work (\w+) key ([0-9A-F]+) VS ([0-9A-F]+) PS ([0-9A-F]+) MSAA (\d+) MRT (\d+): (\d+) calls, (\d+) completed samples, (\d+) skips; selected ([\d.]+) ms, mean ([\d.]+) ms, max ([\d.]+) ms, rough lifetime estimate ([\d.]+) ms')
    history = {}
    contents = log.read_text(errors='replace')
    labels = {}
    for key, mode, name in re.findall(r'GPU transfer label key ([0-9A-F]+) mode (\d+) name (\w+)', contents):
        labels.setdefault(key, {})[int(mode)] = name
    for match in pattern.finditer(contents):
        time, kind, key, vs, ps, msaa, targets, calls, samples, skips, selected, mean, maximum, estimate = match.groups()
        record = dict(time=time, kind=kind, key=key, vs=vs, ps=ps, msaa=int(msaa), targets=int(targets), calls=int(calls), samples=int(samples), skips=int(skips), selected_ms=float(selected), mean_ms=float(mean), maximum_ms=float(maximum), lifetime_estimate_ms=float(estimate), shader=identities.get(ps), vertex_shader=identities.get(vs))
        if kind == 'copy':
            record['copy_operation'] = {0xc0000001:'CopyBufferRegion',0xc0000002:'CopyTextureRegion',0xc0000003:'CopyResource',0xc0000004:'CopyTiles',0xc0010001:'BufferToTexture',0xc0010002:'TextureToBuffer',0xc0010003:'R8ToStencilPlane',0xc0010004:'OtherDepthStencilTexture',0xc0010005:'ColorTargetTexture',0xc0010006:'OtherTexture'}.get(int(key,16),'Unknown')
        history.setdefault((kind, key), []).append(record)
        record['native_transfer_modes'] = labels.get(key)
    output = []
    for records in history.values():
        last = dict(records[-1])
        if len(records) > 1:
            before = records[-2]
            dc, ds = last['calls'] - before['calls'], last['samples'] - before['samples']
            dt = last['selected_ms'] - before['selected_ms']
            last['last_report_delta'] = dict(previous_time=before['time'], calls=dc, samples=ds, selected_ms=dt, estimated_ms=dt / ds * dc if ds > 0 else None)
        output.append(last)
    output.sort(key=lambda r: r.get('last_report_delta', {}).get('estimated_ms') or 0, reverse=True)
    return dict(log=str(log), shaders=str(shaders), groups=output, scope='Sparse selected GPU work, with serialization overhead, skip bias and fence latency. Last-report deltas avoid lifetime menu counts where consecutive group reports are available. Estimates are candidates for investigation, not measured whole-frame cost or verified speedups.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--shaders', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--game-identities', type=Path)
    args = parser.parse_args()
    report = summarize(args.log, args.shaders or args.log.parent / 'shaders', Path(__file__).resolve().parent.parent, args.game_identities)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    for row in report['groups'][:12]:
        print(row['kind'], row['ps'], row.get('copy_operation') or (row['shader']['name'] if row['shader'] else '(unmapped)'), row.get('last_report_delta'))
