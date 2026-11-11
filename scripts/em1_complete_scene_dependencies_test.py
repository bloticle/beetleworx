"""Full GV scene dependency binding, including dialogue and animation files.

Bindings here are private structural fixtures, not converted asset readiness.
"""
import argparse
import collections
import copy
import ctypes as c
import json
import runpy
import tempfile
from pathlib import Path
from em1_scene_upgrade import inspect
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--library',type=Path,required=True);p.add_argument('--image',type=Path,required=True)
a=p.parse_args();d=c.CDLL(str(a.library.resolve()));U=c.POINTER(c.c_ubyte)
d.prepare.argtypes=[c.c_wchar_p,c.c_char_p,c.POINTER(U),c.POINTER(c.c_size_t)];d.prepare.restype=c.c_char_p
d.scene_files.argtypes=[c.c_size_t,c.POINTER(c.c_size_t)];d.scene_files.restype=c.c_char_p
d.scene_file.argtypes=[c.c_size_t,c.POINTER(c.c_uint32),c.POINTER(c.c_uint32),c.POINTER(c.c_char_p),c.POINTER(c.c_char_p),c.POINTER(c.c_size_t)];d.scene_file.restype=c.c_char_p
d.scene_file_storage.argtypes=[c.c_size_t];d.scene_file_storage.restype=c.c_uint32
d.scene_declared_assets.argtypes=[c.c_size_t,c.POINTER(c.c_size_t)];d.scene_declared_assets.restype=c.c_char_p
d.bind_scene.argtypes=[c.c_size_t,c.c_int,c.POINTER(U),c.POINTER(c.c_size_t)];d.bind_scene.restype=c.c_char_p
d.bind_declared_scene.argtypes=[c.c_size_t,c.c_int,c.POINTER(U),c.POINTER(c.c_size_t)];d.bind_declared_scene.restype=c.c_char_p
extensions={'.nif','.hkx','.kfm','.kf','.hkw','.hkb','.hkp','.lua','.bnk','.gsa','.lit_cooked','.bsq','.hpd'}


def extension(path):
    value=path.lower().replace('\\','/').rsplit('/',1)[-1]
    suffix='.'+value.rsplit('.',1)[-1] if '.' in value else ''
    if suffix.removesuffix('_wii') in {'.nif','.hkx','.kfm','.kf','.hkw','.hkb','.hkp'}:suffix=suffix.removesuffix('_wii')
    return suffix if suffix in extensions else None


out,size=U(),c.c_size_t();error=d.prepare(str(a.image),b'gv_zonea_start.level',c.byref(out),c.byref(size));assert not error,error
with tempfile.TemporaryDirectory() as temp:
    p=Path(temp)/'complete.pak';p.write_bytes(c.string_at(out,size.value));scenes=runpy.run_path(str(Path(__file__).with_name('game-pak.py')))['read_pak'](p)
assert len(scenes)==3;totals=collections.Counter();omissions=collections.Counter();results=[];storage_counts=collections.Counter()
for i,(name,data,kind) in enumerate(scenes):
    before=inspect(data);expected=[]
    for entity in before['entities']:
        for comp in entity[3]:
            for prop,typ,mode,amount,values in comp[3]:
                if typ=='String':
                    for value in values:
                        ext=extension(value)
                        if ext:expected.append((comp[0],prop,value,mode));totals[ext]+=1;storage_counts[mode]+=1
    count=c.c_size_t();assert not d.scene_files(i,c.byref(count));assert count.value==len(expected)
    for index,want in enumerate(expected):
        entity,component=c.c_uint32(),c.c_uint32();comp,prop=c.c_char_p(),c.c_char_p();offset=c.c_size_t()
        path=d.scene_file(index,c.byref(entity),c.byref(component),c.byref(comp),c.byref(prop),c.byref(offset))
        assert (comp.value.decode(),prop.value.decode(),path.decode())==want[:3] and d.scene_file_storage(index)==want[3]
    declared=c.c_size_t();assert not d.scene_declared_assets(i,c.byref(declared));assert declared.value==sum(row[3] in (2,3) for row in expected)
    bound,length=U(),c.c_size_t();assert not d.bind_scene(i,0,c.byref(bound),c.byref(length))
    after=inspect(c.string_at(bound,length.value));mapping={}
    for _,_,path,_ in expected:
        key=path.lower().replace('\\','/')
        if key not in mapping:mapping[key]='em1/runtime/bound_'+str(len(mapping))+extension(path)
    entities=copy.deepcopy(before['entities'])
    for entity in entities:
        for comp in entity[3]:
            for prop,typ,mode,amount,values in comp[3]:
                if typ=='String':
                    for n,value in enumerate(values):
                        key=value.lower().replace('\\','/')
                        if key in mapping:values[n]=mapping[key]
    assert after['entities']==entities and after['refs']==before['refs']
    declared_out,declared_size=U(),c.c_size_t();assert not d.bind_declared_scene(i,0,c.byref(declared_out),c.byref(declared_size))
    declared_after=inspect(c.string_at(declared_out,declared_size.value));declared_mapping={}
    for _,_,path,mode in expected:
        key=path.lower().replace('\\','/')
        if mode in (2,3) and key not in declared_mapping:declared_mapping[key]='em1/runtime/declared_'+str(len(declared_mapping))+extension(path)
    declared_entities=copy.deepcopy(before['entities'])
    for entity in declared_entities:
        for comp in entity[3]:
            for prop,typ,mode,amount,values in comp[3]:
                if typ=='String' and mode in (2,3):
                    for n,value in enumerate(values):
                        key=value.lower().replace('\\','/')
                        if key in declared_mapping:values[n]=declared_mapping[key]
    assert declared_after['entities']==declared_entities and declared_after['refs']==before['refs']
    if declared_mapping:
        address=c.cast(declared_out,c.c_void_p).value;length_before=declared_size.value
        assert d.bind_declared_scene(i,1,c.byref(declared_out),c.byref(declared_size))
        assert c.cast(declared_out,c.c_void_p).value==address and declared_size.value==length_before
    for ext,flag in [('.bsq',5),('.kfm',6),('.hkw',7),('.hpd',8)]:
        if not any(extension(path)==ext for _,_,path,_ in expected):continue
        address=c.cast(bound,c.c_void_p).value;old_size=length.value
        error=d.bind_scene(i,flag,c.byref(bound),c.byref(length));assert error and b'Unbound scene asset:' in error,(ext,error)
        assert c.cast(bound,c.c_void_p).value==address and length.value==old_size
        omissions[ext]+=1
    results.append(dict(scene=name,references=len(expected),definitions=len(before['entities'])))
assert all(totals[ext]>0 and omissions[ext]>0 for ext in ['.bsq','.kfm','.hkw','.hpd'])
result=dict(scope='Complete reference detection/transactional structural binding only; assets not declared converted or playable',
            level='gv_zonea_start.level',scenes=results,reference_counts=dict(totals),missing_new_kinds_rejected=dict(omissions),
            all_definitions_components_nonfile_values_and_footer_refs_retained=True,original_storage_modes=dict(storage_counts),declared_asset_modes=[2,3],
            declared_only_binding_preserves_all_ordinary_file_strings=True,declared_asset_fields=storage_counts[2]+storage_counts[3])
Path('analysis/em1-compatibility/complete-scene-dependencies-result.json').write_text(json.dumps(result,indent=2)+'\n')
print('PASS:3complete GV scenes; animation/dialogue/navigation dependencies required; missing BSQ/KFM/HKW/HPD rejected transactionally; authored data retained')
print(f'PASS:{storage_counts[2]+storage_counts[3]} original declared asset fields; {sum(n for mode,n in storage_counts.items() if mode not in (2,3))} ordinary filename fields retained by runtime asset binder')
