"""Original Mickey embedded skin: bounded parse and byte-exact value retention."""
import argparse,ctypes as c,json,struct,tempfile,runpy,hashlib
from pathlib import Path
from em1_rvz import RVZ
from em1_nif import NIF
p=argparse.ArgumentParser();p.add_argument('--library',required=True);p.add_argument('--image',type=Path,required=True);args=p.parse_args()
d=c.CDLL(str(Path(args.library).resolve()));U=c.POINTER(c.c_ubyte)
d.mesh_requirements.argtypes=[c.c_wchar_p,c.c_char_p,c.c_char_p,c.POINTER(c.c_size_t)];d.mesh_requirements.restype=c.c_char_p
d.mesh_requirement.argtypes=[c.c_size_t,c.POINTER(c.c_uint32)];d.mesh_requirement.restype=c.c_char_p
d.mesh_skin_counts.argtypes=[c.c_size_t,c.POINTER(c.c_uint32)];d.mesh_skin_counts.restype=c.c_int
d.cooked_skin.argtypes=[U,c.c_size_t,c.c_int,c.c_uint32,c.POINTER(U),c.POINTER(c.c_size_t)];d.cooked_skin.restype=c.c_char_p
count=c.c_size_t();error=d.mesh_requirements(str(args.image),b'packfiles/_dynamic.pak',b'characters/mickey/mickey.nif_wii',c.byref(count));assert not error,error
assert count.value==1;fields=(c.c_uint32*6)();d.mesh_requirement(0,fields);counts=(c.c_uint32*6)();assert d.mesh_skin_counts(0,counts)
r=RVZ(args.image)
try:
 files={name.lower():span for name,span in r.files(1).items()}
 with tempfile.TemporaryDirectory() as temp:
  path=Path(temp)/'dynamic.pak';path.write_bytes(r.read_partition(1,*files['packfiles/_dynamic.pak']))
  entries=runpy.run_path(str(Path(__file__).with_name('game-pak.py')))['read_pak'](path)
  original=next(b for name,b,k in entries if name.lower()=='characters/mickey/mickey.nif_wii')
finally:r.close()
n=NIF.read(original);tail=n.blocks[fields[0]][-fields[5]:];user=struct.unpack_from('<I',n.prefix,n.prefix.index(b'\n')+6)[0]
at=0
def word():
 global at
 value=struct.unpack_from(n.endian+'I',tail,at)[0];at+=4;return value
has_weights=word();transforms=word();at+=transforms*4;weights=word();at+=weights*24;vertices=word();vertex_start=at;vertex_indices=struct.unpack_from(n.endian+str(vertices)+'I',tail,at);at+=vertices*4
morphs=word();width=word();at+=morphs*width*12+morphs*2
size=struct.unpack_from(n.endian+('H' if user<17 else 'I'),tail,at)[0];at+=2 if user<17 else 4;at+=size
partitions=word();at+=partitions*28;maximum=word();assert at==len(tail)
assert list(counts)==[transforms,weights,vertices,morphs,width,partitions]
assert list(counts)==[1328,229,3521,840,12,61] and all(i<weights for i in vertex_indices)
def roundtrip(blob):
 raw=(c.c_ubyte*len(blob)).from_buffer_copy(blob);output=U();length=c.c_size_t();error=d.cooked_skin(raw,len(blob),int(n.endian=='>'),user,c.byref(output),c.byref(length))
 return error,c.string_at(output,length.value) if not error else None
error,encoded=roundtrip(tail);assert not error,error;assert encoded==tail
invalid=[]
for end in [0,3,7,len(tail)-1]:invalid.append(tail[:end])
bad=bytearray(tail);struct.pack_into(n.endian+'I',bad,8,0x7fc00000);invalid.append(bad)
bad=bytearray(tail);struct.pack_into(n.endian+'I',bad,vertex_start,weights);invalid.append(bad)
for bad in invalid:assert roundtrip(bad)[0]
result=dict(block=fields[0],bytes=len(tail),counts=list(counts),sha256=hashlib.sha256(tail).hexdigest(),roundtrip_exact=True,malformed_rejected=len(invalid),scope='Original embedded skin/morph data retained. No native bone binding, deformation or animation/playback proof.')
Path('analysis/em1-compatibility/cooked-skin-result.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS:',result)

# Deformation uses actual Mickey inverse-bind matrices and weight mappings.
# Poses/vectors are controlled test inputs; this is not animation/renderer proof.
F=c.POINTER(c.c_float)
d.deform_skin.argtypes=[U,c.c_size_t,c.c_int,c.c_uint32,F,c.c_size_t,F,c.POINTER(c.c_uint32),c.c_size_t,c.c_int,c.POINTER(F)];d.deform_skin.restype=c.c_char_p
bones=transforms//16;inverse=list(struct.unpack_from(n.endian+str(transforms)+'f',tail,8));weight_start=8+transforms*4+4
weight_records=[struct.unpack_from(n.endian+'3i3f',tail,weight_start+i*24) for i in range(weights)]
poses=[1.0 if i%5==0 else 0.0 for _ in range(bones) for i in range(16)]
for bone in range(bones):poses[bone*16+12]=bone*.125
vectors=[(float(i%17),float(i%7)*.5,float(i%5)*.25) for i in range(vertices)]
flat=(c.c_float*(vertices*3))(*[v for p in vectors for v in p]);pose_buffer=(c.c_float*len(poses))(*poses);sources=(c.c_uint32*vertices)(*range(vertices));raw=(c.c_ubyte*len(tail)).from_buffer_copy(tail)
palette=[]
for bone in range(bones):
 a=poses[bone*16:bone*16+16];b=inverse[bone*16:bone*16+16]
 palette.append([sum(a[k*4+i%4]*b[i//4*4+k] for k in range(4)) for i in range(16)])
for translate in [0,1]:
 output=F();error=d.deform_skin(raw,len(tail),int(n.endian=='>'),user,pose_buffer,bones,flat,sources,vertices,translate,c.byref(output));assert not error,error
 for vertex,p in enumerate(vectors):
  record=weight_records[vertex_indices[vertex]];expected=[0.,0.,0.]
  for bone,value in zip(record[:3],record[3:]):
   if value==0:continue
   m=palette[bone]
   for axis in range(3):expected[axis]+=value*(m[axis]*p[0]+m[4+axis]*p[1]+m[8+axis]*p[2]+(m[12+axis] if translate else 0.))
  for axis in range(3):assert abs(output[vertex*3+axis]-expected[axis])<=.0002+abs(expected[axis])*.00001
bad_sources=(c.c_uint32*1)(vertices);output=F();assert d.deform_skin(raw,len(tail),int(n.endian=='>'),user,pose_buffer,bones,flat,bad_sources,1,1,c.byref(output))
assert d.deform_skin(raw,len(tail),int(n.endian=='>'),user,pose_buffer,bones-1,flat,sources,vertices,1,c.byref(output))
record=vertex_indices[0];channel=next(i for i,value in enumerate(weight_records[record][3:]) if value>0)
for offset,value in [(weight_start+record*24+channel*4,0xffffffff),(weight_start+record*24+12+channel*4,0xbf000000)]:
 invalid=bytearray(tail);struct.pack_into(n.endian+'I',invalid,offset,value);storage=(c.c_ubyte*len(invalid)).from_buffer_copy(invalid)
 assert d.deform_skin(storage,len(invalid),int(n.endian=='>'),user,pose_buffer,bones,flat,sources,vertices,1,c.byref(output))
result['deformed_vertices']=vertices;result['palette_bones']=bones;result['positions_and_linear_normals_verified']=True
result['scope']='Actual cooked transforms/weights with controlled model-space poses/vectors; independent weighted arithmetic. No rig pose composition, original PPC skin parity, animation or render publication.'
Path('analysis/em1-compatibility/cooked-skin-result.json').write_text(json.dumps(result,indent=2)+'\n')
print('PASS: actual83-bone/3521-vertex deformation, position vs normal translation, invalid source/bone/weight bindings')

d.morph_skin.argtypes=[U,c.c_size_t,c.c_int,c.c_uint32,F,c.c_size_t,F,c.POINTER(c.c_uint32),c.c_size_t,c.POINTER(F)];d.morph_skin.restype=c.c_char_p
morph_offset=vertex_start+vertices*4+8;deltas=struct.unpack_from(n.endian+str(morphs*width*3)+'f',tail,morph_offset)
indices=struct.unpack_from(n.endian+str(morphs)+'H',tail,morph_offset+morphs*width*12);last={source:row for row,source in enumerate(indices)}
slots=[.25,.5,1,0,.75,0,5]+[0]*(width-7);slot_buffer=(c.c_float*width)(*slots);output=F()
error=d.morph_skin(raw,len(tail),int(n.endian=='>'),user,slot_buffer,width,flat,sources,vertices,c.byref(output));assert not error,error
selected=[(i,value) for i,value in enumerate(slots) if value>0][:4]
for vertex,base in enumerate(vectors):
 expected=list(base)
 if vertex in last:
  row=last[vertex]
  for slot,value in selected:
   for axis in range(3):expected[axis]+=deltas[(row*width+slot)*3+axis]*value
 for axis in range(3):assert abs(output[vertex*3+axis]-expected[axis])<=.0002+abs(expected[axis])*.00001
assert d.morph_skin(raw,len(tail),int(n.endian=='>'),user,slot_buffer,width-1,flat,sources,vertices,c.byref(output))
slot_buffer[0]=float('nan');assert d.morph_skin(raw,len(tail),int(n.endian=='>'),user,slot_buffer,width,flat,sources,vertices,c.byref(output))
result['morphed_vertices']=vertices;result['morph_first_four_positive_slots_verified']=True
Path('analysis/em1-compatibility/cooked-skin-result.json').write_text(json.dumps(result,indent=2)+'\n')
print('PASS: original sparse morph rows, first-four positive slots, invalid dimensions/nonfinite slots')

# Repeated sparse rows overwrite the previous row; fifth positive slot is inert.
synthetic=struct.pack('>6I',0,0,0,0,2,5)+struct.pack('>30f',*([100.,100.,100.]*5+[1.,2.,3.]*5))+struct.pack('>2HH2I',1,1,0,0,0)
storage=(c.c_ubyte*len(synthetic)).from_buffer_copy(synthetic);sample_base=(c.c_float*9)(5,6,7,2,3,4,0,0,0);sample_sources=(c.c_uint32*3)(1,2,1);sample_slots=(c.c_float*5)(.1,.2,.3,.4,100)
error=d.morph_skin(storage,len(synthetic),1,15,sample_slots,5,sample_base,sample_sources,3,c.byref(output));assert not error,error
for actual,expected in zip(output[:9],[6,8,10,2,3,4,1,2,3]):assert abs(actual-expected)<.00001
result['duplicate_morph_rows_and_fifth_slot_verified']=True
Path('analysis/em1-compatibility/cooked-skin-result.json').write_text(json.dumps(result,indent=2)+'\n')
print('PASS: repeated sparse rows overwrite; fifth positive morph slot ignored; unmapped vertices retained')
