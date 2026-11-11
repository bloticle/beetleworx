"""Read-only particle shading evidence; not a GPU equivalence test."""
import hashlib
import json
import runpy
import struct
import xml.etree.ElementTree as ET
from pathlib import Path
from em1_nif import NIF

ROOT = Path(__file__).resolve().parents[1]
source = Path("D:/Repos/bluethinner/target/shader-audit")
names = ("FX_alphaBlend_singleChannel", "FX_DiffuseAlpha_VertexColorAlpha", "SFX_accumulatedIndirect_Positive")
templates = {}
for name in names:
    path = source / (name + ".r3mt")
    raw = path.read_bytes()
    tree = ET.fromstring(raw)
    # Drop only XML namespaces; preserve all authored stage and PE attributes.
    for node in tree.iter():
        node.tag = node.tag.rsplit("}", 1)[-1]
    material = tree.find("Material")
    templates[name] = {
        "source": str(path), "sha256": hashlib.sha256(raw).hexdigest(),
        "display_face": material.find("DisplayFace").attrib,
        "stages": [{node.tag: node.attrib for node in stage} for stage in material.findall("Tev/Stages/Stage")],
        "pixel_engine": {node.tag: node.attrib for node in material.find("PE")},
    }

archive = ROOT / "build/runtime23/pc-assets/packfiles/shaders.pak"
entries = runpy.run_path(str(ROOT / "scripts/game-pak.py"))["read_pak"](archive)
extract = runpy.run_path(str(ROOT / "scripts/inspect-scene-shaders.py"))["extract"]
native = {}
for name, data, _kind in entries:
    if name.endswith(("Gen3_FX_AlphaBlend_sc.fxlo", "Gen3_FX_Diff_VertexCol_Alpha.fxlo")):
        programs = extract(data)
        native[name] = [{k: v for k, v in program.items() if k != "words"} for program in programs]
        if name.endswith("Gen3_FX_AlphaBlend_sc.fxlo"):
            # SDK ucode.h: fetch destination components are absolute 3-bit
            # selectors; ALU selectors are 2-bit component-relative offsets.
            # Both ordinary and SA programs share this executable body.
            for program in programs[:2]:
                words = program["words"]
                assert tuple(words[6:12]) == (0x10080001, 0x1F1FFFE8, 0x00004000, 0xC80F8000, 0x00710000, 0xE1000100)
                fetch = [(words[7] >> (i*3)) & 7 for i in range(4)]
                swizzle = [((words[10] >> (16+i*2)) + i) & 3 for i in range(4)]
                assert fetch == [0, 5, 7, 7]  # r0.x=texture.r, r0.y=1; retain z/w.
                assert swizzle == [1, 1, 1, 0]  # r0.yyyx * interpolated colour.
                assert (words[11] >> 24) & 31 == 1  # vector MUL

# Pin the ordinary/SA diffuse executable tails. The pixel programs multiply
# texture * Constant_Color_1 * interpolated colour, with no RGB doubling.
# The vertex exports are identity MAX operations on the fetched colour.
for name, data, _kind in entries:
    if not name.endswith("Gen3_FX_Diff_VertexCol_Alpha.fxlo"):
        continue
    for program in extract(data)[:2]:
        words = program["words"]
        assert tuple(words[9:15]) == (0xC80F0000, 0, 0xA1000000, 0xC80F8000, 0, 0xE1000100)
    vertices = extract(data, pixel=False)
    assert len(vertices) == 2
    for program, first_alu, export_index, colour_register in (
            (vertices[0], 6, 11, 0), (vertices[1], 19, 106, 8)):
        words = program["words"]
        assert tuple(words[export_index*3:export_index*3+3]) == (
            0xC80F8001, 0, 0xE2000000 | colour_register*0x10100)
        # Before the export, no ALU writes that temporary colour register.
        # The pinned programs have only ALUs after these fetch clauses.
        for index in range(first_alu, export_index):
            word0 = words[index*3]
            if word0 & (1 << 15):  # Export writes do not modify temporaries.
                continue
            assert not (word0 & ((1 << 6) | (1 << 14)))  # No indirect destinations.
            assert not ((word0 & 63) == colour_register and (word0 >> 16) & 15)
            assert not (((word0 >> 8) & 63) == colour_register and (word0 >> 20) & 15)
    native[name][0]["diffuse_vertex_programs"] = [
        {k: v for k, v in program.items() if k != "words"} for program in vertices]

textures = []
for filename in ("SmokePuffs_08A_tex.nif_wii", "Cone_Spray_tex.nif_wii"):
    path = ROOT / "analysis/em1-compatibility/portable-textures/Effects/_Shared/textures" / filename
    raw = path.read_bytes()
    model = NIF.read(raw)
    pixels = [model.blocks[i] for i, t in enumerate(model.indices) if model.types[t] == "NiPixelData"]
    assert len(pixels) == 1
    data = pixels[0]
    word = lambda at: struct.unpack_from(model.endian + "I", data, at)[0]
    assert word(0) == 1 and data[4] == 32 and word(67) == 4
    mips = word(63)
    assert 1 <= mips <= 16
    end = 71 + mips*12
    payload = data[end+8:]
    assert len(payload) == word(end) and word(end+4) == 1 and len(payload) % 4 == 0
    levels = []
    for mip in range(mips):
        width, height, offset = struct.unpack_from(model.endian + "3I", data, 71+mip*12)
        level = payload[offset:offset+width*height*4]
        assert len(level) == width*height*4
        differences = sum(level[i] != level[i+3] for i in range(0, len(level), 4))
        assert differences == 0
        levels.append({"width": width, "height": height, "red_alpha_differences": differences})
    textures.append({"path": str(path), "sha256": hashlib.sha256(raw).hexdigest(), "mips": levels})

result = {"templates": templates, "native_programs": native, "textures": textures,
    "conclusions": [
        "Original single-channel TEV uses texture alpha; native ordinary/SA pixel programs use red. Smoke-A and cone-spray converted pixels have R=A in all seven mip levels each, so this channel difference is harmless for those fixtures.",
        "Smoke-B remains unverified; do not generalize the channel equality to arbitrary textures.",
        "Original diffuse-alpha TEV doubles RGB in its second stage and clamps it. Both native pixel programs have only two multiplies; ordinary/SA vertex shaders pass fetched colour through unchanged. Runtime constant/vertex input values must be checked for compensation before declaring a visible brightness defect.",
        "Both original non-distortion templates use source-alpha/inverse-source-alpha blending, LEQUAL depth testing without depth writes, and alpha greater than zero. Distortion uses additive One/One blending. Native effective draw state and GPU results remain unverified."]}
(ROOT / "analysis/em1-compatibility/particle-shading-audit.json").write_text(json.dumps(result, indent=2) + "\n")
print("Audited 3 original templates, native particle programs, and 14 texture mip levels")

