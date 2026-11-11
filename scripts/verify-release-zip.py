"""Read-only verification of the published Beetleworx ZIP payload and manifest."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import stat
import zipfile


EXE = 'beetleworx.exe'
MANIFEST = 'release-manifest.json'
PATCHES = ('UIOptions.pak', 'UIPauseOptions.pak', 'UIhud.pak', 'UIStartGame.pak',
           'UIMainMenu.pak', '_Dynamic.pak', 'shaders.pak')
ROOT_FILES = {EXE, 'dxcompiler.dll', 'dxil.dll', 'START HERE.txt',
              'TESTING & KNOWN ISSUES.txt', 'demo-channel.txt'}
CRT_FILES = {'concrt140.dll', 'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll',
             'msvcp140_atomic_wait.dll', 'msvcp140_codecvt_ids.dll', 'vccorlib140.dll',
             'vcruntime140.dll', 'vcruntime140_1.dll', 'vcruntime140_threads.dll'}
REQUIRED_CRT = {'msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'}
REQUIRED_PATCHES = {'pc-assets/patches/' + p + '.em2patch' for p in PATCHES}
REQUIRED_PATCHES.add('pc-assets/patches/manifest.json')
CATALOG = 'pc-assets/pc-shader-catalog.em2'


def need(condition, why):
    if not condition:
        raise ValueError(why)


def unique_json(pairs):
    result = {}
    for key, value in pairs:
        need(key not in result, f'Duplicate JSON key: {key}')
        result[key] = value
    return result


def digest(value):
    return isinstance(value, str) and re.fullmatch(r'[0-9A-Fa-f]{64}', value)


def path_ok(name):
    need(name.isascii() and 0 < len(name) < 240 and '\\' not in name, f'Invalid ZIP path: {name!r}')
    for part in name.split('/'):
        stem = part.split('.')[0].lower()
        need(part and part not in ('.', '..') and part[-1] not in '. ' and
             not any(ord(c) < 32 or ord(c) == 127 or c in ':*?"<>|' for c in part) and
             stem not in {'con', 'prn', 'aux', 'nul'} and not re.fullmatch(r'(com|lpt)[0-9]', stem),
             f'Unsafe ZIP path: {name!r}')


def verify(path, expected_exe=None, expected_version=None, expected_tag=None, demo=False):
    size = path.stat().st_size
    need(22 <= size <= 1024**3, 'ZIP exceeds the native updater size range')
    with zipfile.ZipFile(path) as archive:
        entries = archive.infolist()
        need(0 < len(entries) <= 20001, 'Invalid ZIP entry count')
        by_name = {}
        folded = set()
        total = 0
        for entry in entries:
            name = entry.filename
            path_ok(name)
            need(name.lower() not in folded, f'Case-colliding ZIP paths: {name}')
            need(not entry.is_dir() and not stat.S_ISLNK(entry.external_attr >> 16), f'Non-file ZIP entry: {name}')
            need(entry.flag_bits & ~0x0808 == 0 and entry.compress_type in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED),
                 f'Unsupported ZIP flags/compression: {name}')
            need(entry.file_size <= 512 * 1024**2, f'Payload exceeds native updater file limit: {name}')
            total += entry.file_size
            need(total <= 2 * 1024**3, 'Expanded ZIP exceeds the native updater size limit')
            folded.add(name.lower())
            by_name[name] = entry
        need(MANIFEST in by_name and by_name[MANIFEST].file_size <= 8 * 1024**2, 'Missing/oversized root manifest')
        manifest = json.loads(archive.read(MANIFEST).decode('utf-8-sig'), object_pairs_hook=unique_json)
        need(manifest['format'] == 2 and manifest['product'] == 'Beetleworx' and manifest['executable'] == EXE,
             'Unexpected release product/format')
        version, tag = manifest['version'], manifest['tag']
        need(isinstance(version, str) and 0 < len(version) < 128 and tag in (version, 'v' + version), 'Invalid release version/tag')
        need(not expected_version or version == expected_version, 'Release version differs from expected version')
        need(not expected_tag or tag == expected_tag, 'Release tag differs from expected tag')
        exe_sha = manifest['exe_sha256']
        need(digest(exe_sha) and (not expected_exe or exe_sha.lower() == expected_exe.lower()),
             'Executable hash differs from approved build')
        if demo:
            need(manifest.get('demo') is True and manifest.get('online_multiplayer') == 'hidden' and
                 manifest.get('expected_exe_sha256', '').lower() == exe_sha.lower(), 'Demo manifest contract mismatch')
            need({'demo-channel.txt', 'TESTING & KNOWN ISSUES.txt'} <= by_name.keys(), 'Missing demo marker/guide')
        need(not any(manifest.get(key) for key in ('includes_full_modified_game_archives', 'includes_full_original_game',
                                                   'includes_personal_data', 'includes_separate_launcher')),
             'Manifest declares an excluded payload')
        abi = manifest['shader_abi']
        need(isinstance(abi, str) and re.fullmatch(r'rex-[A-Za-z0-9_-]+', abi), 'Invalid shader ABI')
        shader = f'pc-assets/pc-shader-library/{abi}/native-profile-v1/translated-dxbc-v1/library.em2dxbcpack'
        fixed = ROOT_FILES | CRT_FILES | REQUIRED_PATCHES | {CATALOG, shader}
        required = {EXE, 'dxcompiler.dll', 'dxil.dll', 'START HERE.txt', CATALOG, shader} | REQUIRED_CRT | REQUIRED_PATCHES
        need(required <= by_name.keys(), 'Missing required runtime, patch, or shader file')
        seen = set()
        for record in manifest['files']:
            name = record['path']
            need(name in by_name and name != MANIFEST and name not in seen, f'Missing/duplicate manifest entry: {name}')
            need(name in fixed or name.startswith('licenses/') and name.lower().endswith(('.txt', '.md', '/license', '/copying')),
                 f'Unexpected release payload: {name}')
            need(type(record['bytes']) is int and record['bytes'] == by_name[name].file_size and digest(record['sha256']),
                 f'Invalid size/digest in manifest: {name}')
            with archive.open(name) as file:
                actual = hashlib.file_digest(file, 'sha256').hexdigest()
            need(actual == record['sha256'].lower(), f'Payload hash mismatch: {name}')
            if name == EXE:
                need(actual == exe_sha.lower(), 'Executable bytes disagree with manifest identity')
            seen.add(name)
        need(seen == by_name.keys() - {MANIFEST} and manifest['file_count'] == len(seen), 'ZIP/manifest inventory mismatch')
        need(manifest['payload_bytes'] == total - by_name[MANIFEST].file_size, 'Manifest payload total mismatch')
    with path.open('rb') as file:
        zip_sha = hashlib.file_digest(file, 'sha256').hexdigest().upper()
    return dict(verified=True, archive=str(path.resolve()), zip_sha256=zip_sha, zip_bytes=size,
                executable=EXE, exe_sha256=exe_sha.upper(), version=version, tag=tag,
                payload_files=len(seen), payload_bytes=manifest['payload_bytes'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('zip', type=Path)
    parser.add_argument('--expected-exe-sha256')
    parser.add_argument('--version')
    parser.add_argument('--tag')
    parser.add_argument('--demo', action='store_true')
    args = parser.parse_args()
    print(json.dumps(verify(args.zip, args.expected_exe_sha256, args.version, args.tag, args.demo), indent=2))
