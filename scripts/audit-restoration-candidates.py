"""Read-only target PAK directory inventory for restoration candidates."""
from pathlib import Path
import struct,json,zlib
ROOT=Path('D:/Modding/Builds/EM2X360/packfiles')
def index(path):
 with path.open('rb') as f:
  h=f.read(32)
  if h[:4]not in (b' KAP',b'PAK '):return []
  e='>'if h[:4]==b' KAP'else'<'
  ver,flags,at,size=struct.unpack_from(e+'4I',h,4)
  if ver!=2 or flags or size>32*1024*1024:raise ValueError(path)
  f.seek(at);d=f.read(size);n=struct.unpack_from(e+'I',d)[0];strings=4+n*24;payload=at+size
  assert strings<=len(d)
  def text(off):a=strings+off;return d[a:d.index(0,a)].decode()
  out=[]
  for i in range(n):
   length,packed,aligned,folder,kind,name=struct.unpack_from(e+'6I',d,4+i*24);full=(text(folder)+'/'+text(name)).lstrip('/')
   out.append(dict(archive=str(path),path=full,kind=f'{kind:08X}',length=length,packed=packed,offset=payload));payload+=aligned
  return out
if __name__=='__main__':
 rows=[]
 for p in ROOT.glob('*.pak'):rows.extend(index(p))
 bsq=[r for r in rows if r['path'].lower().endswith('.bsq')]
 effects=[r for r in rows if ('drip'in r['path'].lower()and('mickey'in r['path'].lower()or'ink'in r['path'].lower())) or ('gremlin'in r['path'].lower()and r['path'].lower().endswith('.bsq'))]
 Path('analysis/restoration-target-pak-index.json').write_text(json.dumps({'bsq':bsq,'candidates':effects},indent=2))
 print(json.dumps({'paks':len(list(ROOT.glob('*.pak'))),'entries':len(rows),'bsq':len(bsq),'candidates':effects[:25]},indent=2))
