"""Compare original Mickey clip metadata/bindings against independent HK7 reads."""
import argparse
import ctypes as c
import json
import runpy
import struct
import tempfile
from pathlib import Path
from em1_rvz import RVZ

p=argparse.ArgumentParser()
p.add_argument('--library',required=True,type=Path)
p.add_argument('--image',required=True,type=Path)
args=p.parse_args()
d=c.CDLL(str(args.library.resolve()))
U=c.POINTER(c.c_ubyte)
d.skeleton_blob.argtypes=[U,c.c_size_t,c.POINTER(c.c_size_t)]
d.skeleton_blob.restype=c.c_char_p
d.skeleton_slot.argtypes=[c.c_size_t]
d.skeleton_slot.restype=c.c_char_p
d.animation_blob.argtypes=[U,c.c_size_t,c.POINTER(c.c_uint32),c.POINTER(c.c_float)]
d.animation_blob.restype=c.c_char_p
d.animation_indices.argtypes=[c.c_int,c.POINTER(c.POINTER(c.c_int16)),c.POINTER(c.c_size_t)]
d.animation_indices.restype=c.c_char_p
d.animation_rig.restype=c.c_char_p
d.animation_offsets.argtypes=[c.c_uint,c.POINTER(c.POINTER(c.c_uint32)),c.POINTER(c.c_size_t)]
d.animation_offsets.restype=c.c_int
d.animation_pack.argtypes=[c.POINTER(U),c.POINTER(c.c_size_t)]
d.animation_pack.restype=None
r=RVZ(args.image)
try:
    files={n.lower():v for n,v in r.files(1).items()}
    with tempfile.TemporaryDirectory() as t:
        path=Path(t)/'source.pak'
        path.write_bytes(r.read_partition(1,*files['packfiles/_dynamic.pak']))
        entries={n.lower():b for n,b,k in runpy.run_path(str(Path(__file__).with_name('game-pak.py')))['read_pak'](path)}
finally:
    r.close()

class Pack:
    def __init__(self,b):
        self.b=b
        self.sections=[struct.unpack_from('>7I',b,84+i*48) for i in range(self.word(20))]
        self.ptr={}
        self.classes={}
        for start,local,global_,virtual,exports,imports,end in self.sections:
            for at in range(start+local,start+global_-7,8):
                source,target=struct.unpack_from('>2I',b,at)
                if source==0xffffffff:break
                self.ptr[start+source]=start+target
            for at in range(start+global_,start+virtual-11,12):
                source,section,target=struct.unpack_from('>3I',b,at)
                if source==0xffffffff:break
                self.ptr[start+source]=self.sections[section][0]+target
            for at in range(start+virtual,start+exports-11,12):
                source,section,target=struct.unpack_from('>3I',b,at)
                if source==0xffffffff:break
                self.classes[start+source]=self.text(self.sections[section][0]+target)
    def word(self,at):return struct.unpack_from('>I',self.b,at)[0]
    def text(self,at):return self.b[at:self.b.index(0,at)].decode()
    def object(self,name):return next(at for at,value in self.classes.items() if value==name)

rig=entries['characters/mickey/characters/mickey_rig_skin.hkx_wii']
raw=(c.c_ubyte*len(rig)).from_buffer_copy(rig)
n=c.c_size_t()
assert not d.skeleton_blob(raw,len(raw),c.byref(n)) and n.value==83
rp=Pack(rig)
sk=rp.object('hkaSkeleton')
slots=[rp.text(rp.ptr[rp.ptr[sk+48]+i*4]) for i in range(rp.word(sk+52))]
assert [d.skeleton_slot(i).decode() for i in range(len(slots))]==slots
assert d.skeleton_slot(len(slots)) is None

def native(blob):
    raw=(c.c_ubyte*len(blob)).from_buffer_copy(blob)
    counts=(c.c_uint32*9)()
    times=(c.c_float*4)()
    return d.animation_blob(raw,len(raw),counts,times),list(counts),list(times)

results=[]
first=None
for name,blob in sorted(entries.items()):
    if not name.startswith('characters/mickey/') or not name.endswith('.hkx_wii'):continue
    source=Pack(blob)
    if 'hkaSplineCompressedAnimation' not in source.classes.values():continue
    a=source.object('hkaSplineCompressedAnimation')
    binding=source.object('hkaAnimationBinding')
    error,counts,times=native(blob)
    assert not error,(name,error)
    expected=[source.word(a+i) for i in [16,20,40,44,48,52,120]]+[blob[binding+40],int(a+24 in source.ptr)]
    assert counts==expected,(name,counts,expected)
    assert struct.pack('>4f',*times)==b''.join(blob[a+i:a+i+4] for i in [12,56,60,64])
    for kind,offset in enumerate([68,80,92,104]):
        values=c.POINTER(c.c_uint32)()
        count=c.c_size_t()
        assert d.animation_offsets(kind,c.byref(values),c.byref(count))
        expected_count=source.word(a+offset+4)
        assert count.value==expected_count
        expected_offsets=list(struct.unpack_from('>'+str(expected_count)+'I',blob,source.ptr[a+offset])) if expected_count else []
        assert list(values[:count.value])==expected_offsets
    owned=U()
    owned_count=c.c_size_t()
    d.animation_pack(c.byref(owned),c.byref(owned_count))
    assert c.string_at(owned,owned_count.value)==blob
    for floats,offset in enumerate([16,28]):
        indices=c.POINTER(c.c_int16)()
        count=c.c_size_t()
        skeleton=d.animation_indices(floats,c.byref(indices),c.byref(count)).decode()
        assert skeleton==source.text(source.ptr[binding+8])
        expected_count=source.word(binding+offset+4)
        assert count.value==expected_count
        expected_indices=list(struct.unpack_from('>'+str(expected_count)+'h',blob,source.ptr[binding+offset])) if expected_count else []
        assert list(indices[:count.value])==expected_indices
    assert not d.animation_rig(),(name,d.animation_rig())
    results.append(dict(resource=name,transform_tracks=counts[0],float_tracks=counts[1],frames=counts[2],blocks=counts[3],duration=times[0],skeleton=skeleton,extracted_motion=bool(counts[8])))
    if first is None:first=(blob,source,a,binding)

assert len(results)==97,len(results)
blob,source,a,binding=first
bad=[blob[:end] for end in [0,63,len(blob)-1]]
for at,value in [(a+8,4),(a+12,0x7fc00000),(a+16,4097),(a+40,0),(a+44,0),(a+120,0xffffffff),(binding+20,82),(binding+32,4097),(source.ptr[a+68],source.word(a+120))]:
    changed=bytearray(blob)
    struct.pack_into('>I',changed,at,value)
    bad.append(changed)
for changed in bad:
    assert native(changed)[0]
# Failed parsing must leave the previously accepted clip intact.
owned=U()
owned_count=c.c_size_t()
d.animation_pack(c.byref(owned),c.byref(owned_count))
assert c.string_at(owned,owned_count.value)==entries[results[-1]['resource']]
# Parsing accepts an authored in-range int16; rig binding rejects a nonexistent bone.
changed=bytearray(blob)
struct.pack_into('>h',changed,source.ptr[binding+16],83)
assert not native(changed)[0] and d.animation_rig()
struct.pack_into('>h',changed,source.ptr[binding+16],-2)
assert native(changed)[0]
# Float destinations must be checked independently against the rig's slot count.
changed=bytearray(blob)
struct.pack_into('>h',changed,source.ptr[binding+28],len(slots))
assert not native(changed)[0] and d.animation_rig()

result=dict(clips=results,float_slots=slots,malformed_rejected=len(bad)+1,invalid_rig_binding_rejected=True,
    scope='Actual HK7 clip times/counts/type/bone/float bindings and source rig validation. Compressed sampling, behavior evaluation and native renderer/gameplay activation remain unimplemented.')
Path('analysis/em1-compatibility/animation-source-result.json').write_text(json.dumps(result,indent=2)+'\n')
print(f'PASS: {len(results)} original Mickey spline clips; {len(slots)} named rig float slots; binding/metadata parity; malformed inputs reject')
