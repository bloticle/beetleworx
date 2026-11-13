"""Match a decoded XEX against the address layout used by the native hooks."""
import argparse
import hashlib
import json
import struct
from pathlib import Path


SUPPORTED_IMAGE = "7bf582d9dc1525b20cf20d20917c23c697144c976c727d353226b7c0a68a663c"
SUPPORTED_LAYOUT = "520f5d88922640cc59df7c06c6156b7efe3d01ab21b57ba15ad2b8880f159a97"


def execution_profile(data):
    def word(offset):
        if offset < 0 or offset + 4 > len(data):
            raise ValueError("Truncated XEX header")
        return struct.unpack_from(">I", data, offset)[0]

    if data[:4] != b"XEX2" or len(data) < 24:
        raise ValueError("Not an XEX2 executable")
    limit, security, count = word(8), word(16), word(20)
    if not 24 <= limit <= min(len(data), 1024 * 1024) or count > 256 or 24 + count * 8 > limit:
        raise ValueError("Invalid XEX header bounds")
    if security < 24 + count * 8 or security + 0x184 > limit:
        raise ValueError("Invalid XEX security bounds")
    pages = word(security + 0x180)
    if not pages or pages > 4096 or security + 0x184 + pages * 24 > limit:
        raise ValueError("Invalid XEX page descriptors")
    profile = bytearray(b"Beetleworx-XEX-execution-v2")
    for offset in (4, security + 4, security + 0x10C, security + 0x110, security + 0x160):
        profile += struct.pack(">I", word(offset))
    profile += struct.pack(">I", pages)
    for index in range(pages):
        profile += struct.pack(">I", word(security + 0x184 + index * 24))
    headers = {}
    for index in range(count):
        key, value = word(24 + index * 8), word(28 + index * 8)
        if key in headers:
            raise ValueError("Duplicate XEX optional header")
        size = key & 255
        if size <= 1:
            payload = struct.pack(">I", value)
        else:
            size = word(value) if size == 255 else size * 4
            if value < 24 + count * 8 or not size or value + size > limit:
                raise ValueError("Invalid XEX optional header bounds")
            payload = data[value:value + size]
        headers[key] = payload
    # Encoding changes are permitted only after exact decoded-image validation.
    # All other optional headers, including imports and TLS, remain identical.
    for key, payload in sorted(headers.items()):
        if key not in (0x000003FF,0x00040310,0x000405FF,0x000183FF):
            if key==0x00040006: payload=payload[4:]
            profile += struct.pack(">II", key, len(payload)) + payload
    return hashlib.sha256(profile).hexdigest()


def file_digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("image", type=Path)
    parser.add_argument("metadata", type=Path)
    parser.add_argument("--stamp", type=Path)
    parser.add_argument("--raw-image", type=Path)
    args = parser.parse_args()

    metadata = json.loads(args.metadata.read_text(encoding="utf-8-sig"))
    layout = {key: metadata[key] for key in ("base", "size", "entry_point", "sections", "imports")}
    layout_hash = hashlib.sha256(
        json.dumps(layout, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    image_hash = file_digest(args.image)
    executable_hash = file_digest(args.executable)
    if (image_hash, layout_hash) != (SUPPORTED_IMAGE, SUPPORTED_LAYOUT):
        raise SystemExit(
            "This Xbox 360 build has an unverified executable layout. "
            "Its native hooks need a build-specific port.\n"
            f"XEX SHA-256: {executable_hash}\nDecoded image SHA-256: {image_hash}"
        )
    if args.stamp is not None:
        if args.raw_image is None or args.raw_image.stat().st_size != metadata["size"]:
            raise SystemExit("Stamp generation requires the untouched decoded image from em2-inspect.")
        if metadata.get("xex_sha256") != executable_hash:
            raise SystemExit("The verified XEX changed during translation.")
        args.stamp.write_text(
            f'#pragma once\ninline constexpr char kTranslatedGameXex[] = "{executable_hash}";\n'
            f'inline constexpr char kTranslatedGameImage[] = "{file_digest(args.raw_image)}";\n'
            f'inline constexpr char kTranslatedGameProfile[] = "{execution_profile(args.executable.read_bytes())}";\n'
            f'inline constexpr unsigned kTranslatedGameBase = {metadata["base"]};\n'
            f'inline constexpr unsigned kTranslatedGameSize = {metadata["size"]};\n',
            encoding="utf-8",
        )
    print(f"Verified Xbox 360 executable layout ({executable_hash})")


if __name__ == "__main__":
    main()
