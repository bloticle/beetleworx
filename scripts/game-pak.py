"""Inspect/extract EM2 version-2 PAKs without changing the source archive.

Format reference: https://github.com/bloticle/epic-mickey-lib-rs/blob/master/src/packfile.rs
Extracted game assets belong under ignored analysis/, never in source control.
"""
import argparse
from pathlib import Path, PurePosixPath
import struct
import zlib


def read_pak(path):
    data = Path(path).read_bytes()
    if data[:4] not in (b" KAP", b"PAK "):
        raise ValueError("Not an EM2 PAK")
    endian = ">" if data[:4] == b" KAP" else "<"
    def u32(offset):
        return struct.unpack_from(endian + "I", data, offset)[0]
    if u32(4) != 2 or u32(8) != 0:
        raise ValueError("Unsupported PAK version/header")
    header = u32(12)
    payload = header + u32(16)
    count = u32(header)
    strings = header + 4 + count * 24
    if not (32 <= header < strings <= payload <= len(data)):
        raise ValueError("Invalid PAK directory boundaries")
    def string(offset):
        start = strings + offset
        if not strings <= start < payload:
            raise ValueError("Invalid string pointer")
        end = data.index(b"\0", start, payload)
        return data[start:end].decode("utf-8")
    files = []
    for index in range(count):
        size, packed, aligned, directory, kind, name = struct.unpack_from(
            endian + "6I", data, header + 4 + index * 24)
        if aligned % 32 or not packed <= aligned or payload + aligned > len(data):
            raise ValueError("Invalid PAK payload boundaries")
        folder = string(directory)
        filename = string(name)
        path = (folder + "/" if folder else "") + filename
        blob = data[payload:payload + packed]
        if packed != size:
            blob = zlib.decompress(blob)
        if len(blob) != size:
            raise ValueError("Decompressed size mismatch: " + path)
        kind_bytes = data[header + 4 + index * 24 + 16:header + 4 + index * 24 + 20]
        files.append((path, blob, kind_bytes))
        payload += aligned
    return files


def write_pak(files, destination):
    strings = bytearray()
    entries = []
    payloads = []
    for name, blob, kind in files:
        folder, _, filename = name.replace("\\", "/").rpartition("/")
        folder_offset = len(strings)
        strings.extend(folder.encode() + b"\0")
        name_offset = len(strings)
        strings.extend(filename.encode() + b"\0")
        packed = zlib.compress(blob, 6)
        if len(packed) >= len(blob):
            packed = blob
        aligned = (len(packed) + 31) & ~31
        entries.append(struct.pack(">4I", len(blob), len(packed), aligned, folder_offset)
                       + kind + struct.pack(">I", name_offset))
        payloads.append(packed + bytes(aligned - len(packed)))
    directory = struct.pack(">I", len(files)) + b"".join(entries) + strings
    payload_offset = (32 + len(directory) + 31) & ~31
    header = b" KAP" + struct.pack(">4I", 2, 0, 32, payload_offset - 32) + bytes(12)
    result = header + directory + bytes(payload_offset - 32 - len(directory)) + b"".join(payloads)
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(result)
    if read_pak(destination) != files:
        raise ValueError("PAK round-trip verification failed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--extract", type=Path)
    args = parser.parse_args()
    for name, blob, _kind in read_pak(args.archive):
        print(f"{len(blob):9d}  {name}")
        if args.extract:
            clean = PurePosixPath(name.replace("\\", "/"))
            if clean.is_absolute() or any(part in ("..", "") or ":" in part for part in clean.parts):
                raise ValueError("Unsafe archive path: " + name)
            destination = args.extract.joinpath(*clean.parts)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(blob)


if __name__ == "__main__":
    main()
