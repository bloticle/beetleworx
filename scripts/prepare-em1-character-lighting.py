"""Generate private exact-program SH bindings from audited native Mickey effects."""
import argparse
import hashlib
import runpy
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PINNED = {
    'Gen3_Char_Mickey_LOD0': (4, 'cdfc3083349ed41251b0a2ed34f50a34422fd0947204ba4c9a3a75a747830f87'),
    'Gen3_Char_Mickey_LODP1': (5, '14ad13d771aec3c3e2adb9e243a40b1d80f64b5084014c70c9569d143ee8b6a6'),
    'Gen3_Char_Mickey_LP1S1': (10, 'a48fe1c69aa799d5855e35471b0209fffe6e1191ec030d5d1ba7313d5bf1a40f'),
    'Gen3_Char_Mickey': (16, '7b01975e66f277b559790ad41e5ea3e80493fa155099675e664c43347c9007f0'),
}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-pak', type=Path, required=True)
    args = parser.parse_args()
    entries = runpy.run_path(str(ROOT/'scripts/game-pak.py'))['read_pak'](args.source_pak)
    extract = runpy.run_path(str(ROOT/'scripts/inspect-scene-shaders.py'))['extract']
    lines = ['#pragma once', '#include <array>', '#include <span>', '#include <cstdint>',
             'namespace em2::em1::character_lighting_data {']
    for name, (start, digest) in PINNED.items():
        blobs = [data for path, data, kind in entries if path == f'cookedshaders/360/{name}.fxlo']
        if len(blobs) != 1 or hashlib.sha256(blobs[0]).hexdigest() != digest:
            raise ValueError(f'Unrecognized effect: {name}')
        # Each effect also contains a nine-word depth prepass without SH inputs.
        programs = [p for p in extract(blobs[0]) if any(c['name']=='SHCoeff0' for c in p['constants'])]
        if len(programs) != 1:
            raise ValueError('Unexpected material permutations')
        p = programs[0]
        bindings = {c['name']: (c['register_set'], c['index'], c['count']) for c in p['constants']}
        if any(bindings.get(f'SHCoeff{i}') != (2, start+i, 1) for i in range(9)):
            raise ValueError('SH constant layout changed')
        if bindings.get('WorldInverseTranspose') != (2, start+13, 3):
            raise ValueError('World-normal transform changed')
        code = p['words']
        lines.append(f'inline constexpr std::array<uint32_t,{len(code)}> {name}={{'+','.join(f'0x{x:08x}u' for x in code)+'};')
    lines.append('struct Binding {std::span<const uint32_t> code;unsigned first;};')
    lines.append('inline constexpr std::array<Binding,4> bindings={{'+','.join('{'+name+','+str(start)+'}' for name,(start,_) in PINNED.items())+'}};')
    lines.append('}')
    output = ROOT/'private/em1_character_lighting_data.h'
    text = '\n'.join(lines)+'\n'
    if not output.exists() or output.read_text() != text:
        output.write_text(text)
    print('Verified and generated all four Mickey SH layouts; source archive unchanged.')

if __name__ == '__main__':
    main()
