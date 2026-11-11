"""Prepare pinned Rust/Lua notices; packaging verification needs no Cargo install."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tomllib


ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "runtime/em1_lua_core/Cargo.toml"
LOCK = MANIFEST.with_name("Cargo.lock")
OUTPUT = ROOT / "docs/licenses/rust-dependencies.txt"
INDEX = OUTPUT.with_suffix(".json")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def generate():
    metadata = json.loads(subprocess.check_output([
        "cargo", "metadata", "--manifest-path", str(MANIFEST),
        "--format-version", "1", "--offline", "--locked",
        "--filter-platform", "x86_64-pc-windows-msvc",
    ], encoding="utf-8"))
    resolved = {node["id"] for node in metadata["resolve"]["nodes"]}
    packages = [p for p in metadata["packages"]
                if p["id"] in resolved and p["source"] is not None]
    packages.sort(key=lambda p: (p["name"], p["version"]))
    locked = {(p["name"], p["version"]): p
              for p in tomllib.loads(LOCK.read_text(encoding="utf-8"))["package"]}
    notices = {}
    records = []
    for package in packages:
        directory = Path(package["manifest_path"]).parent
        files = sorted(p for p in directory.iterdir() if p.is_file() and
                       p.name.lower().startswith(("license", "copying", "copyright", "notice")))
        shared_from = None
        if not files and package["name"] == "mlua-sys":
            # This crate lives below the project's root LICENSE. Require the
            # exact same source revision, not merely a similar MIT declaration.
            parent = next(p for p in packages if p["name"] == "mlua")
            parent_dir = Path(parent["manifest_path"]).parent
            source = json.loads((directory / ".cargo_vcs_info.json").read_text())
            parent_source = json.loads((parent_dir / ".cargo_vcs_info.json").read_text())
            if (source["git"]["sha1"] != parent_source["git"]["sha1"] or
                    package["repository"] != parent["repository"] or
                    source["path_in_vcs"] != "mlua-sys" or parent_source["path_in_vcs"]):
                raise ValueError("mlua-sys/root license provenance changed; review exact source")
            files = [parent_dir / "LICENSE"]
            shared_from = {"package": parent["name"], "version": parent["version"],
                           "git_revision": source["git"]["sha1"]}
        if not files:
            raise ValueError(f"Missing notices: {package['name']} {package['version']}")
        sources = []
        for path in files:
            raw = path.read_bytes()
            key = digest(raw)
            notices.setdefault(key, raw.decode("utf-8"))
            sources.append({"file": path.name, "sha256": key})
        record = {"name": package["name"], "version": package["version"],
                  "declared_license": package["license"], "source": package["source"],
                  "package_checksum": locked[(package["name"], package["version"])]["checksum"],
                  "notices": sources}
        if shared_from:
            record["shared_root_notice"] = shared_from
        records.append(record)
    lock_hash = digest(LOCK.read_bytes())
    lines = ["Rust/Lua resolved dependency notices", f"Cargo.lock SHA-256: {lock_hash}", "",
             "Includes resolved build dependencies and optional source packages; this is",
             "not a claim that every listed package is linked into the player executable.",
             "Notice texts are reproduced from the exact cached source packages.", ""]
    for record in records:
        lines.extend([f"{record['name']} {record['version']}",
                      f"Declared license: {record['declared_license']}"])
        for notice in record["notices"]:
            lines.append(f"  {notice['file']}: notice {notice['sha256']}")
        if "shared_root_notice" in record:
            origin = record["shared_root_notice"]
            lines.append(f"  Shared project root LICENSE at revision {origin['git_revision']}")
        lines.append("")
    for key, notice in sorted(notices.items()):
        lines.extend(["=" * 72, f"Notice {key}", "=" * 72, notice, ""])
    raw = "\n".join(lines).encode("utf-8")
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(raw)
    INDEX.write_text(json.dumps({"format": 1, "cargo_lock_sha256": lock_hash,
                                "notices_sha256": digest(raw), "packages": records},
                               indent=2) + "\n", encoding="utf-8")


def check():
    index = json.loads(INDEX.read_text(encoding="utf-8"))
    if (index.get("format") != 1 or index["cargo_lock_sha256"] != digest(LOCK.read_bytes()) or
            index["notices_sha256"] != digest(OUTPUT.read_bytes())):
        raise ValueError("Rust notices are stale or modified; run scripts/prepare-rust-notices.py")
    locked = {(p["name"], p["version"], p.get("checksum"))
              for p in tomllib.loads(LOCK.read_text(encoding="utf-8"))["package"]}
    seen = set()
    for package in index["packages"]:
        identity = (package["name"], package["version"], package["package_checksum"])
        if identity not in locked or identity in seen or not package["notices"]:
            raise ValueError("Invalid or duplicate dependency notice record")
        seen.add(identity)
    if not seen:
        raise ValueError("Dependency notices are empty")
    print(json.dumps({"packages": len(seen), "bytes": OUTPUT.stat().st_size,
                      "sha256": index["notices_sha256"]}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if not args.check:
        generate()
    check()
