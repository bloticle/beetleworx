"""Check multiple independently controlled native extra humans and local views.

Uses real private Room captures, including explicit mixed-character rosters.
It does not establish sixteen clients,
all actions, visual nametags/colors, transitions or full campaign progression.
"""
import argparse
import json
import math
from pathlib import Path
import re
import runpy

inputs_tools = runpy.run_path(str(Path(__file__).with_name("verify-private-room-input.py")))
require = inputs_tools["require"]
CREATE = re.compile(r"Native Room independent camera: room peer (\d+) controller ([0-9A-F]+) output ([0-9A-F]+) tracker ([0-9A-F]+) own target ([0-9A-F]+) node ([0-9A-F]+); native controller and DefaultCamera/MickeyTracker factories; actor entity ([0-9A-F]+)")
POSE = re.compile(r"Private Room owned camera pose: room peer (\d+) tick (\d+) node ([0-9A-F]+) position ([^/]+)/([^/]+)/([^\s]+)")
BASIS = re.compile(r"Private Room owned camera basis: room peer (\d+) tick (\d+) node ([0-9A-F]+) forward ([^/]+)/([^/]+)/([^\s]+)")
RETAIL_BASIS = re.compile(r"Private Room retail camera basis: slot ([01]) tick (\d+) node ([0-9A-F]+) forward ([^/]+)/([^/]+)/([^\s]+)")
BIND = re.compile(r"Native Room local view rebound: controller ([0-9A-F]+) view ([0-9A-F]+) previous ([0-9A-F]+) retained ([0-9A-F]+) matches true split 0")
HUD = re.compile(r"Native Room HUD health target: profile (\d+) retail ([0-9A-F]+) local extra ([0-9A-F]+);")
CALL = re.compile(r"Native Room owned button callback: tick (\d+) peer (\d+) profile (\d+) action '([^']+)' result (\d+) native return (\d+);")


def yaw(a, b):
    require(math.hypot(a[0], a[2]) > .1 and math.hypot(b[0], b[2]) > .1, "Unusable camera basis")
    return math.atan2(a[0] * b[2] - a[2] * b[0], a[0] * b[0] + a[2] * b[2])


def verify(root):
    roster = json.loads((root / "roster.json").read_text(encoding="utf-8-sig"))
    players = roster["players"]
    require(players >= 4, "This verifier requires multiple extra humans")
    inputs = inputs_tools["verify"](root)
    origin = inputs["first_source_tick"]
    expected = set(range(3, players + 1))
    profiles = {peer: (0 if roster.get("mixed_characters", False) and peer & 1 else 1) for peer in expected}
    mask = ((1 << players) - 1) & ~3
    result = {"passed": True, "players": players, "binary_sha256": inputs["binary_sha256"], "input": inputs, "captures": []}
    all_rows, all_calls = [], []
    for capture in inputs["captures"]:
        text = (Path(capture["root"]) / "game.log").read_text(encoding="utf-8", errors="replace")
        require("Completed native gameplay worlds diverged" not in text, "Native worlds diverged")
        require(f"Native Room extra humans admitted: count {players - 2} mask {mask:04X}" in text, "Complete owned roster not admitted")
        created = {int(m[0]): m[1:] for m in CREATE.findall(text)}
        require(set(created) == expected, "Missing actor/camera identities")
        for index in (0, 1, 2, 3, 4, 5):
            require(len({row[index] for row in created.values()}) == len(expected), "Extra actors share a controller/output/tracker/target/node/entity")
        require(len({row[i] for row in created.values() for i in (1, 2, 3)}) == len(expected) * 3, "Owned native handles alias across actors")
        source = capture["source_peer"]
        bindings = BIND.findall(text)
        hud = HUD.findall(text)
        require(re.search(r"Online single-view layout: local [01], split 0,", text), "Full-window native layout missing")
        if source >= 3:
            controller, _, _, _, node, entity = created[source]
            require(bindings and all(row[0] == controller and row[3] == node and row[2] != node for row in bindings), "Local render view retained another actor's camera")
            require(len(hud) == 1 and hud[0][0] == str(profiles[source]) and hud[0][2] == entity and hud[0][1] != entity, "HUD did not select the exact local extra actor and character profile")
            direction = 1 if source & 1 else -1
            for elapsed, phase in ((60, "forward" if direction > 0 else "backward"), (175, "look right" if direction > 0 else "look left"), (280, "tool button X"), (330, "tool trigger RT")):
                require(re.search(rf"SDK input: peer {source} tick \d+ elapsed {elapsed} phase '{phase}'", text), "Independent source program phase missing")
        else:
            require(not bindings and not hud, "Extra ownership replaced a retail client's view/HUD")
        trajectories = {peer: {} for peer in expected}
        for m in inputs_tools["TRAJECTORY"].findall(text):
            peer, tick = int(m[0]), int(m[1])
            if peer in expected:
                row = tuple(map(float, m[2:8])) + (int(m[8]), m[9], m[10])
                require(tick not in trajectories[peer] or trajectories[peer][tick] == row, "Conflicting actor samples")
                trajectories[peer][tick] = row
        poses = {peer: {} for peer in expected}
        bases = {peer: {} for peer in expected}
        for regex, output in ((POSE, poses), (BASIS, bases)):
            for m in regex.findall(text):
                peer, tick = int(m[0]), int(m[1])
                if peer in expected:
                    require(m[2] == created[peer][4], "Camera sample belongs to another native output")
                    output[peer][tick] = tuple(map(float, m[3:6]))
        retail_turns = []
        for slot in (0, 1):
            rows = {int(m[1]): tuple(map(float, m[3:6])) for m in RETAIL_BASIS.findall(text) if int(m[0]) == slot}
            turns = [yaw(rows[origin + a], rows[origin + b]) for a, b in ((176, 196), (301, 321))]
            require(all(abs(value) < .01 for value in turns), "Extra look rotated a neutral retail camera")
            retail_turns.append(turns)
        actor_results = []
        for peer in sorted(expected):
            require(f"Native Room owned look admitted: peer {peer} " in text, "Original native Look callback was not admitted for each extra")
            rows, camera, basis = trajectories[peer], poses[peer], bases[peer]
            require(len(rows) >= 300 and all(row[7:] == ("true", "true") for row in rows.values()), "Owned input was not active and consumed")
            common = sorted(rows.keys() & camera.keys() & basis.keys())
            require(len(common) >= 300, "Insufficient actor/camera samples")
            distances = [math.hypot(camera[t][0] - rows[t][0], camera[t][2] - rows[t][2]) for t in common]
            require(all(.2 < v < 20 for v in distances) and distances[0] > 1, "Camera detached or failed native startup")
            movements = []
            for begin, end in ((61, 73), (91, 103)):
                a, b = origin + begin, origin + end
                delta = (rows[b][0] - rows[a][0], rows[b][2] - rows[a][2])
                follow = (camera[b][0] - camera[a][0], camera[b][2] - camera[a][2])
                require(math.hypot(*delta) > .03 and math.hypot(*follow) > .01 and sum(x * y for x, y in zip(delta, follow)) > 0, "Owned camera did not follow its actor's active movement")
                movements.append(delta)
            require(sum(a * b for a, b in zip(*movements)) < 0, "Actor did not reverse direction")
            jump_rows = [rows[t] for t in rows if origin + 132 <= t <= origin + 212]
            height = max(row[1] for row in jump_rows) - rows[origin + 131][1]
            require(height > 1 and max(row[4] for row in jump_rows) > 5 and rows[max(rows)][6] == 0, "Owned native jump/landing missing")
            direction = 1 if peer & 1 else -1
            turns = [yaw(basis[origin + a], basis[origin + b]) for a, b in ((176, 196), (301, 321))]
            require(turns[0] * direction > .03 and turns[1] * direction < -.03, "Independent opposite camera look missing")
            a, b = origin + 241, origin + 253
            move = (rows[b][0] - rows[a][0], rows[b][2] - rows[a][2])
            view = basis[a]
            alignment = direction * (move[0] * view[0] + move[1] * view[2]) / (math.hypot(*move) * math.hypot(view[0], view[2]))
            require(alignment > .8, "Movement after look used another camera basis")
            actor_results.append({"peer": peer, "profile": profiles[peer], "jump_height": height, "turns": turns, "view_alignment_after_look": alignment, "camera_samples": len(common)})
        calls = CALL.findall(text)
        require(calls and all(int(row[1]) in expected and row[2] == str(profiles[int(row[1])]) and row[4] == "0" for row in calls), "Invalid owned button admission or character profile")
        for peer in expected:
            names = ("SpinAttack", "Start Paint Brush", "End Paint Brush") if profiles[peer] == 0 else ("SpinAttack2", "Start Electric Stream2", "End Electric Stream2")
            for name in names:
                selected = [row for row in calls if int(row[1]) == peer and row[3] == name]
                require(len(selected) == 1 and selected[0][5] == "1", "Extra actor failed native " + name)
        require("Native Room button callback:" not in text, "Extra actions reached a retail receiver")
        result["captures"].append({"source_peer": source, "actors": actor_results, "retail_turns_radians": retail_turns})
        all_rows.append(trajectories)
        all_calls.append(calls)
    for other in all_rows[1:]:
        for peer in expected:
            common = all_rows[0][peer].keys() & other[peer].keys()
            require(len(common) >= 300 and all(all_rows[0][peer][t] == other[peer][t] for t in common), "Owned body trajectories disagree between worlds")
    require(all(calls == all_calls[0] for calls in all_calls[1:]), "Owned callback order/results disagree between worlds")
    result["scope"] = __doc__.strip()
    return result

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    args = parser.parse_args()
    result = verify(args.root)
    (args.root / "multi-actor-validation.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": True, "players": result["players"], "captures": result["captures"]}, indent=2))
