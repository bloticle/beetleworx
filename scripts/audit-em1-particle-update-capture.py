"""Correlate the bounded native update observer with all five population samples."""
import argparse
import json
import re
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("finish", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
finish = json.loads(args.finish.read_text(encoding="utf-8-sig"))
assert finish["completed"] and finish["normal_user_files_unchanged"]
assert finish["em1_source_metadata_unchanged"] and not finish["gameplay_input_sent"]
lines = (Path(finish["root"]) / "game.log").read_text(encoding="utf-8-sig").splitlines()
systems = {}
current = {}
phases = []
for line in lines:
    if "[em1] particle update trace " in line:
        fields = dict(re.findall(r"(\w+)=([^ ]+)", line.split("[em1]", 1)[1]))
        key = (int(fields["handle"]), fields["object"])
        sample = {k: float(v) if k == "simulation_time" else int(v)
                  for k, v in fields.items() if k not in ("object", "handle")}
        assert key not in current
        current[key] = sample
    match = re.search(r"particle population handle=(\d+) running=(\w+) object=(\w+) class='([^']*)' label='([^']*)' active=(\d+) capacity=(\d+) valid=(\w+)", line)
    if match:
        handle, running, address, cls, label, active, capacity, valid = match.groups()
        key = (int(handle), address)
        assert key in current and valid == "true"
        item = systems.setdefault(key, {"handle": int(handle), "object": address,
                                       "class": cls, "label": label, "samples": []})
        current[key].update(active=int(active), capacity=int(capacity), running=running == "true")
        item["samples"].append(current[key])
    match = re.search(r"particle lifecycle sample completed phase=(\d+) valid=(\w+)", line)
    if match:
        assert match[2] == "true" and len(current) == 24
        assert all("active" in x for x in current.values())
        phases.append(int(match[1]))
        current.clear()
assert phases == [0, 1, 2, 3, 4] and len(systems) == 24 and not current
for item in systems.values():
    samples = item["samples"]
    assert len(samples) == 5
    for name in ("updates", "enabled_updates", "commits", "enabled_commits"):
        assert [s[name] for s in samples] == [0, 30, 120, 240, 360]
    assert abs(samples[-1]["simulation_time"] - samples[0]["simulation_time"] - 12.0) < 0.001
    item["retains_population_after_stop"] = samples[-1]["active"] > 0
result = {
    "finish": finish,
    "phases": ["stopped baseline", "30 running ticks", "120 running ticks", "120 stopped ticks", "240 stopped ticks"],
    "systems": list(systems.values()),
    "retaining_systems": sum(s["retains_population_after_stop"] for s in systems.values()),
    "conclusion": "All 24 systems receive 360 enabled updates and time commits; simulation time advances 12 seconds. This update-context gate does not explain retained populations. Actual particle motion and visual output remain unverified.",
}
if all("finalizations" in s["samples"][-1] for s in systems.values()):
    retaining = [s for s in systems.values() if s["retains_population_after_stop"]]
    draining = [s for s in systems.values() if not s["retains_population_after_stop"]]
    assert len(retaining) == len(draining) == 12
    assert all(s["samples"][-1]["finalizations"] == s["samples"][-1]["removals"] == 0 for s in retaining)
    assert all(s["samples"][-1]["finalizations"] > 0 and s["samples"][-1]["removals"] > 0 for s in draining)
    result["conclusion"] += " The 12 retaining systems have zero native finalizations/removals. All 12 draining systems have positive finalization/removal counts. The divergence is downstream of update/time-commit scheduling; visibility policy is still unverified."
    if all("submissions" in s["samples"][-1] for s in systems.values()):
        assert all([x["submissions"] for x in s["samples"]] == [0, 30, 120, 240, 360] for s in systems.values())
        assert all(s["samples"][-1]["flagged_submissions"] == 360 for s in retaining)
        assert all(s["samples"][-1]["flagged_submissions"] < 360 for s in draining)
        result["conclusion"] = (
            "All 24 systems advance time and receive 360 simulator submission calls. "
            "All 360 attempts for each of the 12 retaining systems have the native suspension flag set; "
            "all 12 draining systems have unflagged attempts and positive finalization/removal counts. "
            "The retail submission audit traces this flag to cached accepted render-collection channels. "
            "Retention follows that native policy. Correctness of bounds, rendered pixels and material output remains unverified."
        )
args.output.write_text(json.dumps(result, indent=2) + "\n")
print(f"24 systems passed five-phase update/time assertions; {result['retaining_systems']} retain populations")
