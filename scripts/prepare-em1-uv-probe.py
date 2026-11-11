"""Stage a private UV-color diagnostic; never install it as a game material.

Replaces only EM1M's pixel instructions, preserving the linked vertex program
and declarations. R/G show fractional UV0, B=0 and A=1. Lighting/postprocessing
still affects the captured colors, so this is not a numeric UV readback.
"""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import struct


def prepare(source, output, checker=False):
    if output.exists() or source.resolve() == output.resolve():
        raise ValueError('Use a new private output path')
    pak = runpy.run_path(str(Path(__file__).with_name('game-pak.py')))
    # FRC r2.xy, r0; MAX export with overlapping vector/scalar masks for A=1.
    words = [0x00001002, 0x00001200, 0xC4000000,
             0x00001003, 0x00002200, 0,
             0xC8030002, 0, 0xE8000000,
             0xC8CB8000, 0, 0xE2020200]
    if checker:
        # Binary UV half-cells: floor(2*frac(UV)). Avoid dependence on tone curve.
        words = [0x00003002, 0x00001200, 0xC4000000,
                 0x00001005, 0x00002200, 0,
                 0xC8030002, 0, 0xE8000000,
                 0xC8030002, 0, 0xE0020200,
                 0xC8030002, 0, 0xEA020000,
                 0xC8CB8000, 0, 0xE2020200]
    code = struct.pack(f'>{len(words)}I', *words)
    entries, changed = [], 0
    for name, data, kind in pak['read_pak'](source):
        if name == 'cookedshaders/360/EM1M_StaticEnv_Blend.fxlo':
            if len(data) != 7756 or data[0x1504:0x1519] != b'EM1M_StaticEnv_Blend\0':
                raise ValueError('Unexpected private effect layout')
            edited = bytearray(data)
            edited[3956:4472] = code + bytes(516 - len(code))
            data = bytes(edited)
            changed += 1
        entries.append((name, data, kind))
    if changed != 1:
        raise ValueError('Expected exactly one masked effect')
    output.parent.mkdir(parents=True, exist_ok=True)
    pak['write_pak'](entries, output)
    return {'scope': 'Private UV diagnostic, not a material fix',
            'checker': checker, 'source': str(source), 'changed_effect': 'EM1M_StaticEnv_Blend',
            'pixel_words': [hex(w) for w in words],
            'sha256': hashlib.sha256(output.read_bytes()).hexdigest()}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path); p.add_argument('output', type=Path)
    p.add_argument("--checker",action="store_true")
    args = p.parse_args()
    print(json.dumps(prepare(args.source, args.output, args.checker), indent=2))
