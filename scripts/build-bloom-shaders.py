"""Build a private EM2 shader archive with bilinear Gaussian bloom sampling.

Requires the verified Xbox 360 shader asset. Only min/mag filter override bits
in the two seven-tap Gaussian pixel programs change. Never edits game content.
"""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import struct

ROOT = Path(__file__).resolve().parents[1]
ORIGINAL_SHA = "fe93c858b7c1b82c8f64b71562e5b80e5c968011d90cc5270a2a4473819164aa"
ASSET = "cookedshaders/360/PP_Bloom.fxlo"


def patch_bloom(original):
    if hashlib.sha256(original).hexdigest() != ORIGINAL_SHA:
        raise ValueError("Unrecognized PP_Bloom.fxlo; refusing to patch shader offsets")
    result = bytearray(original)
    changes = []
    # Program locations are from the original FXLO. The GPU instruction body
    # begins at container+0x19c; its first three instruction slots are control
    # flow, followed by seven texture fetches. Keep all arithmetic/coordinates,
    # weights, LOD, mip filtering, and the threshold/composite programs intact.
    for label, container in (("horizontal", 0x162C), ("vertical", 0x18A8)):
        if struct.unpack_from(">I", original, container)[0] != 0x102A1100:
            raise ValueError("Bloom pixel shader container mismatch")
        for slot in range(3, 10):
            offset = container + 0x19C + slot * 12
            first, filters, third = struct.unpack_from(">3I", original, offset)
            if first & 31 != 1 or filters != 0x1F1FF688 or third != 0x4000:
                raise ValueError("Bloom texture fetch signature mismatch")
            # Xenos TextureFetchInstruction word 1: mag bits 12:13, min 14:15.
            # 3 = inherit fetch constant, 1 = linear. No other bits change.
            linear = (filters & ~0xF000) | 0x5000
            struct.pack_into(">I", result, offset + 4, linear)
            changes.append({"pass": label, "offset": offset + 4,
                            "before": filters, "after": linear})
    allowed = {c["offset"] + i for c in changes for i in range(4)}
    if len(changes) != 14 or len(original) != len(result):
        raise ValueError("Unexpected shader patch extent")
    if any(a != b and i not in allowed for i, (a, b) in enumerate(zip(original, result))):
        raise ValueError("Shader patch changed unrelated bytes")
    return bytes(result), changes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-dir", type=Path, required=True)
    args = parser.parse_args()
    pak = runpy.run_path(str(ROOT / "scripts/game-pak.py"))
    source = args.game_dir / "packfiles/shaders.pak"
    files = pak["read_pak"](source)
    found = [i for i, (name, _, _) in enumerate(files) if name == ASSET]
    if len(found) != 1:
        raise ValueError("Expected exactly one bloom effect in shaders.pak")
    index = found[0]
    name, original, kind = files[index]
    patched, changes = patch_bloom(original)
    # Local, ignored fixtures for same-executable translator regression tests.
    # Regenerate from the verified asset rather than committing game bytecode.
    lines = ["#pragma once", "#include <array>", "#include <cstdint>", "namespace em2::bloom_test_data {"]
    for label, container in (("horizontal", 0x162C), ("vertical", 0x18A8)):
        for variant, effect in (("original", original), ("linear", patched)):
            words = struct.unpack_from(">54I", effect, container + 0x19C)
            lines.append(f"inline constexpr std::array<uint32_t,54> {label}_{variant} = {{")
            lines.append(",".join(f"0x{w:08X}u" for w in words) + "};")
    for label, container, expected_bytes in (("threshold",0x12FC,168),("composite",0x1B24,84)):
        resource,info=struct.unpack_from(">I",original,container+4)[0],struct.unpack_from(">I",original,container+24)[0]
        offset,size=struct.unpack_from(">2I",original,container+info)
        if size!=expected_bytes or offset not in (0,64):
            raise ValueError("Bloom chain program layout mismatch")
        code=struct.unpack_from(f">{size//4}I",original,container+resource+offset)
        lines.append(f"inline constexpr std::array<uint32_t,{len(code)}> {label} = {{"+
                     ",".join(f"0x{word:08X}u" for word in code)+"};")
        if label == "composite":
            linear = list(code)
            for slot, first in ((2, 0x10081001), (3, 0x10180001)):
                if tuple(code[slot*3:slot*3+3]) != (first, 0x1F1FF688, 0x4000):
                    raise ValueError("Bloom composite fetch signature mismatch")
                linear[slot*3+1] = (linear[slot*3+1] & ~0xF000) | 0x5000
            lines.append(f"inline constexpr std::array<uint32_t,{len(linear)}> composite_linear = {{"+
                         ",".join(f"0x{word:08X}u" for word in linear)+"};")
    # Native literal descriptor starts at register508 (pixel c252),16 words.
    # c255 contains the Gaussian weights; preserve the asset's exact bits.
    literals = original[0x162C+0x15C:0x162C+0x19C]
    if (struct.unpack_from(">I", original, 0x162C+0x10C)[0] != 0x01FC0010 or
            literals != original[0x18A8+0x15C:0x18A8+0x19C]):
        raise ValueError("Bloom literal layout mismatch")
    lines.append("inline constexpr std::array<uint32_t,16> literal_bits = {" +
                 ",".join(f"0x{w:08X}u" for w in struct.unpack(">16I", literals)) + "};")
    lines.append("}")
    header = ROOT / "private/bloom_shader_test_data.h"
    text = "\n".join(lines) + "\n"
    if not header.exists() or header.read_text() != text:
        header.write_text(text)
    shadow_name = "cookedshaders/360/ShadowScreenSpace.fxlo"
    shadow = [data for entry, data, _ in files if entry == shadow_name]
    if len(shadow) != 1 or hashlib.sha256(shadow[0]).hexdigest() != "f186081ff9f781905645425c953d737d22b812f6e3b67b7b7f3d9302c7da8286":
        raise ValueError("Unrecognized native shadow effect; cannot regenerate analysis fixture")
    shadow = shadow[0]
    container = 0x8A4
    resource, resource_size = struct.unpack_from(">2I", shadow, container + 4)
    metadata = struct.unpack_from(">I", shadow, container + 24)[0]
    offset, size = struct.unpack_from(">2I", shadow, container + metadata)
    if offset + size != resource_size or size % 12:
        raise ValueError("Native shadow GPU metadata/program extent mismatch")
    words = struct.unpack_from(f">{size // 4}I", shadow, container + resource + offset)
    kernel = runpy.run_path(str(ROOT / "scripts/shadow-kernel.py"))
    programs = {"original": words, "control": kernel["generate"](words, 1),
                "samples8": kernel["generate"](words, 2), "samples16": kernel["generate"](words, 4),
                "adaptive16": kernel["generate"](words,4,adaptive=True),
                "radius_probe": kernel["generate"](words,4,adaptive=True,radius_probe=True)}
    vertex_container = 0x698
    vertex_resource = struct.unpack_from(">I", shadow, vertex_container + 4)[0]
    vertex_metadata = struct.unpack_from(">I", shadow, vertex_container + 24)[0]
    vertex_offset, vertex_size = struct.unpack_from(">2I", shadow, vertex_container + vertex_metadata)
    programs["vertex"] = struct.unpack_from(f">{vertex_size // 4}I", shadow, vertex_container + vertex_resource + vertex_offset)
    programs["literal_bits"] = struct.unpack_from(">32I", shadow, container + resource)
    shadow_text = "#pragma once\n#include <array>\n#include <cstdint>\nnamespace em2::shadow_test_data {\n"
    for label, program in programs.items():
        shadow_text += f"inline constexpr std::array<uint32_t,{len(program)}> {label} = {{\n"
        shadow_text += ",".join(f"0x{word:08X}u" for word in program) + "};\n"
        if label in ("samples8", "samples16", "adaptive16"):
            digest = hashlib.sha256(struct.pack(f">{len(program)}I", *program)).hexdigest()[:16]
            key=f'pcf-{label[7:]}' if label.startswith('samples') else label
            shadow_text += f'inline constexpr char cache_{label}[] = "shadow-{key}-{digest}";\n'
    shadow_text += "}\n"
    shadow_header = ROOT / "private/shadow_shader_test_data.h"
    if not shadow_header.exists() or shadow_header.read_text() != shadow_text:
        shadow_header.write_text(shadow_text)
    files[index] = (name, patched, kind)
    destination = ROOT / "private/pc-assets/packfiles/shaders.pak"
    if not destination.exists() or pak["read_pak"](destination) != files:
        pak["write_pak"](files, destination)
    # write_pak verifies every extracted entry, not just the changed effect.
    report = {"asset": ASSET, "source_sha256": ORIGINAL_SHA,
              "patched_sha256": hashlib.sha256(patched).hexdigest(),
              "archive_entries": len(files), "changes": changes,
              "visual_validation": "pending"}
    (ROOT / "analysis/bloom-shader-patch.json").write_text(json.dumps(report, indent=2))
    print("Bloom shaders: 14 Gaussian sample min/mag overrides set to linear; other entries preserved")


if __name__ == "__main__":
    main()
