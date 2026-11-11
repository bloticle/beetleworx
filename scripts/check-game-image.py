"""Match a decoded XEX against the address layout used by the native hooks."""
import argparse
import hashlib
import json
from pathlib import Path


SUPPORTED_IMAGE = "7bf582d9dc1525b20cf20d20917c23c697144c976c727d353226b7c0a68a663c"
SUPPORTED_LAYOUT = "520f5d88922640cc59df7c06c6156b7efe3d01ab21b57ba15ad2b8880f159a97"


def file_digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("image", type=Path)
    parser.add_argument("metadata", type=Path)
    parser.add_argument("--stamp", type=Path)
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
        if metadata.get("xex_sha256") != executable_hash:
            raise SystemExit("The verified XEX changed during translation.")
        args.stamp.write_text(
            f'#pragma once\ninline constexpr char kTranslatedGameXex[] = "{executable_hash}";\n',
            encoding="utf-8",
        )
    print(f"Verified Xbox 360 executable layout ({executable_hash})")


if __name__ == "__main__":
    main()
