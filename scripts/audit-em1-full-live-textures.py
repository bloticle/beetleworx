"""Compare complete live untiled base levels with portable NiPixelData fixtures.

The fixtures are independently decoded Wii textures; this checks the native
upload/swizzle path including captured mip levels, not filtering or final pixels.
"""
import argparse
import json
from pathlib import Path
import re
import struct
from em1_nif import NIF


def audit(root, log):
    expected = {}
    expected_mips = {}
    for path in root.rglob('*tex.nif_wii'):
        nif = NIF.read(path.read_bytes())
        for index, kind in enumerate(nif.indices):
            if nif.types[kind] != 'NiPixelData':
                continue
            block = nif.blocks[index]
            word = lambda offset: struct.unpack_from(nif.endian + 'I', block, offset)[0]
            if word(0) != 1 or block[4] != 32:
                raise ValueError('Unaudited portable format')
            mips = word(63)
            for mip in range(mips):
                width, height, offset = (word(i + mip * 12) for i in (71, 75, 79))
                start = 71 + mips * 12 + 8 + offset
                rgba = block[start:start + width * height * 4]
                if len(rgba) != width * height * 4:
                    raise ValueError('Truncated portable texture')
                digest = 14695981039346656037
                for byte in rgba:
                    digest = ((digest ^ byte) * 1099511628211) & ((1 << 64) - 1)
                key = (width, height, f'{digest:016X}')
                expected_mips[(str(path), mip)] = key
                if mip == 0:
                    expected.setdefault(key, []).append(str(path))
    observations = []
    for base, slot, width, height, digest, u, v in re.findall(
            r'full texture base=(\w+) slot=(\d+) width=(\d+) height=(\d+) rgbaFNV=(\w+) repeat=(\d+)/(\d+)',
            log.read_text()):
        observations.append({'base': base, 'slot': int(slot), 'width': int(width),
                             'height': int(height), 'rgba_fnv': digest,
                             'wrap': [int(u), int(v)],
                             'matches': expected.get((int(width), int(height), digest), [])})
    if not observations:
        raise ValueError('No complete live texture observations')
    by_base = {o['base']: o['matches'] for o in observations}
    mip_observations = []
    for base, level, width, height, digest in re.findall(
            r'full texture mip base=(\w+) level=(\d+) width=(\d+) height=(\d+) rgbaFNV=(\w+)',
            log.read_text()):
        key = (int(width), int(height), digest)
        references = [path for path in by_base.get(base, []) if (path, int(level)) in expected_mips]
        mip_observations.append({'base': base, 'level': int(level), 'rgba_fnv': digest,
                                 'source_mip_available': bool(references),
                                 'matches': [path for path in references
                                             if expected_mips[(path, int(level))] == key]})
    missing_levels = []
    captured = {(m['base'], m['level']) for m in mip_observations}
    for o in observations:
        for path in o['matches']:
            for file, level in expected_mips:
                if file == path and level and (o['base'], level) not in captured:
                    missing_levels.append({'base': o['base'], 'level': level})
    return {'log': str(log), 'portable_reference_root': str(root),
            'observations': observations,
            'unmatched': sum(not x['matches'] for x in observations),
            'mip_observations': mip_observations,
            'mip_unmatched': sum(x['source_mip_available'] and not x['matches'] for x in mip_observations),
            'mips_without_source_reference': sum(not x['source_mip_available'] for x in mip_observations),
            'missing_mip_levels': missing_levels,
            'scope': 'Complete RGBA hashes after native untile/swizzle. Mip completeness reported separately; filtering and framebuffer unverified.'}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('root', type=Path); p.add_argument('log', type=Path)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args(); result = audit(a.root, a.log)
    a.output.write_text(json.dumps(result, indent=2) + '\n')
    print(f"{len(result['observations'])} full native texture observations; {result['unmatched']} unmatched")
    print(f"{len(result['mip_observations'])} mip observations; {result['mip_unmatched']} unmatched; {len(result['missing_mip_levels'])} missing; {result['mips_without_source_reference']} without source reference")
    raise SystemExit(bool(result['unmatched'] or result['mip_unmatched'] or result['missing_mip_levels']))
