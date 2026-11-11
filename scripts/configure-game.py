"""Find verified Xbox SDK register helper sequences in the loaded EM2 image."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--game-dir", type=Path, default=Path("D:/Modding/Builds/EM2X360"))
    args = parser.parse_args()
    image = (ROOT / "private/image.bin").read_bytes()
    metadata = json.loads((ROOT / "analysis/image.json").read_text(encoding="utf-8-sig"))
    base = metadata["base"]
    patterns = {
        "restgprlr_14": (0xE9C1FF68, 0xE9E1FF70, 0xEA01FF78),
        "savegprlr_14": (0xF9C1FF68, 0xF9E1FF70, 0xFA01FF78),
        "restfpr_14": (0xC9CCFF70, 0xC9ECFF78, 0xCA0CFF80),
        "savefpr_14": (0xD9CCFF70, 0xD9ECFF78, 0xDA0CFF80),
        "restvmx_14": (0x3960FEE0, 0x7DCB60CE),
        "savevmx_14": (0x3960FEE0, 0x7DCB61CE),
        "restvmx_64": (0x3960FC00, 0x100B60CB),
        "savevmx_64": (0x3960FC00, 0x100B61CB),
        "setjmp": (0x3C808341, 0x8004ACC0, 0x2C000000, 0x7C0903A6,
                   0x4C820420, 0x7C0802A6, 0x7C800026, 0xD9C30000),
        "longjmp": (0x7C0802A6, 0x9421FFB0, 0x90010008, 0x7C862378,
                    0x2C040000, 0x80030138, 0x2C800000, 0x7C671B78),
    }
    helpers = {}
    for name, words in patterns.items():
        pattern = struct.pack(">" + "I" * len(words), *words)
        matches = []
        for section in metadata["sections"]:
            if not section["flags"] & 2:
                continue
            start = section["base"] - base
            end = start + section["size"]
            offset = image.find(pattern, start, end)
            while offset >= 0:
                if offset % 4 == 0:
                    matches.append(base + offset)
                offset = image.find(pattern, offset + 4, end)
        if len(matches) != 1:
            raise RuntimeError(f"Expected exactly one {name} sequence, found {list(map(hex, matches))}")
        helpers[name] = matches[0]
    config = ROOT / "config/em2.toml"
    config.parent.mkdir(parents=True, exist_ok=True)
    text = '[main]\nfile_path = "../private/default.xex"\nout_directory_path = "../generated/ppc"\nswitch_table_file_path = "em2_switch_tables.toml"\n\n'
    text += "# All register optimizations stay disabled for initial runtime integration.\n"
    for name, address in helpers.items():
        text += f"{name}_address = 0x{address:08X}\n"
    # Xbox VEH metadata at the tail of .text is data, not PowerPC code.
    marker = bytes.fromhex("00485645")
    for section in metadata["sections"]:
        if section["name"] != ".text":
            continue
        end = section["base"] - base + section["size"]
        offset = image.find(marker, max(0, end - 4096), end)
        if offset >= 8 and image[offset-4:offset] == bytes.fromhex("48000000") and image[offset+4:offset+8] == bytes.fromhex("48000000"):
            text += f'\n[[main.invalid_instructions]]\ndata = 0x00485645\nsize = 0x{end-offset:X}\n'
    config.write_text(text, encoding="utf-8")
    metadata["helpers"] = helpers
    metadata["xex_sha256"] = hashlib.sha256((args.game_dir / "default.xex").read_bytes()).hexdigest()
    (ROOT / "analysis/image.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({name: hex(address) for name, address in helpers.items()}, indent=2))


if __name__ == "__main__":
    main()
