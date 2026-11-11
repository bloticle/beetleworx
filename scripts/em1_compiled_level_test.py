"""Check Xbox registry codec against original bytes and malformed inputs."""
import argparse,ctypes as c,hashlib,json,runpy,struct
from pathlib import Path
parser=argparse.ArgumentParser();parser.add_argument('--library',type=Path,required=True);parser.add_argument('--archive',type=Path,required=True);args=parser.parse_args()
files=runpy.run_path(str(Path(__file__).with_name('game-pak.py')))['read_pak'](args.archive)
raw=next(data for name,data,_ in files if name=='levels/master.lvlc')
dll=c.CDLL(str(args.library.resolve()));U=c.POINTER(c.c_ubyte)
dll.compiled.argtypes=[U,c.c_size_t,c.POINTER(U),c.POINTER(c.c_size_t)];dll.compiled.restype=c.c_char_p
def convert(data):
    source=(c.c_ubyte*len(data)).from_buffer_copy(data);pointer=U();size=c.c_size_t()
    error=dll.compiled(source,len(data),c.byref(pointer),c.byref(size))
    return error,None if error else c.string_at(pointer,size.value)
error,encoded=convert(raw);assert not error,error;assert encoded==raw
pool=struct.unpack_from('>I',raw,8)[0];zones=struct.unpack_from('>I',raw,12)[0]
# The independent, pre-existing console catalog has205 original states.
# Exact whole-file equality protects all state/link/resource/metadata records,
# including fields not yet semantically mapped for EM1.
malformed=[b'',raw[:15],raw[:pool-1],raw[:-1]]
for offset,value in [(0,8),(8,15),(8,len(raw)+4),(12,0xffffffff),(16,0xffffffff)]:
    changed=bytearray(raw);struct.pack_into('>I',changed,offset,value);malformed.append(bytes(changed))
for data in malformed:
    error,_=convert(data);assert error,'Malformed compiled registry accepted'
result=dict(original_sha256=hashlib.sha256(raw).hexdigest(),bytes=len(raw),zones=zones,
    exact_roundtrip=True,malformed_rejected=len(malformed),
    scope='Original Xbox registry format roundtrip, not EM1 state mapping or native activation.')
Path('analysis/em1-compatibility/compiled-level-codec-result.json').write_text(json.dumps(result,indent=2)+'\n')
print(f'PASS: {zones} zones, {len(raw)} bytes exactly preserved; {len(malformed)} malformed inputs rejected')
