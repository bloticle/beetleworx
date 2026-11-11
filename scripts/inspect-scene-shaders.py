"""Extract pinned scene-depth programs and their native constant tables.

Game bytecode stays in ignored private fixtures. No game archive is modified.
Offsets are decoded from the Xbox shader container rather than guessed from
effect names. These inputs support native translation audits, not a claim of
world-position/depth reconstruction correctness.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import runpy
import struct

ROOT = Path(__file__).resolve().parents[1]
PINNED = {
    "ZPrepass": "774ec15dbff93bec6f41a1cad2c321dcd56009c20559ba7164628e8e38403250",
    "ScreenSpace_DepthFog": "a390263d7e94be4bb1ec90b31d3880121c89dca2bcf3ff69fe2a129730783450",
}


def extract(data, pixel=True):
    programs = []
    for match in re.finditer(re.escape(bytes.fromhex("102a1100" if pixel else "102a1101")), data):
        base = match.start()
        header = struct.unpack_from(">9I", data, base)
        native = struct.unpack_from(">8I", data, base + header[6])
        start, length = base + header[1] + native[0], native[1]
        if length == 0 or length % 12 or start + length > len(data):
            raise ValueError("Invalid native shader extent")
        # D3DX constant table follows a four-byte size prefix. Its register
        # indices describe original guest constants, not packed DXBC indices.
        table = base + header[4] + 4
        size, creator, version, count, info, flags, target = struct.unpack_from(">7I", data, table)
        if size != 28 or version != (0xFFFF0300 if pixel else 0xFFFE0300) or count > 256:
            raise ValueError("Unexpected shader constant table")
        constants = []
        for i in range(count):
            name, kind, index, registers, reserved, typ, default = struct.unpack_from(">I4H2I", data, table + info + 20 * i)
            end = data.index(b"\0", table + name)
            text = data[table + name:end].decode("ascii")
            if kind not in (0, 1, 2, 3) or not registers or index + registers > 256:
                raise ValueError("Invalid native constant binding")
            constants.append({"name": text, "register_set": kind, "index": index, "count": registers})
        programs.append({"container_offset": base, "code_offset": start,
                         "code_bytes": length, "code_sha256": hashlib.sha256(data[start:start+length]).hexdigest(),
                         "constants": constants, "words": struct.unpack_from(f">{length//4}I", data, start)})
    if not programs:
        raise ValueError("No native pixel programs")
    return programs


def position_probe(words):
    # Keep every original fetch and ALU through instruction16, where native
    # r0 contains world Y/X/Z. Only reschedule clauses and add an unclamped
    # XYZ export. No replacement reconstruction arithmetic is substituted.
    groups = []
    for pair in range(4):
        a, b, c = words[pair*3:pair*3+3]
        for cf in (a | ((b & 65535) << 32), (b >> 16) | (c << 16)):
            op = cf >> 44
            if op == 12:
                groups.append((cf, []))
            elif op in (1, 2):
                address, count, sequence = cf & 4095, (cf >> 12) & 7, (cf >> 16) & 4095
                kept = [(sequence >> (i*2) & 3, tuple(words[(address+i)*3:(address+i+1)*3])) for i in range(count) if address+i <= 16]
                if kept:
                    groups.append((cf & ~((1 << 28)-1), kept))
            elif op != 0:
                raise ValueError("Unexpected fog control flow")
    kernel = runpy.run_path(str(ROOT / "scripts/shadow-kernel.py"))
    export = kernel["alu"](0, 15, 2, 0, 0, 0xCD, 0xCD)
    export = (export[0] | 0x8000 | (8 << 20), export[1], export[2])
    groups[-1][1].append((0, export))
    cf, instructions = groups[-1]
    groups[-1] = ((cf & ~(15 << 44)) | (2 << 44), instructions)
    if len(groups) & 1:
        groups.append((0, []))
    body, control = [], []
    for cf, instructions in groups:
        if not instructions:
            control.append(cf)
            continue
        if len(instructions) > 6:
            raise ValueError("Unexpected probe clause length")
        control.append(cf | (len(groups)//2 + len(body)//3) | (len(instructions) << 12) | sum(s << (16+2*i) for i, (s, _) in enumerate(instructions)))
        for _, instruction in instructions:
            body.extend(instruction)
    result = []
    for a, b in zip(control[::2], control[1::2]):
        result.extend((a & 0xFFFFFFFF, (a >> 32) | ((b & 65535) << 16), b >> 16))
    output = result + body
    if output[6:-3] != list(words[12:51]):
        raise ValueError("Position probe did not preserve every original reconstruction instruction")
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-dir", type=Path, required=True)
    args = parser.parse_args()
    pak = runpy.run_path(str(ROOT / "scripts/game-pak.py"))
    entries = pak["read_pak"](args.game_dir / "packfiles/shaders.pak")
    report = {}
    lines = ["#pragma once", "#include <array>", "#include <cstdint>", "namespace em2::scene_shader_data {"]
    for effect, digest in PINNED.items():
        matches = [data for name, data, kind in entries if name == f"cookedshaders/360/{effect}.fxlo"]
        if len(matches) != 1 or hashlib.sha256(matches[0]).hexdigest() != digest:
            raise ValueError(f"Unrecognized scene effect {effect}")
        programs = extract(matches[0])
        for i, p in enumerate(programs):
            name = f"{effect}_{i}"
            words = p.pop("words")
            lines.append(f"inline constexpr std::array<uint32_t,{len(words)}> {name}={{" + ",".join(f"0x{x:08x}u" for x in words) + "};")
            if effect == "ScreenSpace_DepthFog":
                probe = position_probe(words)
                lines.append(f"inline constexpr std::array<uint32_t,{len(probe)}> depth_fog_position_probe={{" + ",".join(f"0x{x:08x}u" for x in probe) + "};")
                p["position_probe_sha256"] = hashlib.sha256(struct.pack(f">{len(probe)}I", *probe)).hexdigest()
        report[effect] = {"effect_sha256": digest, "programs": programs}
        if effect == "ScreenSpace_DepthFog":
            vertex = extract(matches[0], pixel=False)
            if len(vertex) != 1:
                raise ValueError("Unexpected fog vertex program count")
            words = vertex[0].pop("words")
            lines.append(f"inline constexpr std::array<uint32_t,{len(words)}> depth_fog_vertex={{" + ",".join(f"0x{x:08x}u" for x in words) + "};")
            linked=list(words)
            for slot,offset in ((3,0),(4,2)):
                linked[slot*3+1]=(linked[slot*3+1]&~0x3F3FF000)|(37<<16)|(1<<13)
                linked[slot*3+2]=(linked[slot*3+2]&0x80000000)|4|(offset<<8)
            lines.append(f"inline constexpr std::array<uint32_t,{len(linked)}> depth_fog_vertex_linked_example={{" + ",".join(f"0x{x:08x}u" for x in linked) + "};")
            literal=struct.unpack_from(">4I",matches[0],vertex[0]["code_offset"]-16)
            if literal!=(0xC0000000,0x3F800000,0xBF800000,0):raise ValueError("Native fog vertex literals changed")
            lines.append("inline constexpr std::array<uint32_t,4> depth_fog_vertex_literals={"+",".join(f"0x{x:08x}u" for x in literal)+"};")
            report[effect]["vertex_programs"] = vertex
    fog = report["ScreenSpace_DepthFog"]["programs"]
    if len(fog) != 1 or len(report["ZPrepass"]["programs"]) != 2:
        raise ValueError("Unexpected scene program count")
    bindings = {c["name"]: (c["register_set"], c["index"], c["count"]) for c in fog[0]["constants"]}
    expected = {"InvWorldViewProjection": (2, 13, 4), "ViewInverse": (2, 10, 3),
                "depthMapSampler": (3, 0, 1), "fogMapSampler": (3, 1, 1)}
    if any(bindings.get(k) != v for k, v in expected.items()):
        raise ValueError("Scene input binding ABI changed")
    lines.append("}")
    fixture = ROOT / "private/scene_shader_data.h"
    text = "\n".join(lines) + "\n"
    if not fixture.exists() or fixture.read_text() != text:
        fixture.write_text(text)
    (ROOT / "analysis/scene-shader-inputs.json").write_text(json.dumps(report, indent=2) + "\n")
    print("Pinned scene input audit: two Z-prepass pixels and one depth-fog pixel; native reflection confirms inverse VP c13..16, view inverse c10..12, depth s0 and fog s1. Reconstruction semantics remain unverified.")


if __name__ == "__main__":
    main()
