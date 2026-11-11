"""Compare completed native observations from a private Room pair (no game input)."""
import argparse
import json
import re
from pathlib import Path


OBSERVATION = re.compile(
    r"Native gameplay observation v(?P<version>[45]): level '(?P<level>[^']*)' state '(?P<state>[^']*)' "
    r"interior '(?P<interior>[^']*)' epoch \d+, tick (?P<tick>\d+), "
    r"modes (?P<modes>[^,]+), bits (?P<positions>[^,]+), life (?P<life>[^,]+), "
    r"ammo (?P<ammo>[^,]+), CRT calls/seeds (?P<random_calls>[^,]+), "
    r"last state (?P<random_state>[0-9A-Fa-f]+), stream (?P<random_stream>[0-9A-Fa-f]+), "
    r"digest (?P<digest>[0-9a-f]{64})(?:, agreed (?P<agreed>true|false))?"
)


def read_observations(path):
    observations, conflicts = {}, set()
    ignored = {"pre_admission": 0, "unagreed": 0}
    legacy_started = False
    with path.open(encoding="utf-8", errors="replace") as source:
        for line in source:
            match = OBSERVATION.search(line)
            if not match:
                continue
            fields = match.groupdict()
            tick = int(fields.pop("tick"))
            agreed = fields.pop("agreed")
            if agreed == "false":
                ignored["unagreed"] += 1
                continue
            if agreed is None and not legacy_started:
                # This harness negotiates first_tick=0. Unagreed logging uses
                # native tick_number, incremented before observation (>=1).
                # Old logs therefore start their agreed stream at tick zero;
                # pre-load diagnostics may otherwise collide with later ticks.
                if tick != 0:
                    ignored["pre_admission"] += 1
                    continue
                legacy_started = True
            if tick in observations and observations[tick] != fields:
                conflicts.add(tick)
            else:
                observations[tick] = fields
    return observations, sorted(conflicts), ignored


def compare(host, guest, minimum, minimum_character_ticks=0):
    left, left_conflicts, left_ignored = read_observations(host)
    right, right_conflicts, right_ignored = read_observations(guest)
    shared = sorted(left.keys() & right.keys())
    mismatches = [tick for tick in shared if left[tick]["digest"] != right[tick]["digest"]]
    contiguous = bool(shared) and shared == list(range(shared[0], shared[-1] + 1))
    character_ticks = sum(left[tick]["version"] == right[tick]["version"] == "5" for tick in shared)
    first = mismatches[0] if mismatches else None
    differences = {} if first is None else {
        field: {"host": left[first][field], "guest": right[first][field]}
        for field in left[first] if left[first][field] != right[first][field]
    }
    passed = (len(shared) >= minimum and character_ticks >= minimum_character_ticks and contiguous and shared[0] == 0
              and not mismatches and not left_conflicts and not right_conflicts)
    return {
        "matching_capture": passed, "minimum_shared_ticks": minimum,
        "minimum_character_ticks": minimum_character_ticks,
        "character_physics_shared_ticks": character_ticks,
        "host_ticks": len(left), "guest_ticks": len(right), "shared_ticks": len(shared),
        "first": shared[0] if shared else None, "last": shared[-1] if shared else None,
        "contiguous": contiguous, "mismatches": mismatches,
        "first_mismatch_fields": differences,
        "duplicate_conflicts": {"host": left_conflicts, "guest": right_conflicts},
        "ignored_diagnostics": {"host": left_ignored, "guest": right_ignored},
        "limitation": "Only recorded observation fields are compared; this does not establish full-world determinism or playable multiplayer.",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pair", type=Path, help="Private pair directory or pair.json")
    parser.add_argument("--minimum-ticks", type=int, default=300)
    parser.add_argument("--minimum-character-ticks", type=int, default=0,
                        help="Require this many shared v5 character-physics proofs; old v4 logs do not count")
    parser.add_argument("--output", type=Path, help="New JSON file; existing files are refused")
    args = parser.parse_args()
    if args.minimum_ticks < 1:
        parser.error("--minimum-ticks must be positive")
    if args.minimum_character_ticks < 0:
        parser.error("--minimum-character-ticks must be nonnegative")
    metadata = args.pair / "pair.json" if args.pair.is_dir() else args.pair
    pair = json.loads(metadata.read_text(encoding="utf-8-sig"))
    result = compare(Path(pair["host"]["root"]) / "game.log",
                     Path(pair["guest"]["root"]) / "game.log", args.minimum_ticks, args.minimum_character_ticks)
    rendered = json.dumps(result, indent=2) + "\n"
    if args.output:
        with args.output.open("x", encoding="utf-8") as target:
            target.write(rendered)
    print(rendered, end="")
    return 0 if result["matching_capture"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
