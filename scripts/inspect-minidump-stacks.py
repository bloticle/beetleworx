"""List executable-address candidates from minidump thread stacks (not an unwind)."""
import pathlib, struct, sys
data=pathlib.Path(sys.argv[1]).read_bytes()
u32=lambda p:struct.unpack_from('<I',data,p)[0]
u64=lambda p:struct.unpack_from('<Q',data,p)[0]
streams={u32(p):(u32(p+4),u32(p+8)) for p in range(u32(12),u32(12)+12*u32(8),12)}
_,rva=streams[4]
modules=[]
for i in range(u32(rva)):
    p=rva+4+i*108;n=u32(p+20)
    modules.append((u64(p),u32(p+8),data[n+4:n+4+u32(n)].decode('utf-16le')))
# Archived crash evidence retains the original module name.
exe=next(m for m in modules if m[2].lower().endswith(('beetleworx.exe','em2recomp.exe')))
print('EXE',exe)
_,rva=streams[3]
for i in range(u32(rva)):
    p=rva+4+i*48;context=u32(p+44);rip=u64(context+248);rsp=u64(context+152)
    mod=next((m[2] for m in modules if m[0]<=rip<m[0]+m[1]),'?')
    print(f'THREAD {u32(p)} RIP {rip:X} {mod} RSP {rsp:X}')
    start=u64(p+24);size=u32(p+32);memory=u32(p+36);begin=max(0,rsp-start)
    for offset in range(begin,min(size,begin+16384)-7,8):
        address=u64(memory+offset)
        if exe[0]<=address<exe[0]+exe[1]:
            print(f'  {start+offset:X} 0x{address-exe[0]+0x140000000:X}')
