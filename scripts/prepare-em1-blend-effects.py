"""Prepare private EM1 effect candidates for the original native shader loader.

Does not install or activate them. Original source archive stays untouched.
Pixel programs must come from a successful canonical-executable GPU diagnostic.
The full native loader / mesh-binding check is still required after staging.
"""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import struct

ROOT = Path(__file__).resolve().parents[1]

def clone_template_state_effect(defaults, variant):
    """Preserve the actual fallback programs, replace their 2D render state.

    Gen3_DefaultShaderTextured is the fallback used by the imported Wii
    techniques. Its explicit depth-off/blended/two-sided pass overrides Ni
    properties. Keep all shader/constant/sampler interfaces byte-for-byte;
    give audited opaque, alpha and read-only opaque surfaces distinct states.
    """
    if hashlib.sha256(defaults).hexdigest() != '3b750cebc33be1a6d44c34827beeb9a736f2c3a0fdada5c907fdc67f8a8bf7d6':
        raise ValueError('Unrecognized native fallback effect')
    if variant not in ('O','T','R'):
        raise ValueError('Unknown template-state variant')
    result=bytearray(defaults)
    before=(8,0x28,0,0x30,0,0x38,0,0x3c,1,0x48,6,0x4c,7,0x60,0,0x6c,0)
    if struct.unpack_from('>17I',defaults,0x1364)!=before:
        raise ValueError('Native fallback scalar state table changed')
    # Native scalar methods: depth enable +28, comparison +2c (supplied by
    # the explicit NiZBuffer property), write +30, cull +38, blend +3c.
    for offset,value in [(0x136c,1),(0x1374,int(variant=='O')),
                         (0x137c,2),(0x1384,int(variant=='T'))]:
        struct.pack_into('>I',result,offset,value)
    # Alpha testing stays disabled (+60), so its zero reference record is
    # unnecessary. Use that slot for explicit reverse-depth LEQUAL (+2c=6),
    # avoiding comparison state inherited from a preceding effects draw.
    struct.pack_into('>2I',result,0x13a0,0x2c,6)
    # All four technique names are private, preventing collisions with the
    # original UI defaults. Lengths and every enclosing offset stay unchanged.
    for suffix in ('BaseTextured','ShaderTextured','NonTextured','VertColor'):
        before=('Gen3_Default'+suffix+'\0').encode()
        after=('EM1'+variant+'_Default'+suffix+'\0').encode()
        if len(before)!=len(after) or defaults.count(before)!=1:
            raise ValueError('Native fallback technique inventory changed')
        at=defaults.index(before);result[at:at+len(before)]=after
    extract=runpy.run_path(str(ROOT/'scripts/inspect-scene-shaders.py'))['extract']
    for pixel in (False,True):
        a,b=extract(defaults,pixel),extract(result,pixel)
        if a!=b:
            raise ValueError('Template-state clone changed a native shader or interface')
    return 'EM1'+variant+'_DefaultShaderTextured',bytes(result)

def masked_property_cutout_program(baseline):
    """Independent exact edit oracle: old masked program plus final-alpha kill.

    Keep every old fetch/ALU and its order; lengthen only the final exec clause
    and insert one KILL_GE immediately before export. This is checked against
    the actual canonical-executable GPU output before archive generation.
    """
    if hashlib.sha256(baseline).hexdigest()!='30074701ffe42fd2dd33a2a1d423738455b5201e62115a9ecd9438850a1a5e8b':
        raise ValueError('Original default masked program changed')
    words=list(struct.unpack('>45I',baseline))
    if words[4]!=0x400B1200 or words[-3:]!=[0xC80F8000,0,0xE2060600]:
        raise ValueError('Original masked execution/export changed')
    words[4]=0x500B1200
    words[-3:-3]=[0xC80F0007,0x006C1B00,0x7AFC0600]
    return struct.pack('>48I',*words)

def clone_simple_alpha_effect(simple):
    """One-map Simple RGB unchanged, with texture * authored vertex alpha.

    This is not the two-map Blend interface. Preserve the donor's sampler0,
    MinimumColourLevel, material state records and all enclosing extents.
    Native alpha-test/depth binding still needs its own runtime validation.
    """
    if hashlib.sha256(simple).hexdigest() != '8241c2af7050e3245fbe8a33a3aaa2de1e08a9938847b72f56171fb2a7cd354a':
        raise ValueError('Unrecognized native Simple effect')
    result=bytearray(preserve_vertex_alpha(simple))
    changes={
        0x82C:(0x00001442,0x00001842), # PS consumes RGBA color, not RGB.
        0x83C:(0x00007151,0x0000F151),
        2112+2*12+4:(0x1F1FFE88,0x1F1FF688), # Sample texture W, not constant1.
        2112+4*12:(0xC88FC000,0xC80FC000), # Export multiplied W, not constant1.
        2112+4*12+4:(0x00C0C000,0), # RGB unchanged; W must select W, not Z.
    }
    for offset,(before,after) in changes.items():
        if struct.unpack_from('>I',simple,offset)[0]!=before:
            raise ValueError('Unexpected Simple pixel alpha layout')
        struct.pack_into('>I',result,offset,after)
    before=b'Gen3_StaticEnv_Simple\0';after=b'EM1S_StaticEnv_Simple\0'
    if len(before)!=len(after) or simple.count(before)!=1:
        raise ValueError('Unexpected native Simple technique name')
    at=simple.index(before);result[at:at+len(before)]=after
    return 'EM1S_StaticEnv_Simple',bytes(result)

def preserve_vertex_alpha(simple):
    """Keep native position/UV math, but export the authored color alpha too.

    The donor never fetches or writes the authored W component. Accumulate
    doubled RGB in the temporary register first, preserving its alpha, then
    export RGBA together. The final previously unreachable checksum-sized
    slot fits the extra instruction without changing container extents.
    """
    # Preserve the donor's color-fetch scheduling metadata (0x0030A005).
    # Copying 0x0020A005 from a different shader lets the native linker move
    # the r0 color write ahead of UV fetches that still need r0.x as the index.
    result=bytearray(simple)
    changes={
        1160+332+20:(0x00001442,0x00001842), # Six exported components, two interpolators.
        1160+0x184:(0x00017151,0x0001F151), # Output component mask: RGBA.
        1160+0x18C:(0x0000100B,0x0000100C), # Color export fixup now instruction 12.
        1560+24:(0x0000200A,0x0000300A), # Execute all three final ALUs.
        1560+5*12+4:(0x00000E88,0x00000688), # Fetch alpha instead of masking the W lane.
        1560+11*12:(0xC8078001,0xC8070000), # Double RGB in r0, preserve r0.w.
        1560+12*12:(0x4E4A0001,0xC80F8001), # Export all of r0 to parameter 1.
        1560+12*12+4:(0x1A863251,0),
        1560+12*12+8:(0xDE60F04D,0xE2000000),
    }
    for offset,(before,after) in changes.items():
        if struct.unpack_from('>I',simple,offset)[0]!=before:
            raise ValueError('Unexpected Simple vertex alpha/export layout')
        struct.pack_into('>I',result,offset,after)
    return bytes(result)


def build_dual_uv_vertex_container(simple):
    """Four source streams, packed UV0/UV1 and RGBA; no tangent dependency.

    This is a private candidate until native linking and GPU execution have
    verified the added TEXCOORD1 fetch. Preserve the donor's WVP arithmetic.
    """
    alpha=preserve_vertex_alpha(simple)
    result=bytearray(alpha[1160:1560])
    # Both UV fetches need r0.x as their vertex index. Keep color last,
    # since fetching it replaces r0. Describe the same order to the linker.
    result[0x17C:0x17C]=struct.pack('>I',0x00015005)
    struct.pack_into('>I',result,0x180,0x0030A006)
    for offset,value in [(4,404),(8,168),(332+4,168),(332+20,0x2042),
                         (332+28,4),(0x184,0x0000F050),
                         (0x18C,0x0000100B),(0x190,0x0000100D)]:
        struct.pack_into('>I',result,offset,value)
    instructions=[list(struct.unpack_from('>3I',alpha,1560+i*12)) for i in range(13)]
    instructions[0][0]=0x70554003  # Four fetches at instruction 3.
    instructions[1][0]=0x00004007  # WVP starts after the added fetch.
    instructions[2][0]=0x0000300B  # Three final ALUs, now at instruction 11.
    instructions.insert(5,[0x05F81000,0x0000023F,0]) # r1.zw = UV1.xy, preserve r1.xy.
    instructions[11][0]=0xC80F8000 # Export both UV sets together at parameter 0.
    instructions[11][1]=0 # Identity swizzles: the donor only preserved XY.
    for instruction in instructions:result.extend(struct.pack('>3I',*instruction))
    if len(result)!=572:raise ValueError('Unexpected dual UV container extent')
    return bytes(result)


def clone_effect(blend, simple, pixel, masked, vertex_alpha_candidate=False, dual_uv_candidate=False,
                 cutout_candidate=False, property_cutout=None):
    def check(condition, message):
        if not condition:
            raise ValueError(message)
    check(hashlib.sha256(blend).hexdigest() ==
          '6770839b1f96de25ff5388d61bfe570b15d849abddfaeff7bd0f5e174d483959',
          'Unrecognized native Blend effect')
    check(hashlib.sha256(simple).hexdigest() ==
          '8241c2af7050e3245fbe8a33a3aaa2de1e08a9938847b72f56171fb2a7cd354a',
          'Unrecognized native Simple effect')
    check(pixel and len(pixel) % 12 == 0 and len(pixel) <= 516,
          'Pixel program does not fit the audited native allocation')
    result = bytearray(blend)
    if cutout_candidate:
        # The native effect's scalar-state table calls the device cull setter
        # (method offset 0x38) after NiStencilProperty/material state setup.
        # Match the audited Wii cutout material's DRAW_BOTH requirement here.
        check(struct.unpack_from('>2I', blend, 4552) == (0x38, 2),
              'Native cutout donor cull-state record changed')
        struct.pack_into('>I', result, 4556, 0)
    check(not dual_uv_candidate or (vertex_alpha_candidate and not masked),
          'Dual UV candidate requires the alpha interface and unmasked material')
    check(not cutout_candidate or (vertex_alpha_candidate and not masked and not dual_uv_candidate),
          'Cutout candidate requires the simple alpha interface')
    check(property_cutout in (None,'greater127','gequal127','maskedgreater127'), 'Unknown authored cutout comparison')
    check(not property_cutout or (vertex_alpha_candidate and masked==(property_cutout=='maskedgreater127') and not dual_uv_candidate and not cutout_candidate),
          'Property cutout requires the audited one-map RGBA interface')
    if property_cutout:
        # Complete scalar render state table is identical in the two original
        # donors. Retain Simple state, including cull, rather than EM1A's
        # unrelated DrawBoth override. Only sampler1 is read by this program.
        check(simple[2244:2328]==blend[4532:4616], 'Simple/Blend scalar states differ')
    alias = b'EM1H_StaticEnv_Blend' if property_cutout=='maskedgreater127' else b'EM1G_StaticEnv_Blend' if property_cutout=='greater127' else b'EM1Q_StaticEnv_Blend' if property_cutout else b'EM1A_StaticEnv_Blend' if cutout_candidate else b'EM1D_StaticEnv_Blend' if dual_uv_candidate else b'EM1M_StaticEnv_Blend' if masked else b'EM1W_StaticEnv_Blend'
    check(blend[0x1504:0x1504+21] == b'Gen3_StaticEnv_Blend\0',
          'Native technique string moved')
    result[0x1504:0x1504+21] = alias + b'\0'
    # Retain the outer opaque record extent so all FXLO offsets remain stable.
    # The inner native shader is self-contained. Simple needs only position,
    # UV0 and vertex color; the donor Blend vertex program also needs tangents
    # and a second UV stream, which most EM1 architecture does not provide.
    check(struct.unpack_from('>I', blend, 1732)[0] == 1428 and
          struct.unpack_from('>I', simple, 1156)[0] == 556,
          'Native vertex-container extents changed')
    alpha_vertex=preserve_vertex_alpha(simple) if vertex_alpha_candidate else simple
    vertex=build_dual_uv_vertex_container(simple) if dual_uv_candidate else alpha_vertex[1160:1716]
    result[1736:3164] = vertex + bytes(1428-len(vertex))
    result[3956:4472] = pixel + bytes(516-len(pixel))
    interface_changes=[]
    if vertex_alpha_candidate:
        # The replacement PS reads UV.xy at r0 and RGBA at r1, unlike the
        # retail Blend PS's three inputs. Retain its texture/constant table.
        interface_changes=[(3868,0x00002C63,0x00002042 if dual_uv_candidate else 0x00001842),
                           (3872,0x00070007,0x00030003),
                           (3880,0x0000F050,0x0000F050 if dual_uv_candidate else 0x00003050),
                           (3884,0x00007151,0x0000F151)]
        for offset,before,after in interface_changes:
            check(struct.unpack_from('>I',blend,offset)[0]==before,
                  'Native pixel interpolator layout changed')
            struct.pack_into('>I',result,offset,after)
    if cutout_candidate or property_cutout:
        check(struct.unpack_from('>I',blend,3892)[0]==0x41700000,
              'Native c252 literal moved')
        struct.pack_into('>f',result,3892,(127 if property_cutout else 128)/255)
    # Source shader-map array order, not the retail donor's normal/light maps.
    changes = [(0x18AC, 2, 1)]  # diffuse two -> source map one
    if masked:
        changes.append((0x188C, 1, 2))  # sampler two -> source mask map two
    for offset, before, after in changes:
        check(struct.unpack_from('>I', blend, offset)[0] == before,
              'Native NTMIndex annotation changed')
        struct.pack_into('>I', result, offset, after)
    check(len(result) == len(blend), 'Clone changed enclosing offsets')
    allowed = [(0x1504,0x1519),(1736,3164),(3956,4472)]
    allowed += [(offset,offset+4) for offset,_,_ in changes]
    allowed += [(offset,offset+4) for offset,_,_ in interface_changes]
    if cutout_candidate or property_cutout:allowed += [(3892,3896)]
    if cutout_candidate:allowed += [(4556,4560)]
    check(all(a == b or any(lo <= i < hi for lo, hi in allowed)
              for i,(a,b) in enumerate(zip(blend,result))),
          'Clone modified an unaudited byte')
    return alias.decode(), bytes(result)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-pak',type=Path,required=True)
    parser.add_argument('--pixel-test',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--replace-existing-candidates',action='store_true',
                        help='Rebuild named EM1 candidates into a new archive, preserving other effects')
    parser.add_argument('--vertex-alpha-candidate',action='store_true',
                        help='Private unvalidated native vertex-alpha candidate; requires vertex GPU/binding checks')
    parser.add_argument('--dual-uv-candidate',action='store_true',
                        help='Also stage an unvalidated four-stream dual UV effect')
    parser.add_argument('--cutout-candidate',action='store_true',
                        help='Stage cutout pixel output; native alpha/depth material binding still required')
    parser.add_argument('--require-simple-alpha',action='store_true',
                        help='Require the dedicated one-map alpha program GPU result')
    parser.add_argument('--require-property-cutouts',action='store_true',
                        help='Require exact GREATER127 and GEQUAL127 one-map GPU results')
    parser.add_argument('--require-masked-property-cutout',action='store_true',
                        help='Require source masked blend GREATER127 GPU result')
    args=parser.parse_args()
    if args.output.exists() or args.source_pak.resolve()==args.output.resolve():
        raise ValueError('Output must be a new private archive')
    report=json.loads((args.pixel_test/'wii-blend-results.json').read_text())
    if report['draws'] not in (24,36,48,54,60,72,78) or report['invalid'] or report['device_errors']:
        raise ValueError('Both vertex interfaces must pass the GPU pixel diagnostic')
    if args.require_simple_alpha and report['draws'] not in (60,72,78):
        raise ValueError('Rebuild the canonical executable for the one-map alpha GPU diagnostic')
    if args.require_property_cutouts and report['draws'] not in (72,78):
        raise ValueError('Rebuild the canonical executable for authored property-cutout GPU diagnostics')
    if args.require_masked_property_cutout and report['draws']!=78:
        raise ValueError('Rebuild the canonical executable for source masked-property cutout GPU diagnostics')
    pak=runpy.run_path(str(ROOT/'scripts/game-pak.py'))
    entries=pak['read_pak'](args.source_pak)
    files={name:(data,kind) for name,data,kind in entries}
    blend,kind=files['cookedshaders/360/Gen3_StaticEnv_Blend.fxlo']
    simple,_=files['cookedshaders/360/Gen3_StaticEnv_Simple.fxlo']
    added=[]
    candidates=[(False,'simple-blend'),(True,'simple-masked')]
    if args.dual_uv_candidate:
        if not args.vertex_alpha_candidate or report['draws'] not in (48,54,60,72,78):
            raise ValueError('Dual UV staging requires alpha candidate and all eight pixel variants')
        candidates.append((False,'dual-simple-blend'))
    if args.cutout_candidate:
        if not args.vertex_alpha_candidate or report['draws'] not in (54,60,72,78):
            raise ValueError('Cutout staging requires alpha candidate and cutout GPU boundary tests')
        candidates.append((False,'cutout'))
    for masked,label in candidates:
        pixel=(args.pixel_test/(label+'.ucode.txt.bin')).read_bytes()
        name,effect=clone_effect(blend,simple,pixel,masked,args.vertex_alpha_candidate,
                                label=='dual-simple-blend',label=='cutout')
        path='cookedshaders/360/'+name+'.fxlo'
        if path in files and not args.replace_existing_candidates:
            raise ValueError('Candidate name already present')
        if path in files:
            entries=[(n,effect,kind) if n==path else (n,d,k) for n,d,k in entries]
        else:
            entries.append((path,effect,kind))
        added.append({'name':path,'sha256':hashlib.sha256(effect).hexdigest()})
    if report['draws'] in (60,72,78):
        # New alias is only published after its exact one-map pixel program
        # passed the canonical GPU diagnostic. Older reports remain usable.
        name,effect=clone_simple_alpha_effect(simple)
        generated=runpy.run_path(str(ROOT/'scripts/inspect-scene-shaders.py'))['extract'](effect)[0]
        actual=effect[generated['code_offset']:generated['code_offset']+generated['code_bytes']]
        if actual!=(args.pixel_test/'simple-alpha.ucode.txt.bin').read_bytes():
            raise ValueError('One-map alpha program differs from GPU-validated bytes')
        path='cookedshaders/360/'+name+'.fxlo'
        if path in files and not args.replace_existing_candidates:
            raise ValueError('Candidate name already present')
        entries=[(n,effect,kind) if n==path else (n,d,k) for n,d,k in entries] if path in files else entries+[(path,effect,kind)]
        added.append({'name':path,'sha256':hashlib.sha256(effect).hexdigest()})
    if report['draws'] in (72,78):
        for comparison in ('greater127','gequal127'):
            pixel=(args.pixel_test/(comparison+'.ucode.txt.bin')).read_bytes()
            name,effect=clone_effect(blend,simple,pixel,False,True,property_cutout=comparison)
            path='cookedshaders/360/'+name+'.fxlo'
            if path in files and not args.replace_existing_candidates:
                raise ValueError('Candidate name already present')
            entries=[(n,effect,kind) if n==path else (n,d,k) for n,d,k in entries] if path in files else entries+[(path,effect,kind)]
            added.append({'name':path,'sha256':hashlib.sha256(effect).hexdigest()})
    if report['draws']==78:
        pixel=(args.pixel_test/'masked-greater127.ucode.txt.bin').read_bytes()
        if pixel!=masked_property_cutout_program((args.pixel_test/'simple-masked.ucode.txt.bin').read_bytes()):
            raise ValueError('Masked property-cutout differs from exact validated edit')
        name,effect=clone_effect(blend,simple,pixel,True,True,property_cutout='maskedgreater127')
        path='cookedshaders/360/'+name+'.fxlo'
        if path in files and not args.replace_existing_candidates:
            raise ValueError('Candidate name already present')
        entries=[(n,effect,kind) if n==path else (n,d,k) for n,d,k in entries] if path in files else entries+[(path,effect,kind)]
        added.append({'name':path,'sha256':hashlib.sha256(effect).hexdigest()})
    defaults,_=files['cookedshaders/360/Gen3_Defaults.fxlo']
    for variant in ('O','T','R'):
        name,effect=clone_template_state_effect(defaults,variant)
        path='cookedshaders/360/'+name+'.fxlo'
        if path in files and not args.replace_existing_candidates:
            raise ValueError('Template-state effect name already present')
        entries=[(n,effect,kind) if n==path else (n,d,k) for n,d,k in entries] if path in files else entries+[(path,effect,kind)]
        added.append({'name':path,'sha256':hashlib.sha256(effect).hexdigest(),'shader_programs':'unchanged native fallback'})
    args.output.parent.mkdir(parents=True,exist_ok=True)
    pak['write_pak'](entries,args.output)
    if args.vertex_alpha_candidate and args.dual_uv_candidate and args.cutout_candidate:
        runpy.run_path(str(ROOT/'scripts/em1_vertex_fetch_order_test.py'))['check'](simple,args.output)
    print(json.dumps({'added':added,'entries':len(entries),'vertex_alpha_candidate':args.vertex_alpha_candidate,
                      'dual_uv_candidate':args.dual_uv_candidate,
                      'cutout_candidate':args.cutout_candidate,
                      'native_loader_validation':'pending','activated':False},indent=2))


if __name__=='__main__':
    main()
