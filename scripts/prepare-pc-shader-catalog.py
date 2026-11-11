"""Extract the complete mounted shader archive for PC preparation; never modify it."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import struct

root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--game-dir',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
pak=runpy.run_path(str(root/'scripts/game-pak.py'))
shader=runpy.run_path(str(root/'scripts/inspect-scene-shaders.py'))
source=args.game_dir/'packfiles/shaders.pak'
pairs=[];effects=0
for name,data,kind in pak['read_pak'](source):
    if not name.lower().endswith('.fxlo'):continue
    effects+=1
    vertex=shader['extract'](data,pixel=False);pixel=shader['extract'](data,pixel=True)
    if len(vertex)!=len(pixel):raise ValueError('Unmatched shader stages in '+name)
    for v,p in zip(vertex,pixel):pairs.append((v,p))
out=bytearray(b'EM2UCOD1')+struct.pack('<I',len(pairs))
for pair in pairs:
    for program in pair:
        code=struct.pack('>'+str(len(program['words']))+'I',*program['words'])
        out+=struct.pack('<I',len(code))+code
args.output.parent.mkdir(parents=True,exist_ok=True)
args.output.write_bytes(out)
report={'archive_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'effects':effects,'pairs':len(pairs),'programs':len(pairs)*2,'catalog_sha256':hashlib.sha256(out).hexdigest(),'catalog_bytes':len(out),'scope':'Original Xbox microcode, paired by effect program order. No PC compilation or runtime variant coverage claim.'}
args.output.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report))
