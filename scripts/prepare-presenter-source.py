"""Recover SDK presenter shaders and build border-clamped CAS replacements.

The installed SDK archive is read only. The SDR CAS shaders and HDR CAS/prefilter programs are rebuilt locally as
SM 5.1 DXBC; the other shaders retain their exact pinned bytecode and public ABI.
"""
import argparse,hashlib,json,re,struct,subprocess
from pathlib import Path
from cas_shader_compiler import compile_cas_shaders

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('sdk',type=Path);p.add_argument('output',type=Path)
p.add_argument('--ar',type=Path,required=True);a=p.parse_args()
archive=a.sdk/'lib/rexui.lib'
header=a.sdk/'include/rex/ui/d3d12/d3d12_presenter.h'
if hashlib.sha256(header.read_text(encoding='utf-8').encode()).hexdigest()!='e68c7f73b4ae4a4d369ac50e344a8b9ecc9db39685fe24714ddbd09fcc6328ce':
    raise SystemExit('Presenter header layout changed; review the source-built ABI')
if hashlib.sha256(archive.read_bytes()).hexdigest()!='43a9315705a876daf7dd46b0c81a93c55431ea9d6f45144ec1282983d001ed04':
    raise SystemExit('Presenter archive changed; review source and shader ABI')
obj=subprocess.check_output([str(a.ar),'p',str(archive),'d3d12_presenter.cpp.obj'])
machine,sections,_,table,count,optional,_=struct.unpack_from('<HHIIIHH',obj)
if machine!=0x8664 or optional:raise SystemExit('Expected AMD64 presenter COFF')
strings_at=table+count*18
strings=obj[strings_at:strings_at+struct.unpack_from('<I',obj,strings_at)[0]]
found={};i=0
while i<count:
    at=table+i*18;raw=obj[at:at+8]
    name=(strings[struct.unpack_from('<I',raw,4)[0]:] if raw[:4]==bytes(4) else raw).split(b'\0',1)[0].decode()
    value,section,kind,storage,aux=struct.unpack_from('<IhHBB',obj,at+8)
    match=re.fullmatch(r'\?(guest_output_[a-z0-9_]+)@shaders@d3d12@ui@rex@@3QBEB',name)
    if match:
        if not 1<=section<=sections:raise SystemExit('Shader has no data section')
        sh=20+(section-1)*40;size,start=struct.unpack_from('<II',obj,sh+16)
        if value+32>size or obj[start+value:start+value+4]!=b'DXBC':raise SystemExit('Invalid presenter shader')
        length=struct.unpack_from('<I',obj,start+value+24)[0]
        if length<32 or value+length>size:raise SystemExit('Shader leaves its COFF section')
        key=match[1]
        if key in found:raise SystemExit('Duplicate presenter shader')
        found[key]=obj[start+value:start+value+length]
    i+=1+aux
expected={'guest_output_triangle_strip_rect_vs','guest_output_bilinear_ps','guest_output_bilinear_dither_ps',
    'guest_output_ffx_cas_resample_ps','guest_output_ffx_cas_resample_dither_ps',
    'guest_output_ffx_cas_sharpen_ps','guest_output_ffx_cas_sharpen_dither_ps',
    'guest_output_ffx_fsr_easu_ps','guest_output_ffx_fsr_rcas_ps','guest_output_ffx_fsr_rcas_dither_ps'}
if set(found)!=expected:raise SystemExit('Presenter shader registry changed')
original=found.copy()
cas_root=Path(__file__).resolve().parent.parent/'runtime/shaders/cas'
cas_sources={path.name:hashlib.sha256(path.read_bytes()).hexdigest() for path in sorted(cas_root.iterdir()) if path.is_file()}
overrides=compile_cas_shaders(cas_root)
found.update(overrides)
for name,data in found.items():
    target=a.output/'ui/shaders/bytecode/d3d12_5_1'/(name+'.h');target.parent.mkdir(parents=True,exist_ok=True)
    origin='Build-time border-clamped CAS DXBC' if name in overrides else 'Exact pinned ReXGlue SDK DXBC'
    text='// '+origin+'; see shader-manifest.json.\nconst unsigned char '+name+'[] = {\n'
    text+='\n'.join(','.join(f'0x{x:02X}' for x in data[i:i+24])+',' for i in range(0,len(data),24))+'\n};\n'
    if not target.exists() or target.read_text()!=text:target.write_text(text)
manifest={name:{'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),
    'origin':'clamped-cas' if name in overrides else 'pinned-sdk',
    'sdk_sha256':hashlib.sha256(original[name]).hexdigest() if name in original else None,
    **({'sources':cas_sources,'profile':'ps_5_1','compiler':'Windows D3DCompiler_47, build time'} if name in overrides else {})} for name,data in found.items()}
a.output.mkdir(parents=True,exist_ok=True)
(a.output/'shader-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
(a.output/'ready.stamp').write_text(hashlib.sha256(json.dumps(manifest,sort_keys=True).encode()).hexdigest()+'\n')
