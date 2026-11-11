"""Recover the pinned SDK's exact render-target shader bytecode for a source build.

The installed SDK archive is read only. Neither game assets nor shader programs
are changed. The source-built render-target uses the same public ABI and DXBC.
"""
import argparse,hashlib,json,re,struct,subprocess
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('sdk',type=Path);p.add_argument('output',type=Path)
p.add_argument('--ar',type=Path,required=True);a=p.parse_args()
archive=a.sdk/'lib/rexgraphics.lib'
header=a.sdk/'include/rex/graphics/d3d12/render_target_cache.h'
if hashlib.sha256(header.read_text(encoding='utf-8').encode()).hexdigest()!='e86614f87935c3fe8d41090d36e51c00eee01a107d408e9273ff31a3709f2292':
    raise SystemExit('Render-target header layout changed; review the source-built ABI')
if hashlib.sha256(archive.read_bytes()).hexdigest()!='bda95313e5f5a9c6db99c0e4363d9543c9e83cdd3ce477615c8ae77291eaa951':
    raise SystemExit('Render-target archive changed; review source and shader ABI')
obj=subprocess.check_output([str(a.ar),'p',str(archive),'render_target_cache.cpp.obj'])
machine,sections,_,table,count,optional,_=struct.unpack_from('<HHIIIHH',obj)
if machine!=0x8664 or optional:raise SystemExit('Expected AMD64 render-target COFF')
strings_at=table+count*18
strings=obj[strings_at:strings_at+struct.unpack_from('<I',obj,strings_at)[0]]
found={};i=0
while i<count:
    at=table+i*18;raw=obj[at:at+8]
    name=(strings[struct.unpack_from('<I',raw,4)[0]:] if raw[:4]==bytes(4) else raw).split(b'\0',1)[0].decode()
    value,section,kind,storage,aux=struct.unpack_from('<IhHBB',obj,at+8)
    match=re.fullmatch(r'\?([a-z0-9_]+)@shaders@d3d12@graphics@rex@@3QBEB',name)
    if match:
        if not 1<=section<=sections:raise SystemExit('Shader has no data section')
        sh=20+(section-1)*40;size,start=struct.unpack_from('<II',obj,sh+16)
        if value+32>size or obj[start+value:start+value+4]!=b'DXBC':raise SystemExit('Invalid render-target shader')
        length=struct.unpack_from('<I',obj,start+value+24)[0]
        if length<32 or value+length>size:raise SystemExit('Shader leaves its COFF section')
        key=match[1]
        if key in found:raise SystemExit('Duplicate render-target shader')
        found[key]=obj[start+value:start+value+length]
    i+=1+aux
source=Path(__file__).resolve().parent.parent/'runtime/rex_render_target/render_target_cache.cpp'
expected=set(re.findall(r'#include "(?:../)?shaders/bytecode/d3d12_5_1/([a-z0-9_]+)\.h"',source.read_text()))
if not expected or set(found)!=expected:raise SystemExit('Render-target shader registry changed: '+str(set(found)^expected))
for name,data in found.items():
    target=a.output/'shaders/bytecode/d3d12_5_1'/(name+'.h');target.parent.mkdir(parents=True,exist_ok=True)
    text='// Exact DXBC recovered from the pinned installed ReXGlue SDK.\nconst unsigned char '+name+'[] = {\n'
    text+='\n'.join(','.join(f'0x{x:02X}' for x in data[i:i+24])+',' for i in range(0,len(data),24))+'\n};\n'
    if not target.exists() or target.read_text()!=text:target.write_text(text)
manifest={name:{'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()} for name,data in found.items()}
a.output.mkdir(parents=True,exist_ok=True)
(a.output/'shader-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
(a.output/'ready.stamp').write_text(hashlib.sha256(obj).hexdigest()+'\n')
