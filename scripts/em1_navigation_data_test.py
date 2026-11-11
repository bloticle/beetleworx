"""Validate original navigation data/closure, not pathfinding or AI gameplay."""
import argparse
import ctypes as c
import hashlib
import json
import struct
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--library',type=Path,required=True)
p.add_argument('--image',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();before=a.image.stat();d=c.CDLL(str(a.library.resolve()));U=c.POINTER(c.c_ubyte);S=c.c_size_t;I=c.c_uint32
def api(name,args,result):
    f=getattr(d,name);f.argtypes=args;f.restype=result;return f
open_source=api('navigation_open',[c.c_wchar_p,c.POINTER(S)],c.c_char_p)
path_at=api('navigation_path',[S],c.c_char_p)
read=api('navigation_read',[c.c_char_p,c.POINTER(U),c.POINTER(S)],c.c_char_p)
sector=api('navigation_sector',[U,S,c.POINTER(I),c.POINTER(I),c.POINTER(S)],c.c_char_p)
mesh_inspect=api('navigation_mesh_inspect',[U,S,c.POINTER(I)],c.c_char_p)
floor_portal=api('navigation_floor_portal',[S,S,c.POINTER(c.c_float),c.POINTER(I)],c.c_int)
neighbor_at=api('navigation_neighbor',[S,c.POINTER(I),c.POINTER(c.POINTER(I)),c.POINTER(S)],c.c_int)
scan=api('navigation_scan',[c.POINTER(S)],c.c_char_p)
database_path=api('navigation_database_path',[S,c.POINTER(c.c_char_p)],c.c_char_p)
load=api('navigation_load',[c.c_char_p,c.c_char_p,c.POINTER(S),c.POINTER(S),c.POINTER(S)],c.c_char_p)
hpd=api('navigation_loaded_hpd',[c.POINTER(S)],U)
companion=api('navigation_loaded_sector_path',[S],c.c_char_p)
count=S();assert not (error:=open_source(str(a.image),c.byref(count))),error
paths=[path_at(i) for i in range(count.value)];sectors=[];sample=None;mesh_totals=[0]*9;geometry_cases={}
for path in paths:
    pointer,length=U(),S();assert not (error:=read(path,c.byref(pointer),c.byref(length))),error
    original=c.string_at(pointer,length.value);nodes,edges,mesh=I(),I(),S()
    assert not (error:=sector(pointer,length.value,c.byref(nodes),c.byref(edges),c.byref(mesh))),(path,error)
    expected=struct.unpack_from('<II',original,16)
    assert (nodes.value,edges.value)==expected and mesh.value==24+expected[0]*36+expected[1]*12
    assert c.string_at(pointer,length.value)==original
    # Independent original field inventory and exact endpoint/target comparison.
    m=mesh.value;word=lambda at:struct.unpack_from('<I',original,m+at)[0]
    width,height=word(56),word(60);table=word(68);floors=[]
    for cell in range(width*height):
        begin=word(table+cell*4);n=word(begin)
        for layer in range(n):floors.append((word(begin+4+layer*12),word(begin+16+layer*12)))
    by_offset={at:index for index,(at,end) in enumerate(floors)}
    counts=[len(floors),0,0,0,0,0,0,0,width,height,0]
    decoded=(I*11)();assert not (error:=mesh_inspect(pointer,length.value,decoded)),(path,error)
    for index,(at,end) in enumerate(floors):
        for field,column in [(64,1),(40,2),(48,3),(56,4),(72,5)]:counts[column]+=word(at+field)
        for link in range(word(at+40)):
            pos=at+word(at+44)+link*32;points=(c.c_float*6)();target=I()
            assert floor_portal(index,link,points,c.byref(target))
            assert tuple(points)==struct.unpack_from('<6f',original,m+pos)
            assert target.value==by_offset[word(pos+24)]
            geometry_cases.setdefault('portal_target',(original,m+pos+24))
            geometry_cases.setdefault('portal_size',(original,m+pos+28))
        if word(at+64):geometry_cases.setdefault('boundary_nan',(original,m+at+word(at+word(at+68))))
        if word(at+72):
            records=at+word(at+76);point_end=at+word(records+16)
            geometry_cases.setdefault('open_patch',(original,m+point_end-12))
        geometry_cases.setdefault('floor_identity',(original,m+at+4))
    neighbor_index=0
    for i in range(word(72)):
        pos=word(76)+i*12;target_sector,n,groups=(word(pos+x) for x in [0,4,8])
        for j in range(n):
            group=word(groups+j*4);x,z,layer,refs=(word(group+k*4) for k in range(4))
            header=(I*4)();links=c.POINTER(I)();link_count=S();assert neighbor_at(neighbor_index,header,c.byref(links),c.byref(link_count))
            assert tuple(header)==(target_sector,x,z,layer) and link_count.value==refs
            assert list(links[:refs])==[word(group+16+k*4) for k in range(refs)]
            if refs:geometry_cases.setdefault('neighbor_nonlocal',(original,m+group+16))
            neighbor_index+=1;counts[7]+=refs
    counts[6]=neighbor_index;counts[10]=counts[3]-counts[7]
    assert list(decoded)==counts,(path,list(decoded),counts)
    for column in range(8):mesh_totals[column]+=counts[column]
    mesh_totals[8]+=counts[10]
    geometry=dict(zip(['floors','segments','portals','unbound_links','polylines','patches','neighbor_groups','neighbor_references','unassigned_links'],counts[:8]+[counts[10]]))
    sectors.append(dict(path=path.decode(),nodes=nodes.value,edges=edges.value,bytes=length.value,sha256=hashlib.sha256(original).hexdigest(),runtime_geometry=geometry,runtime_mesh_sha256=hashlib.sha256(original[mesh.value:]).hexdigest()))
    if sample is None and nodes.value>1 and edges.value:sample=original
assert len(sectors)==100 and sum(x['nodes'] for x in sectors)==24212 and sum(x['edges'] for x in sectors)==102106
assert mesh_totals==[2757,55235,5633,752,2975,441,196,726,26],mesh_totals
assert sample is not None
bad=[]
for kind in ['truncated','nan_position','edge_outside_graph','mesh_length']:
    b=bytearray(sample);n,e=struct.unpack_from('<II',b,16)
    if kind=='truncated':b=b[:-1]
    elif kind=='nan_position':struct.pack_into('<I',b,28,0x7FC00000)
    elif kind=='edge_outside_graph':struct.pack_into('<I',b,24+n*36+8,n)
    else:struct.pack_into('<I',b,len(b)-8,0)
    data=(c.c_ubyte*len(b)).from_buffer_copy(b);x,y,z=I(123),I(456),S(789)
    error=sector(data,len(b),c.byref(x),c.byref(y),c.byref(z));assert error and (x.value,y.value,z.value)==(123,456,789)
    assert bytes(data)==b;bad.append(dict(kind=kind,error=error.decode()))
geometry_bad=[]
assert len(geometry_cases)==6,geometry_cases.keys()
for kind,(original,at) in geometry_cases.items():
    b=bytearray(original)
    if kind=='boundary_nan':value=0x7FC00000
    elif kind=='open_patch':value=struct.unpack_from('<I',b,at)[0]^0x1000
    elif kind=='portal_size':value=struct.unpack_from('<I',b,at)[0]+1
    else:value=0xFFFFFFFF
    struct.pack_into('<I',b,at,value);data=(c.c_ubyte*len(b)).from_buffer_copy(b);counts=(I*11)(*([123]*11))
    error=mesh_inspect(data,len(b),counts);assert error and list(counts)==[123]*11,(kind,error,list(counts))
    assert bytes(data)==b;geometry_bad.append(dict(kind=kind,error=error.decode()))
assert not (error:=scan(c.byref(count))),error
databases=[];unique=set();companions=set()
for i in range(count.value):
    archive=c.c_char_p();path=database_path(i,c.byref(archive));n,v,e=S(),S(),S()
    assert not (error:=load(archive,path,c.byref(n),c.byref(v),c.byref(e))),(archive.value,path,error)
    length=S();pointer=hpd(c.byref(length));raw=c.string_at(pointer,length.value)
    assert raw[:12]==b'Kynogon HPD\0' and struct.unpack_from('<II',raw,12)==(2,2)
    assert struct.unpack_from('<I',raw,20)[0]==n.value
    digest=hashlib.sha256(raw).hexdigest();unique.add((path.lower(),digest))
    for index in range(n.value):companions.add(companion(index).lower())
    databases.append(dict(archive=archive.value.decode(),path=path.decode(),sectors=n.value,nodes=v.value,edges=e.value,sha256=digest))
after=a.image.stat();assert (before.st_size,before.st_mtime_ns)==(after.st_size,after.st_mtime_ns)
result=dict(scope=__doc__,library_sha256=hashlib.sha256(a.library.read_bytes()).hexdigest(),
    source_identity=dict(path=str(a.image),bytes=before.st_size,mtime_ns=before.st_mtime_ns),
    loose_sectors=sectors,packed_databases=databases,unique_database_variants=len(unique),referenced_companion_files=len(companions),
    rejected_invalid_inputs=bad,input_bytes_preserved=True,native_pathfinding_proven=False,world_activated=False)
result['source_runtime_geometry']=dict(zip(['floors','segments','portals','unbound_links','polylines','patches','neighbor_groups','neighbor_references','unassigned_links'],mesh_totals))
result['source_runtime_geometry'].update(exact_portal_endpoints_and_targets_checked=True,exact_neighbor_references_checked=True,
    all_hpd_neighbor_floor_targets_resolved=True,unassigned_links_connected=False,walkable_surfaces_generated=False,
    rejected_invalid_inputs=geometry_bad)
a.output.write_text(json.dumps(result,indent=2)+'\n')
print(f'PASS:100sectors/2757floors/5633portals/726neighbor refs;{len(databases)}packed HPDs/{len(unique)}unique variants;{len(companions)}companions;4graph+6geometry bad inputs rejected; no world/AI claim')
