"""Summarize observed jump-input windows from passive EM2 mesh-motion logs."""
import argparse
import json
import re
from pathlib import Path

PATTERN = re.compile(r"Skinned mesh motion ([0-9A-F]+): epoch (\d+), world \(([^)]+)\), root bone \(([^)]+)\), P1 buttons ([0-9A-F]+)")
ANCESTOR_PATTERN = re.compile(r"Skinned mesh ancestor ([0-9A-F]+) depth (\d+) node ([0-9A-F]+): epoch (\d+), world \(([^)]+)\), P1 buttons ([0-9A-F]+)")
ENTITY_PATTERN = re.compile(r"Player entity motion P(\d+) ([0-9A-F]+) component ([0-9A-F]+) handle ([0-9A-F]+): epoch (\d+), world \(([^)]+)\), P1 buttons ([0-9A-F]+)")


def summarize(path, ticks):
    meshes = {}
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            match = PATTERN.search(line)
            if match:
                mesh, epoch, world, root, buttons = match.groups()
            else:
                match = ANCESTOR_PATTERN.search(line)
                if match:
                    mesh, depth, node, epoch, world, buttons = match.groups()
                    mesh = f"{mesh}/ancestor{depth}/{node}"
                else:
                    match = ENTITY_PATTERN.search(line)
                    if not match:
                        continue
                    player, entity, component, handle, epoch, world, buttons = match.groups()
                    mesh = f"player{player}/{entity}/{component}/{handle}"
                root = None
            sample = {"epoch": int(epoch),
                "world": [float(v) for v in world.split(",")],
                "buttons": int(buttons, 16)}
            if root is not None:
                sample["root_bone"] = [float(v) for v in root.split(",")]
            meshes.setdefault(mesh, []).append(sample)
    windows = []
    for mesh, samples in meshes.items():
        for index in range(1, len(samples)):
            if not samples[index]["buttons"] & 0x1000 or samples[index-1]["buttons"] & 0x1000:
                continue
            before = samples[index-1]
            window = samples[index:index+ticks]
            if len(window) != ticks or before["epoch"]+1 != window[0]["epoch"] or any(
                row["epoch"] != window[0]["epoch"]+i for i, row in enumerate(window)):
                windows.append({"mesh": mesh, "start_epoch": samples[index]["epoch"],
                    "complete": False, "reason": "missing or nonconsecutive simulation samples"})
                continue
            result = {"mesh": mesh, "start_epoch": window[0]["epoch"], "complete": True,
                "ticks": ticks, "jump_button_ticks": sum(bool(row["buttons"] & 0x1000) for row in window)}
            for field in ("world", "root_bone"):
                if field not in before:
                    continue
                relative = [[row[field][axis]-before[field][axis] for axis in range(3)] for row in window]
                result[field] = {"baseline": before[field], "relative_samples": relative,
                    "minimum": [min(row[axis] for row in relative) for axis in range(3)],
                    "maximum": [max(row[axis] for row in relative) for axis in range(3)],
                    "final": relative[-1]}
            windows.append(result)
    return {"source": str(path.resolve()), "interpretation": "Mesh and native entity-transform motion correlated with P1 jump input; not a Havok body or collision proof.",
        "meshes": len({mesh.split('/')[0] for mesh in meshes if not mesh.startswith('player')}), "tracks": len(meshes), "windows": windows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--ticks", type=int, default=90)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.ticks < 2:
        parser.error("--ticks must be at least 2")
    result = summarize(args.log, args.ticks)
    if args.output:
        args.output.write_text(json.dumps(result, indent=2)+"\n", encoding="utf-8")
    print(json.dumps({"meshes": result["meshes"], "windows": [
        {key: value for key, value in window.items() if key not in ("world", "root_bone")}
        for window in result["windows"]]}, indent=2))


if __name__ == "__main__":
    main()
