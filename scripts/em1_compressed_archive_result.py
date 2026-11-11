"""Independent PAK/zlib decoding: original resource bytes and streaming sizes.

This validates archive packing, not native world activation or playability.
"""
import argparse, hashlib, json, runpy, struct
from pathlib import Path

p=argparse.ArgumentParser()
p.add_argument('--original-profile',type=Path,required=True)
p.add_argument('--profile',type=Path,required=True)
a=p.parse_args()
read=runpy.run_path(str(Path(__file__).with_name('game-pak.py')))['read_pak']
def archives(root):
    files=list((root/'user_data').glob('em1-native-paks-*/*.pak'))
    assert files, f'No disk archive capture in {root}'
    return {f.name:f for f in files}
before,after=archives(a.original_profile),archives(a.profile)
assert before.keys()==after.keys()
rows=[]
for name,path in sorted(after.items()):
    expected,actual=read(before[name]),read(path)
    assert actual==expected, f'Resource names, tags or decoded bytes changed: {name}'
    raw=path.read_bytes(); directory=struct.unpack_from('>I',raw,12)[0]
    count=struct.unpack_from('>I',raw,directory)[0]
    sizes=[struct.unpack_from('>6I',raw,directory+4+i*24)[:3] for i in range(count)]
    assert max(r[1] for r in sizes)<=8*1024*1024
    rows.append(dict(archive=name,resources=count,compressed_resources=sum(x!=y for x,y,_ in sizes),
        maximum_original_resource=max(r[0] for r in sizes),maximum_stored_resource=max(r[1] for r in sizes),
        original_archive_bytes=before[name].stat().st_size,packed_archive_bytes=len(raw),
        packed_sha256=hashlib.sha256(raw).hexdigest(),all_decoded_bytes_identical=True))
assert any(r['compressed_resources'] for r in rows)
result=dict(archives=rows,resource_count=sum(r['resources'] for r in rows),world_activated=False)
(a.profile/'compressed-archive-result.json').write_text(json.dumps(result,indent=2))
print(json.dumps(result,indent=2))
