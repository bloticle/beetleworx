"""Find native EM2 config lookups by literal names and length-seeded CRC32.

Candidate constants still require disassembly: a hash match alone does not
prove that a setting is read, or that changing it improves rendering.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import zlib

ROOT = Path(__file__).resolve().parents[1]


def config_hash(key):
    # 831BCC08 lowercases the name and calls 83103F68(length, bytes, length).
    # 83103A58 implements IEEE CRC32, with the length as the initial seed.
    data = key.lower().encode("ascii")
    return zlib.crc32(data, len(data))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--key", action="append", required=True)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("address_refs", ROOT / "scripts/find-address-references.py")
    refs = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(refs)
    data = (ROOT / "private/image.bin").read_bytes()
    metadata = json.loads((ROOT / "analysis/image.json").read_text(encoding="utf-8-sig"))
    base = metadata["base"]
    targets = {}
    result = []
    for key in args.key:
        value = config_hash(key)
        item = {"key": key, "hash": f"{value:08X}", "literal_addresses": [], "candidates": []}
        targets.setdefault(value, []).append((item, "hash"))
        start = 0
        while (offset := data.find(key.encode("ascii") + b"\0", start)) >= 0:
            address = base + offset
            item["literal_addresses"].append(f"{address:08X}")
            targets.setdefault(address, []).append((item, "literal"))
            start = offset + 1
        result.append(item)
    for pc, register, value, _ in refs.references(data, metadata["sections"], base,
                                                 {v: "candidate" for v in targets}, False):
        for item, kind in targets[value]:
            item["candidates"].append({"pc": f"{pc:08X}", "register": register, "kind": kind})
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
