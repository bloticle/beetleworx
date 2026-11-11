"""Summarize native Off/On windows without treating them as deterministic replay."""
import argparse
import json
import re
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("log", type=Path)
parser.add_argument("--output", type=Path)
args = parser.parse_args()
pattern = re.compile(
    r"(?P<label>Direct-color-resolve|Draw-extent|Fused-depth-stencil|Bounded-division|Stencil-merge|Stencil-plane|Stencil-clear|Stencil-value|Zero-stencil|Transfer clipping) A/B phase (?P<phase>\d+) "
    r"(?:enabled|bounded) (?P<enabled>[01]) complete: (?P<seconds>[\d.]+)s, "
    r"(?P<swaps>\d+) swaps, (?P<fps>[\d.]+) swaps/s, (?P<draws>[\d.]+) accepted draws/swap"
    r"(?:, (?P<skipped>[\d.]+) stencil bit draws skipped/swap)?"
    r"(?:, (?P<value_meshes>[\d.]+) R8 value meshes/swap)?"
    r"(?:, (?P<clears>[\d.]+) scratch clears skipped/swap)?"
    r"(?:, (?P<planes>[\d.]+) stencil-only transitions/swap)?"
    r"(?:, (?P<copies>[\d.]+) copy commands/swap)?"
    r"(?:, (?P<saved>[\d.]+) copy commands saved/swap)?"
)
phases = []
contents = args.log.read_text(encoding="utf-8", errors="replace")
for line in contents.splitlines():
    match = pattern.search(line)
    if match:
        values = match.groupdict()
        phases.append({"target": values["label"], "phase": int(values["phase"]),
                       "enabled": values["enabled"] == "1",
                       "seconds": float(values["seconds"]), "swaps": int(values["swaps"]),
                       "fps": float(values["fps"]), "accepted_draws_per_swap": float(values["draws"]),
                       "skipped_stencil_draws_per_swap": float(values["skipped"]) if values["skipped"] else None,
                       "r8_value_meshes_per_swap": float(values["value_meshes"]) if values["value_meshes"] else None,
                       "scratch_clears_skipped_per_swap": float(values["clears"]) if values["clears"] else None,
                       "stencil_plane_transfers_per_swap": float(values["planes"]) if values["planes"] else None,
                       "copy_commands_per_swap": float(values["copies"]) if values["copies"] else None,
                       "copy_commands_saved_per_swap": float(values["saved"]) if values["saved"] else None})
# Report only the latest complete/incomplete sequence, not an average across
# different launches or benchmark targets concatenated in a log.
start = max((i for i, phase in enumerate(phases) if phase["phase"] == 0), default=0)
phases = phases[start:]
for phase in phases:
    if phase["target"] == "Direct-color-resolve":
        counts = re.findall(r"Direct-color-resolve A/B phase " + str(phase["phase"]) + r" direct dispatches/swap: ([\d.]+)", contents)
        phase["direct_dispatches_per_swap"] = float(counts[-1]) if counts else None

pairs = []
for index in (0, 2):
    off = next((p for p in phases if p["phase"] == index and not p["enabled"]), None)
    on = next((p for p in phases if p["phase"] == index + 1 and p["enabled"]), None)
    if off and on and off["target"] == on["target"]:
        pairs.append({"off_phase": index, "on_phase": index + 1,
                      "fps_ratio": on["fps"] / off["fps"],
                      "accepted_draw_count_change_percent": 100 * (on["accepted_draws_per_swap"] / off["accepted_draws_per_swap"] - 1)})
groups = {}
for enabled, name in ((False, "off"), (True, "on")):
    selected = [p for p in phases if p["enabled"] == enabled]
    if selected:
        seconds = sum(p["seconds"] for p in selected)
        swaps = sum(p["swaps"] for p in selected)
        groups[name] = {"seconds": seconds, "swaps": swaps, "fps": swaps / seconds}
result = {"log": str(args.log), "phases": phases, "pairs": pairs, "aggregate": groups,
          "four_phases_complete": [p["phase"] for p in phases] == [0, 1, 2, 3],
          "startup": "Native automatic Continue" if "Offline startup requested native Continue" in contents else "Not inferred from log",
          "scope": "Live in-game measurement, without movement/camera automation. Camera, animation, loading/PSO warmup and actor state can affect results. Compare both pairs and draw counts; these are not deterministic replay measurements."}
if "off" in groups and "on" in groups:
    result["aggregate_fps_ratio"] = groups["on"]["fps"] / groups["off"]["fps"]
encoded = json.dumps(result, indent=2) + "\n"
if args.output:
    args.output.write_text(encoded, encoding="utf-8")
print(encoded, end="")
