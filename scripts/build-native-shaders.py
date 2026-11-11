"""Compile the PC renderer's finite HLSL variants during the build, not play.

Uses the Windows SDK compiler DLL in-process; never builds or launches a helper
game executable. All inputs and compiler options are retained for differential
verification by the canonical executable's headless diagnostic.
"""
import argparse
import ctypes as c
import hashlib
import json
import re
from pathlib import Path

OPTIMIZE = 1 << 15
STRICT = OPTIMIZE | (1 << 13)


def programs(root):
    def expanded_hlsl(path, seen=None):
        # The direct-resolve overlay keeps upstream headers verbatim. Inline
        # their guarded local includes for D3DCompile's source-only interface
        # and for the executable's immutable diagnostic source record.
        seen = set() if seen is None else seen
        path = path.resolve()
        if not path.is_relative_to(root.resolve()):
            raise ValueError(f'Shader include escapes runtime: {path}')
        if path in seen:
            return '// Previously included guarded shader header.\n'
        seen.add(path)
        source = path.read_text(encoding='utf-8')
        return re.sub(r'^\s*#include\s+"([^"]+)"\s*$',
                      lambda m: expanded_hlsl(path.parent / m[1], seen),
                      source, flags=re.MULTILINE)

    def hlsl(name):
        path = root / (name + '.hlsl')
        return expanded_hlsl(path) if name == 'direct_color_resolve' else path.read_text(encoding='utf-8')

    result = []

    def add(id, source, name, entry='main', profile='cs_5_1', flags=STRICT, macro=None):
        result.append(dict(id=id, source=source, name=name, entry=entry,
                           profile=profile, flags=flags, macro=macro))

    auxiliary = hlsl('auxiliary_depth_resample')
    for suffix, entry, profile in [('VS', 'vs', 'vs_5_1'), ('Depth', 'depth_ps', 'ps_5_1'), ('Stencil', 'stencil_ps', 'ps_5_1')]:
        for ms in [False, True]:
            add('Auxiliary' + ('MS' if ms else '') + suffix, auxiliary,
                'auxiliary-depth-resample', entry, profile, OPTIMIZE,
                ('MULTISAMPLED', str(int(ms))))
    add('HdrGuestView', hlsl('hdr_guest_view'), 'hdr-guest-view')
    add('HdrOutput', hlsl('hdr_output'), 'em2-hdr-output')
    ui = hlsl('hdr_presenter_ui')
    add('HdrUIVS', ui, 'hdr-host-ui', 'vs', 'vs_5_1', OPTIMIZE)
    add('HdrUIPS', ui, 'hdr-host-ui', 'ps', 'ps_5_1', OPTIMIZE)
    add('HdrSDRPaint', hlsl('hdr_presenter_sdr_paint'), 'hdr-sdr-paint', profile='ps_5_1', flags=OPTIMIZE)
    for ms in [False, True]:
        add('Hdr' + ('MS' if ms else '') + 'Resolve', hlsl('hdr_scene_resolve'),
            'em2-hdr-resolve', macro=('MSAA', str(int(ms))))
    add('ScaledResolve', hlsl('scaled_resolve_migration'), 'scaled-resolve-migration', flags=OPTIMIZE)
    add('SceneAmbient', hlsl('scene_ambient'), 'scene-ambient')
    add('SceneGeometry', hlsl('scene_unproject') + hlsl('scene_geometry'), 'scene-geometry')
    add('SceneIndirect', hlsl('scene_indirect'), 'scene-indirect')
    add('SceneIndirectLoopEmitters', hlsl('scene_indirect'), 'scene-indirect', macro=('EM2_GI_LOOP_LAYOUT', '1'))
    add('SceneIndirectLoopAll', hlsl('scene_indirect'), 'scene-indirect', macro=('EM2_GI_LOOP_LAYOUT', '2'))
    add('SceneOcclusion', hlsl('scene_occlusion'), 'scene-occlusion')
    add('SceneRasterGeometry', hlsl('scene_unproject') + hlsl('scene_raster_geometry'), 'scene-raster-geometry', profile='ps_5_1')
    add('DirectColorResolve', hlsl('direct_color_resolve'), 'direct-color-resolve')
    return result


class Macro(c.Structure):
    _fields_ = [('name', c.c_char_p), ('value', c.c_char_p)]


def blob_bytes(blob):
    if not blob.value:
        return b''
    table = c.cast(blob, c.POINTER(c.POINTER(c.c_void_p))).contents
    pointer = c.WINFUNCTYPE(c.c_void_p, c.c_void_p)(table[3])(blob)
    size = c.WINFUNCTYPE(c.c_size_t, c.c_void_p)(table[4])(blob)
    try:
        return c.string_at(pointer, size)
    finally:
        c.WINFUNCTYPE(c.c_ulong, c.c_void_p)(table[2])(blob)


def build(root, output):
    dll = c.WinDLL('d3dcompiler_47.dll')
    compile = dll.D3DCompile
    compile.argtypes = [c.c_void_p, c.c_size_t, c.c_char_p, c.POINTER(Macro), c.c_void_p,
                        c.c_char_p, c.c_char_p, c.c_uint, c.c_uint, c.POINTER(c.c_void_p), c.POINTER(c.c_void_p)]
    compile.restype = c.c_long
    entries, records, report = [], [], []
    for i, p in enumerate(programs(root)):
        source = p['source'].encode('utf-8')
        macros = None if not p['macro'] else (Macro * 2)(Macro(*(v.encode() for v in p['macro'])), Macro())
        bytecode, errors = c.c_void_p(), c.c_void_p()
        hr = compile(source, len(source), p['name'].encode(), macros, None,
                     p['entry'].encode(), p['profile'].encode(), p['flags'], 0,
                     c.byref(bytecode), c.byref(errors))
        diagnostic, data = blob_bytes(errors), blob_bytes(bytecode)
        if hr < 0 or not data.startswith(b'DXBC') or int.from_bytes(data[24:28], 'little') != len(data):
            raise RuntimeError(f"{p['id']}: {hr:#x}: {diagnostic.decode(errors='replace')}")
        # Unique delimiter makes source text exact, including leading/trailing LF.
        delimiter = 'EM2_' + hashlib.sha256(source).hexdigest()[:10]
        assert ')' + delimiter + '"' not in p['source']
        entries.append(f'alignas(8) constexpr uint8_t code_{i}[]={{' + ','.join(map(str, data)) + '};\n'
                       f'constexpr char source_{i}[]=R"{delimiter}({p["source"]}){delimiter}";\n')
        strings = [p['name'], p['entry'], p['profile']] + list(p['macro'] or (None, None))
        strings = ','.join('nullptr' if v is None else json.dumps(v) for v in strings)
        records.append(f'{{em2::BuiltShader::{p["id"]},{{code_{i},sizeof(code_{i})}},source_{i},{strings},{p["flags"]}}}')
        report.append({k: v for k, v in p.items() if k != 'source'} | {
            'source_sha256': hashlib.sha256(source).hexdigest(),
            'dxbc_sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data)})
    content = '// Generated by build-native-shaders.py. Do not edit.\nnamespace {\n' + ''.join(entries)
    content += 'constexpr em2::BuiltShaderRecord kBuiltShaderRecords[]={\n' + ',\n'.join(records) + '\n};\n'
    content += 'static_assert(std::size(kBuiltShaderRecords)==static_cast<unsigned>(em2::BuiltShader::Count));\n}\n'
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix('.tmp')
    temporary.write_text(content, encoding='utf-8', newline='\n')
    temporary.replace(output)
    output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'Compiled {len(report)} native renderer shaders: {sum(p["bytes"] for p in report)} bytes')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    build(args.root, args.output)
