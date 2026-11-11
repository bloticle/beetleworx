"""Compare native DSP conversion with independent arithmetic on original RVZ media."""
import argparse,ctypes as c,json,struct,sys
from pathlib import Path
from em1_rvz import RVZ
p=argparse.ArgumentParser();p.add_argument('--library',required=True);p.add_argument('--image',type=Path,required=True);args=p.parse_args()
dll=c.CDLL(str(Path(args.library).resolve()));U=c.POINTER(c.c_ubyte);S=c.POINTER(c.c_int16)
dll.dsp.argtypes=[U,c.c_size_t,c.POINTER(S),c.POINTER(c.c_size_t),c.POINTER(c.c_uint32)];dll.dsp.restype=c.c_char_p

dsp_loop=dll.dsp_loop
dsp_loop.argtypes=[U,c.c_size_t,c.c_uint16,S,c.c_size_t,c.POINTER(c.c_size_t),c.c_size_t];dsp_loop.restype=c.c_char_p

def stream(blob,passes,capacity,block):
 raw=(c.c_ubyte*len(blob)).from_buffer_copy(blob);output=(c.c_int16*capacity)();written=c.c_size_t()
 error=dsp_loop(raw,len(blob),passes,output,capacity,c.byref(written),block)
 return error,list(output[:written.value])

def chunks(b,start,pad=False):
 out={};at=start
 while at<len(b):
  name=b[at:at+4];n=struct.unpack_from('>I',b,at+4)[0];end=at+8+n
  assert end<=len(b) and name not in out
  out[name]=(b[at+8:end],at+8);at=end+(n%2 if pad and end<len(b) else 0)
 return out

def native(b):
 raw=(c.c_ubyte*len(b)).from_buffer_copy(b);samples=S();size=c.c_size_t();rate=c.c_uint32()
 error=dll.dsp(raw,len(b),c.byref(samples),c.byref(size),c.byref(rate))
 return error,([samples[i] for i in range(size.value)],rate.value) if not error else None
r=RVZ(args.image);results=[];rejected=0
try:
 for partition in range(len(r.parts)):
  files={name.lower():span for name,span in r.files(partition).items()}
  for bank in ['audio/wii/global.bnk','audio/wii/animatronicdaisy.bnk']:
   if bank not in files:continue
   raw=r.read_partition(partition,*files[bank]);parts=chunks(raw,0)
   assert struct.unpack_from('>I',parts[b'BKHD'][0])[0]==46
   index=parts[b'DIDX'][0];data=parts[b'DATA'][0];done=0
   for at in range(0,len(index),12):
    identity,offset,length=struct.unpack_from('>III',index,at);media=data[offset:offset+length]
    if media[:4]!=b'RIFX':continue
    m=chunks(media,12,True);fmt=m[b'fmt '][0]
    if len(fmt)!=40 or struct.unpack_from('>HH',fmt)!=(0xfff0,1):continue
    header=m[b'WiiH'][0];encoded=m[b'data'][0];count=struct.unpack_from('>I',fmt,8)[0]
    coefficients=struct.unpack_from('>16h',header);h1,h2=struct.unpack_from('>hh',header,36);expected=[]
    for frame in range(0,len(encoded),8):
     predictor=encoded[frame]//16;scale=2**(encoded[frame]%16)
     for v in encoded[frame+1:frame+8]:
      for n in [v//16,v%16]:
       n=n if n<8 else n-16
       sample=(n*scale*2048+1024+coefficients[2*predictor]*h1+coefficients[2*predictor+1]*h2)//2048
       sample=max(-32768,min(32767,sample));expected.append(sample);h2,h1=h1,sample
    expected=expected[:count];error,observed=native(media);assert not error,error
    assert observed==(expected,struct.unpack_from('>I',fmt,4)[0])
    for block in [1,13,257]:
     error,streamed=stream(media,1,len(expected)+20,block);assert not error,error;assert streamed==expected
    for end in [0,4,11,len(media)-1]:assert native(media[:end])[0];rejected+=1
    invalid=bytearray(media);invalid[m[b'data'][1]]=0xf0;assert native(invalid)[0];rejected+=1
    results.append(dict(bank=bank,media_id=f'{identity:08x}',samples=count,rate=observed[1]));done+=1
    if done==3:break
finally:r.close()
assert len(results)>=3,results
Path('analysis/em1-compatibility/audio-dsp-result.json').write_text(json.dumps(dict(clips=results,rejected=rejected,scope='Native mono DSP PCM only; no bank graph conversion, event routing, loop application or in-game playback.'),indent=2)+'\n')
print('PASS:',len(results),'original media clips;',sum(x['samples'] for x in results),'PCM samples;',rejected,'malformed media rejected')

# Alternate loop history and a partial-frame start distinguish actual DSP
# context restoration from repeating the first-pass PCM segment.
fmt=bytearray(40);struct.pack_into('>HHIIHH',fmt,0,0xfff0,1,32000,28,8,4)
header=bytearray(46);struct.pack_into('>h',header,0,1024);struct.pack_into('>hh',header,36,10,-4);struct.pack_into('>Hhh',header,40,0,100,-50)
sampler=bytearray(60);struct.pack_into('>II',sampler,28,1,0);struct.pack_into('>II',sampler,44,2,5)
encoded=bytes([0,0x12,0x34,0x56,0x78,0x9a,0xbc,0xde])*2
body=b'WAVE'+b''.join(tag+struct.pack('>I',len(data))+data for tag,data in [(b'fmt ',fmt),(b'WiiH',header),(b'smpl',sampler),(b'data',encoded)])
fixture=b'RIFX'+struct.pack('>I',len(body))+body
for passes in [0,1,2,3]:
 expected=[];position=0;remaining=passes;h1,h2=10,-4
 while len(expected)<100:
  if remaining!=1 and position==5:
   if remaining>1:remaining-=1
   position=2;h1,h2=100,-50
  if position==28:break
  frame,index=divmod(position,14);packed=encoded[frame*8+1+index//2];n=(packed>>4) if index%2==0 else packed&15;n=n if n<8 else n-16
  sample=max(-32768,min(32767,(n*2048+1024+1024*h1)//2048));expected.append(sample);h2,h1=h1,sample;position+=1
 for block in [1,3,17,100]:
  error,observed=stream(fixture,passes,100,block);assert not error,error;assert observed==expected,(passes,block,observed,expected)
print('PASS: original clips stream at 1/13/257 sample blocks; finite/infinite partial-frame loops restore alternate DSP history')

Path('analysis/em1-compatibility/audio-loop-result.json').write_text(json.dumps(dict(original_clips=len(results),original_block_sizes=[1,13,257],loop_passes=[0,1,2,3],loop_block_sizes=[1,3,17,100],partial_frame_loop=[2,5],alternate_history=[100,-50],scope='Sequential encoded-bank DSP voice reader only; original single-pass PCM and synthetic finite/infinite intro/loop/tail/context behavior. No audio-device or in-game event output.'),indent=2)+'\n')
