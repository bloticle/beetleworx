"""Check baseline resolution failures against live mapped particle bindings."""
import argparse
import json
import re
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("baseline", type=Path)
parser.add_argument("candidate", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
def read_capture(path):
    finish = json.loads(path.read_text(encoding="utf-8-sig"))
    assert finish["completed"] and finish["normal_user_files_unchanged"]
    assert finish["em1_source_metadata_unchanged"] and not finish["gameplay_input_sent"]
    log = (Path(finish["root"]) / "game.log").read_text(encoding="utf-8-sig")
    assert re.findall(r"particle lifecycle sample completed phase=(\d+) valid=true", log) == ["0", "1", "2", "3", "4"]
    return finish, log

baseline, original = read_capture(args.baseline)
candidate, mapped = read_capture(args.candidate)
failures = re.findall(r"original particle material lookup name='([^']+)' result=([0-9A-F]+)", original)
assert len(failures) == 6 and all(int(result, 16) == 0 for name, result in failures)
assert {name for name, result in failures} == {
    "FX_alphaBlend_singleChannel", "FX_DiffuseAlpha_VertexColorAlpha", "SFX_accumulatedIndirect_Positive"}
systems = {}
for line in mapped.splitlines():
    match = re.search(r"particle material binding handle=(\d+) object=(\w+) slot=(\d+) material=(\w+) class=(\w+) technique='([^']+)' selected=true", line)
    if match:
        handle, address, slot, material, cls, technique = match.groups()
        assert int(material, 16) != 0
        system = systems.setdefault(address, {"handle": int(handle), "object": address, "selected": []})
        system["selected"].append(technique)
    match = re.search(r"particle population handle=(\d+) running=\w+ object=(\w+) class='([^']+)' label='([^']+)'", line)
    if match:
        assert match[2] in systems
        systems[match[2]]["label"] = match[4]
assert len(systems) == 24
counts = {"alpha_blend": 0, "diffuse_alpha": 0, "distortion_fallback": 0}
for system in systems.values():
    names = system["selected"]
    assert len(names) == 5 and len(set(names)) == 1
    if system["label"] == "Distortion_PSystem_BGShape":
        assert names[0] == "Gen3_DefaultShaderTextured"
        counts["distortion_fallback"] += 1
    elif system["label"] == "Steam_Shoot_PSystemShape":
        assert names[0] in ("Gen3_FX_DiffuseAlpha_VertexColorAlpha", "Gen3_FX_DiffuseAlpha_VertexColorAlpha_SA")
        counts["diffuse_alpha"] += 1
    else:
        assert names[0] in ("Gen3_FX_alphaBlend_singleChannel", "Gen3_FX_alphaBlend_singleChannel_SA")
        counts["alpha_blend"] += 1
assert counts == {"alpha_blend": 16, "diffuse_alpha": 4, "distortion_fallback": 4}
args.output.write_text(json.dumps({"baseline": baseline, "candidate": candidate,
    "original_lookup_failures": failures, "counts": counts, "systems": list(systems.values()),
    "scope": "Five source systems map to native particle techniques on 20 live instances, including native SA selection, through all five lifecycle samples and unload. Four distortion instances still select a generic fallback. Binding evidence does not establish pixel/blend/depth equivalence; candidate remains opt-in."}, indent=2) + "\n")
print("20 mapped native particle bindings verified; 4 distortion fallbacks identified; unload guards passed")
