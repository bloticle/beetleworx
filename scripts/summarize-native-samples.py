"""Group same-executable guest instruction samples by nearest linker-map symbol.

These are intrusive wall-time samples (including waits), not CPU percentages.
Always use the map from the exact sampled build. Folded symbol aliases and
nearest-symbol boundaries require verification before choosing a native hook.
"""
import argparse
import gzip
import bisect
from collections import Counter, defaultdict
import json
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('samples', type=Path)
    parser.add_argument('map', type=Path)
    parser.add_argument('--skip', type=int, default=0, help='Exclude this many initial samples, for example boot/loading')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    with (gzip.open(args.map, 'rt', errors='replace') if args.map.suffix == '.gz' else args.map.open(errors='replace')) as source:
        mapping = source.read()
    preferred = int(re.search(r'Preferred load address is ([0-9A-Fa-f]+)', mapping)[1], 16)
    aliases = defaultdict(set)
    for match in re.finditer(r'^\s*0001:[0-9A-Fa-f]+\s+(\S+)\s+([0-9A-Fa-f]{16})\s', mapping, re.M):
        if not match[1].startswith('.weak.'):
            aliases[int(match[2], 16)].add(match[1])
    addresses = sorted(aliases)
    counts = Counter()
    offsets = defaultdict(Counter)
    modules = Counter()
    selected = 0
    read = 0
    raw = args.samples.read_text(encoding='utf-8', errors='replace')
    lines = raw.splitlines()
    if raw and not raw.endswith('\n'):
        lines = lines[:-1]  # A live writer may be between buffered writes.
    for line in lines:
        if line.startswith('#') or not line.strip():
            continue
        fields = line.split('\t', 2)
        if len(fields) != 3:
            raise ValueError(f'Invalid sample row: {line!r}')
        read += 1
        if read <= args.skip:
            continue
        selected += 1
        rip, base = int(fields[0], 16), int(fields[1], 16)
        module = re.split(r'[\\/]', fields[2])[-1] or 'unknown'
        modules[module] += 1
        if module.lower() not in ('beetleworx.exe','em2recomp.exe'):
            counts[(module, rip-base)] += 1
            continue
        mapped = rip-base+preferred
        index = bisect.bisect_right(addresses, mapped)-1
        address = addresses[index] if index >= 0 else mapped
        counts[(module, address)] += 1
        offsets[address][mapped-address] += 1
    result = {
        'samples': str(args.samples), 'map': str(args.map), 'skipped': args.skip,
        'selected_samples': selected, 'modules': dict(modules),
        'interpretation': 'Intrusive wall-time IP hits, including waits. Nearest map symbols; aliases may be folded. Not CPU percentages or a speed benchmark.',
        'hotspots': [],
    }
    for (module, address), count in counts.most_common(30):
        names = sorted(aliases.get(address, ()), key=lambda n: (not n.startswith('__imp__sub_'), n)) if module.lower() in ('beetleworx.exe','em2recomp.exe') else []
        result['hotspots'].append({'module': module, 'map_address_or_module_offset': f'{address:X}',
                                  'hits': count, 'fraction_of_selected': count/selected,
                                  'symbol_alias_count': len(names), 'symbol_aliases': names[:6],
                                  'hot_offsets': [{'offset': f'{offset:X}', 'hits': n}
                                                  for offset, n in offsets[address].most_common(6)] if names else []})
    formatted = json.dumps(result, indent=2)+'\n'
    if args.output:
        args.output.write_text(formatted)
    print(formatted)


if __name__ == '__main__':
    main()
