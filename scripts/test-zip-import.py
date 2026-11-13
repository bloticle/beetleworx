"""Exercise the native ZIP importer with independent standard-library fixtures."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import tempfile
import zipfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--exe", type=Path, required=True)
parser.add_argument("--work", type=Path, required=True)
parser.add_argument("--report", type=Path)
parser.add_argument("--keep", action="store_true")
args = parser.parse_args()
exe = args.exe.resolve(strict=True)
work = args.work.resolve()
work.mkdir(parents=True, exist_ok=True)
root = Path(tempfile.mkdtemp(prefix="zip-import-", dir=work)).resolve()
assert root.parent == work and root.name.startswith("zip-import-")
reference = root / "reference"
(reference / "packfiles").mkdir(parents=True)
xex = bytearray(80)
xex[:4] = b"XEX2"
struct.pack_into(">III", xex, 20, 1, 0x40006, 48)
struct.pack_into(">I", xex, 60, 0x42560816)
rng = random.Random(0x42560816)
payload = b"".join(rng.randbytes(1024) * 32 for _ in range(257))[:8 * 1024 * 1024 + 171]
content = {"default.xex": bytes(xex), "ConfigFiles.ini": b"config",
           "packfiles/level.pak": payload, "empty.bin": b""}
for name, data in content.items():
    (reference / name).write_bytes(data)


class Streaming(io.BytesIO):
    def seek(self, *unused):
        raise io.UnsupportedOperation("streaming fixture")


def make(path, *, method=zipfile.ZIP_DEFLATED, prefix="", wide=False,
         streaming=False, iso=False, comment=b"", special=None, directories=False, prelude=False):
    sink = Streaming() if streaming else io.BytesIO()
    if prelude:
        sink.write(b"Private archive prelude\0" * 256)
    old_limit = zipfile.ZIP64_LIMIT
    if wide:
        zipfile.ZIP64_LIMIT = 0
    try:
        with zipfile.ZipFile(sink, "w", compression=method, allowZip64=True) as archive:
            archive.comment = comment
            if directories:
                archive.writestr(prefix + "packfiles/", b"")
            values = {"Disc.iso": make_iso()} if iso else dict(content)
            if special == "traversal": values["../escape"] = b"bad"
            if special == "duplicate": values["CONFIGFILES.INI"] = b"config"
            for name, data in values.items():
                if special == "symlink" and name == "empty.bin":
                    info = zipfile.ZipInfo(prefix + name)
                    info.create_system = 3
                    info.external_attr = 0o120777 << 16
                    archive.writestr(info, b"../escape")
                else:
                    with archive.open(prefix + name, "w", force_zip64=wide) as file:
                        file.write(data)
    finally:
        zipfile.ZIP64_LIMIT = old_limit
    data = bytearray(sink.getvalue())
    if wide and not streaming:
        # Force the sentinel EOCD fields so the native ZIP64 locator path runs.
        end = data.rfind(b"PK\x05\x06")
        struct.pack_into("<HHII", data, end + 8, 65535, 65535, 0xffffffff, 0xffffffff)
    path.write_bytes(data)
    return path


def make_iso():
    image = bytearray(((46 * 2048 + len(payload) + 2047) // 2048) * 2048)
    image[0x10000:0x10014] = image[0x107ec:0x10800] = b"MICROSOFT*XBOX*MEDIA"
    struct.pack_into("<II", image, 0x10014, 40, 2048)
    def entry(table, at, name, sector, size, directory=False, right=0):
        encoded = name.encode()
        struct.pack_into("<HHIIBB", image, table * 2048 + at,
                         0, right, sector, size, 0x10 if directory else 0x80, len(encoded))
        image[table * 2048 + at + 14:table * 2048 + at + 14 + len(encoded)] = encoded
    entry(40, 0, "ConfigFiles.ini", 43, 6, right=8)
    entry(40, 32, "default.xex", 44, 80, right=16)
    entry(40, 64, "packfiles", 42, 2048, True)
    entry(42, 0, "level.pak", 46, len(payload))
    image[43 * 2048:43 * 2048 + 6] = b"config"
    image[44 * 2048:44 * 2048 + 80] = xex
    image[46 * 2048:46 * 2048 + len(payload)] = payload
    return bytes(image)


results = []


def run(name, path, good=True, iso=False, resume=None):
    destination = root / ((resume or name) + "-output")
    log = root / (name + ".log")
    command = [str(exe), f"--em2_test_xbox_disc={path}",
               f"--em2_test_xbox_reference={reference}",
               f"--em2_test_xbox_destination={destination}",
               f"--em2_user_data_directory={root / (name + '-profile')}",
               f"--log_file={log}"]
    process = subprocess.run(command, cwd=exe.parent, timeout=90,
                             creationflags=0x08000000 if os.name == "nt" else 0,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    text = log.read_text(errors="replace") if log.exists() else ""
    assert process.returncode == (0 if good else 1), (name, process.returncode, text[-4000:], root)
    if good:
        assert "Read-only Xbox disc comparison passed" in text, (name, text)
        expected = {k: v for k, v in content.items() if not iso or k != "empty.bin"}
        for relative, data in expected.items():
            assert (destination / relative).read_bytes() == data, (name, relative)
        identity = hashlib.sha256(hashlib.sha256(path.read_bytes()).digest()).hexdigest()
        assert (destination / ".em2-iso-import/identity.txt").read_text() == identity + "\n", name
    else:
        assert not destination.exists(), (name, "malformed ZIP reached extraction")
    results.append({"name": name, "expected": "accept" if good else "reject",
                    "exit_code": process.returncode, "log_tail": text.splitlines()[-2:]})
    print(name + ": PASS", flush=True)


try:
    positives = [
        ("stored", {"method": zipfile.ZIP_STORED}),
        ("deflate", {}),
        ("wrapped-unicode", {"prefix": "Épic Mickey/"}),
        ("zip64", {"wide": True}),
        ("descriptor32", {"streaming": True}),
        ("descriptor64", {"wide": True, "streaming": True}),
        ("stored-descriptor", {"method": zipfile.ZIP_STORED, "streaming": True}),
        ("explicit-directory", {"directories": True}),
        ("comment", {"comment": b"Native game archive regression"}),
        ("comment-end-signature", {"comment": b"PK\x05\x06" + bytes(18)}),
        ("prefixed-zip", {"prelude": True}),
        ("stored-iso", {"method": zipfile.ZIP_STORED, "iso": True}),
        ("deflate-iso", {"iso": True}),
    ]
    for name, options in positives:
        run(name, make(root / (name + ".zip"), **options), iso=options.get("iso", False))
    run("resume-deflate", root / "deflate.zip", resume="deflate")
    run("misnamed-extension", make(root / "zip-with-zar-extension.zar"))
    run("seven-extension", make(root / "zip-with-seven-extension.7z"))
    reordered = bytearray((root / "deflate.zip").read_bytes())
    with zipfile.ZipFile(io.BytesIO(reordered)) as archive:
        start = archive.start_dir
        at = start
        records = []
        for unused in archive.infolist():
            length = 46 + sum(struct.unpack_from("<HHH", reordered, at + 28))
            records.append(bytes(reordered[at:at + length]))
            at += length
    reordered[start:at] = b"".join(reversed(records))
    path = root / "reordered-directory.zip"
    path.write_bytes(reordered)
    run("reordered-directory", path)
    for name in ["traversal", "duplicate", "symlink"]:
        run(name, make(root / (name + ".zip"), special=name), False)
    original = (root / "deflate.zip").read_bytes()
    for name in ["crc", "encryption", "unsupported", "local-name", "truncated", "missing-zip64"]:
        data = bytearray(original)
        with zipfile.ZipFile(io.BytesIO(original)) as archive:
            info = archive.getinfo("packfiles/level.pak")
            local = info.header_offset
        central = data.find(b"PK\x01\x02", data.find(b"PK\x01\x02"))
        while data[central + 46:central + 46 + struct.unpack_from("<H", data, central + 28)[0]] != b"packfiles/level.pak":
            central += 46 + sum(struct.unpack_from("<HHH", data, central + 28))
        if name == "crc":
            for at in [local + 14, central + 16]: struct.pack_into("<I", data, at, struct.unpack_from("<I", data, at)[0] ^ 1)
        elif name == "encryption":
            for at in [local + 6, central + 8]: struct.pack_into("<H", data, at, struct.unpack_from("<H", data, at)[0] | 1)
        elif name == "unsupported":
            for at in [local + 8, central + 10]: struct.pack_into("<H", data, at, 12)
        elif name == "local-name": data[local + 30] ^= 1
        elif name == "truncated": del data[-13:]
        elif name == "missing-zip64": struct.pack_into("<I", data, central + 24, 0xffffffff)
        path = root / (name + ".zip")
        path.write_bytes(data)
        run(name, path, False)
    path = make(root / "bad-descriptor.zip", streaming=True)
    data = bytearray(path.read_bytes())
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        info = archive.getinfo("packfiles/level.pak")
        name_len, extra_len = struct.unpack_from("<HH", data, info.header_offset + 26)
        descriptor = info.header_offset + 30 + name_len + extra_len + info.compress_size
        assert data[descriptor:descriptor + 4] == b"PK\x07\x08"
        data[descriptor + 4] ^= 1
    path.write_bytes(data)
    run("bad-descriptor", path, False)
    report = {"result": "PASS", "cases": results, "fixture_payload_bytes": len(payload),
              "fixture_payload_sha256": hashlib.sha256(payload).hexdigest(),
              "archive_producer": "Python standard-library zipfile",
              "coverage": "Stored, DEFLATE, ZIP64, streaming descriptors, implicit folders, Unicode wrapper, empty files, contained ISOs, extraction/resume reads and backward checkpoint seeks"}
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"result": "PASS", "cases": len(results)}))
except BaseException:
    print("Preserved failing fixtures: " + str(root), flush=True)
    raise
else:
    if not args.keep:
        resolved_root = root.resolve(strict=True)
        assert resolved_root == root and resolved_root.parent == work
        assert root.is_relative_to(work) and root.name.startswith("zip-import-")
        shutil.rmtree(root)
