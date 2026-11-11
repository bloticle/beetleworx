"""Bounded opaque-block NIF20.6.5 reader for EM1 platform conversion.
Preserves every block and string byte. Renderer/mesh payloads are not inferred.
"""
import struct
from dataclasses import dataclass

@dataclass
class NIF:
    prefix:bytes
    endian:str
    types:list
    indices:list
    strings:bytes
    blocks:list
    footer:bytes
    @classmethod
    def read(cls,data):
        newline=data.find(b"\n",0,128)
        if newline<0 or not data.startswith(b"Gamebryo File Format"):raise ValueError("Not a NIF")
        pos=newline+1
        if pos+13>len(data):raise ValueError("Truncated NIF header")
        version,flag,user,count=struct.unpack_from("<IBII",data,pos);pos+=13
        if version!=0x14060500 or flag not in (0,1):raise ValueError("Unsupported NIF version")
        if count>100000:raise ValueError("Too many NIF blocks")
        prefix=data[:pos];e="<" if flag else ">"
        def take(n):
            nonlocal pos
            if n<0 or n>len(data)-pos:raise ValueError("Truncated NIF field")
            value=data[pos:pos+n];pos+=n;return value
        def integer(fmt="I"):return struct.unpack(e+fmt,take(struct.calcsize(fmt)))[0]
        types=[take(integer()).decode("utf-8") for _ in range(integer("H"))]
        indices=[integer("H") for _ in range(count)]
        if any(i>=len(types) for i in indices):raise ValueError("Invalid NIF type index")
        sizes=[integer() for _ in range(count)];start=pos
        strings=integer();maxlen=integer()
        if strings>100000:raise ValueError("Too many NIF strings")
        for _ in range(strings):
            size=integer()
            if size>maxlen:raise ValueError("Invalid NIF string length")
            take(size)
        if integer()!=0:raise ValueError("Object groups not supported yet")
        blob=data[start:pos];blocks=[take(n) for n in sizes];start=pos
        roots=integer()
        if roots>count:raise ValueError("Invalid NIF root count")
        for _ in range(roots):
            index=integer()
            if index!=0xffffffff and index>=count:raise ValueError("Invalid NIF root")
        if pos!=len(data):raise ValueError("Unexpected trailing NIF bytes")
        return cls(prefix,e,types,indices,blob,blocks,data[start:])
    def write(self):
        def number(n,fmt="I"):return struct.pack(self.endian+fmt,n)
        out=bytearray(self.prefix);out+=number(len(self.types),"H")
        for name in self.types:
            raw=name.encode("utf-8");out+=number(len(raw))+raw
        for index in self.indices:out+=number(index,"H")
        for block in self.blocks:out+=number(len(block))
        out+=self.strings
        for block in self.blocks:out+=block
        return bytes(out)+self.footer
