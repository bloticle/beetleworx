"""Read-only RVZ/Wii file access for EM1 compatibility development.
Format: Dolphin docs/WiaAndRvz.md. Python3.14's bundled Zstd is used;
no emulator, extractor process or new game executable is required.
"""
from pathlib import Path
from compression import zstd
import hashlib, struct
from functools import lru_cache

def u32(b,o=0):return struct.unpack_from(">I",b,o)[0]
def u64(b,o=0):return struct.unpack_from(">Q",b,o)[0]
def decode(data,limit):
    dec=zstd.ZstdDecompressor();out=dec.decompress(data,max_length=limit+1)
    if len(out)>limit or not dec.eof or dec.unused_data:raise ValueError("Invalid/oversized Zstd frame")
    return out

def padding(seed,size,offset):
    state=list(struct.unpack(">17I",seed))+[0]*504
    for n in range(17,521):state[n]=((state[n-17]<<23)^(state[n-16]>>9)^state[n-1])&0xffffffff
    def advance():
        for n in range(521):state[n]^=state[(n-32)%521]
    for _ in range(4):advance()
    skip=offset%0x8000;out=bytearray();index=0
    while len(out)<size+skip:
        if index==521:advance();index=0
        w=state[index];index+=1;out.extend((w>>24,(w>>18)&255,(w>>8)&255,w&255))
    return bytes(out[skip:skip+size])

def unpack(data,expected,offset):
    out=bytearray();pos=0
    while pos<len(data):
        if pos+4>len(data):raise ValueError("Truncated RVZ packing")
        word=u32(data,pos);pos+=4;count=word&0x7fffffff
        if count>expected-len(out):raise ValueError("RVZ packed run exceeds group")
        n=68 if word>>31 else count
        if pos+n>len(data):raise ValueError("Truncated RVZ packed run")
        out+=padding(data[pos:pos+n],count,offset+len(out)) if word>>31 else data[pos:pos+n]
        pos+=n
    if len(out)!=expected:raise ValueError("RVZ unpacked length mismatch")
    return bytes(out)

class RVZ:
    def __init__(self,path):
        self.path=Path(path);self.file=self.path.open("rb");self.size=self.path.stat().st_size
        h=self.read_at(0,0x48)
        if h[:4]!=b"RVZ\x01" or hashlib.sha1(h[:0x34]).digest()!=h[0x34:]:raise ValueError("Invalid RVZ header")
        if u32(h,8)>0x01000000 or not 220<=u32(h,12)<=1048576 or u64(h,44)!=self.size:raise ValueError("Unsupported RVZ version/length")
        h2=self.read_at(0x48,u32(h,12))
        if hashlib.sha1(h2).digest()!=h[16:36]:raise ValueError("Invalid RVZ disc header hash")
        if u32(h2)!=2 or u32(h2,4)!=5:raise ValueError("This reader supports Zstd Wii RVZ images")
        self.chunk=u32(h2,12);self.disc_id=h2[16:22].decode("ascii")
        if self.chunk<0x8000 or self.chunk>0x200000 or self.chunk&(self.chunk-1):raise ValueError("Unsupported chunk size")
        np,size=u32(h2,144),u32(h2,148)
        if not 0<np<=64 or size!=48:raise ValueError("Invalid partition table")
        parts=self.read_at(u64(h2,152),np*size)
        if hashlib.sha1(parts).digest()!=h2[160:180]:raise ValueError("Invalid partition table hash")
        self.parts=[[struct.unpack_from(">4I",parts,n*48+16+i*16) for i in range(2)] for n in range(np)]
        ng,packed=u32(h2,196),u32(h2,208)
        if not 0<ng<=1000000 or packed>16000000:raise ValueError("Invalid group table size")
        groups=decode(self.read_at(u64(h2,200),packed),ng*12)
        if len(groups)!=ng*12:raise ValueError("Group table length mismatch")
        self.groups=list(struct.iter_unpack(">III",groups))
        for part in self.parts:
            for first,sectors,index,count in part:
                if index+count>ng or count!=(sectors*0x8000+self.chunk-1)//self.chunk:raise ValueError("Invalid partition group span")
    def close(self):self.file.close()
    def read_at(self,offset,size):
        if offset<0 or size<0 or size>self.size-offset:raise ValueError("Read outside RVZ")
        self.file.seek(offset);data=self.file.read(size)
        if len(data)!=size:raise ValueError("Short RVZ read")
        return data
    @lru_cache(maxsize=8)
    def group(self,index,expected,offset):
        at,flags,packed=self.groups[index];size=flags&0x7fffffff
        if not size:return bytes(expected)
        if size>self.chunk+131072:raise ValueError("Invalid compressed group size")
        data=self.read_at(at*4,size);compressed=bool(flags>>31)
        if compressed:data=decode(data,self.chunk+131072)
        # Wii groups contain one exception list for <=2MiB chunks. Hashes
        # are irrelevant to decrypted file reads, but list bounds are checked.
        if len(data)<2:raise ValueError("Missing hash exceptions")
        end=2+struct.unpack_from(">H",data)[0]*22
        if end>len(data):raise ValueError("Truncated hash exception list")
        if not compressed:end=(end+3)&~3
        data=data[end:]
        if packed:
            if len(data)!=packed:raise ValueError("RVZ packed length mismatch")
            return unpack(data,expected,offset)
        if len(data)!=expected:raise ValueError("Decrypted group length mismatch")
        return data
    def read_partition(self,partition,offset,size):
        if offset<0 or size<0 or size>256*1024*1024:raise ValueError("Invalid partition request")
        part=self.parts[partition];origin=part[0][0];out=bytearray()
        while size:
            match=None
            for first,sectors,index,count in part:
                start=(first-origin)*0x7c00;length=sectors*0x7c00
                if start<=offset<start+length:match=(start,length,index);break
            if match is None:raise ValueError("Partition read outside segments")
            start,length,index=match;stride=self.chunk//0x8000*0x7c00
            group=(offset-start)//stride;inside=(offset-start)%stride;expected=min(stride,length-group*stride)
            data=self.group(index+group,expected,start+group*stride);n=min(size,len(data)-inside)
            out+=data[inside:inside+n];offset+=n;size-=n
        return bytes(out)
    def files(self,partition):
        header=self.read_partition(partition,0,0x440)
        at,size=u32(header,0x424)*4,u32(header,0x428)*4
        if not 12<=size<=32*1024*1024:raise ValueError("Invalid Wii FST size")
        fst=self.read_partition(partition,at,size);count=u32(fst,8)
        if not 1<=count<=len(fst)//12:raise ValueError("Invalid FST count")
        strings=count*12;stack=[(count,"")];files={}
        for n in range(1,count):
            while stack and n>=stack[-1][0]:stack.pop()
            if not stack:raise ValueError("Invalid FST directory range")
            kind=u32(fst,n*12);nameoff=strings+(kind&0xffffff)
            end=fst.find(b"\0",nameoff)
            if nameoff>=len(fst) or end<0:raise ValueError("Invalid FST name")
            name=fst[nameoff:end].decode("utf-8")
            if not name or name in (".","..") or any(c in name for c in "/\\:"):raise ValueError("Unsafe FST name")
            full=stack[-1][1]+name
            if kind>>24:
                stop=u32(fst,n*12+8)
                if kind>>24!=1 or not n<stop<=stack[-1][0]:raise ValueError("Invalid FST child range")
                stack.append((stop,full+"/"))
            else:
                if full in files:raise ValueError("Duplicate FST path")
                files[full]=(u32(fst,n*12+4)*4,u32(fst,n*12+8))
        return files

if __name__=="__main__":
    import argparse,json
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image",type=Path)
    parser.add_argument("--partition",type=int,default=1)
    parser.add_argument("--file",help="Exact FST-relative file path")
    parser.add_argument("--output",type=Path)
    args=parser.parse_args()
    if bool(args.file)!=bool(args.output):parser.error("--file and --output must be provided together")
    rvz=RVZ(args.image)
    try:
        if not 0<=args.partition<len(rvz.parts):parser.error("Partition out of range")
        files=rvz.files(args.partition)
        if args.file:
            if args.file not in files:parser.error("File is not in the selected partition")
            data=rvz.read_partition(args.partition,*files[args.file])
            with args.output.open("xb") as output:output.write(data)
            print(json.dumps(dict(file=args.file,bytes=len(data),sha256=hashlib.sha256(data).hexdigest())))
        else:
            print(json.dumps(dict(disc_id=rvz.disc_id,partition=args.partition,files=files),indent=2))
    finally:rvz.close()
