"""Create private XEX compatibility fixtures from the user's verified game."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("executable", type=Path)
parser.add_argument("image", type=Path)
parser.add_argument("raw_image", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
spec = importlib.util.spec_from_file_location("game_check", Path(__file__).with_name("check-game-image.py"))
game_check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(game_check)
source, translated = args.executable.read_bytes(), args.image.read_bytes()
image = args.raw_image.read_bytes()
if hashlib.sha256(translated).hexdigest() != game_check.SUPPORTED_IMAGE or len(image) != len(translated):
    raise SystemExit("Fixtures require the verified decoded image")
profile = game_check.execution_profile(source)
word = lambda offset: struct.unpack_from(">I", source, offset)[0]
headers = {word(24 + i * 8): 28 + i * 8 for i in range(word(20))}
header = bytearray(source[:word(8)])
format_offset = word(headers[0x000003FF])
struct.pack_into(">IHH", header, format_offset, 8, 0, 0)
same = header + image
assert game_check.execution_profile(same) == profile
altered = same.copy()
altered[-1] ^= 1
entry = same.copy()
entry[headers[0x00010100] + 3] ^= 4
imports = same.copy()
imports[word(headers[0x000103FF]) + 4] ^= 1
assert game_check.execution_profile(entry) != profile
assert game_check.execution_profile(imports) != profile
args.output.mkdir(parents=True, exist_ok=True)
for name, data in {
    "original.xex": source,
    "same-image.xex": same,
    "altered-image.xex": altered,
    "altered-entry.xex": entry,
    "altered-imports.xex": imports,
    "truncated.xex": source[:32],
}.items():
    (args.output / name).write_bytes(data)
    print(f"{name}: {len(data)} bytes, {hashlib.sha256(data).hexdigest()}")
