"""Verify original palettized Wii textures, every mip/pixel and alpha byte."""
import argparse,ctypes as c,json,runpy,struct,tempfile
from pathlib import Path
from em1_rvz import RVZ
from em1_nif import NIF
p=argparse.ArgumentParser();p.add_argument('--library',required=True);p.add_argument('--image',type=Path,required=True);args=p.parse_args()
d=c.CDLL(str(Path(args.library).resolve()));U=c.POINTER(c.c_ubyte);d.texture_asset.argtypes=[U,c.c_size_t,c.POINTER(U),c.POINTER(c.c_size_t)];d.texture_asset.restype=c.c_char_p
baseline=Path('analysis/em1-compatibility/full-level-model-conversion-before-palettes.json')
models=json.loads(baseline.read_text());requests={}
d.portable_pixels.argtypes=[U,c.c_size_t,c.c_int];d.portable_pixels.restype=c.c_char_p
rejected=0
for row in models['results']:
 if (row['error'] or '').startswith('Unsupported EM1 Wii texture format'):
  name=row['error'].split('[original texture: ',1)[1][:-1];requests.setdefault(name,row['archive'])
read=runpy.run_path(str(Path(__file__).with_name('game-pak.py')))['read_pak'];r=RVZ(args.image);reports=[]
try:
 files={n.lower():v for n,v in r.files(1).items()};cache={}
 with tempfile.TemporaryDirectory() as temp:
  for name,archive in requests.items():
   raw=None
   for path in [archive,'packfiles/_dynamic.pak']:
    if path not in cache:
     file=Path(temp)/'source.pak';file.write_bytes(r.read_partition(1,*files[path]));cache[path]={n.lower():b for n,b,k in read(file)}
    if name in cache[path]:raw=cache[path][name];break
   assert raw is not None
   before=NIF.read(raw);word=lambda b,at:struct.unpack_from(before.endian+'I',b,at)[0]
   storage=(c.c_ubyte*len(raw)).from_buffer_copy(raw);pointer=U();size=c.c_size_t();error=d.texture_asset(storage,len(raw),c.byref(pointer),c.byref(size));assert not error,(name,error)
   after=NIF.read(c.string_at(pointer,size.value));mips_checked=0
   for index,block in enumerate(before.blocks):
    if before.types[before.indices[index]]!='NiPersistentSrcTextureRendererData':continue
    assert word(block,0)==3 and block[4] in [4,8];bits=block[4];palette=before.blocks[word(block,59)];colors=[palette[5+i*4:9+i*4] for i in range(word(palette,1))]
    mip_count=word(block,63);source_base=71+12*mip_count+16;out=after.blocks[index];target_base=71+12*mip_count+8
    assert after.types[after.indices[index]]=='NiPixelData' and word(out,63)==mip_count
    def validate(payload):
     storage=(c.c_ubyte*len(payload)).from_buffer_copy(payload)
     return d.portable_pixels(storage,len(payload),int(before.endian=='>'))
    assert not validate(out)
    for position,value in [(0,0),(59,0),(63,17),(67,8),(71,0),(79,1),(71+12*mip_count+4,0)]:
     bad=bytearray(out);struct.pack_into(before.endian+'I',bad,position,value);assert validate(bad);rejected+=1
    assert validate(out[:-1]);rejected+=1

    tile_height=8 if bits==4 else 4
    for mip in range(mip_count):
     width,height,offset=struct.unpack_from(before.endian+'III',block,71+12*mip)
     expected=bytearray()
     for y in range(height):
      for x in range(width):
       tile=(y//tile_height)*((width+7)//8)+x//8;pixel=(y%tile_height)*8+x%8
       value=block[source_base+offset+tile*32+(pixel//2 if bits==4 else pixel)]
       if bits==4:value=(value>>(0 if pixel%2 else 4))&15
       expected+=colors[value]
     w,h,position=struct.unpack_from(after.endian+'III',out,71+12*mip);assert (w,h)==(width,height)
     assert out[target_base+position:target_base+position+len(expected)]==expected
     mips_checked+=1
   reports.append(dict(resource=name,mips=mips_checked));assert mips_checked
finally:r.close()
assert len(reports)>=6
Path('analysis/em1-compatibility/palettized-texture-result.json').write_text(json.dumps(dict(textures=reports,malformed_portable_pixels_rejected=rejected,scope='Native conversion vs independently addressed original palettized texture pixels/all mips, including alpha. No native draw/frame comparison.'),indent=2)+'\n')
print('PASS:',len(reports),'original textures;',sum(x['mips'] for x in reports),'complete mips')
