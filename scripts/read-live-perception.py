"""Bounded, read-only EM2 guest perception/registry sample; no input or writes."""
import argparse,ctypes as c,ctypes.wintypes as w,struct,json,datetime
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--pid',type=int,required=True);p.add_argument('--selector',type=lambda v:int(v,0),required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--entity-words',action='store_true');a=p.parse_args()
k=c.WinDLL('kernel32',use_last_error=True)
k.OpenProcess.argtypes=[w.DWORD,w.BOOL,w.DWORD];k.OpenProcess.restype=w.HANDLE
k.CloseHandle.argtypes=[w.HANDLE]
k.ReadProcessMemory.argtypes=[w.HANDLE,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)];k.ReadProcessMemory.restype=w.BOOL
k.QueryFullProcessImageNameW.argtypes=[w.HANDLE,w.DWORD,w.LPWSTR,c.POINTER(w.DWORD)];k.QueryFullProcessImageNameW.restype=w.BOOL
h=k.OpenProcess(0x410,False,a.pid)
if not h:raise c.WinError(c.get_last_error())
try:
 name=c.create_unicode_buffer(32768);length=w.DWORD(len(name))
 if not k.QueryFullProcessImageNameW(h,0,name,c.byref(length)):raise c.WinError(c.get_last_error())
 if Path(name.value).resolve()!=(Path(__file__).resolve().parents[1]/'build/runtime23/beetleworx.exe').resolve():raise RuntimeError('Not the canonical EM2 game path')
 base=0x100000000
 def read(address,size):
  if address<4096 or address+size>0x100000000:raise ValueError('Guest range rejected')
  data=c.create_string_buffer(size);copied=c.c_size_t()
  host=base+address+(4096 if address>=0xE0000000 else 0)
  if not k.ReadProcessMemory(h,host,data,size,c.byref(copied)) or copied.value!=size:raise c.WinError(c.get_last_error())
  return data.raw
 def word(address):return struct.unpack('>I',read(address,4))[0]
 def optional_word(address):
  try:return word(address)
  except (OSError,ValueError):return 0
 registry=word(0x833DA7E4)
 if word(registry+4)!=0x827B58D0 or word(registry+8)!=0x82A69948:raise RuntimeError('Guest base/native registry signature mismatch')
 def buckets(mapping):
  pointer,count=struct.unpack('>II',read(mapping+16,8))
  if not 0<count<=65536:raise RuntimeError('Bucket count outside bound')
  return struct.unpack('>'+str(count)+'I',read(pointer,count*4))
 reverse={};visited=set()
 for head in buckets(registry):
  node=head
  while node:
   if node in visited or len(visited)>=16384:raise RuntimeError('Registry cycle or node limit')
   visited.add(node);handle,entity,node=struct.unpack('>III',read(node,12));reverse[entity]=handle
 if word(a.selector)!=0x820E4630:raise RuntimeError('Selected address is not the observed native selector class')
 records=[];visited=set();scene_before=word(0x833FCA4C)
 for bucket,head in enumerate(buckets(a.selector+52)):
  node=head
  while node:
   if node in visited or len(visited)>=4096:raise RuntimeError('Selector cycle or node limit')
   visited.add(node);target,field4,last,gate,field16,nxt=struct.unpack('>6I',read(node,24));attached=optional_word(target+60)
   entity=next((v for v in [target,target+8,attached,attached+8 if attached else 0] if v in reverse),0)
   records.append(dict(bucket=bucket,node=f'{node:08X}',target=f'{target:08X}',attached=f'{attached:08X}',entity=f'{entity:08X}',handle=f'{reverse.get(entity,0):08X}',last=f'{last:08X}',gate=gate,field4=f'{field4:08X}',field16=f'{field16:08X}'))
   if a.entity_words and entity:
    try:
     words=struct.unpack('>32I',read(entity,128));records[-1]['entity_words']=[f'{v:08X}' for v in words];records[-1]['handle_word_offsets']=[i*4 for i,v in enumerate(words) if v==reverse[entity]]
    except OSError:records[-1]['entity_words_unavailable']=True
   node=nxt
 result=dict(utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),pid=a.pid,image=name.value,guest_base=f'{base:016X}',selector=f'{a.selector:08X}',registry_entries=len(reverse),scene_before=f'{scene_before:08X}',scene_after=f'{word(0x833FCA4C):08X}',targets=records,scope='Bounded live sample; no process writes, input, pause or termination. Engine may update concurrently; this is not an atomic snapshot.')
 a.output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(dict(report=str(a.output),registry_entries=len(reverse),targets=len(records),resolved=sum(v['handle']!='00000000' for v in records))))
finally:k.CloseHandle(h)
