"""Build the private native effect for the authored EM1 Mickey ink ribbons.

Original archives are read-only. This stages a candidate, never activates it.
The native loader, ribbon input binding and GPU output must still be tested by
the canonical game executable. Position/normal/UV arithmetic comes from the
pinned native effect; its COLOR input is replaced by the verified empty-curve
default alpha of the three original Mickey ribbon assets.
"""
import argparse
import hashlib
import json
from pathlib import Path
import random
import runpy
import struct

ROOT = Path(__file__).resolve().parents[1]
DONOR = 'cookedshaders/360/Gen3_FX_Env_Mask_VertAlpha.fxlo'
TARGET = 'cookedshaders/360/EM1I_FX_Env_Mask_VertAlpha.fxlo'
DONOR_SHA256 = 'dfd6ed096816198fac3baffb22a95bdad820cf96e36a26c4a06108f60dbeedb3'
PROGRAM_SHA256 = (
    '1632edb06deee19149e7ea3ea890ef230b73f6a5ee4a788022687eb1d35339c8',
    '9b23baf7d7bd33deb5b3558684ed49e9e971040a86a4ce0d94a2bf6d070c7b07',
    '6bb9dac606414a534d04d7cdc2534e4f727b0eb7f22401c6a0687b0b50cdc6bd',
    '87bbbb8d4b85f4c69ff8589b1ec61a86c9ae92de9de45eb2e6154d097c9108d9',
)


def word(data, offset):
    return struct.unpack_from('>I', data, offset)[0]


def alu(dest, mask, opcode, a, b, a_swizzle=0, b_swizzle=0,
        b_constant=False, export=False, clamp=False):
    # Xenos scalar RETAIN_PREV, no scalar writes, predication or addressing.
    return (0xC8000000 | (mask << 16) | dest |
            (int(export) << 15) | (int(clamp) << 24),
            (a_swizzle << 16) | (b_swizzle << 8),
            0xA0000000 | (0 if b_constant else 0x40000000) |
            (opcode << 24) | (a << 16) | (b << 8))


def ink_pixel_program(original):
    """Keep native sphere mapping, replace only mask fetch W and final combine.

    Input r1 contains the view-space normal XYZ and authored vertex alpha W.
    The existing native prefix constructs the reflection coordinates in r1.xy,
    retaining W. Fetch9 samples footprint UV0 into r3; fetch10 samples the
    environment into r0. c0 is native Attribute EnvMapHighlightColour, supplied
    by a clone of the original Color_Register_1 NiColorExtraData on the ribbon.
    """
    digest = hashlib.sha256(original).hexdigest()
    if digest not in PROGRAM_SHA256:
        raise ValueError('Unrecognized native reflection pixel program')
    depth = PROGRAM_SHA256.index(digest) >= 2
    words = list(struct.unpack('>'+str(len(original)//4)+'I', original))
    fetch, begin, end, footprint, tint = (26,28,35,5,6) if depth else (9,11,17,3,0)
    expected_fetch = [0x10285001,0x1F1FFE88,0x4000] if depth else [0x10183001,0x1F1FFE88,0x4000]
    if words[fetch*3:fetch*3+3] != expected_fetch:
        raise ValueError('Native footprint fetch moved')
    # The original mask fetch forced W=1; source footprint opacity requires W.
    words[fetch*3+1] = 0x1F1FF688
    if depth:
        # The original soft-particle combine packs env RGBA as R,A,G,B in r0.
        # Our source equation uses ordinary RGBA, including its original RGB.
        if words[27*3:27*3+3] != [0x10180021,0x1F1FF458,0x4000]:
            raise ValueError('Native depth environment fetch moved')
        words[27*3+1] = 0x1F1FF688
    # Execute the same final slots and keep the original export position,
    # enclosing byte count, instruction clauses and native linker metadata.
    # Native depth variants use the same parameter1 normal/alpha, sampler1 env,
    # sampler2 footprint and c6 tint. Retain their prefix/inputs/constant table,
    # but do not apply depth fading absent from the original Wii material.
    tail = [alu(4,8,1,footprint,1,0x1B,0x1B),
            alu(4,7,1,0,tint,b_constant=True)]
    tail += [alu(4,0,2,4,4)]*(end-begin-3)
    tail += [alu(0,15,2,4,4,export=True,clamp=True)]
    words[begin*3:end*3] = [w for instruction in tail for w in instruction]
    return struct.pack('>'+str(len(words))+'I', *words)


def annotation(data, index, name, kind, expected):
    """Validate the actual native annotation descriptor, name and value span."""
    descriptors, values, names = (12 + word(data, at) for at in (0x26C, 0x278, 0x2AC))
    if word(data, 0x270) != 96:
        raise ValueError('Native annotation count changed')
    string_at = 12 + word(data, names + 4*index)
    if data[string_at:data.index(0, string_at)].decode() != name:
        raise ValueError('Native annotation name changed')
    typ, packed = struct.unpack_from('>2I', data, descriptors + 8*index)
    if typ != kind or packed >> 16 != 1:
        raise ValueError('Native annotation is not the audited scalar')
    at = values + 4*(packed & 65535)
    if word(data, at) != expected:
        raise ValueError('Native annotation value changed')
    return at


def color_free_vertex(donor, program):
    """Remove the unused source COLOR declaration, retaining authored alpha1.

    This is restricted to the three restored ribbons: their original simulator
    general steps are28 zero bytes, containing no color keys. The source empty
    color curve defaults to RGBA1, and R3MT material/ambient alpha are255. Their
    runtime ribbon streams omit COLOR entirely; transparency comes from the
    footprint texture. Do not use this effect for ribbons with authored curves.
    """
    base = program['container_offset']
    header = struct.unpack_from('>9I', donor, base)
    end = program['code_offset']+program['code_bytes']
    blob = bytearray(donor[base:end])
    native = header[6]
    count = word(blob, native+28)
    if count not in (4,7):
        raise ValueError('Unexpected environment vertex declaration count')
    entries = [(native+40+4*i,word(blob,native+40+4*i)) for i in range(count)]
    colors = [(at,value) for at,value in entries if (value>>12)&15==10]
    if len(colors)!=1 or (colors[0][1]>>16)&15:
        raise ValueError('Expected exactly one native COLOR0 declaration')
    declaration, descriptor = colors[0]
    fetch_index = descriptor&4095
    words = list(program['words'])
    fetch = words[fetch_index*3:fetch_index*3+3]
    if fetch[0]&31:
        raise ValueError('Native COLOR declaration does not identify a vertex fetch')
    dest = (fetch[0]>>12)&63
    lanes = [(fetch[1]>>(3*i))&7 for i in range(4)]
    if any(lane not in (3,7) for lane in lanes) or lanes.count(3)!=1:
        raise ValueError('Native COLOR input reads more than the source default alpha')
    mask = sum(1<<i for i,lane in enumerate(lanes) if lane==3)
    # A finite vertex index compared with itself yields the proven source
    # default alpha1 without introducing a new constant-buffer binding. Keep
    # the original temporary/lane expected by all subsequent native arithmetic.
    words[fetch_index*3:fetch_index*3+3] = alu(dest,mask,6,0,0,0x6C,0x6C)
    first_cf = words[0] | ((words[1]&65535)<<32)
    first, number = first_cf&4095, (first_cf>>12)&7
    if first_cf>>44 !=1 or not first<=fetch_index<first+number:
        raise ValueError('Native alpha fetch is not in the audited first clause')
    flag = 1<<(16+2*(fetch_index-first))
    if not first_cf&flag:
        raise ValueError('Native alpha operation is not marked as a fetch')
    first_cf &= ~flag
    words[0] = first_cf&0xFFFFFFFF
    words[1] = (words[1]&0xFFFF0000)|(first_cf>>32)
    code = program['code_offset']-base
    blob[code:code+program['code_bytes']] = struct.pack('>'+str(len(words))+'I',*words)
    # Removing one input declaration moves only the later local tables,
    # literals and bytecode. Keep the outer FXLO opaque-record extent fixed.
    struct.pack_into('>I',blob,native+28,count-1)
    struct.pack_into('>I',blob,4,header[1]-4)
    del blob[declaration:declaration+4]
    blob.extend(bytes(4))
    return bytes(blob), {'container_offset':base,'color_fetch':fetch_index,
        'alpha_temporary':dest,'alpha_mask':mask,'inputs_before':count,
        'inputs_after':count-1,'source_default_alpha':1,
        'source_alpha_scope':'three original zero-key Mickey ink ribbon simulator steps'}


def clone_ink_effect(donor):
    if hashlib.sha256(donor).hexdigest() != DONOR_SHA256:
        raise ValueError('Unrecognized original native ink effect donor')
    extract = runpy.run_path(str(ROOT/'scripts/inspect-scene-shaders.py'))['extract']
    before_vs, before_ps = extract(donor, False), extract(donor)
    if len(before_vs) != 4 or len(before_ps) != 4:
        raise ValueError('Native effect permutation inventory changed')
    output = bytearray(donor)
    allowed, programs, aliases, vertices = [], [], [], []
    for suffix in ('', '_SA', '_depthBlend', '_depthBlend_SA'):
        before = ('Gen3_FX_Env_Mask_VertAlpha'+suffix+'\0').encode()
        after = ('EM1I_FX_Env_Mask_VertAlpha'+suffix+'\0').encode()
        if len(before) != len(after) or donor.count(before) != 1:
            raise ValueError('Native technique inventory changed')
        at = donor.index(before)
        output[at:at+len(before)] = after
        allowed.append((at, at+len(before)))
        aliases.append(after[:-1].decode())
    for program in before_vs:
        at = program['container_offset']
        binary, description = color_free_vertex(donor,program)
        output[at:at+len(binary)] = binary
        allowed.append((at,at+len(binary)))
        vertices.append(description)
    for program in before_ps:
        at, size = program['code_offset'], program['code_bytes']
        binary = ink_pixel_program(donor[at:at+size])
        output[at:at+size] = binary
        allowed.append((at, at+size))
        programs.append({'offset': at, 'bytes': size,
                         'source_sha256': program['code_sha256'],
                         'sha256': hashlib.sha256(binary).hexdigest()})
    # Source texture-map order: footprint0, environment1. Apply the same mapping
    # to the transform annotations; no NIF texture descriptor is reordered.
    remaps = []
    for index, old, new, meaning in (
        (17, 0, 1, 'envSampler'), (21, 1, 0, 'mask1Sampler'),
        (25, 2, 0, 'unused mask2Sampler'),
        (38, 0, 1, 'env transform'), (42, 1, 0, 'mask1Transform'),
        (46, 2, 0, 'unused mask2Transform'),
    ):
        at = annotation(donor, index, 'NTMIndex', 4, old)
        struct.pack_into('>I', output, at, new)
        allowed.append((at, at+4))
        remaps.append({'binding': meaning, 'offset': at, 'from': old, 'to': new})
    # Existing source-over, LEQUAL and depth-write-off state is already correct.
    # Disable the donor's extra alpha test: the Wii template has continuous alpha.
    expected = (13, 0x28,1, 0x2C,6, 0x30,0, 0x38,2, 0x3C,1,
                0x48,6, 0x4C,7, 0x60,1, 0x64,0, 0x68,4, 0xCC,0, 0xD0,0, 0x180,1)
    if struct.unpack_from('>27I', donor, 0x37E4) != expected:
        raise ValueError('Native source-over/depth state changed')
    struct.pack_into('>I', output, 0x3824, 0)
    allowed.append((0x3824, 0x3828))
    if len(output) != len(donor):
        raise ValueError('Ink clone changed enclosing extents')
    after_vs=extract(output,False)
    for old,new in zip(before_vs,after_vs):
        if old['constants']!=new['constants'] or old['code_bytes']!=new['code_bytes']:
            raise ValueError('Ink clone changed native vertex constants or instruction extent')
    after_ps = extract(output)
    for a, b in zip(before_ps, after_ps):
        if a['constants'] != b['constants'] or a['code_bytes'] != b['code_bytes']:
            raise ValueError('Ink clone changed native pixel binding interface')
    if any(a != b and not any(lo <= i < hi for lo, hi in allowed)
           for i, (a,b) in enumerate(zip(donor, output))):
        raise ValueError('Ink clone modified unaudited bytes')
    report = {'donor': DONOR, 'donor_sha256': DONOR_SHA256, 'target': TARGET,
              'sha256': hashlib.sha256(output).hexdigest(), 'aliases': aliases,
              'programs': programs, 'vertices':vertices,'remaps': remaps,
              'tint_attribute': 'EnvMapHighlightColour',
              'source_tint_attribute': 'Color_Register_1',
              'equation': 'RGB=environment.rgb*tint.rgb; A=footprint.a*vertex.a',
              'native_load_proven': False, 'gpu_output_proven': False,
              'reflection_coordinate_arithmetic_unchanged': True,
              'source_empty_curve_default_alpha':1,'color_stream_required':False,
              'depth_blend_supported': True,
              'depth_fade_applied': False}
    return bytes(output), report


def reference(environment, footprint, vertex, tint):
    return [max(0., min(1., environment[i]*tint[i])) for i in range(3)] + [
        max(0., min(1., footprint[3]*vertex[3]))]


def test_tail(program):
    """Decode real emitted ALUs independently, comparing to the source equation.

    This checks arithmetic/exports, not GPU texture sampling, native sphere-map
    equivalence or native material binding. Those remain explicit runtime gates.
    """
    depth = len(program)==432
    if len(program) not in (216,432):
        raise ValueError('Unexpected ink pixel program extent')
    words = struct.unpack('>'+str(len(program)//4)+'I', program)
    begin,end,footprint,tint_register = (28,35,5,6) if depth else (11,17,3,0)
    rng = random.Random(0x1A7D11)
    cases = 0
    for case in range(4096):
        values = [[rng.random() for _ in range(4)] for _ in range(4)]
        if case < 32:
            values[1][3] = (0., 1., 127/255, 128/255)[case % 4]
            values[2][3] = (0., 1.)[(case//4) % 2]
        env, mask, vertex, tint = values
        registers = [[0.]*4 for _ in range(8)]
        registers[0], registers[1], registers[footprint] = env[:], vertex[:], mask[:]
        result = [float('nan')]*4
        for index in range(begin,end):
            a,b,c = words[index*3:index*3+3]
            def source(reg, swizzle, temporary):
                if not temporary and reg != tint_register:
                    raise ValueError('Ink program uses an unexpected tint register')
                value = registers[reg] if temporary else tint
                return [value[((swizzle >> (2*i))+i)&3] for i in range(4)]
            x = source((c >> 16)&255, (b >> 16)&255, bool(c & (1<<31)))
            y = source((c >> 8)&255, (b >> 8)&255, bool(c & (1<<30)))
            op = (c >> 24)&31
            if op not in (1,2):
                raise ValueError('Unexpected emitted ink arithmetic')
            value = [x[i]*y[i] if op == 1 else max(x[i],y[i]) for i in range(4)]
            if a & (1<<24):
                value = [max(0., min(1.,v)) for v in value]
            destination = result if a & (1<<15) else registers[a & 63]
            for i in range(4):
                if a & (1<<(16+i)):
                    destination[i] = value[i]
        if result != reference(env, mask, vertex, tint):
            raise ValueError('Ink ALU output differs from the original template equation')
        cases += 1
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-pak', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--output-pak', type=Path)
    parser.add_argument('--replace-output', action='store_true')
    args = parser.parse_args()
    pak = runpy.run_path(str(ROOT/'scripts/game-pak.py'))
    source_hash = hashlib.sha256(args.source_pak.read_bytes()).hexdigest()
    entries = pak['read_pak'](args.source_pak)
    donor, kind = next((data, kind) for name,data,kind in entries if name == DONOR)
    effect, report = clone_ink_effect(donor)
    report['cpu_cases'] = sum(test_tail(effect[p['offset']:p['offset']+p['bytes']])
                              for p in report['programs'])
    report['source_pak_sha256'] = source_hash
    args.output_dir.mkdir(parents=True, exist_ok=True)
    def save(path, data):
        if path.resolve() == args.source_pak.resolve():
            raise ValueError('Cannot replace the source archive')
        if path.exists():
            if not args.replace_output:
                raise ValueError('Output already exists: '+str(path))
            # A staged archive can be a hardlink to original game data. Removing
            # the directory entry first guarantees a fresh output file inode.
            path.unlink()
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    save(args.output_dir/Path(TARGET).name, effect)
    for i,p in enumerate(report['programs']):
        save(args.output_dir/('ink-'+str(i)+'.ucode.bin'), effect[p['offset']:p['offset']+p['bytes']])
    if args.output_pak:
        if args.output_pak.resolve() == args.source_pak.resolve():
            raise ValueError('Cannot replace the source archive')
        if args.output_pak.exists():
            if not args.replace_output:
                raise ValueError('Output archive already exists')
            args.output_pak.unlink()
        entries = [(n,d,k) for n,d,k in entries if n != TARGET]
        pak['write_pak'](entries+[(TARGET,effect,kind)], args.output_pak)
        report['output_pak'] = str(args.output_pak)
    if hashlib.sha256(args.source_pak.read_bytes()).hexdigest() != source_hash:
        raise ValueError('Source archive changed during staging')
    save(args.output_dir/'ink-effect-manifest.json', (json.dumps(report,indent=2)+'\n').encode())
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
