"""Recover console names from the original level database and audited script index.

Never modifies original archives. State records are a name/root pair followed
by a counted list of 12-byte zone settings. The preceding word is their count.
"""
from pathlib import Path
import hashlib,json,re,runpy,struct
ROOT=Path(__file__).resolve().parents[1]
archive=Path('D:/Modding/Builds/EM2X360/packfiles/levels.pak')
files=runpy.run_path(str(ROOT/'scripts/game-pak.py'))['read_pak'](archive)
data=next(blob for name,blob,_ in files if name=='levels/master.lvlc')
u32=lambda p:struct.unpack_from('>I',data,p)[0]
assert u32(0)==9
strings=u32(8)
def text(offset):
    p=strings+offset
    assert strings<=p<len(data) and (p==strings or data[p-1]==0)
    return data[p:data.index(b'\0',p)].decode('ascii')
def pair(p):
    try:
        name,parent=text(u32(p)),text(u32(p+4))
        return bool(re.fullmatch(r'[a-z0-9_]+(?:\.[a-z0-9_]+)?',name) and name.split('.')[0]==parent)
    except (AssertionError,ValueError,UnicodeError):return False
starts=[p for p in range(40,strings-12,4) if pair(p)]
start=starts[0];count=u32(start-4);assert count==205
position=start;levels=[]
for _ in range(count):
    assert pair(position)
    levels.append(text(u32(position)));zones=u32(position+8);assert zones<=u32(12)
    for i in range(zones):
        zone=position+12+i*12
        assert u32(zone)<=16 and u32(zone+8)<=1
        assert re.fullmatch(r'[a-z0-9_]+\.[a-z0-9_]+',text(u32(zone+4)))
    position+=12+12*zones
assert len(set(levels))==count and position<=strings and 'meanstreet_north.visit_1' in levels
exports=json.loads((ROOT/'analysis/native-script-exports.json').read_text())
image=(ROOT/'private/image.bin').read_bytes()
for entry in exports:
    pc=int(entry['registration'],16);word=struct.unpack_from('>I',image,pc-0x82000000)[0]
    displacement=word&0x03fffffc
    if displacement&0x02000000:displacement-=0x04000000
    assert word>>26==18 and word&1 and ((pc+displacement)&0xffffffff)==0x82731C90
names=sorted(set(x['name'] for x in exports));assert len(names)>900
def signature(entry):
    # Retail registration calls the callback with a null VM to advertise its
    # parameter declaration. Recover that short null branch, not guessed args.
    address=int(entry['callback'],16);registers=[None]*32;registers[3]=0
    for pc in range(address,address+256,4):
        instruction=struct.unpack_from('>I',image,pc-0x82000000)[0]
        op=instruction>>26;rd,ra,rb=(instruction>>21)&31,(instruction>>16)&31,(instruction>>11)&31
        immediate=instruction&65535
        if immediate&32768:immediate-=65536
        if op in (14,15):registers[rd]=None if ra and registers[ra] is None else ((0 if not ra else registers[ra])+(immediate<<(16 if op==15 else 0)))&0xffffffff
        elif op==24:registers[ra]=None if registers[rd] is None else registers[rd]|(instruction&65535)
        elif op==31 and ((instruction>>1)&1023)==444:registers[ra]=registers[rd] if rd==rb else None
        elif op in (32,33,34,35,40,41,42,43,46,58):registers[rd]=None
        elif op==18 and instruction&1:
            displacement=instruction&0x03fffffc
            if displacement&0x02000000:displacement-=0x04000000
            target=(pc+displacement)&0xffffffff
            if target==0x8272FCA0:
                ptr=registers[4]
                if ptr is not None and 0x82000000<=ptr<0x822f0000:
                    start=ptr-0x82000000;end=image.find(b'\0',start)
                    if 0<=end-start<600:
                        value=image[start:end]
                        if all(32<=x<127 for x in value):return value.decode('ascii')
                return ''
            if not 0x82b69500<=target<=0x82b6a100:
                for reg in range(3,13):registers[reg]=None
    return ''
signatures={entry['name']:signature(entry) for entry in exports}
lines=['#pragma once','#include <string_view>','namespace em2 {','inline constexpr std::string_view kNativeLevelStates[]={']
lines += [json.dumps(x)+',' for x in sorted(levels)]
lines += ['};','inline constexpr std::string_view kNativeScriptExports[]={']
lines += [json.dumps(x)+',' for x in names]
lines += ['};','inline constexpr std::string_view kNativeScriptSignatures[]={']
lines += [json.dumps(signatures[name])+',' for name in names]
lines += ['};','}']
(ROOT/'runtime/game_console_catalog.h').write_text('\n'.join(lines)+'\n')
report={'master_sha256':hashlib.sha256(data).hexdigest(),'state_table':hex(start),'state_table_end':hex(position),'state_count':count,'exports':len(names),'signature_count':sum(bool(s) for s in signatures.values()),'signatures':signatures,'registration_target':'82731C90','level_states':sorted(levels)}
(ROOT/'analysis/game-console-catalog.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({k:v for k,v in report.items() if k not in ['level_states','signatures']}))
