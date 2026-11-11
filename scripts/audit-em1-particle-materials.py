"""Read original steam material bindings and compare exact staged FXLO names.

This is an asset audit, not proof of runtime shader selection or pixel equivalence.
No source game assets or runtime archives are modified.
"""
import hashlib
import json
import re
import runpy
import struct
from pathlib import Path
from em1_nif import NIF

ROOT = Path(__file__).resolve().parents[1]
archive = ROOT / "build/runtime23/pc-assets/packfiles/shaders.pak"
entries = runpy.run_path(str(ROOT / "scripts/game-pak.py"))["read_pak"](archive)
catalog = {}
for name, data, _kind in entries:
    if name.lower().endswith(".fxlo"):
        for text in re.findall(rb"[\x20-\x7e]{4,}", data):
            catalog.setdefault(text.decode("ascii"), []).append(name)

results = []
for filename in ("Common_Steam_02.nif_wii", "Steam_Jet_01.nif_wii"):
    path = ROOT / "analysis/em1-compatibility/tutorial-actor-resources/Effects/_Shared" / filename
    raw = path.read_bytes()
    model = NIF.read(raw)
    def word(data, offset):
        return struct.unpack_from(model.endian + "I", data, offset)[0]
    def short(data, offset):
        return struct.unpack_from(model.endian + "H", data, offset)[0]
    strings = []
    at = 8
    for _ in range(word(model.strings, 0)):
        size = word(model.strings, at)
        at += 4
        strings.append(model.strings[at:at+size].decode("utf-8"))
        at += size
    def string(index):
        return None if index == 0xFFFFFFFF else strings[index]
    def block(index, expected=None):
        kind = model.types[model.indices[index]]
        if expected is not None:
            assert kind == expected, (index, kind, expected)
        return kind, model.blocks[index]
    for index, type_index in enumerate(model.indices):
        kind = model.types[type_index]
        if kind not in ("NiPSParticleSystem", "NonUniformParticleSystem"):
            continue
        data = model.blocks[index]
        extra_count = word(data, 4)
        extras = {}
        colours = {}
        for j in range(extra_count):
            extra_kind, extra = block(word(data, 8+j*4))
            if extra_kind == "NiIntegerExtraData":
                assert len(extra) == 8
                extras[string(word(extra, 0))] = word(extra, 4)
            elif extra_kind == "NiColorExtraData":
                assert len(extra) == 20
                colours[string(word(extra, 0))] = list(struct.unpack_from(model.endian + "4f", extra, 4))
        at = 12 + extra_count*4 + 54
        property_count = word(data, at)
        at += 4
        properties = [word(data, at+j*4) for j in range(property_count)]
        at += property_count*4 + 4  # Property links and collision reference.
        technique_count = word(data, at)
        at += 4
        assert technique_count == 1, (filename, index, technique_count)
        technique = string(word(data, at))
        candidate = "Gen3_" + technique
        row = {"file": filename, "source_sha256": hashlib.sha256(raw).hexdigest(),
               "block": index, "class": kind, "label": string(word(data, 0)),
               "technique": technique, "exact_native_matches": catalog.get(technique, []),
               "candidate": candidate, "candidate_native_matches": catalog.get(candidate, []),
               "integer_metadata": extras, "colour_metadata": colours, "textures": [], "alpha_properties": []}
        for prop in properties:
            prop_kind, payload = block(prop)
            base = 12 + word(payload, 4)*4
            if prop_kind == "NiAlphaProperty":
                assert len(payload) == base + 3
                row["alpha_properties"].append({"flags": short(payload, base), "threshold": payload[base+2]})
            if prop_kind != "NiTexturingProperty":
                continue
            standard = word(payload, base+2)
            assert standard == 9 and payload[base+6:base+15] == bytes(9)
            pos = base+15
            maps = word(payload, pos)
            pos += 4
            assert maps == 1 and payload[pos] == 1
            pos += 1
            source = word(payload, pos)
            flags, anisotropy = short(payload, pos+4), short(payload, pos+6)
            assert payload[pos+8] == 0 and pos+13 == len(payload)
            _, texture = block(source, "NiSourceTexture")
            texture_base = 12 + word(texture, 4)*4
            assert len(texture) == texture_base+24 and texture[texture_base] == 1
            row["textures"].append({"source_block": source,
                "path": string(word(texture, texture_base+1)), "flags": flags,
                "anisotropy": anisotropy, "shader_map_id": word(payload, pos+9)})
        assert len(row["textures"]) == 1
        assert not row["exact_native_matches"], technique
        results.append(row)
assert len(results) == 6
assert sum(bool(r["candidate_native_matches"]) for r in results) == 5
output = {"shader_archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
          "systems": results,
          "conclusion": "All six source systems use material names absent from the staged FXLO string catalog. Five systems have exact Gen3-prefixed candidates; the distortion system has no such candidate. This identifies a mapping candidate, not a verified replacement: runtime alias behavior, native selection, blend/depth state and pixel equivalence still require validation."}
destination = ROOT / "analysis/em1-compatibility/particle-material-bindings.json"
destination.write_text(json.dumps(output, indent=2) + "\n")
print(f"Audited {len(results)} particle systems, six texture bindings, and exact staged material names")

