"""Convert cooked Wii textures into portable NiPixelData RGBA8 blocks.

Independent format implementation; Wii CMPR interpolation/alpha behavior is
checked against Dolphin's TextureDecoder_Generic.cpp and TextureDecoder_Util.h.
The containing mesh/controller payloads remain unconverted and unverified.
"""
import struct
from em1_nif import NIF

def expand(value,bits):
    if bits==3:return (value<<5)|(value<<2)|(value>>1)
    if bits==4:return value*17
    if bits==5:return (value<<3)|(value>>2)
    if bits==6:return (value<<2)|(value>>4)
    raise ValueError("Unsupported channel precision")

def rgb565(word):return [expand(word>>11,5),expand((word>>5)&63,6),expand(word&31,5),255]

def decode_gx(data,width,height,mode,palette=None):
    tiles={"I4":(8,8,32),"I8":(8,4,32),"IA4":(8,4,32),"IA8":(4,4,32),
        "RGB565":(4,4,32),"RGB5A3":(4,4,32),"RGBA8":(4,4,64),"CMPR":(8,8,32),"C8":(8,4,32)}
    if not 1<=width<=8192 or not 1<=height<=8192 or mode not in tiles:raise ValueError("Unsupported texture dimensions/format")
    tw,th,ts=tiles[mode];cols=(width+tw-1)//tw;rows=(height+th-1)//th;size=cols*rows*ts
    if len(data)<size:raise ValueError("Truncated Wii texture mip")
    output=bytearray(width*height*4)
    for ty in range(rows):
        for tx in range(cols):
            block=data[(ty*cols+tx)*ts:(ty*cols+tx+1)*ts]
            colors=[]
            if mode=="CMPR":
                for at in range(0,32,8):
                    first,second=struct.unpack_from(">HH",block,at);a,b=rgb565(first),rgb565(second)
                    if first>second:
                        c=[(5*a[i]+3*b[i])//8 for i in range(3)]+[255]
                        d=[(3*a[i]+5*b[i])//8 for i in range(3)]+[255]
                    else:
                        mid=[a[i]+int((b[i]-a[i])/2) for i in range(3)]
                        c=mid+[255];d=mid+[0]
                    colors.append([a,b,c,d])
            for y in range(min(th,height-ty*th)):
                for x in range(min(tw,width-tx*tw)):
                    index=y*tw+x
                    if mode=="I4":v=expand((block[index//2]>>(4 if index%2==0 else 0))&15,4);rgba=[v]*4
                    elif mode=="I8":rgba=[block[index]]*4
                    elif mode=="IA4":v=expand(block[index]&15,4);rgba=[v,v,v,expand(block[index]>>4,4)]
                    elif mode=="IA8":v=block[index*2+1];rgba=[v,v,v,block[index*2]]
                    elif mode=="RGB565":rgba=rgb565(int.from_bytes(block[index*2:index*2+2],"big"))
                    elif mode=="RGB5A3":
                        word=int.from_bytes(block[index*2:index*2+2],"big")
                        if word&0x8000:rgba=[expand((word>>10)&31,5),expand((word>>5)&31,5),expand(word&31,5),255]
                        else:rgba=[expand((word>>8)&15,4),expand((word>>4)&15,4),expand(word&15,4),expand((word>>12)&7,3)]
                    elif mode=="RGBA8":rgba=[block[index*2+1],block[32+index*2],block[33+index*2],block[index*2]]
                    elif mode=="C8":
                        if palette is None or block[index]>=len(palette):raise ValueError("Invalid texture palette")
                        rgba=palette[block[index]]
                    else:
                        sub=(y//4)*2+x//4;selection=(block[sub*8+4+y%4]>>(6-2*(x%4)))&3
                        rgba=colors[sub][selection]
                    at=((ty*th+y)*width+tx*tw+x)*4;output[at:at+4]=bytes(rgba)
    return bytes(output)

def convert(data):
    nif=NIF.read(data);converted=[];pixel_type=None
    modes={(11,4):"I4",(11,8):"I8",(12,8):"IA4",(12,16):"IA8",(0,16):"RGB565",(1,16):"RGB5A3",(1,32):"RGBA8",(4,0):"CMPR",(2,8):"C8"}
    def integer(block,at):
        if at<0 or at+4>len(block):raise ValueError("Truncated texture metadata")
        return struct.unpack_from(nif.endian+"I",block,at)[0]
    def numbers(*values):return struct.pack(nif.endian+"I"*len(values),*values)
    for index in range(len(nif.blocks)):
        if nif.types[nif.indices[index]]!="NiPersistentSrcTextureRendererData":continue
        block=nif.blocks[index]
        if len(block)<87:raise ValueError("Truncated texture")
        mode=modes.get((integer(block,0),block[4]));mips=integer(block,63);end=71+mips*12
        if mode is None or integer(block,14)!=2 or not 1<=mips<=16:raise ValueError("Unsupported Wii texture layout")
        size=integer(block,end);faces=integer(block,end+8);platform=integer(block,end+12);payload=block[end+16:]
        if faces!=1 or platform!=4 or len(payload)!=size:raise ValueError("Unsupported Wii texture face/platform/length")
        palette=None
        if mode=="C8":
            link=integer(block,59)
            if link>=len(nif.blocks) or nif.types[nif.indices[link]]!="NiPalette":raise ValueError("Missing palette block")
            pal=nif.blocks[link];count=integer(pal,1)
            if not 1<=count<=256 or len(pal)!=5+count*4:raise ValueError("Invalid palette block")
            palette=[pal[5+i*4:9+i*4] for i in range(count)]
        pixels=bytearray();levels=[]
        for mip in range(mips):
            width,height,offset=struct.unpack_from(nif.endian+"3I",block,71+mip*12)
            if offset>=len(payload):raise ValueError("Invalid mip offset")
            if len(pixels)+width*height*4>256*1024*1024:raise ValueError("Expanded texture too large")
            levels.append((width,height,len(pixels)));pixels+=decode_gx(payload[offset:],width,height,mode,palette)
        # NiPixelFormat: RGBA8, linear pixels, little-endian packed channels.
        fmt=numbers(1)+bytes([32])+numbers(0xffffffff,0)+bytes([1])+numbers(0)+block[18:19]
        for channel in range(4):fmt+=numbers(channel,0)+bytes([8,0])
        new=fmt+numbers(0xffffffff,mips,4)
        for mip in levels:new+=numbers(*mip)
        new+=numbers(len(pixels),1)+pixels
        if pixel_type is None:
            pixel_type=len(nif.types);nif.types.append("NiPixelData")
        nif.indices[index]=pixel_type;nif.blocks[index]=new
        converted.append(dict(block=index,mode=mode,mips=mips,width=levels[0][0],height=levels[0][1],rgba_bytes=len(pixels)))
    changed={item["block"] for item in converted}
    for index,block in enumerate(nif.blocks):
        if nif.types[nif.indices[index]]!="NiSourceTexture":continue
        base=12+4*integer(block,4)
        if len(block)!=base+24:raise ValueError("Unsupported source texture layout")
        link=integer(block,base+5)
        if link in changed:
            raw=bytearray(block);raw[base]=0;raw[-2:]=bytes(2);nif.blocks[index]=bytes(raw)
    result=nif.write();NIF.read(result)
    return result,converted
