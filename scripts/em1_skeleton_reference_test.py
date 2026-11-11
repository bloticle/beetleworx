"""Original skeleton arrays/poses and model inverse-bind compatibility checks."""
import argparse,ctypes as c,struct,runpy,tempfile,json,math
from pathlib import Path
from em1_rvz import RVZ
from em1_nif import NIF
p=argparse.ArgumentParser();p.add_argument('--library',required=True);p.add_argument('--image',type=Path,required=True);args=p.parse_args()
d=c.CDLL(str(Path(args.library).resolve()));U=c.POINTER(c.c_ubyte);F=c.POINTER(c.c_float)
d.skeleton_blob.argtypes=[U,c.c_size_t,c.POINTER(c.c_size_t)];d.skeleton_blob.restype=c.c_char_p
d.skeleton_bone.argtypes=[c.c_size_t,c.POINTER(c.c_int32),c.POINTER(c.c_uint8),F];d.skeleton_bone.restype=c.c_char_p
d.reference_skin_palette.argtypes=[U,c.c_size_t,c.c_int,c.c_uint32,c.POINTER(F),c.POINTER(c.c_size_t)];d.reference_skin_palette.restype=c.c_char_p
r=RVZ(args.image)
try:
 files={n.lower():v for n,v in r.files(1).items()}
 with tempfile.TemporaryDirectory() as temp:
  path=Path(temp)/'source.pak';path.write_bytes(r.read_partition(1,*files['packfiles/_dynamic.pak']))
  entries={n.lower():b for n,b,k in runpy.run_path(str(Path(__file__).with_name('game-pak.py')))['read_pak'](path)}
finally:r.close()
blob=entries['characters/mickey/characters/mickey_rig_skin.hkx_wii'];word=lambda at:struct.unpack_from('>I',blob,at)[0]
sections=[struct.unpack_from('>7I',blob,84+i*48) for i in range(word(20))];pointers={};classes={}
def text(at):return blob[at:blob.index(0,at)].decode()
for index,(start,local,global_,virtual,exports,imports,end) in enumerate(sections):
 for at in range(start+local,start+global_,8):
  source,target=struct.unpack_from('>2I',blob,at)
  if source==0xffffffff:break
  pointers[start+source]=start+target
 for at in range(start+global_,start+virtual,12):
  source,section,target=struct.unpack_from('>3I',blob,at)
  if source==0xffffffff:break
  pointers[start+source]=sections[section][0]+target
 for at in range(start+virtual,start+exports,12):
  source,section,target=struct.unpack_from('>3I',blob,at)
  if source==0xffffffff:break
  classes[start+source]=text(sections[section][0]+target)
base=next(at for at,name in classes.items() if name=='hkaSkeleton');count=word(base+16);assert count==83 and word(base+28)==count and word(base+40)==count
parent_at,bone_at,pose_at=(pointers[base+i] for i in [12,24,36])
def native(raw):
 source=(c.c_ubyte*len(raw)).from_buffer_copy(raw);n=c.c_size_t();return d.skeleton_blob(source,len(raw),c.byref(n)),n.value
error,n=native(blob);assert not error,error;assert n==count
names=[]
for i in range(count):
 parent=c.c_int32();lock=c.c_uint8();pose=(c.c_float*12)();name=d.skeleton_bone(i,c.byref(parent),c.byref(lock),pose).decode()
 assert parent.value==struct.unpack_from('>h',blob,parent_at+i*2)[0]
 assert lock.value==blob[bone_at+i*8+4] and name==text(pointers[bone_at+i*8])
 assert struct.pack('>12f',*pose)==blob[pose_at+i*48:pose_at+(i+1)*48];names.append(name)
model=NIF.read(entries['characters/mickey/mickey.nif_wii']);tail=model.blocks[7][-166167:];user=struct.unpack_from('<I',model.prefix,model.prefix.index(b'\n')+6)[0]
source=(c.c_ubyte*len(tail)).from_buffer_copy(tail);matrices=F();n=c.c_size_t();error=d.reference_skin_palette(source,len(tail),1,user,c.byref(matrices),c.byref(n));assert not error,error;assert n.value==83
maximum=max(abs(matrices[i*16+j]-(1 if j%5==0 else 0)) for i in range(83) for j in range(16))
assert maximum<.01,maximum
bad=[]
for end in [0,63,len(blob)-1]:bad.append(blob[:end])
for at,value in [(base+28,82),(pose_at,0x7fc00000)]:
 changed=bytearray(blob);struct.pack_into('>I',changed,at,value);bad.append(changed)
changed=bytearray(blob);struct.pack_into('>h',changed,parent_at+2,1);bad.append(changed)
for changed in bad:assert native(changed)[0]
result=dict(bones=count,names=names,reference_pose_lanes_verified=count*12,reference_skin_palette_max_identity_error=maximum,malformed_rejected=len(bad),scope='Actual HK7 skeleton/name/parent/Qs data and actual Mickey inverse binds. Reference-pose compatibility only; animation clips, live runtime pose, native rendering not proven.')
Path('analysis/em1-compatibility/skeleton-reference-result.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS: original83bone names/parents/996pose lanes; palette max identity deviation',maximum)
