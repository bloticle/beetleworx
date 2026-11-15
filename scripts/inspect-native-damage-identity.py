"""Compare a failed native Damage identity against private Room snapshots.

Read-only diagnostics. Snapshot observations do not prove lifecycle causality:
the sampler may miss entities born and destroyed between its 30-tick samples.
"""
import argparse
import json
import re
import struct
from pathlib import Path


def inspect_peer(root, identity):
    directory = root / "entities"
    observations = []
    for path in sorted(directory.glob("*.bin"), key=lambda p: int(p.stem)):
        data = path.read_bytes()
        if len(data) < 12 or data[:8] != b"EM2ENT1\0":
            raise ValueError(f"Unsupported snapshot: {path}")
        count = struct.unpack_from(">I", data, 8)[0]
        if count > 100000 or len(data) != 12 + count * 72:
            raise ValueError(f"Malformed snapshot: {path}")
        record = None
        for offset in range(12, len(data), 72):
            if data[offset:offset + 16] != identity:
                continue
            handle, vtable = struct.unpack_from(">II", data, offset + 16)
            positioned = struct.unpack_from(">I", data, offset + 56)[0]
            record = {
                "handle": f"{handle:08X}", "vtable": f"{vtable:08X}",
                "component_mask": data[offset + 24:offset + 56].hex(),
                "position": list(struct.unpack_from(">fff", data, offset + 60)) if positioned else None,
            }
            break
        observations.append({"tick": int(path.stem), "present": record is not None, "record": record})
    present = [row for row in observations if row["present"]]
    transitions = []
    for index, row in enumerate(observations):
        if not index or row["present"] != observations[index - 1]["present"]:
            transitions.append({"sample_tick": row["tick"], "present": row["present"]})
    name = None
    labels = directory / "names.tsv"
    if labels.exists():
        for line in labels.read_text(encoding="utf-8", errors="replace").splitlines()[1:]:
            fields = line.split("\t", 3)
            if len(fields) == 4 and fields[0].lower() == identity.hex():
                name = fields[3]
                break
    return {
        "root": str(root), "sample_count": len(observations),
        "matching_sample_count": len(present), "catalog_name": name,
        "first_matching_sample": present[0] if present else None,
        "last_matching_sample": present[-1] if present else None,
        "last_sample": observations[-1] if observations else None,
        "presence_transitions": transitions,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pair", type=Path)
    parser.add_argument("--guid", help="Inspect a specific four-word GUID, with or without slashes, instead of the last missing-reference error.")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    pair = json.loads((args.pair / "pair.json").read_text(encoding="utf-8-sig"))
    guest_root = Path(pair["guest"]["root"])
    log = (guest_root / "game.log").read_text(encoding="utf-8", errors="replace")
    missing = re.findall(r"Native Damage replay missing (target|instigator) GUID ([0-9A-F]{8})/([0-9A-F]{8})/([0-9A-F]{8})/([0-9A-F]{8})", log)
    if args.guid:
        text = args.guid.replace("/", "")
        if not re.fullmatch(r"[0-9A-Fa-f]{32}", text):
            parser.error("--guid requires four eight-digit hexadecimal words.")
        kind, words = "requested", [text[index:index + 8].upper() for index in range(0, 32, 8)]
    else:
        if not missing:
            parser.error("The guest has not logged a missing Damage identity; keep observing its current process.")
        kind, *words = missing[-1]
    identity = bytes.fromhex("".join(words))
    result = {
        "missing_kind": kind, "guid": "/".join(words),
        "host": inspect_peer(Path(pair["host"]["root"]), identity),
        "guest": inspect_peer(guest_root, identity),
        "limits": "30-tick samples and the initial name catalog cannot prove exact creation/destruction order or observe every transient entity.",
    }
    encoded = json.dumps(result, indent=2, allow_nan=False) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")


if __name__ == "__main__":
    main()
