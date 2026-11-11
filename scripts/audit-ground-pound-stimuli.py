"""Check movement damage IDs against the retail Lua constant registration.

This audits original translated instructions and the loaded Xbox image. It
does not exercise collisions, combat scripts, or damage in a running level.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def audit(header_text=None):
    symbol = "PPC_FUNC_IMPL(__imp__sub_8295E1A8)"
    if shutil.which("rg"):
        matches = subprocess.check_output(
            ["rg", "-l", "-F", symbol, str(ROOT / "generated/ppc")], text=True
        ).splitlines()
    else:
        matches = [str(path) for path in (ROOT / "generated/ppc").glob("ppc_recomp*.cpp") if symbol in path.read_text()]
    assert len(matches) == 1, "retail stimulus registration must be unambiguous"
    source = Path(matches[0])
    text = source.read_text()
    body = text[text.index(symbol):].split("\nPPC_FUNC_IMPL", 1)[0]
    image = (ROOT / "private/image.bin").read_bytes()
    metadata = json.loads((ROOT / "analysis/image.json").read_text(encoding="utf-8-sig"))
    registers = [None] * 32
    pending = None
    constants = {}
    for line in body.splitlines():
        instruction = line.strip().removeprefix("// ")
        match = re.fullmatch(r"(li|lis) r(\d+),(-?\d+)", instruction)
        if match:
            op, destination, value = match.groups()
            registers[int(destination)] = (int(value) << (16 if op == "lis" else 0)) & 0xFFFFFFFF
        match = re.fullmatch(r"addi r(\d+),r(\d+),(-?\d+)", instruction)
        if match:
            destination, operand, value = map(int, match.groups())
            registers[destination] = None if registers[operand] is None else (registers[operand] + value) & 0xFFFFFFFF
        match = re.fullmatch(r"bl 0x([0-9a-f]+)", instruction)
        if match:
            target = int(match[1], 16)
            if target == 0x827300F0:
                pending = registers[4]  # Retail integer push.
            elif target == 0x82732488:
                assert pending is not None and registers[4] is not None
                offset = registers[4] - metadata["base"]
                assert 0 <= offset < len(image)
                name = image[offset:image.index(b"\0", offset)].decode("ascii")
                if name.startswith("ST_"):
                    assert name not in constants
                    constants[name] = pending
                pending = None
            for volatile in (0, *range(3, 13)):
                registers[volatile] = None
    expected = {"ST_PHYSICAL": 2, "ST_KNOCKBACK": 3, "ST_MAGIC": 7, "ST_SPIN": 8}
    assert all(constants.get(name) == value for name, value in expected.items()), constants
    header = (ROOT / "runtime/gameplay_mods.h").read_text() if header_text is None else header_text
    declaration = re.search(r"enum class PoundDamageType[^\{]*\{([^}]+)\}", header)[1]
    configured = {name: int(value) for name, value in re.findall(r"(\w+)\s*=\s*(\d+)", declaration)}
    assert configured == {"Physical": constants["ST_PHYSICAL"], "Spin": constants["ST_SPIN"]}, configured
    return {
        "verified": True,
        "scope": "Static retail registration and host enum agreement; no live combat validation",
        "registration": "0x8295E1A8",
        "registration_source": str(source.relative_to(ROOT)),
        "image_sha256": hashlib.sha256(image).hexdigest(),
        "constants": constants,
        "host_damage_types": configured,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "analysis/ground-pound-stimulus-audit.json")
    args = parser.parse_args()
    result = audit()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print("Verified retail ST_PHYSICAL=2, ST_KNOCKBACK=3, ST_MAGIC=7, ST_SPIN=8 and host damage types")
