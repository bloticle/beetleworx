"""Check complete actual GV architecture color paths without claiming PC readiness."""
import argparse,ctypes as c,json,runpy,tempfile
from pathlib import Path
from em1_rvz import RVZ
from em1_nif import NIF
p=argparse.ArgumentParser();p.add_argument('--library',required=True,type=Path);p.add_argument('--image',required=True,type=Path);args=p.parse_args()
d=c.CDLL(str(args.library.resolve()));U=c.POINTER(c.c_ubyte)
d.baked_source.argtypes=[U,c.c_size_t,U,c.c_size_t,c.c_uint32,c.POINTER(U),c.POINTER(c.c_size_t),c.POINTER(c.c_uint32)];d.baked_source.restype=c.c_char_p
r=RVZ(args.image)
try:
    files={n.lower():v for n,v in r.files(1).items()}
    with tempfile.TemporaryDirectory() as temp:
        path=Path(temp)/'source.pak';path.write_bytes(r.read_partition(1,*files['packfiles/gremlinvillage_zonea.pak']));entries={n.lower():b for n,b,k in runpy.run_path(str(Path(__file__).with_name('game-pak.py')))['read_pak'](path)}
finally:r.close()
prefix='environments/thepark/fantasyland/smallworld/';light=entries[prefix+'gsa/gremlinvillage_zonea.lit_cooked'];light_array=(c.c_ubyte*len(light)).from_buffer_copy(light)
audit=runpy.run_path(str(Path(__file__).with_name('audit-em1-baked-lighting.py')))['audit'];results=[]
with tempfile.TemporaryDirectory() as temp:
    temp=Path(temp);lighting=temp/'scene.lit_cooked';lighting.write_bytes(light)
    for part,entity,expected in [('inert01',0x20439,60),('toon01',0x203fd,30),('toon02',0x203ff,63)]:
        raw=entries[prefix+'zones/gv_zonea_'+part+'.nif_wii'];array=(c.c_ubyte*len(raw)).from_buffer_copy(raw);out=U();size=c.c_size_t();count=c.c_uint32()
        error=d.baked_source(array,len(raw),light_array,len(light),entity,c.byref(out),c.byref(size),c.byref(count));assert not error,error;assert count.value==expected
        output=c.string_at(out,size.value);path=temp/(part+'.nif');path.write_bytes(output);result=audit(lighting,path,entity)
        assert result['matched_meshes']==expected and result['changed_vertices']==0
        before,after=NIF.read(raw),NIF.read(output)
        assert before.footer==after.footer
        edited={x['block'] for x in result['meshes']}
        for i,type_index in enumerate(before.indices):
            if i not in edited:assert before.blocks[i]==after.blocks[i],'Unlisted source mesh, original color stream or Jigsaw payload changed'
        results.append(dict(model=part,entity=entity,meshes=expected,vertices=sum(x['vertices'] for x in result['meshes']),jigsaw_payload_preserved=True))
result=dict(models=results,scope='Raw original-format baked-color stage only. All original paths/colors verified; Jigsaw payload unchanged and still requires mesh-buffer binding/conversion before native publication. No world/level activation.')
Path('analysis/em1-compatibility/gv-baked-lighting-result.json').write_text(json.dumps(result,indent=2)+'\n')
print('PASS: all153authored GV baked mesh paths/colors match; unlisted meshes and source Jigsaw/physics payloads preserved')
