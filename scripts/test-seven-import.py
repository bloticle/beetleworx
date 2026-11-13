"""Exercise the native 7z importer with independent py7zr fixtures."""
import argparse
import hashlib
import binascii
import io
import json
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import tempfile
import sys
import importlib

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--exe", type=Path, required=True)
parser.add_argument("--work", type=Path, required=True)
parser.add_argument("--report", type=Path)
parser.add_argument("--keep", action="store_true")
parser.add_argument("--modules", type=Path, help="Optional private py7zr module directory")
args = parser.parse_args()
if args.modules:
    sys.path.insert(0, str(args.modules.resolve(strict=True)))
py7zr = importlib.import_module("py7zr")
exe = args.exe.resolve(strict=True)
work = args.work.resolve()
work.mkdir(parents=True, exist_ok=True)
root = Path(tempfile.mkdtemp(prefix="seven-import-", dir=work)).resolve()
assert root.parent == work and root.name.startswith("seven-import-")
reference = root / "reference"
(reference / "packfiles").mkdir(parents=True)
xex = bytearray(80)
xex[:4] = b"XEX2"
struct.pack_into(">III", xex, 20, 1, 0x40006, 48)
struct.pack_into(">I", xex, 60, 0x42560816)
rng = random.Random(0x42560816)
payload = b"".join(rng.randbytes(1024) * 32 for _ in range(65))[:2 * 1024 * 1024 + 171]
content = {"default.xex": bytes(xex), "ConfigFiles.ini": b"config",
           "packfiles/level.pak": payload, "empty.bin": b""}
for name, data in content.items():
    (reference / name).write_bytes(data)


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
    source_hash = hashlib.sha256(path.read_bytes()).digest()
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
        assert not destination.exists(), (name, "malformed 7z reached extraction")
    assert hashlib.sha256(path.read_bytes()).digest() == source_hash, (name, "Source was modified")
    results.append({"name": name, "expected": "accept" if good else "reject",
                    "exit_code": process.returncode, "log_tail": text.splitlines()[-2:]})
    print(name + ": PASS", flush=True)



def make(path, *, method=None, prefix="", iso=False, encoded=True, password=None, special=None, append=False, header_encryption=False, directories=False):
    values = {"Disc.iso": make_iso()} if iso else dict(content)
    if special == "traversal": values["safe-escape"] = b"bad"
    if special == "duplicate": values["CONFIGFILES.INI"] = b"config"
    if append:
        values.pop("ConfigFiles.ini")
    filters = method or [{"id": py7zr.FILTER_LZMA2, "dict_size": 1 << 20}]
    if password:
        filters = filters + [{"id": py7zr.FILTER_CRYPTO_AES256_SHA256}]
    with py7zr.SevenZipFile(path, "w", filters=filters, password=password, header_encryption=header_encryption) as archive:
        archive.set_encoded_header_mode(encoded)
        if directories:
            archive.write(reference / "packfiles", prefix + "packfiles")
        for name, data in values.items():
            archive.writestr(data, prefix + name)
            if special == "traversal" and name == "safe-escape":
                archive.header.files_info.files[-1]["filename"] = "../escape"
            if special == "symlink" and name == "empty.bin":
                archive.header.files_info.files[-1]["attributes"] = 0xA1FF0000
    if append:
        with py7zr.SevenZipFile(path, "a", filters=filters) as archive:
            archive.writestr(content["ConfigFiles.ini"], "ConfigFiles.ini")
    return path

try:
    before_cache = set(Path(tempfile.gettempdir()).glob("beetleworx-7z-*.tmp"))
    cases = [
        ("lzma2-solid", {}),
        ("copy", {"method": [{"id": py7zr.FILTER_COPY}]}),
        ("lzma", {"method": [{"id": py7zr.FILTER_LZMA, "dict_size": 1 << 20}]}),
        ("bcj-lzma2", {"method": [{"id": py7zr.FILTER_X86}, {"id": py7zr.FILTER_LZMA2, "dict_size": 1 << 20}]}),
        ("plain-header", {"encoded": False}),
        ("explicit-directory", {"directories": True}),
        ("multiple-blocks", {"append": True}),
        ("wrapped-unicode", {"prefix": "Épic Mickey/"}),
        ("contained-iso", {"iso": True}),
    ]
    for name, options in cases:
        run(name, make(root / (name + ".7z"), **options), iso=options.get("iso", False))
    run("resume-solid", root / "lzma2-solid.7z", resume="lzma2-solid")
    run("misnamed", make(root / "archive.zip"))
    run("unicode-source", make(root / "\u00c9pic Mickey.7z"))
    for name in ["traversal", "duplicate", "symlink"]:
        run(name, make(root / (name + ".7z"), special=name), False)
    run("encrypted", make(root / "encrypted.7z", password="private-fixture-password"), False)
    run("encrypted-header", make(root / "encrypted-header.7z", password="private-fixture-password", header_encryption=True), False)
    cancel_log = root / "cancel.log"
    cancel = subprocess.run([str(exe), f"--em2_test_xbox_disc={root / 'lzma2-solid.7z'}",
                             "--em2_test_seven_cancel=true", f"--em2_user_data_directory={root / 'cancel-profile'}",
                             f"--log_file={cancel_log}"], cwd=exe.parent, timeout=60,
                            creationflags=0x08000000 if os.name == "nt" else 0,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    assert cancel.returncode == 0 and "7z cancellation regression: PASS" in cancel_log.read_text(), cancel_log
    results.append({"name": "cancel-during-decode", "result": "PASS"})
    print("cancel-during-decode: PASS", flush=True)
    # Advertise a 4 GiB LZMA2 dictionary in a valid plain header, preserving its CRCs.
    oversized = bytearray((root / "plain-header.7z").read_bytes())
    header_at = 32 + struct.unpack_from("<Q", oversized, 12)[0]
    coder = oversized.index(b"\x21\x21\x01\x10", header_at)
    oversized[coder + 3] = 40
    struct.pack_into("<I", oversized, 28, binascii.crc32(oversized[header_at:]))
    struct.pack_into("<I", oversized, 8, binascii.crc32(oversized[12:32]))
    path = root / "oversized-dictionary.7z"
    path.write_bytes(oversized)
    run("oversized-dictionary", path, False)
    original = (root / "copy.7z").read_bytes()
    broken = bytearray(original)
    broken[33] ^= 1
    path = root / "bad-data-crc.7z"
    path.write_bytes(broken)
    run("bad-data-crc", path, False)
    path = root / "truncated.7z"
    path.write_bytes(original[:-10])
    run("truncated", path, False)
    assert set(Path(tempfile.gettempdir()).glob("beetleworx-7z-*.tmp")) == before_cache, "7z cache leaked after success/failure"
    if args.report:
        args.report.write_text(json.dumps({"result": "PASS", "cases": results}, indent=2))
    if not args.keep:
        assert root.resolve() == root and root.parent == work and root.name.startswith("seven-import-")
        shutil.rmtree(root)
except BaseException:
    print("Retained failed fixtures: " + str(root), flush=True)
    raise
