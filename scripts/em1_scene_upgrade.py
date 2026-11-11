"""Lossless structural EM1 scene -> EM2 retail scene adapter.

This preserves property bytes/IDs, not engine component semantics. Converted
scenes still require platform assets and runtime component compatibility.
Format independently checked against local epic-mickey-lib-rs scene_file.rs.
"""
import argparse, json, struct
from pathlib import Path

SIZES={"Boolean":4,"Integer":4,"Unsigned Integer":4,"Short":4,
       "Unsigned Short":4,"Float":4,"String":4,"Point2":8,"Point3":12,
       "Matrix3":36,"Color (RGB)":12,"Color (RGBA)":16,"Entity Pointer":4}

def inspect(data):
    def u32(at):
        if at<0 or at+4>len(data): raise ValueError("Truncated scene")
        return struct.unpack_from(">I",data,at)[0]
    modern=u32(0)==0x01000001
    start=8 if modern else 4
    end=u32(4 if modern else 0)+(4 if modern else 0)
    if not start<=end<=len(data)-8: raise ValueError("Invalid string table")
    def string(offset):
        at=offset+(4 if modern else 0)
        if not start<=at<end-2: raise ValueError("String pointer outside table")
        stop=data.find(b"\0",at+2,end)
        if stop<0: raise ValueError("Unterminated JPS string")
        return data[at+2:stop].decode("utf-8")
    pos=end
    if modern:
        if u32(pos)!=0x02000002 or u32(pos+4)!=0: raise ValueError("Expected retail scene without extra strings")
        pos+=8; uuid=None
    else:
        if pos+16>len(data): raise ValueError("Truncated scene UUID")
        uuid=data[pos:pos+16].hex();pos+=16
    entities,refs=u32(pos),u32(pos+4);pos+=8
    if entities>len(data)//20 or refs>len(data)//4: raise ValueError("Invalid scene counts")
    rows=[];spans=[]
    for _ in range(entities):
        entity_start=pos;name=string(u32(pos));ids=data[pos+4:pos+16].hex();pos+=16
        unknown=u32(pos) if modern else 0
        if modern:pos+=4
        count=u32(pos);pos+=4;components=[]
        if count>len(data)//20:raise ValueError("Invalid component count")
        for _ in range(count):
            klass=string(u32(pos));template=string(u32(pos+4));links=data[pos+8:pos+16].hex();countp=u32(pos+16);pos+=20;props=[]
            if countp>len(data)//16:raise ValueError("Invalid property count")
            for _ in range(countp):
                pname=string(u32(pos));typ=string(u32(pos+4));mode=u32(pos+8);amount=u32(pos+12);pos+=16
                if mode not in range(6) or typ not in SIZES:raise ValueError("Unsupported property: "+typ)
                n=amount if mode in (1,3,5) else 1;size=n*SIZES[typ]
                if size>len(data)-pos:raise ValueError("Property payload exceeds scene")
                raw=data[pos:pos+size];pos+=size
                value=[string(struct.unpack_from(">I",raw,i)[0]) for i in range(0,size,4)] if typ=="String" else raw.hex()
                props.append((pname,typ,mode,amount,value))
            components.append((klass,template,links,props))
        spans.append((entity_start,pos));rows.append((name,ids,unknown,components))
    if pos+refs*4!=len(data):raise ValueError("Trailing or truncated scene references")
    return dict(modern=modern,uuid=uuid,string_end=end,entities=rows,spans=spans,refs=data[pos:].hex())

def upgrade(data):
    before=inspect(data)
    if before["modern"]:raise ValueError("Already an EM2 scene")
    end=before["string_end"]
    output=bytearray(struct.pack(">II",0x01000001,end)+data[4:end])
    output+=struct.pack(">IIII",0x02000002,0,len(before["entities"]),len(bytes.fromhex(before["refs"]))//4)
    for begin,stop in before["spans"]:
        output+=data[begin:begin+16]+bytes(4)+data[begin+16:stop]
    output+=bytes.fromhex(before["refs"])
    after=inspect(output)
    if before["entities"]!=after["entities"] or before["refs"]!=after["refs"]:raise ValueError("Semantic scene roundtrip failed")
    return bytes(output),dict(source_uuid=before["uuid"],entities=len(before["entities"]),components=sorted({c[0] for e in before["entities"] for c in e[3]}),scope="Structural conversion only; engine/component/platform compatibility unverified")

if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("source",type=Path);p.add_argument("destination",type=Path);a=p.parse_args()
    converted,report=upgrade(a.source.read_bytes())
    with a.destination.open("xb") as f:f.write(converted)
    print(json.dumps(report,indent=2))
