"""Resolve a local crash log against the linker's public-symbol map."""
import argparse
import bisect
import re
from pathlib import Path
root = Path(__file__).resolve().parents[1]
directory = root / "build/runtime23"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--log", type=Path, default=directory / "crash.log")
parser.add_argument("--map", type=Path, default=directory / "beetleworx.map")
args = parser.parse_args()
log = args.log.read_text()
module = int(re.search(r"module=([0-9A-Fa-f]+)", log)[1],16)
text = args.map.read_text()
preferred = int(re.search(r"Preferred load address is ([0-9A-Fa-f]+)",text)[1],16)
symbols = []
for line in text.splitlines():
    match = re.match(r"\s+[0-9A-Fa-f]+:[0-9A-Fa-f]+\s+(\S+)\s+([0-9A-Fa-f]{16})\s",line)
    if match:
        symbols.append((int(match[2],16), match[1]))
symbols.sort()
addresses=[s[0] for s in symbols]
for frame, value in re.findall(r"frame\[(\d+)\]=([0-9A-Fa-f]+)",log):
    address=int(value,16)
    rebased=address-module+preferred
    index=bisect.bisect_right(addresses,rebased)-1
    if index >= 0 and 0<=rebased-symbols[index][0]<0x100000:
        print(f"{frame}: {symbols[index][1]}+0x{rebased-symbols[index][0]:X}")
    else:
        print(f"{frame}: 0x{address:X}")
