"""Compare observed native spawn batches; never modifies gameplay or matches pointers.

Only completed batches are compared. Native logs can end mid-operation after a
failure; those incomplete observations are retained separately, not called passes.
"""
import argparse
import json
import re
from pathlib import Path


BEGIN = re.compile(r"Private native spawn batch begin: tick (\d+) caller ([0-9A-F]+), owner-valid (true|false) GUID ([0-9A-F/]+), anchor-valid (true|false) GUID ([0-9A-F/]+), selector (-?\d+), flag (\d+), collection (true|false), count (\d+), choice (\d+), used ([0-9A-F]+), CRT-valid (true|false) state ([0-9A-F]+)")
END = re.compile(r"Private native spawn batch end: tick (\d+) caller ([0-9A-F]+), owner GUID ([0-9A-F/]+), owner-unchanged (true|false), count (\d+), choice (\d+), used ([0-9A-F]+), result-valid (true|false) GUID ([0-9A-F/]+), CRT-valid (true|false) state ([0-9A-F]+)")
CHILD = re.compile(r"Private native spawner operation: tick (\d+) caller ([0-9A-F]+) component [0-9A-F]+, owner-valid (true|false) GUID ([0-9A-F/]+), anchor-valid (true|false) GUID ([0-9A-F/]+), template ID ([0-9A-F/]+), flags (\d+)/(\d+)/(\d+), result-valid (true|false) entity [0-9A-F]+ GUID ([0-9A-F/]+), CRT-valid (true|false) states ([0-9A-F]+)/([0-9A-F]+)")


def read_batches(root):
    stack, complete = [], []
    for line in (root / "game.log").read_text(encoding="utf-8", errors="replace").splitlines():
        if match := BEGIN.search(line):
            tick, caller, valid, owner, av, anchor, selector, flag, collection, count, choice, used, crt, state = match.groups()
            stack.append({"tick": int(tick), "caller": caller, "owner_valid": valid == "true", "owner": owner,
                          "anchor_valid": av == "true", "anchor": anchor, "selector": int(selector), "flag": int(flag),
                          "collection": collection == "true", "before": {"count": int(count), "choice": int(choice), "used": used,
                          "crt_valid": crt == "true", "crt": state}, "children": []})
        elif match := END.search(line):
            tick, caller, owner, unchanged, count, choice, used, valid, result, crt, state = match.groups()
            if not stack or (stack[-1]["tick"], stack[-1]["caller"], stack[-1]["owner"]) != (int(tick), caller, owner):
                raise ValueError(f"Unmatched native batch end in {root}")
            batch = stack.pop()
            batch["after"] = {"owner_unchanged": unchanged == "true", "count": int(count), "choice": int(choice),
                              "used": used, "result_valid": valid == "true", "result": result,
                              "crt_valid": crt == "true", "crt": state}
            complete.append(batch)
        elif stack and (match := CHILD.search(line)):
            tick, caller, valid, owner, av, anchor, template, index, ordinal, flag, rv, result, crt, before, after = match.groups()
            stack[-1]["children"].append({"tick": int(tick), "caller": caller, "owner_valid": valid == "true", "owner": owner,
                                         "anchor_valid": av == "true", "anchor": anchor, "template": template,
                                         "flags": [int(index), int(ordinal), int(flag)], "result_valid": rv == "true", "result": result,
                                         "crt_valid": crt == "true", "crt_before": before, "crt_after": after})
    return complete, stack


def compare(pair_root):
    pair = json.loads((pair_root / "pair.json").read_text(encoding="utf-8-sig"))
    host, host_incomplete = read_batches(Path(pair["host"]["root"]))
    guest, guest_incomplete = read_batches(Path(pair["guest"]["root"]))
    # Preserve native call order, including nested completion order. An absent
    # call is a divergence, never aligned away by matching position or address.
    first = next((i for i, (h, g) in enumerate(zip(host, guest)) if h != g), min(len(host), len(guest)))
    def gameplay(value):
        if isinstance(value, dict):
            return {key: gameplay(item) for key, item in value.items() if not key.startswith("crt")}
        if isinstance(value, list):
            return [gameplay(item) for item in value]
        return value
    first_gameplay = next((i for i, (h, g) in enumerate(zip(host, guest)) if gameplay(h) != gameplay(g)), min(len(host), len(guest)))
    return {"host_completed": len(host), "guest_completed": len(guest), "equal_prefix_batches": first,
            "first_difference": {"index": first, "host": host[first] if first < len(host) else None,
                                 "guest": guest[first] if first < len(guest) else None} if first < max(len(host), len(guest)) else None,
            "equal_gameplay_prefix_batches": first_gameplay,
            "first_gameplay_difference": {"index": first_gameplay, "host": host[first_gameplay] if first_gameplay < len(host) else None,
                                          "guest": guest[first_gameplay] if first_gameplay < len(guest) else None} if first_gameplay < max(len(host), len(guest)) else None,
            "host_incomplete": host_incomplete, "guest_incomplete": guest_incomplete,
            "limits": "Read-only observations, not spawn replication. Different CRT state alone does not prove a spawn difference. Captures may have different end ticks."}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pair", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = json.dumps(compare(args.pair), indent=2) + "\n"
    if args.output:
        args.output.write_text(result, encoding="utf-8")
    print(result, end="")
