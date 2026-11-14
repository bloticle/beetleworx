"""Check actual Room extra-human trajectories, not just input submission logs."""
import argparse
import json
import math
from pathlib import Path
import re
import runpy

TRAJECTORY = re.compile(
    r"Private Room actor trajectory: room peer (\d+) tick (\d+) "
    r"position ([^/]+)/([^/]+)/([^ ]+) velocity ([^/]+)/([^/]+)/([^ ]+) "
    r"native state (\d+) active (true|false) consumed (true|false)"
)


def require(value, why):
    if not value:
        raise ValueError(why)


def verify(root):
    roster = json.loads((root / "roster.json").read_text(encoding="utf-8-sig"))
    observation_tools = runpy.run_path(str(Path(__file__).with_name("compare-online-observations.py")))
    compare = observation_tools["compare"]
    require(roster.get("scripted_sdk_input"), "Roster was not launched with the explicit scripted SDK program")
    native_result = None
    if roster.get("native_room_menu"):
        native_result = json.loads((root / "result.json").read_text(encoding="utf-8-sig"))
        require(native_result.get("scripted_sdk_input") and native_result.get("observation_complete") and not native_result.get("error"), "Native menu SDK capture did not complete")
    captures, trajectories, sources, first_ticks = [], [], [], []
    for record in roster["processes"]:
        capture = Path(record["root"])
        text = (capture / "game.log").read_text(encoding="utf-8", errors="replace")
        if native_result is None:
            completion = json.loads((capture / "completion-result.json").read_text(encoding="utf-8-sig"))
        else:
            process = next(p for p in native_result["processes"] if p["root"] == record["root"] and p["pid"] == record["pid"])
            completion = {"completed": process.get("scripted_sdk_program_completed") and process["process_exited"] and process["exit_code"] == 0 and not process.get("forced_close"),
                          "normal_user_files_unchanged": native_result["normal_user_files_unchanged"], "evidence": str(root / "result.json")}
            text = "\n".join(line for line in text.splitlines() if not line.startswith("[") or line[1:24] < native_result["close_started_local"])
        require(completion["completed"] and completion["normal_user_files_unchanged"], "Capture or normal-save integrity failed")
        require("Native Room extra humans admitted:" in text and "Native Room gameplay first frame consumed:" in text, "Native actor/world admission missing")
        require("Native Room gameplay stopped:" not in text and "Native Room actor admission failed:" not in text, "Native transaction fault")
        require("phase 'complete' reads" in text, "SDK program did not finish")
        origin = re.search(r"Private Room SDK input: peer (\d+) tick (\d+) elapsed 0 ", text)
        require(origin is not None, "SDK program origin missing")
        source = int(origin[1]); sources.append(source); first_ticks.append(int(origin[2]))
        if source == 3:
            for elapsed, phase in ((60, "forward"), (90, "backward"), (130, "jump")):
                require(re.search(rf"Private Room SDK input: peer 3 tick \d+ elapsed {elapsed} phase '{phase}' reads", text), "Third source did not emit its native SDK input phase")
        rows = {}
        for match in TRAJECTORY.findall(text):
            if int(match[0]) != 3:
                continue
            tick = int(match[1])
            row = {"position": list(map(float, match[2:5])), "velocity": list(map(float, match[5:8])),
                   "state": int(match[8]), "active": match[9] == "true", "consumed": match[10] == "true"}
            require(tick not in rows or rows[tick] == row, "Conflicting trajectory for one native tick")
            rows[tick] = row
        require(rows and all(row["active"] and row["consumed"] for row in rows.values()), "Extra body inactive or native input not consumed")
        observed, conflicts, _ = observation_tools["read_observations"](capture / "game.log")
        require(not conflicts, "Conflicting completed observations")
        retail = [value for tick, value in sorted(observed.items()) if int(origin[2]) + 59 <= tick <= int(origin[2]) + 240]
        require(retail and len({row["positions"] for row in retail}) == 1 and all(row["life"] == "5/5/5/5" for row in retail),
                "Extra-player actions moved or killed a neutral retail player")
        captures.append({"root": str(capture), "source_peer": source, "completion": completion})
        trajectories.append(rows)
    require(sorted(sources) == list(range(1, roster["players"] + 1)), "Room sources are not unique/complete")
    require(len(set(first_ticks)) == 1, "Private source programs used different agreed origins")
    require(len({record["exe_sha256"] for record in roster["processes"]}) == 1, "Executable identities differ")
    origin = first_ticks[0]
    # Use the actual input-held boundaries, accounting for the negotiated two
    # ticks of delay. Neutral settling can move a body on sloped contacts and
    # is not part of the active forward/backward displacement.
    rows = trajectories[0]
    delay = 2
    def displacement(begin, end):
        a = rows[origin + begin + delay]["position"]
        b = rows[origin + end + delay]["position"]
        return [b[i] - a[i] for i in (0, 2)]
    forward = displacement(59, 71)
    backward = displacement(89, 101)
    forward_distance, backward_distance = math.hypot(*forward), math.hypot(*backward)
    direction_dot = sum(a * b for a, b in zip(forward, backward))
    require(forward_distance > .03 and backward_distance > .03 and direction_dot < 0, "Body did not move in opposing directions during held inputs")
    jump_base = rows[origin + 129 + delay]["position"][1]
    jump_rows = [row for tick, row in rows.items() if origin + 130 + delay <= tick <= origin + 210 + delay]
    height = max(row["position"][1] for row in jump_rows) - jump_base
    up_speed = max(row["velocity"][1] for row in jump_rows)
    final = rows[max(rows)]
    speed = math.hypot(final["velocity"][0], final["velocity"][2])
    require(height > 1 and up_speed > 5 and any(row["state"] != 0 for row in jump_rows), "Native body did not jump")
    require(final["state"] == 0 and speed < .1, "Body did not land and stop after release")
    comparisons = []
    for index in range(1, len(captures)):
        result = compare(Path(captures[0]["root"]) / "game.log", Path(captures[index]["root"]) / "game.log", 300, 250)
        require(result["matching_capture"], "Completed world observations differ")
        common = sorted(rows.keys() & trajectories[index].keys())
        require(len(common) >= 250 and all(rows[tick] == trajectories[index][tick] for tick in common), "Extra actor trajectories differ")
        result["matching_extra_trajectory_ticks"] = len(common)
        comparisons.append(result)
    return {"passed": True, "binary_sha256": roster["processes"][0]["exe_sha256"], "captures": captures,
            "scripted_sdk_input_observed": True, "gameplay_input_sent": True,
            "first_source_tick": origin, "forward_distance": forward_distance, "backward_distance": backward_distance,
            "direction_dot": direction_dot, "jump_height": height, "up_speed": up_speed,
            "final_horizontal_speed": speed, "comparisons": comparisons,
            "scope": "Actual third human Movement2/Jump2 through isolated synthetic SDK source, original native interpreters, TCP frames and native physics. One level; independent extra cameras, full actions, sixteen clients and campaign lifecycle are not established.",
            "metadata_note": "Older generic finisher gameplay_input_sent=false does not classify this new scripted SDK source; actual program and trajectory evidence above establish input delivery."}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    args = parser.parse_args()
    result = verify(args.root)
    (args.root / "active-input-validation.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: result[key] for key in ("passed", "forward_distance", "backward_distance", "direction_dot", "jump_height", "up_speed", "final_horizontal_speed")}, indent=2))
