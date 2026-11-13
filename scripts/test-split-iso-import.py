"""Check native raw split ISO import using independent byte-split fixtures."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--exe", type=Path, required=True)
parser.add_argument("--work", type=Path, required=True)
parser.add_argument("--report", type=Path)
args = parser.parse_args()
exe = args.exe.resolve(strict=True)
work = args.work.resolve()
work.mkdir(parents=True, exist_ok=True)
root = Path(tempfile.mkdtemp(prefix="split-import-", dir=work)).resolve()
reference = root / "reference"
(reference / "packfiles").mkdir(parents=True)
xex = bytearray(80)
xex[:4] = b"XEX2"
struct.pack_into(">III", xex, 20, 1, 0x40006, 48)
struct.pack_into(">I", xex, 60, 0x42560816)
payload = bytes(range(256)) * 1031
files = {"ConfigFiles.ini": b"config", "default.xex": bytes(xex), "packfiles/level.pak": payload}
image = bytearray(((46 * 2048 + len(payload) + 2047) // 2048) * 2048)
image[0x10000:0x10014] = image[0x107ec:0x10800] = b"MICROSOFT*XBOX*MEDIA"
struct.pack_into("<II", image, 0x10014, 40, 2048)
def entry(table, at, name, sector, size, directory=False, right=0):
    encoded = name.encode()
    struct.pack_into("<HHIIBB", image, table * 2048 + at, 0, right, sector, size,
                     0x10 if directory else 0x80, len(encoded))
    image[table * 2048 + at + 14:table * 2048 + at + 14 + len(encoded)] = encoded
entry(40, 0, "ConfigFiles.ini", 43, 6, right=8)
entry(40, 32, "default.xex", 44, 80, right=16)
entry(40, 64, "packfiles", 42, 2048, True)
entry(42, 0, "level.pak", 46, len(payload))
for name, sector in [("ConfigFiles.ini", 43), ("default.xex", 44), ("packfiles/level.pak", 46)]:
    data = files[name]
    image[sector * 2048:sector * 2048 + len(data)] = data
    (reference / name).write_bytes(data)
identity = hashlib.sha256(image).hexdigest()
results = []
def run(name, selected, good=True, output=None):
    destination = root / ((output or name) + "-output")
    log = root / (name + ".log")
    command = [str(exe), f"--em2_test_xbox_disc={selected}",
               f"--em2_test_xbox_reference={reference}", f"--em2_test_xbox_destination={destination}",
               f"--em2_user_data_directory={root / (name + '-profile')}", f"--log_file={log}"]
    result = subprocess.run(command, cwd=exe.parent, timeout=60,
                            creationflags=0x08000000 if os.name == "nt" else 0,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    text = log.read_text(errors="replace") if log.exists() else ""
    assert result.returncode == (0 if good else 1), (name, result.returncode, text[-4000:], root)
    if good:
        assert "Read-only Xbox disc comparison passed" in text, (name, text)
        for path, data in files.items():
            assert (destination / path).read_bytes() == data, (name, path)
        assert (destination / ".em2-iso-import/identity.txt").read_text() == identity + "\n", name
    elif output is None:
        assert not destination.exists(), (name, "Invalid split set reached extraction")
    results.append({"name": name, "result": "PASS"})
    print(name + ": PASS", flush=True)
try:
    raw = root / "original.iso"
    raw.write_bytes(image)
    run("raw", raw)
    for ext in ["iso", "xiso"]:
        for scheme in ["xbox", "zero", "padded"]:
            name = scheme + "-" + ext
            folder = root / name
            folder.mkdir()
            # Eleven pieces exercise selection of two-digit part numbers.
            # Deliberately unaligned cuts split filesystem records and file data.
            cuts = [len(image) * i // 11 for i in range(12)]
            cuts[2] = 0x10005
            cuts = sorted(cuts)
            parts = []
            for i in range(11):
                filename = f"disc.{i+1}.{ext}" if scheme == "xbox" else f"disc.{ext}.{i}" if scheme == "zero" else f"disc.{ext}.{i+1:03}"
                part = folder / filename
                part.write_bytes(image[cuts[i]:cuts[i+1]])
                parts.append(part)
            run(name + "-last", parts[-1], output="raw")
            run(name + "-first", parts[0])
            for index, label in [(0, "missing-first"), (5, "missing-middle"), (10, "empty-final")]:
                data = parts[index].read_bytes()
                if label == "empty-final":
                    parts[index].write_bytes(b"")
                else:
                    parts[index].unlink()
                run(name + "-" + label, parts[1], False)
                parts[index].write_bytes(data)
    if args.report:
        args.report.write_text(json.dumps({"result": "PASS", "cases": results}, indent=2))
    # Delete only the freshly created fixture tree inside the specified workspace.
    assert root.resolve() == root and root.parent == work and root.name.startswith("split-import-")
    shutil.rmtree(root)
except BaseException:
    print("Retained failed fixtures: " + str(root), flush=True)
    raise
