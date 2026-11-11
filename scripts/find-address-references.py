"""Find candidate PPC constant-address references in the loaded local image.

This is a small linear constant tracker, not a control-flow proof. Verify its
results against disassembly before using an address as a native hook.
"""
import argparse
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]


def references(data, sections, base, targets, include_memory=False):
    words = struct.unpack_from(">" + str(len(data) // 4) + "I", data)
    for section in sections:
        if not section["flags"] & 2:
            continue
        registers = [None] * 32
        for index in range((section["base"] - base) // 4,
                           (section["base"] - base + section["size"]) // 4):
            word = words[index]
            opcode = word >> 26
            rd, ra, rb = (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
            immediate = (word & 65535) - (65536 if word & 32768 else 0)
            destination, value = None, None
            # PPC globals are usually addressed directly by lis + lwz/stw,
            # without ever forming their full address in a register.
            memory_address = None
            if opcode in range(32,56) or opcode in (58,62):
                displacement = immediate & ~3 if opcode in (58,62) else immediate
                if ra == 0 or registers[ra] is not None:
                    memory_address = ((0 if ra == 0 else registers[ra]) + displacement) & 0xFFFFFFFF
                    if include_memory and memory_address in targets:
                        yield base + index * 4, ra, memory_address, 'memory: '+targets[memory_address]
            if opcode in (14, 15):
                destination = rd
                if ra == 0 or registers[ra] is not None:
                    value = ((0 if ra == 0 else registers[ra]) +
                             (immediate << 16 if opcode == 15 else immediate)) & 0xFFFFFFFF
            elif opcode == 24:
                destination = ra
                value = registers[rd] | (word & 65535) if registers[rd] is not None else None
            elif opcode == 31 and ((word >> 1) & 1023) == 444:
                destination = ra
                value = registers[rd] if rd == rb else None
            elif opcode == 18 and word & 1:
                for register in (0, *range(3, 13)):
                    registers[register] = None
            elif opcode == 19 and ((word >> 1) & 1023) in (16,528):
                # bclrl/bcctrl have the same volatile-register contract as bl.
                # A non-link return/tail call must not carry constants into
                # the next function in this linear scan.
                if word & 1:
                    for register in (0, *range(3, 13)):
                        registers[register] = None
                else:
                    registers = [None] * 32
            elif opcode in (7, 8, 12, 13, 32, 33, 34, 35, 40, 41, 42, 43, 46, 58):
                destination = rd
            elif opcode in (20, 21, 23, 25, 26, 27, 28, 29):
                destination = ra
            elif opcode == 31:
                if ((word >> 1) & 1023) not in (0, 32, 144, 467, 598, 854, 918, 982, 1014, 151, 183, 215, 247, 407, 439):
                    destination = rd
            if destination is not None:
                registers[destination] = value
                if value in targets:
                    yield base + index * 4, destination, value, targets[value]
            if (opcode in (33,35,37,39,41,43,45,49,51,53,55) or
                opcode in (58,62) and word & 3 == 1):
                registers[ra] = memory_address


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--string", action="append", default=[])
    parser.add_argument("--address", action="append", default=[], type=lambda s: int(s, 0))
    parser.add_argument("--memory", action="store_true", help="Also find candidate direct load/store effective addresses")
    args = parser.parse_args()
    data = (ROOT / "private/image.bin").read_bytes()
    metadata = json.loads((ROOT / "analysis/image.json").read_text(encoding="utf-8-sig"))
    base = metadata["base"]
    targets = {address: hex(address) for address in args.address}
    for text in args.string:
        pattern = text.encode() + b"\0"
        offset = data.find(pattern)
        while offset >= 0:
            targets[base + offset] = text
            offset = data.find(pattern, offset + 1)
    for pc, register, address, name in references(data, metadata["sections"], base, targets, args.memory):
        if name.startswith('memory: '):
            print(f"{pc:08X} memory via r{register:02d} -> {address:08X}  {name[8:]}")
        else:
            print(f"{pc:08X} r{register:02d} = {address:08X}  {name}")


if __name__ == "__main__":
    main()
