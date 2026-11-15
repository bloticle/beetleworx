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
    log = (root / "game.log").read_text(encoding="utf-8", errors="replace")
    lifecycle = []
    lifecycle_positions = []
    pattern = r"Private native entity lifecycle: (insert|remove) tick (\d+) agreed (true|false) entity ([0-9A-F]{8}) GUID ([0-9A-F/]+) vtable [0-9A-F]+ caller ([0-9A-F]{8})"
    for match in re.finditer(pattern, log):
        action, tick, agreed, entity, guid, caller = match.groups()
        lifecycle.append({"action": action, "tick": int(tick), "agreed": agreed == "true", "local_entity": entity, "guid": guid, "caller": caller})
        lifecycle_positions.append(match.start())
    exact = [index for index, row in enumerate(lifecycle) if row["guid"].replace("/", "").lower() == identity.hex()]
    context = []
    for index in exact:
        event = lifecycle[index]
        # Addresses correlate events within one local observed lifetime only.
        # Never use an equal address to pair objects across host and guest.
        birth = next((row for row in reversed(lifecycle[:index + 1]) if row["local_entity"] == event["local_entity"] and row["action"] == "insert"), None)
        entry = {"matching_event": event, "nearest_prior_local_insert": birth,
                 "guid_differs_from_insert": bool(birth and birth["guid"] != event["guid"])}
        if entry not in context:
            context.append(entry)
    # Clone output identifies the final generated GUID, which can differ from
    # the constructor's registration-time GUID recorded by the lifecycle hook.
    clone_context = []
    clone_pattern = r"Private native entity clone: tick (\d+) caller ([0-9A-F]+) template ([0-9A-F]+) readable (true|false) name '(.*?)' ID ([0-9A-F/]+), flags (\d+)/(\d+), result-valid true entity ([0-9A-F]+) GUID ([0-9A-F/]+)"
    for match in re.finditer(clone_pattern, log):
        tick, caller, prototype, readable, template_name, template_id, initialize, new_guid, entity, guid = match.groups()
        if guid.replace("/", "").lower() != identity.hex():
            continue
        stack = re.findall(rf"Private native entity factory stack: tick {tick} entity {entity} callers ([0-9A-F/]+)", log)
        birth = next((row for row, position in zip(reversed(lifecycle), reversed(lifecycle_positions))
                      if position < match.start() and row["local_entity"] == entity and row["action"] == "insert"), None)
        clone_context.append({"tick": int(tick), "caller": caller, "local_prototype": prototype,
                              "template_readable": readable == "true", "template_name": template_name, "template_guid": template_id,
                              "initialize": int(initialize), "generate_guid": int(new_guid), "local_entity": entity,
                              "registration_guid": birth["guid"] if birth else None,
                              "guid_changed_during_clone": bool(birth and birth["guid"] != guid),
                              "factory_callers": stack[-1].split("/") if stack else []})
    spawner_context = []
    operation_pattern = r"Private native spawner operation: tick (\d+) caller ([0-9A-F]+) component ([0-9A-F]+), owner-valid (true|false) GUID ([0-9A-F/]+), anchor-valid (true|false) GUID ([0-9A-F/]+), template ID ([0-9A-F/]+), flags (\d+)/(\d+)/(\d+), result-valid true entity ([0-9A-F]+) GUID ([0-9A-F/]+), CRT-valid (true|false) states ([0-9A-F]+)/([0-9A-F]+)"
    for tick, caller, component, owner_valid, owner, anchor_valid, anchor, template, index, ordinal, flag, entity, guid, crt_valid, before, after in re.findall(operation_pattern, log):
        if guid.replace("/", "").lower() != identity.hex():
            continue
        spawner_context.append({"tick": int(tick), "caller": caller, "local_component": component,
                                "owner_valid": owner_valid == "true", "owner_guid": owner,
                                "anchor_valid": anchor_valid == "true", "anchor_guid": anchor,
                                "template_guid": template, "flags": [int(index), int(ordinal), int(flag)],
                                "local_entity": entity, "crt_owner_unchanged": crt_valid == "true",
                                "crt_before": before, "crt_after": after})
    return {
        "root": str(root), "sample_count": len(observations),
        "matching_sample_count": len(present), "catalog_name": name,
        "first_matching_sample": present[0] if present else None,
        "last_matching_sample": present[-1] if present else None,
        "last_sample": observations[-1] if observations else None,
        "presence_transitions": transitions,
        "lifecycle_context": context,
        "clone_context": clone_context,
        "spawner_context": spawner_context,
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
        "limits": "30-tick samples can miss transient entities. Lifecycle hooks report native insert/remove but not every GUID write. Local addresses correlate one local lifetime only, never cross-peer identities.",
    }
    encoded = json.dumps(result, indent=2, allow_nan=False) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")


if __name__ == "__main__":
    main()
