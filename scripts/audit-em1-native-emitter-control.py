"""Read-only retail ABI audit for native particle start/stop controls."""
import argparse
import hashlib
import json
from pathlib import Path
import capstone

BASE = 0x82000000
BLOCKS = {"component_lookup": (0x82761C50, 0x54), "controller": (0x828013F0, 0xA4), "start": (0x82801768, 0xF4), "stop": (0x82801890, 0xF4)}
EXPECTED = {
    0x82761C58: ("lwz", "r11, 0x58(r3)"),
    0x82761C84: ("lwz", "r10, 0x24(r3)"),
    0x82761C94: ("addi", "r3, r11, -8"),
    0x828017E8: ("bl", "0x82761c50"),
    0x82801800: ("li", "r5, 0"),
    0x82801804: ("li", "r4, 1"),
    0x8280180C: ("bl", "0x828013f0"),
    0x82801818: ("addi", "r3, r30, 8"),
    0x82801820: ("li", "r5, 1"),
    0x82801824: ("lwz", "r9, 0x110(r10)"),
    0x8280182C: ("lwz", "r11, -0x3d3c(r11)"),
    0x82801830: ("addi", "r4, r11, 4"),
    0x82801910: ("bl", "0x82761c50"),
    0x82801928: ("li", "r5, 0"),
    0x8280192C: ("li", "r4, 0"),
    0x82801934: ("bl", "0x828013f0"),
    0x82801948: ("li", "r5, 0"),
    0x8280194C: ("lwz", "r9, 0x110(r10)"),
    0x82801954: ("lwz", "r11, -0x3d3c(r11)"),
}

def audit(image):
    data = image.read_bytes()
    decoder = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
    decoded = {}
    for address, expected in EXPECTED.items():
        instructions = list(decoder.disasm(data[address-BASE:address-BASE+4], address))
        if len(instructions) != 1 or (instructions[0].mnemonic, instructions[0].op_str) != expected:
            raise ValueError(f"Retail emitter ABI mismatch at {address:08X}")
    for name, (address, length) in BLOCKS.items():
        decoded[name] = [f"{i.address:08X} {i.mnemonic} {i.op_str}" for i in decoder.disasm(data[address-BASE:address-BASE+length], address)]
    return {"image_sha256": hashlib.sha256(data).hexdigest(), "asserted_instructions": len(EXPECTED), "blocks": decoded, "scope": "Retail entity-interface lookup, controller call and reflected enabled-property setter; does not prove visible emission."}

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, default=Path("private/image.bin"))
    parser.add_argument("--output", type=Path, default=Path("analysis/em1-compatibility/native-emitter-control-audit.json"))
    args = parser.parse_args()
    result = audit(args.image)
    args.output.write_text(json.dumps(result, indent=2)+"\n")
    print(f"Verified {result['asserted_instructions']} retail emitter ABI instructions")
