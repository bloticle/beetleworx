"""Independent original NIF physics-buffer versus native HKX geometry checks."""
import argparse,ctypes as c,struct,runpy,tempfile,json
from pathlib import Path
from em1_rvz import RVZ
from em1_nif import NIF
p=argparse.ArgumentParser();p.add_argument('--library',required=True,type=Path);p.add_argument('--image',required=True,type=Path);args=p.parse_args()
d=c.CDLL(str(args.library.resolve()));U=c.POINTER(c.c_ubyte)
d.jigsaw_bind_model.argtypes=[U,c.c_size_t,c.POINTER(U),c.POINTER(c.c_size_t),c.POINTER(c.c_uint32)];d.jigsaw_bind_model.restype=c.c_char_p
def native(blob):
    raw=(c.c_ubyte*len(blob)).from_buffer_copy(blob);out=U();size=c.c_size_t();count=c.c_uint32();error=d.jigsaw_bind_model(raw,len(raw),c.byref(out),c.byref(size),c.byref(count))
    return error,c.string_at(out,size.value) if not error else None,count.value
class Reader:
    def __init__(self,b):self.b=b;self.at=0
    def word(self):v=struct.unpack_from('>I',self.b,self.at)[0];self.at+=4;return v
    def short(self):v=struct.unpack_from('>H',self.b,self.at)[0];self.at+=2;return v
    def array(self):return [self.word() for _ in range(self.word())]
    def take(self,n):v=self.b[self.at:self.at+n];assert len(v)==n;self.at+=n;return v
class Pack:
    def __init__(self,b):
        self.b=b;self.sections=[struct.unpack_from('>7I',b,84+i*48) for i in range(3)];self.ptr={};self.start=self.sections[2][0]
        for st,lo,gl,vi,ex,im,end in self.sections:
            for at in range(st+lo,st+gl-7,8):
                source,target=struct.unpack_from('>2I',b,at)
                if source!=0xffffffff:self.ptr[st+source]=st+target
            for at in range(st+gl,st+vi-11,12):
                source,section,target=struct.unpack_from('>3I',b,at)
                if source!=0xffffffff:self.ptr[st+source]=self.sections[section][0]+target
    def word(self,p):return struct.unpack_from('>I',self.b,p)[0]
    def short(self,p):return struct.unpack_from('>H',self.b,p)[0]
    def code(self):
        obj=self.ptr[self.start+0x14];start=self.ptr[obj+0x20];return self.b[start:start+self.word(obj+0x24)]
def embedded(block):
    at=block.index(bytes.fromhex('57e0e05710c0c010'));size=struct.unpack_from('>I',block,at-4)[0];return at,block[at:at+size],block[at+size:]
def buffers(model,link,region):
    r=Reader(model.blocks[link]);size=r.word();r.word();regions=[(r.word(),r.word()) for _ in range(r.word())];assert r.word()==1;format=r.word();raw=r.take(size);r.take(1);assert r.at==len(r.b)
    width={0x30437:12,0x10105:1,0x10215:2,0x10425:4}[format];start,count=regions[region];return raw[start*width:(start+count)*width],count,format
rvz=RVZ(args.image)
try:
    files={n.lower():v for n,v in rvz.files(1).items()}
    with tempfile.TemporaryDirectory() as temp:
        path=Path(temp)/'source.pak';path.write_bytes(rvz.read_partition(1,*files['packfiles/gremlinvillage_zonea.pak']));entries={n.lower():b for n,b,k in runpy.run_path(str(Path(__file__).with_name('game-pak.py')))['read_pak'](path)}
        path.write_bytes(rvz.read_partition(1,*files['packfiles/dbc_1st_zoneb.pak']));convex_entries={n.lower():b for n,b,k in runpy.run_path(str(Path(__file__).with_name('game-pak.py')))['read_pak'](path)}
finally:rvz.close()
results=[]
for part,expected_parts in [('toon01',26),('toon02',34)]:
    raw=entries['environments/thepark/fantasyland/smallworld/zones/gv_zonea_'+part+'.nif_wii'];before=NIF.read(raw)
    block=next(i for i,k in enumerate(before.indices) if before.types[k]=='JPSJigsawNode');at,old_hkx,suffix=embedded(before.blocks[block]);r=Reader(suffix)
    vertex_links,index_links=r.array(),r.array();region_at=r.at+4;regions=[r.short() for _ in range(r.word())];reserved_at=r.at;assert r.word()==r.word()==0
    triangle_vertex_at=r.at+4
    triangle_vertices,triangle_indices,triangle_pieces,convex_pieces=(r.array() for _ in range(4));assert r.at==len(suffix)
    error,output,count=native(raw);assert not error,error;assert count==1
    after=NIF.read(output);assert before.footer==after.footer and before.types==after.types and before.indices==after.indices
    assert all(b==after.blocks[i] for i,b in enumerate(before.blocks) if i!=block)
    new_at,new_hkx,new_suffix=embedded(after.blocks[block]);assert at==new_at and suffix==new_suffix and before.blocks[block][:at-4]==after.blocks[block][:at-4]
    old,new=Pack(old_hkx),Pack(new_hkx);assert new.word(12)==9 and new_hkx[18]==0 and old.code()==new.code()
    old_mesh=old.ptr[old.start+0x34];new_mesh=new.ptr[new.start+0x34];n=old.word(old_mesh+0xb8);assert n==expected_parts==new.word(new_mesh+0xbc)==len(triangle_vertices)
    assert old.word(old_mesh+0xb0)==new.word(new_mesh+0xb4)
    old_parts,new_parts=old.ptr[old_mesh+0xb4],new.ptr[new_mesh+0xb8];triangles=0;shared_vertices=set()
    for i in range(n):
        a,b=old_parts+i*0x60,new_parts+i*0x70;vb,vertices,vformat=buffers(before,vertex_links[triangle_vertices[i]],regions[triangle_vertices[i]]);ib,_,iformat=buffers(before,index_links[triangle_indices[i]],0)
        count=old.word(a+0x14);stride=old.word(a+0x44);kind=old_hkx[a+0x48];width={1:1,2:2,3:4}[kind]
        assert new.word(b+0x14)==count and old.word(a+0x20)==vertices==new.word(b+0x1c)
        assert new.short(b+0x24)==12 and new.short(b+0x2c)==stride and new_hkx[b+0x2e:b+0x30]==old_hkx[a+0x48:a+0x4a]
        vp,ip=new.ptr[b+0x18],new.ptr[b+0x20];shared_vertices.add(vp)
        assert new_hkx[vp:vp+len(vb)]==vb and new_hkx[ip:ip+count*stride]==ib[:count*stride]
        for j in range(count):
            indices=struct.unpack_from('>'+{1:'3B',2:'3H',4:'3I'}[width],ib,j*stride)
            assert all(index<vertices for index in indices)
            for index in indices:assert struct.unpack_from('>3f',new_hkx,vp+index*12)==struct.unpack_from('>3f',vb,index*12)
        triangles+=count
    assert len(shared_vertices)<=len(vertex_links)
    # Corrupt a real suffix stream link; original file on disk remains untouched.
    rejected=0
    for offset,fmt,value in [(4,'I',0xffffffff),(region_at,'H',0xffff),(reserved_at,'I',1),(triangle_vertex_at,'I',0xffffffff)]:
        malformed=NIF.read(raw);bad=bytearray(malformed.blocks[block]);struct.pack_into('>'+fmt,bad,at+len(old_hkx)+offset,value);malformed.blocks[block]=bytes(bad);assert native(malformed.write())[0];rejected+=1
    # Data-stream format and finite position checks use actual referenced blocks.
    vertex_link=vertex_links[0];stream_reader=Reader(before.blocks[vertex_link]);stream_reader.word();stream_reader.word();stream_reader.take(stream_reader.word()*8);assert stream_reader.word()==1
    format_at=stream_reader.at;stream_reader.word();position_at=stream_reader.at
    for offset,value in [(format_at,0xffffffff),(position_at,0x7fc00000)]:
        malformed=NIF.read(raw);bad=bytearray(malformed.blocks[vertex_link]);struct.pack_into('>I',bad,offset,value);malformed.blocks[vertex_link]=bytes(bad);assert native(malformed.write())[0];rejected+=1
    results.append(dict(model=part,triangle_parts=n,triangles=triangles,unique_native_vertex_buffers=len(shared_vertices),source_vertex_bindings=len(vertex_links),malformed_bindings_rejected=rejected,mopp_bytes_unchanged=True,nif_prefix_suffix_other_blocks_unchanged=True))
convex_results=[]
for part,expected in [('toon01',2),('toon04',4)]:
    raw=convex_entries['environments/thepark/darkbeautycastle/zones/dbc_zonee_'+part+'.nif_wii'];before=NIF.read(raw);block=next(i for i,k in enumerate(before.indices) if before.types[k]=='JPSJigsawNode')
    at,old_hkx,suffix=embedded(before.blocks[block]);error,output,count=native(raw);assert not error,error
    after=NIF.read(output);_,new_hkx,new_suffix=embedded(after.blocks[block]);assert suffix==new_suffix
    old,new=Pack(old_hkx),Pack(new_hkx);a,b=old.ptr[old.start+0x34],new.ptr[new.start+0x34]
    assert old.word(a+0xb8)==new.word(b+0xbc)==0
    assert old.word(a+0xc4)==new.word(b+0xc8)==expected and old.code()==new.code()
    source_parts,target_parts=old.ptr[a+0xc0],new.ptr[b+0xc4];shapes=0
    for i in range(expected):
        s,t=source_parts+i*0x40,target_parts+i*0x40;n=old.word(s+0x18);assert n==new.word(t+0x18)
        assert old_hkx[s+0x20:s+0x40]==new_hkx[t+0x20:t+0x40]
        sc,tc=old.ptr[s+0x14],new.ptr[t+0x14]
        for j in range(n):
            source,target=old.ptr[sc+j*4],new.ptr[tc+j*4]
            assert old.word(source+0x44)==new.word(target+0x44) and old.word(source+0x4c)==new.word(target+0x4c)
            for pointer,length in [(0x40,old.word(source+0x44)*48),(0x54,old.word(source+0x58)*16)]:
                x,y=old.ptr[source+pointer],new.ptr[target+pointer];assert old_hkx[x:x+length]==new_hkx[y:y+length]
            shapes+=1
    convex_results.append(dict(model='dbc_zonee_'+part,convex_parts=expected,convex_shapes=shapes,triangles=0,transforms_vertices_planes_and_mopp_unchanged=True))
Path('analysis/em1-compatibility/jigsaw-mesh-bindings-result.json').write_text(json.dumps(dict(models=results,convex_only=convex_results,scope='Actual original NIF physics streams versus converted native HKX geometry and unchanged MOPP, including convex-only collections. No native scene/component/world admission or live ray proof.'),indent=2)+'\n')
print('PASS: actual GV60triangle parts bind original physics streams; every indexed vertex and MOPP byte match; no source blocks discarded')
