"""Verify that the native prepared bundle contains every loose record unchanged.

This checks package completeness and byte preservation. The runtime's existing
EM2DXPK1 loader remains responsible for record checksums and shader semantics.
No files are modified, unpacked or regenerated.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct


def verify(directory):
    directory = Path(directory)
    pack = directory / 'translated-dxbc-v1/library.em2dxbcpack'
    records = sorted(directory.rglob('*.em2dxbc'))
    if not 0 < len(records) <= 16384:
        raise ValueError('Prepared record count is empty or exceeds the native limit')
    if pack.stat().st_size > 256 * 1024 * 1024 + 16384 * 128:
        raise ValueError('Shader bundle exceeds the native loader budget')
    expected = Counter()
    for record in records:
        if not 64 <= record.stat().st_size <= 8 * 1024 * 1024:
            raise ValueError(f'Invalid loose shader record size: {record.name}')
        with record.open('rb') as file:
            expected[hashlib.file_digest(file, 'sha256').digest()] += 1
    with pack.open('rb') as file:
        if file.read(8) != b'EM2DXPK1':
            raise ValueError('Prepared shader bundle header is invalid')
        header = file.read(4)
        if len(header) != 4 or struct.unpack('<I', header)[0] != len(records):
            raise ValueError('Shader bundle count differs from prepared records')
        for _ in records:
            header = file.read(4)
            if len(header) != 4:
                raise ValueError('Truncated shader bundle record size')
            size = struct.unpack('<I', header)[0]
            if not 64 <= size <= 8 * 1024 * 1024:
                raise ValueError('Shader bundle record exceeds native bounds')
            data = file.read(size)
            if len(data) != size:
                raise ValueError('Truncated shader bundle record')
            digest = hashlib.sha256(data).digest()
            if expected[digest] <= 0:
                raise ValueError('Shader bundle has changed, extra or duplicate records')
            expected[digest] -= 1
        if file.read(1) or any(expected.values()):
            raise ValueError('Shader bundle has trailing data or missing records')
    return dict(format='EM2DXPK1', records=len(records), bytes=pack.stat().st_size,
                scope='All prepared loose records preserved byte-for-byte in the native bundle')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    print(json.dumps(verify(args.directory)))
