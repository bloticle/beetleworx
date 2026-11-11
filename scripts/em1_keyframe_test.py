"""Original KF source ownership, every sequence root and byte retention."""
import argparse
import ctypes as c
import hashlib
import json
import runpy
import struct
import tempfile
from pathlib import Path
from em1_nif import NIF
from em1_rvz import RVZ
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--library',type=Path,required=True);p.add_argument('--image',type=Path,required=True)
a=p.parse_args();d=c.CDLL(str(a.library.resolve()));U=c.POINTER(c.c_ubyte)
d.keyframe.argtypes=[U,c.c_size_t];d.keyframe.restype=c.c_char_p
d.keyframe_source.argtypes=[c.c_wchar_p,c.c_char_p,c.c_char_p,c.POINTER(c.c_char_p),c.POINTER(U),c.POINTER(c.c_size_t),c.POINTER(c.c_uint32)];d.keyframe_source.restype=c.c_char_p
r=RVZ(a.image);files={name.lower():span for name,span in r.files(1).items()};results=[];sequences=evaluators=0


def rejected(data):
    raw=(c.c_ubyte*len(data)).from_buffer_copy(data);assert d.keyframe(raw,len(data))


try:
    for archive in ['packfiles/_dynamic.pak','packfiles/gv_shared_za.pak','packfiles/gremlinvillage_zonea.pak','packfiles/gv_zonea_airlock.pak']:
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'original.pak';path.write_bytes(r.read_partition(1,*files[archive]))
            for name,data,kind in runpy.run_path(str(Path(__file__).with_name('game-pak.py')))['read_pak'](path):
                if not name.lower().endswith('.kf_wii'):continue
                n=NIF.read(data);roots=struct.unpack(n.endian+'I'*(len(n.footer)//4),n.footer)[1:]
                source_evaluators=sum(struct.unpack_from(n.endian+'I',n.blocks[root],4)[0] for root in roots)
                assert roots and all(n.types[n.indices[root]]=='NiSequenceData' for root in roots)
                origin,out,size,counts=c.c_char_p(),U(),c.c_size_t(),(c.c_uint32*2)()
                error=d.keyframe_source(str(a.image),archive.encode(),name.encode(),c.byref(origin),c.byref(out),c.byref(size),counts);assert not error,(name,error)
                assert c.string_at(out,size.value)==data,'An original animation byte changed'
                assert list(counts)==[len(roots),source_evaluators] and archive.encode() in origin.value and b'kf-original-v1' in origin.value
                for bad in [b'',data[:-1],data+b'\0']:rejected(bad)
                root=roots[0];saved=n.blocks[root];bad=bytearray(saved);struct.pack_into(n.endian+'I',bad,4,0xffffffff);n.blocks[root]=bytes(bad);rejected(n.write())
                bad=bytearray(saved);struct.pack_into(n.endian+'f',bad,len(bad)-12,0);n.blocks[root]=bytes(bad);rejected(n.write());n.blocks[root]=saved
                evaluator=next(i for i,t in enumerate(n.indices) if n.types[t].endswith('Evaluator'));saved=n.blocks[evaluator];bad=bytearray(saved);struct.pack_into(n.endian+'I',bad,0,0xfffffffe);n.blocks[evaluator]=bytes(bad);rejected(n.write());n.blocks[evaluator]=saved
                if 'NiConstBoolEvaluator' in n.types:
                    index=next(i for i,t in enumerate(n.indices) if n.types[t]=='NiConstBoolEvaluator');bad=bytearray(n.blocks[index]);struct.pack_into(n.endian+'f',bad,len(bad)-4,2);n.blocks[index]=bytes(bad);rejected(n.write())
                sequences+=len(roots);evaluators+=source_evaluators;results.append(dict(archive=archive,resource=name,roots=len(roots),evaluators=source_evaluators,sha256=hashlib.sha256(data).hexdigest()))
finally:r.close()
assert len(results)==16 and sequences>16 and evaluators>sequences
report=dict(scope='Original source/container ownership and all-root retention; native loading/playback is a separate test',files=results,sequences=sequences,evaluators=evaluators,original_bytes_unchanged=True,malformed_rejected=True)
Path('analysis/em1-compatibility/keyframe-source-result.json').write_text(json.dumps(report,indent=2)+'\n')
print(f'PASS:16original keyframe files, {sequences} sequences, {evaluators} evaluators, exact byte retention and malformed inputs rejected')
