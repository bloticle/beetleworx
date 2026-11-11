"""Locate unique generated function definitions for per-source build policies."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('directory', type=Path)
parser.add_argument('symbols', nargs='+')
args = parser.parse_args()
owners = {name: [] for name in args.symbols}
needles = {name: f'PPC_FUNC_IMPL({name}) {{'.encode('ascii') for name in owners}
for path in sorted(args.directory.glob('ppc_recomp.*.cpp')):
    data = path.read_bytes()
    for name, needle in needles.items():
        count = data.count(needle)
        if count:
            owners[name].extend([path.resolve()] * count)
for name, paths in owners.items():
    if len(paths) != 1:
        raise SystemExit(f'Expected one generated definition of {name}, found {len(paths)}')
print(';'.join(path.as_posix() for path in sorted({paths[0] for paths in owners.values()})))
