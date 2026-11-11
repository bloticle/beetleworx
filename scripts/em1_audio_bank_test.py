"""Native bank framing and event action IDs against independently read RVZ banks."""
import argparse,ctypes as c,json,struct
from pathlib import Path
from em1_rvz import RVZ
p=argparse.ArgumentParser();p.add_argument('--library',required=True);p.add_argument('--image',type=Path,required=True);args=p.parse_args()
d=c.CDLL(str(Path(args.library).resolve()));U=c.POINTER(c.c_ubyte);W=c.POINTER(c.c_uint32)
d.bank.argtypes=[U,c.c_size_t,c.POINTER(c.c_size_t),c.POINTER(c.c_size_t),c.POINTER(c.c_uint32)];d.bank.restype=c.c_char_p
d.bank_record.argtypes=[c.c_size_t,c.c_int,c.POINTER(c.c_uint32),c.POINTER(c.c_uint32),c.POINTER(c.c_size_t)];d.bank_record.restype=U
d.bank_event.argtypes=[c.c_uint32,c.POINTER(W),c.POINTER(c.c_size_t)];d.bank_event.restype=c.c_char_p

d.bank_source.argtypes=[c.c_wchar_p,c.c_char_p,c.POINTER(c.c_uint32)];d.bank_source.restype=c.c_char_p

d.bank_action.argtypes=[c.c_uint32,c.POINTER(c.c_uint32),c.POINTER(c.c_uint32),c.POINTER(c.c_uint32),c.POINTER(c.c_size_t)];d.bank_action.restype=c.c_char_p
d.bank_sound.argtypes=[c.c_uint32,c.POINTER(c.c_uint32),c.POINTER(c.c_size_t),c.POINTER(c.c_uint32)];d.bank_sound.restype=c.c_char_p

d.bank_chain.argtypes=[c.c_uint32,c.POINTER(c.c_uint32),c.POINTER(c.c_size_t)];d.bank_chain.restype=c.c_char_p

d.bank_limit.argtypes=[c.c_uint32,c.POINTER(c.c_uint32),c.POINTER(c.c_uint32)];d.bank_limit.restype=c.c_char_p

def parse(b):
 chunks={};at=0
 while at<len(b):
  name,n=struct.unpack_from('>4sI',b,at);chunks[name]=b[at+8:at+8+n];at+=8+n
 assert at==len(b)
 objects=[];h=chunks.get(b'HIRC',b'\0'*4);at=4
 for _ in range(struct.unpack_from('>I',h)[0]):
  kind,size,identity=struct.unpack_from('>III',h,at);objects.append((kind,identity,h[at+12:at+8+size]));at+=8+size
 assert at==len(h)
 media=[];index=chunks.get(b'DIDX',b'');data=chunks.get(b'DATA',b'')
 for at in range(0,len(index),12):
  identity,offset,size=struct.unpack_from('>III',index,at);media.append((0,identity,data[offset:offset+size]))
 return chunks,objects,media

def native(b):
 buf=(c.c_ubyte*len(b)).from_buffer_copy(b);objects=c.c_size_t();media=c.c_size_t();identity=c.c_uint32()
 return d.bank(buf,len(b),c.byref(objects),c.byref(media),c.byref(identity)),objects.value,media.value,identity.value
r=RVZ(args.image);results=[];rejected=0
try:
 for partition in range(len(r.parts)):
  files={name.lower():span for name,span in r.files(partition).items()}
  for name in ['audio/wii/global.bnk','audio/wii/init.bnk','audio/wii/animatronicdaisy.bnk','audio/wii/animatronichook.bnk']:
   if name not in files:continue
   raw=r.read_partition(partition,*files[name]);chunks,objects,media=parse(raw)
   error,n,m,identity=native(raw);assert not error,error
   assert (n,m,identity)==(len(objects),len(media),struct.unpack_from('>I',chunks[b'BKHD'],4)[0])
   bank_id=c.c_uint32();assert not d.bank_source(str(args.image),name.encode(),c.byref(bank_id));assert bank_id.value==identity
   events=0
   for which,records in enumerate([objects,media]):
    for index,(kind,identity,payload) in enumerate(records):
     k=c.c_uint32();i=c.c_uint32();size=c.c_size_t();ptr=d.bank_record(index,which,c.byref(k),c.byref(i),c.byref(size))
     assert (k.value,i.value,c.string_at(ptr,size.value))==(kind,identity,payload)
     if which==0 and kind==4:
      actions=W();count=c.c_size_t();error=d.bank_event(identity,c.byref(actions),c.byref(count));assert not error,error
      expected=struct.unpack_from('>I',payload)[0];assert len(payload)==4+4*expected
      assert [actions[j] for j in range(count.value)]==list(struct.unpack_from('>'+str(expected)+'I',payload,4));events+=1
   if name=='audio/wii/global.bnk':
    kind=c.c_uint32();target=c.c_uint32();section=c.c_uint32();size=c.c_size_t()
    assert not d.bank_action(0x3d07767c,c.byref(kind),c.byref(target),c.byref(section),c.byref(size))
    assert (kind.value,target.value,section.value,size.value)==(0x4011,0x09075f8e,33,37)
    media_id=c.c_uint32();count=c.c_size_t();rate=c.c_uint32()
    error=d.bank_sound(target.value,c.byref(media_id),c.byref(count),c.byref(rate));assert not error,error
    assert media_id.value==0x17d3ec7f and rate.value==16000 and count.value>10000
    parent=c.c_uint32();nodes=c.c_size_t();error=d.bank_chain(0x09075f8e,c.byref(parent),c.byref(nodes));assert not error,error
    assert parent.value==0x24bad405 and nodes.value>1
    limit_node=c.c_uint32();maximum=c.c_uint32();error=d.bank_limit(0x09075f8e,c.byref(limit_node),c.byref(maximum));assert not error,error
    assert (limit_node.value,maximum.value)==(0x09075f8e,1)
    hirc_at=raw.index(b'HIRC')+8;position=hirc_at+4;sound_offset=None
    for object_kind,object_id,payload in objects:
     if object_kind==2 and object_id==0x09075f8e:sound_offset=position+12
     position+=12+len(payload)
    assert sound_offset is not None
    for field in [0,4,8,16,20,24,28]:
     changed=bytearray(raw);changed[sound_offset+field+3]^=1
     assert not native(changed)[0], 'Framing unexpectedly depends on source interpretation'
     assert d.bank_sound(0x09075f8e,c.byref(media_id),c.byref(count),c.byref(rate)), 'Invalid source binding accepted'
    for field,value in [(33+6,0x09075f8e),(33+6,0xdeadbeef),(33+14,0x7fc00000)]:
     changed=bytearray(raw);struct.pack_into('>I',changed,sound_offset+field,value)
     assert not native(changed)[0]
     assert d.bank_chain(0x09075f8e,c.byref(parent),c.byref(nodes)), 'Invalid/cyclic audio ancestry accepted'
    assert not d.bank_source(str(args.image),name.encode(),c.byref(bank_id))
    assert d.bank_action(0xd76ae37e,c.byref(kind),c.byref(target),c.byref(section),c.byref(size)), 'Event accepted as action'
    assert d.bank_sound(0x3d07767c,c.byref(media_id),c.byref(count),c.byref(rate)), 'Action accepted as sound'
   for end in [0,7,len(raw)-1]:assert native(raw[:end])[0];rejected+=1
   invalid=bytearray(raw);invalid[11]=62;assert native(invalid)[0];rejected+=1
   results.append(dict(bank=name,objects=n,media=m,events=events))
finally:r.close()
assert len(results)>=3
Path('analysis/em1-compatibility/audio-bank-result.json').write_text(json.dumps(dict(banks=results,rejected=rejected,scope='Original bank framing/payload/media ownership and event-action identities; no interpreted graph or runtime playback.'),indent=2)+'\n')
print('PASS:',len(results),'original banks;',sum(x['objects'] for x in results),'objects;',sum(x['media'] for x in results),'media;',sum(x['events'] for x in results),'events;',rejected,'invalid banks rejected')
