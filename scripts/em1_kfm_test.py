"""Audit original Wii/Xbox KFM dependency relocation; not native playback proof."""
import argparse
import ctypes as c
import hashlib
import json
import posixpath
import runpy
import struct
import tempfile
from pathlib import Path
from em1_rvz import RVZ

p=argparse.ArgumentParser(description=__doc__);p.add_argument('--library',type=Path,required=True);p.add_argument('--image',type=Path,required=True)
p.add_argument('--native-archive',type=Path,default=Path('D:/Modding/Builds/EM2X360/packfiles/_dynamic.pak'))
a=p.parse_args();d=c.CDLL(str(a.library.resolve()));U=c.POINTER(c.c_ubyte);S=c.POINTER(c.c_char_p)
d.kfm_inspect.argtypes=[U,c.c_size_t,c.POINTER(c.c_uint32)];d.kfm_inspect.restype=c.c_char_p
d.kfm_path.argtypes=[c.c_size_t,c.POINTER(c.c_size_t),c.POINTER(c.c_size_t),c.POINTER(c.c_int)];d.kfm_path.restype=c.c_char_p
d.kfm_bind.argtypes=[U,c.c_size_t,c.c_char_p,c.c_char_p,S,S,c.c_size_t,c.POINTER(U),c.POINTER(c.c_size_t)];d.kfm_bind.restype=c.c_char_p
d.kfm_source.argtypes=[c.c_wchar_p,c.c_char_p,c.c_char_p,c.c_char_p,S,S,c.c_size_t,c.POINTER(c.c_char_p),c.POINTER(U),c.POINTER(c.c_size_t)];d.kfm_source.restype=c.c_char_p


def inspect(data):
    raw=(c.c_ubyte*len(data)).from_buffer_copy(data);counts=(c.c_uint32*4)();error=d.kfm_inspect(raw,len(data),counts);assert not error,error
    fields=[]
    for i in range(counts[3]):
        start,size,model=c.c_size_t(),c.c_size_t(),c.c_int();name=d.kfm_path(i,c.byref(start),c.byref(size),c.byref(model))
        assert name is not None
        fields.append((start.value,size.value,name.decode(),bool(model.value)))
    return list(counts),fields


def bind(data,manager,native,bindings):
    raw=(c.c_ubyte*len(data)).from_buffer_copy(data);keys=(c.c_char_p*len(bindings))(*[s.encode() for s in bindings]);values=(c.c_char_p*len(bindings))(*[s.encode() for s in bindings.values()])
    output,size=U(),c.c_size_t(12345);error=d.kfm_bind(raw,len(data),manager.encode(),native.encode(),keys,values,len(bindings),c.byref(output),c.byref(size))
    if error:assert not output and size.value==12345;return error,None
    return None,c.string_at(output,size.value)


fixtures=[];original_resources={};r=RVZ(a.image)
try:
    files={name.lower():span for name,span in r.files(1).items()}
    for archive in ['packfiles/_dynamic.pak','packfiles/gv_shared_za.pak','packfiles/gremlinvillage_zonea.pak','packfiles/gv_zonea_airlock.pak']:
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'original.pak';path.write_bytes(r.read_partition(1,*files[archive]))
            for name,data,kind in runpy.run_path(str(Path(__file__).with_name('game-pak.py')))['read_pak'](path):
                original_resources[(archive,name.lower())]=(data,kind)
                if name.lower().endswith(('.kfm_wii','.kfm')):fixtures.append((archive,name.lower(),data))
finally:r.close()
assert len(fixtures)>=10
native_fixture=Path('analysis/em1-compatibility/original-em2-sample-oswald_xStreams_01a.kfm')
fixtures.append((None,'native/sample.kfm',native_fixture.read_bytes()))
for name,data,kind in runpy.run_path(str(Path(__file__).with_name('game-pak.py')))['read_pak'](a.native_archive):
    if name.lower().endswith('.kfm'):fixtures.append((None,'native/'+name.lower(),data))
results=[]
for archive,name,data in fixtures:
    counts,fields=inspect(data);endian='>' if data[data.index(b'\n')+1]==0 else '<'
    native='em1/runtime/camera/manager.kfm';bindings={};expected=[];dependencies=[]
    for i,(start,size,filename,model) in enumerate(fields):
        assert struct.unpack_from(endian+'I',data,start)[0]==size-4 and data[start+4:start+size]==filename.encode()
        if not filename:continue
        source=posixpath.normpath(posixpath.join(posixpath.dirname(name),filename.replace('\\','/'))).lower()
        if archive:
            owner=next(((pak,path) for pak in dict.fromkeys([archive,'packfiles/_dynamic.pak']) for path in [source,source+'_wii'] if (pak,path) in original_resources),None)
            assert owner,(name,'Missing original KFM dependency',source)
            dependencies.append(dict(authored=filename,archive=owner[0],resource=owner[1],kind=original_resources[owner][1].hex()))
        if source not in bindings:bindings[source]=f'em1/runtime/{"models" if model else "clips"}/asset_{i}.{"nif" if model else "kf"}'
        relative=posixpath.relpath(bindings[source],posixpath.dirname(native));expected.append((start,size,relative))
    error,output=bind(data,name,native,bindings);assert not error,(name,error)
    original_other=b'';previous=0;rebuilt=bytearray()
    for start,size,relative in expected:
        rebuilt+=data[previous:start]+struct.pack(endian+'I',len(relative))+relative.encode();original_other+=data[previous:start];previous=start+size
    rebuilt+=data[previous:];original_other+=data[previous:];assert output==bytes(rebuilt),name
    after_counts,after_fields=inspect(output);assert counts==after_counts
    other=b'';previous=0
    for start,size,filename,model in after_fields:
        if filename:other+=output[previous:start];previous=start+size
    other+=output[previous:];assert other==original_other,'Non-filename payload changed'
    if bindings:
        incomplete=dict(bindings);incomplete.pop(next(iter(incomplete)));assert bind(data,name,native,incomplete)[0]
        wrong=dict(bindings);first=next(iter(wrong));wrong[first]='em1/runtime/incorrect_type.bin';assert bind(data,name,native,wrong)[0]
    if archive:
        keys=(c.c_char_p*len(bindings))(*[s.encode() for s in bindings]);values=(c.c_char_p*len(bindings))(*[s.encode() for s in bindings.values()]);origin,out,size=c.c_char_p(),U(),c.c_size_t()
        error=d.kfm_source(str(a.image),archive.encode(),name.encode(),native.encode(),keys,values,len(bindings),c.byref(origin),c.byref(out),c.byref(size));assert not error,(name,error)
        assert c.string_at(out,size.value)==output and archive.encode() in origin.value and b'kfm-paths-v1' in origin.value
        changed=dict(bindings);first=next(iter(changed));changed[first]=changed[first].replace('asset_','alternate_')
        changed_values=(c.c_char_p*len(changed))(*[s.encode() for s in changed.values()]);old_origin=origin.value
        error=d.kfm_source(str(a.image),archive.encode(),name.encode(),native.encode(),keys,changed_values,len(changed),c.byref(origin),c.byref(out),c.byref(size));assert not error,error
        assert old_origin!=origin.value,'Cache identity aliases different dependencies'
    results.append(dict(archive=archive,resource=name,counts=counts,dependencies=dependencies,non_filename_sha256=hashlib.sha256(other).hexdigest()))

# Validate representative Wii/Xbox corruptions without touching outputs.
for _,name,data in (fixtures[0],fixtures[-1]):
    counts,fields=inspect(data);endian='>' if data[data.index(b'\n')+1]==0 else '<'
    invalid=[b'',data[:-1],data+b'\0',data.replace(b'20.6.5.0b',b'20.6.5.0x',1)]
    bad=bytearray(data);bad[data.index(b'\n')+1]=2;invalid.append(bytes(bad))
    bad=bytearray(data);struct.pack_into(endian+'I',bad,fields[0][0],0x7fffffff);invalid.append(bytes(bad))
    bad=bytearray(data);bad[fields[0][0]+4]=0;invalid.append(bytes(bad))
    for value in invalid:
        raw=(c.c_ubyte*len(value)).from_buffer_copy(value);out=(c.c_uint32*4)(91,92,93,94)
        assert d.kfm_inspect(raw,len(value),out) and list(out)==[91,92,93,94]
    for authored,expected_error in [('/absolute.nif_wii',b'Absolute authored'),('C:\\absolute.nif_wii',b'Invalid KFM resource'),('../../../../../../outside.nif_wii',b'escapes source root')]:
        start,size,_,_=fields[0];bad=data[:start]+struct.pack(endian+'I',len(authored))+authored.encode()+data[start+size:]
        error,_=bind(bad,name,'em1/runtime/camera/manager.kfm',{});assert error and expected_error in error,error
    for invalid_manager in ['em1/runtime/space name.kfm','em1/runtime/../outside.kfm','foreign/runtime/manager.kfm']:
        error,_=bind(data,name,invalid_manager,{});assert error and b'Invalid native KFM manager name' in error,error

report=dict(scope='Original source/native binary KFM parsing, exact path-only relocation and source ownership; no native animation playback or level activation proof',files=results,
            missing_or_wrong_type_bindings_rejected=True,cache_identity_separates_bindings=True,malformed_outputs_unchanged=True)
Path('analysis/em1-compatibility/kfm-path-bindings-result.json').write_text(json.dumps(report,indent=2)+'\n')
print(f'PASS:{len(results)} original Wii/Xbox KFM files; sequence/transition/group bytes retained, exact source ownership and dependency-sensitive identities verified')
