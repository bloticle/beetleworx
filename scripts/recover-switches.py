"""Recover EM2 switch-table idioms missed by XenonAnalyse.

Only accept a register-consistent lis/rlwinm/addi/lwzx/mtctr/bctr sequence,
with its inline pointer table ending exactly at the earliest case target.
Also recover scheduled byte-offset and scaled halfword-offset tables. Every
target must be an aligned executable address. No disassembler dependency.
"""
import json
import struct
import tomllib
from pathlib import Path

root = Path(__file__).resolve().parents[1]
meta = json.loads((root / "analysis/image.json").read_text())
image = (root / "private/image.bin").read_bytes()
base = meta["base"]
code = [s for s in meta["sections"] if s["flags"] & 2]


def executable(address):
    return address % 4 == 0 and any(s["base"] <= address < s["base"] + s["size"] for s in code)


def signed16(value):
    return (value & 0x7FFF) - (value & 0x8000)


def guarded_bound(head, index):
    """Find an unsigned compare followed by bgt on the same condition field."""
    for pos in range(head - 4, max(-1, head - 64), -4):
        word = struct.unpack_from(">I", image, pos)[0]
        if word >> 26 != 10 or (word >> 16) & 31 != index or word & 0x00200000:
            continue
        condition = ((word >> 23) & 7) * 4 + 1
        expected = 0x41800000 | (condition << 16)
        for branch_pos in range(pos + 4, min(head, pos + 16), 4):
            branch = struct.unpack_from(">I", image, branch_pos)[0]
            if branch & 0xFFDF0000 == expected and not branch & 1:
                return (word & 0xFFFF) + 1
    return None


# Reviewed outer guards: their index is copied to another register, or their
# compare is shared by multiple dispatches. Verify the exact guard instruction
# before using its bound; the full table pattern and every case are still checked.
outer_bounds = {
    # Another scheduled absolute table: six instructions separate compare/bgt.
    0x828B8684: (16, 0x828B8654, 0x2B0B000F, 0x828B866C, 0x419902E0),
    # Level setup: stores separate the r25 compare and bgt by 0x48 bytes;
    # three small helper blocks precede the earliest switch-case target.
    # Both the compare and late branch must match before accepting 12 words.
    0x82B59620: (12, 0x82B595C0, 0x2B19000B, 0x82B59608, 0x41990088),
    0x826C1DA0: (19, 0x826C1D70, 0x2B040012),  # r31 is copied from r4.
    0x82BE846C: (15, 0x82BE83E4, 0x2B09000E),  # Shared r9 guard.
    0x82C6F21C: (14, 0x82C6F1B8, 0x2B07000D),
    0x82CB8C04: (16, 0x82CB88CC, 0x2B1D000F),  # Shared r29 guard.
    0x82F627F0: (106, 0x82F627C4, 0x2B030069), # r4 is copied from r3.
    0x832E108C: (29, 0x832E0F64, 0x2B13001C),
}


def reviewed_bound(dispatch):
    record = outer_bounds.get(dispatch)
    if record:
        count, address, expected = record[:3]
        branch_matches = len(record)==3 or struct.unpack_from(">I",image,record[3]-base)[0]==record[4]
        if branch_matches and struct.unpack_from(">I", image, address - base)[0] == expected:
            return count
    return None


tables = []
for section in code:
    start = section["base"] - base
    end = start + section["size"]
    for offset in range(start + 20, end - 4, 4):
        if image[offset:offset+4] != b"\x4e\x80\x04\x20":
            continue
        lis, scale, addi, load, mtctr, _ = struct.unpack_from(">6I", image, offset - 20)
        table_reg = (lis >> 21) & 31
        index_reg = (scale >> 21) & 31
        scaled_reg = (scale >> 16) & 31
        if lis >> 26 != 15 or (lis >> 16) & 31:
            continue
        if scale & 0xFC00FFFF != 0x5400103A:
            continue
        if addi >> 26 != 14 or (addi >> 21) & 31 != table_reg or (addi >> 16) & 31 != table_reg:
            continue
        expected_load = 0x7C00002E | (scaled_reg << 21) | (table_reg << 16) | (scaled_reg << 11)
        if load != expected_load or mtctr != 0x7C0903A6 | (scaled_reg << 21):
            continue
        table_address = (((lis & 0xFFFF) << 16) + signed16(addi)) & 0xFFFFFFFF
        if table_address != base + offset + 4:
            continue
        table_offset = table_address - base
        labels = []
        first_case = end + base
        bound = reviewed_bound(base + offset) or guarded_bound(offset - 20, index_reg)
        while table_address + 4 * len(labels) < first_case and len(labels) < 65536:
            if bound and len(labels) == bound:
                break
            ptr_offset = table_offset + 4 * len(labels)
            if ptr_offset + 4 > len(image):
                break
            target = struct.unpack_from(">I", image, ptr_offset)[0]
            if not executable(target):
                break
            # Cases can share an earlier block. Only forward cases establish
            # the end of the inline pointer table.
            if target >= table_address:
                first_case = min(first_case, target)
            labels.append(target)
        table_end = table_address + 4 * len(labels)
        if labels and (table_end == first_case or (bound == len(labels) and table_end <= first_case)):
            tables.append({"base": base + offset - 20, "dispatch": base + offset,
                           "r": index_reg, "table": table_address, "labels": labels})

# Byte-offset tables emitted with scheduling NOPs between materialization steps.
for section in code:
    start = section["base"] - base
    end = start + section["size"]
    for offset in range(start + 48, end, 4):
        if image[offset:offset+4] != b"\x4e\x80\x04\x20":
            continue
        ops = []
        for pos in range(offset, max(start - 1, offset - 64), -4):
            word = struct.unpack_from(">I", image, pos)[0]
            if word != 0x60000000:
                ops.append((pos, word))
            if len(ops) == 8:
                break
        if len(ops) != 8:
            continue
        ops.reverse()
        (_, hi), (_, lo), (_, load), (_, ahi), (_, alo), (_, add), (_, mtctr), _ = ops
        tr = (hi >> 21) & 31
        anchor_reg = (ahi >> 21) & 31
        loaded = (load >> 21) & 31
        index = (load >> 11) & 31
        if hi >> 26 != 15 or (hi >> 16) & 31 or ahi >> 26 != 15 or (ahi >> 16) & 31:
            continue
        if lo >> 26 != 14 or (lo >> 21) & 31 != tr or (lo >> 16) & 31 != tr:
            continue
        if alo >> 26 != 14 or (alo >> 21) & 31 != anchor_reg or (alo >> 16) & 31 != anchor_reg:
            continue
        if load != 0x7C0000AE | (loaded << 21) | (tr << 16) | (index << 11):
            continue
        if add != 0x7C000214 | (anchor_reg << 21) | (anchor_reg << 16) | (loaded << 11):
            continue
        if mtctr != 0x7C0903A6 | (anchor_reg << 21):
            continue
        table_address = (((hi & 0xFFFF) << 16) + signed16(lo)) & 0xFFFFFFFF
        anchor = (((ahi & 0xFFFF) << 16) + signed16(alo)) & 0xFFFFFFFF
        count = None
        for pos in range(ops[0][0] - 4, max(start - 1, ops[0][0] - 64), -4):
            word = struct.unpack_from(">I", image, pos)[0]
            if word >> 26 == 10 and (word >> 16) & 31 == index:
                count = (word & 0xFFFF) + 1
                break
        if not count or count > 65536 or not base <= table_address < base + len(image) - count:
            continue
        labels = [anchor + value for value in image[table_address-base:table_address-base+count]]
        if labels and all(executable(value) for value in labels):
            tables.append({"base": base + ops[0][0], "dispatch": base + offset, "r": index,
                           "table": table_address, "labels": labels, "kind": "byte-offset"})

# Halfword offsets scale the index before lhzx; compact byte tables instead
# scale the loaded offset by four. Both forms have nine non-NOP instructions.
for section in code:
    start = section["base"] - base
    end = start + section["size"]
    for offset in range(start + 32, end, 4):
        if image[offset:offset+4] != b"\x4e\x80\x04\x20":
            continue
        ops = []
        for pos in range(offset, max(start - 1, offset - 72), -4):
            word = struct.unpack_from(">I", image, pos)[0]
            if word != 0x60000000:
                ops.append((pos, word))
            if len(ops) == 9:
                break
        if len(ops) != 9:
            continue
        ops.reverse()
        (_, hi), (_, scale), (_, lo), (_, load), (_, ahi), (_, alo), (_, add), (_, mtctr), _ = ops
        byte_scaled = scale >> 26 == 14
        if byte_scaled:
            (_, hi), (_, lo), (_, load), (_, scale), (_, ahi), (_, alo), (_, add), (_, mtctr), _ = ops
        tr, ar = (hi >> 21) & 31, (ahi >> 21) & 31
        loaded = (load >> 21) & 31
        index = (load >> 11) & 31 if byte_scaled else (scale >> 21) & 31
        scaled = index if byte_scaled else (scale >> 16) & 31
        if hi >> 26 != 15 or (hi >> 16) & 31 or ahi >> 26 != 15 or (ahi >> 16) & 31:
            continue
        if byte_scaled:
            if scale != 0x5400103A | (loaded << 21) | (loaded << 16):
                continue
        elif scale & 0xFC00FFFF != 0x5400083C:
            continue
        if lo != 0x38000000 | (tr << 21) | (tr << 16) | (lo & 0xFFFF):
            continue
        if alo != 0x38000000 | (ar << 21) | (ar << 16) | (alo & 0xFFFF):
            continue
        load_opcode = 0x7C0000AE if byte_scaled else 0x7C00022E
        if load != load_opcode | (loaded << 21) | (tr << 16) | (scaled << 11):
            continue
        if add != 0x7C000214 | (ar << 21) | (ar << 16) | (loaded << 11):
            continue
        if mtctr != 0x7C0903A6 | (ar << 21):
            continue
        count = None
        for pos in range(ops[0][0] - 4, max(start - 1, ops[0][0] - 64), -4):
            word = struct.unpack_from(">I", image, pos)[0]
            if word >> 26 == 10 and (word >> 16) & 31 == index:
                count = (word & 0xFFFF) + 1
                break
        table_address = (((hi & 0xFFFF) << 16) + signed16(lo)) & 0xFFFFFFFF
        anchor = (((ahi & 0xFFFF) << 16) + signed16(alo)) & 0xFFFFFFFF
        count = reviewed_bound(base + offset) or count
        width = 1 if byte_scaled else 2
        if not count or count > 65536 or not base <= table_address <= base + len(image) - count * width:
            continue
        values = struct.unpack_from(f">{count}{'B' if byte_scaled else 'H'}", image, table_address-base)
        labels = [anchor + (value * 4 if byte_scaled else value) for value in values]
        if labels and all(executable(value) for value in labels):
            tables.append({"base": base + ops[0][0], "dispatch": base + offset, "r": index,
                           "table": table_address, "labels": labels,
                           "kind": "byte-scaled-offset" if byte_scaled else "halfword-offset"})

path = root / "config/em2_switch_tables.toml"
existing = tomllib.loads(path.read_text()).get("switch", [])
known = {item["base"] for item in existing}
with path.open("a", encoding="utf-8") as stream:
    stream.write("\n# EM2 reordered absolute inline tables (validated from the loaded image).\n")
    for table in tables:
        if table["base"] in known:
            continue
        stream.write(f'\n[[switch]]\nbase = 0x{table["base"]:08X}\nr = {table["r"]}\nlabels = [\n')
        stream.writelines(f"    0x{label:08X},\n" for label in table["labels"])
        stream.write("]\n")
(root / "analysis/switches.json").write_text(json.dumps(tables, indent=2) + "\n")
print(f"Recovered {len(tables)} validated switch tables")
