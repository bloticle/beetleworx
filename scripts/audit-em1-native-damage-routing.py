"""Audit retail damage delivery and I/O callback semantics before bridging EM1 Lua.

Reads the extracted Xbox image only. Does not inject damage or run game code.
"""
import argparse
import hashlib
import json
from pathlib import Path
import capstone

BASE = 0x82000000
BLOCKS = {
    "damage_constructor": (0x827D1E50, 0x94),
    "health_damage_emit": (0x827D0368, 0x8C),
    "immediate_listener_filter": (0x82734380, 0x80),
    "io_listener_registration": (0x82755254, 0x34),
    "io_callback_dispatch": (0x82755D50, 0x24C),
    "io_lua_lookup": (0x82755A48, 0x60),
    "io_lua_schedule": (0x82755BA8, 0x28),
}
# These are independently interpreted instructions establishing the ABI; reject
# another image/revision rather than applying these addresses speculatively.
EXPECTED = {
    0x827D1E70: ("li", "r10, 0xf"),
    0x827D1E7C: ("sth", "r10, 4(r3)"),
    0x827D1E90: ("stw", "r5, 8(r3)"),
    0x827D1E9C: ("stw", "r3, 0xc(r30)"),
    0x827D036C: ("lwz", "r11, 0x30(r27)"),
    0x827D03A0: ("bl", "0x827d1e50"),
    0x827D03E8: ("bl", "0x827340e0"),
    0x827343AC: ("lwz", "r11, 8(r26)"),
    0x827343C0: ("cmplw", "cr6, r6, r11"),
    0x8275526C: ("addi", "r5, r23, 0x40"),
    0x82755284: ("bl", "0x82735e08"),
    0x82755DE0: ("lwz", "r10, 8(r29)"),
    0x82755DE8: ("cmplw", "cr6, r9, r10"),
    0x82755E74: ("bl", "0x82755710"),
    0x82755E88: ("lhz", "r11, 0xac(r31)"),
    0x82755E8C: ("clrlwi", "r10, r11, 0x1f"),
    0x82755EA0: ("stw", "r10, 4(r30)"),
    0x82755A54: ("bl", "0x82730050"),
    0x82755A9C: ("beq", "cr6, 0x82755c10"),
    0x82755BC4: ("lfs", "f1, 0x20(r25)"),
    0x82755BCC: ("bl", "0x82732b88"),
}


def audit(image):
    data = image.read_bytes()
    decoder = capstone.Cs(capstone.CS_ARCH_PPC,
                          capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
    blocks, instructions = {}, {}
    for name, (address, size) in BLOCKS.items():
        code = list(decoder.disasm(data[address-BASE:address-BASE+size], address))
        if sum(i.size for i in code) != size:
            raise ValueError(f"Incomplete disassembly: {name}")
        blocks[name] = [f"{i.address:08X} {i.mnemonic} {i.op_str}" for i in code]
        instructions.update({i.address: (i.mnemonic, i.op_str) for i in code})
    for address, expected in EXPECTED.items():
        if instructions.get(address) != expected:
            raise ValueError(f"Retail ABI mismatch at {address:08X}: {instructions.get(address)}")
    return {
        "image_sha256": hashlib.sha256(data).hexdigest(),
        "checked_instructions": len(EXPECTED),
        "event_type": 15,
        "damage_constructor": "0x827D1E50",
        "event_offsets": {"type_u16": 4, "damaged_entity_handle": 8,
                          "other_handle": 12, "float_payloads": [16, 20],
                          "owned_damage_object": 24},
        "health_dispatch": "0x827340E0 (immediate), not 0x827347D0 (queued)",
        "io_listener": "0x82755D50; registered at component raw + 0x40",
        "io_connection_executor": "0x82755710",
        "connection_offsets": {"input_name": 20, "parameters": 24,
                               "flags_u16": 28, "delay_f32": 32},
        "one_shot": "Bit 0 removes a native connection after executor returns, even when Lua lookup skips the call.",
        "bridge_design": [
            "Intercept an eligible EM1 connection at 0x82755710, after native listener and source-handle filtering.",
            "Use the admitted owner identity, original callback/parameters, and authored delay; do not also dispatch from the global event bus.",
            "Do not retain stack event/connection pointers across a yielding Lua callback.",
            "Respect one-shot state; do not partially run DoorGear while required engine bindings are missing.",
        ],
        "limitations": ["Static retail-code audit, not a live player hit or original Lua tutorial execution.",
                        "No runtime hook installed by this audit."],
        "blocks": blocks,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, default=Path("private/image.bin"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = audit(args.image)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(f"Verified {result['checked_instructions']} retail ABI instructions; damage uses the immediate dispatcher.")
