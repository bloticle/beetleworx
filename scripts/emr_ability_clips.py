"""Convert the supplied EM2-rig GLB clips to the game's real 2011 spline HKX.

Uses the original clip's class/signature/container/binding, with newly authored
degree-one curves. Rotation control points are full float quaternions; vectors
use 16-bit scalar quantization. No model, physics, or extracted root motion is
installed. The source rig must match every native bone, parent and bind pose.

The payload uses the target Xbox 360 sampler's big-endian layout. Havok's
endian conversion for uncompressed quaternions only swaps the first three
components, so encoding a little-endian payload would corrupt the fourth.

Spline format cross-check: PredatorCZ/HavokLib hkaSplineDecompressor and
Meowmaritus/MVDX2 Havok/SplineCompressedAnimation.cs. This encoder is independent
Python code, not a copy of a decompressor. Native sampling is separately tested.
"""
import argparse
import bisect
import hashlib
import json
import math
import runpy
import struct
from pathlib import Path

Graph = runpy.run_path(str(Path(__file__).with_name('patch-player-behaviors.py')))['Graph']
CLIPS = {
    'A_mickey_dash': 'emr_dash',
    'A_mickey_sprint': 'emr_sprint',
    'A_mickey_GroundPound_Start': 'emr_pound_start',
    'AM_mickey_GroundPound': 'emr_pound_drop',
    'AM_Mickey_Land_Hard': 'emr_land_hard',
}

def unit(q):
    length = math.sqrt(sum(v*v for v in q))
    if not math.isfinite(length) or length < 1e-12: raise ValueError('Degenerate quaternion')
    return tuple(v/length for v in q)

def rotation(a, b, alpha):
    dot = sum(x*y for x,y in zip(a,b))
    if dot < 0: b = tuple(-v for v in b); dot = -dot
    if dot > .9995: return unit(tuple(x+(y-x)*alpha for x,y in zip(a,b)))
    theta = math.acos(min(1.,dot)); divisor = math.sin(theta)
    return unit(tuple((math.sin((1-alpha)*theta)*x+math.sin(alpha*theta)*y)/divisor for x,y in zip(a,b)))

class Glb:
    def __init__(self, source):
        self.blob = Path(source).read_bytes()
        if self.blob[:4] != b'glTF' or struct.unpack_from('<II',self.blob,4) != (2,len(self.blob)):
            raise ValueError('Expected a complete GLB 2.0')
        size,kind=struct.unpack_from('<II',self.blob,12)
        if kind != 0x4e4f534a: raise ValueError('Missing GLB JSON')
        self.doc=json.loads(self.blob[20:20+size]); offset=20+size
        size,kind=struct.unpack_from('<II',self.blob,offset)
        if kind != 0x004e4942: raise ValueError('Missing embedded GLB BIN')
        self.data=self.blob[offset+8:offset+8+size]
        if len(self.doc.get('skins',[])) != 1: raise ValueError('Expected one authored rig')
        self.joints=self.doc['skins'][0]['joints']
    def accessor(self, index):
        a=self.doc['accessors'][index]
        if a.get('sparse') or a['componentType']!=5126: raise ValueError('Expected nonsparse float accessor')
        lanes={'SCALAR':1,'VEC3':3,'VEC4':4}[a['type']]
        view=self.doc['bufferViews'][a['bufferView']]
        if view.get('buffer',0): raise ValueError('External buffer is unsupported')
        stride=view.get('byteStride',lanes*4); start=view.get('byteOffset',0)+a.get('byteOffset',0)
        if stride<lanes*4 or start+a['count']*stride>len(self.data)+stride-lanes*4: raise ValueError('Accessor exceeds embedded data')
        rows=[struct.unpack_from('<'+'f'*lanes,self.data,start+i*stride) for i in range(a['count'])]
        if any(not math.isfinite(v) for row in rows for v in row): raise ValueError('Nonfinite GLB data')
        return rows
    def rig(self, native):
        sk=native.objects('hkaSkeleton')
        if len(sk)!=1: raise ValueError('Expected one native skeleton')
        sk=sk[0]; count=native.u(sk+28)
        if count!=95 or len(self.joints)!=count: raise ValueError('The original 95-bone Mickey rig is required')
        bones=native.ptr(sk+24); parent=native.ptr(sk+12); reference=native.ptr(sk+36)
        parents={child:at for at,node in enumerate(self.doc['nodes']) for child in node.get('children',[])}
        names=[]; maximum=0
        for i,nodeid in enumerate(self.joints):
            node=self.doc['nodes'][nodeid]; name=native.string(native.ptr(bones+i*8))
            if name!=node.get('name'): raise ValueError('Bone order/name mismatch: '+name)
            p=struct.unpack_from('>h',native.data,parent+i*2)[0]
            if p>=0 and parents.get(nodeid)!=self.joints[p]: raise ValueError('Bone hierarchy mismatch: '+name)
            pose=struct.unpack_from('>12f',native.data,reference+i*48)
            t=node.get('translation',[0,0,0]);q=unit(node.get('rotation',[0,0,0,1]));s=node.get('scale',[1,1,1])
            err=max(*(abs(t[k]-pose[k]) for k in range(3)),*(abs(s[k]-pose[8+k]) for k in range(3)))
            angular=1-abs(sum(x*y for x,y in zip(q,unit(pose[4:8]))))
            if err>1e-4 or angular>1e-8: raise ValueError('Bind pose mismatch: '+name)
            maximum=max(maximum,err);names.append(name)
        return maximum
    def poses(self, animation, time):
        channels={}
        for channel in animation['channels']:
            target=channel['target'];key=(target['node'],target['path'])
            if key in channels: raise ValueError('Duplicate animation channel')
            sampler=animation['samplers'][channel['sampler']]
            if sampler.get('interpolation','LINEAR')!='LINEAR': raise ValueError('Expected authored LINEAR channels')
            keys=[x[0] for x in self.accessor(sampler['input'])]; values=self.accessor(sampler['output'])
            if len(keys)!=len(values) or any(a>=b for a,b in zip(keys,keys[1:])): raise ValueError('Invalid animation key times')
            slot=max(0,min(len(keys)-2,bisect.bisect_right(keys,time)-1))
            alpha=max(0.,min(1.,(time-keys[slot])/(keys[slot+1]-keys[slot])))
            channels[key]=rotation(unit(values[slot]),unit(values[slot+1]),alpha) if key[1]=='rotation' else tuple(a+(b-a)*alpha for a,b in zip(values[slot],values[slot+1]))
        return [(channels.get((i,'translation'),self.doc['nodes'][i].get('translation',[0,0,0])),
                 channels.get((i,'rotation'),unit(self.doc['nodes'][i].get('rotation',[0,0,0,1]))),
                 channels.get((i,'scale'),self.doc['nodes'][i].get('scale',[1,1,1]))) for i in self.joints]

def spline_bytes(poses):
    tracks=len(poses[0]);frames=len(poses);data=bytearray(tracks*4)
    def align(n=4): data.extend(bytes((-len(data))%n))
    def knots():
        data.extend(struct.pack('>HB',frames-1,1));data.extend(bytes([0]+list(range(frames))+[frames-1]));align()
    def vector(values,identity):
        dynamic=any(max(row[k] for row in values)-min(row[k] for row in values)>1e-7 for k in range(3))
        if dynamic:
            knots();bounds=[(min(row[k] for row in values),max(row[k] for row in values)) for k in range(3)]
            for lo,hi in bounds:data.extend(struct.pack('>2f',lo,hi))
            for row in values:
                for k,(lo,hi) in enumerate(bounds):data.extend(struct.pack('>H',0 if hi==lo else round((row[k]-lo)/(hi-lo)*65535)))
            mask=0x70
        else:
            mask=0
            for k in range(3):
                if abs(values[0][k]-identity)>1e-7:mask|=1<<k;data.extend(struct.pack('>f',values[0][k]))
        align();return mask
    for track in range(tracks):
        values=[p[track] for p in poses];tm=vector([v[0] for v in values],0.)
        quats=[]
        for v in values:
            q=unit(v[1])
            if quats and sum(a*b for a,b in zip(quats[-1],q))<0:q=tuple(-x for x in q)
            quats.append(q)
        moving=any(max(abs(a-b) for a,b in zip(quats[0],q))>1e-7 for q in quats)
        if moving:knots()
        for q in quats if moving else quats[:1]:data.extend(struct.pack('>4f',*q))
        align();sm=vector([v[2] for v in values],1.)
        data[track*4:track*4+4]=bytes([1|(5<<2)|(1<<6),tm,0xf0 if moving else 0xf,sm])
    align(16);return bytes(data)

def encode_clip(template, poses, duration):
    g=Graph(template);animations=g.objects('hkaSplineCompressedAnimation');bindings=g.objects('hkaAnimationBinding')
    if len(animations)!=1 or len(bindings)!=1 or g.u(animations[0]+16)!=95:raise ValueError('Unexpected native Mickey clip template')
    a=animations[0];binding=bindings[0];payload=spline_bytes(poses);step=duration/(len(poses)-1)
    for at,value in ((12,duration),(56,step*255),(60,1/(step*255)),(64,step)):struct.pack_into('>f',g.data,a+at,value)
    for at,value in ((20,0),(32,0),(36,0x80000000),(40,len(poses)),(44,1),(48,256),(52,95*4),(128,1)):g.put(a+at,value)
    g.ref(a+24,None,True);g.ref(a+28,None)
    for at,values in ((68,[0]),(80,[len(payload)]),(92,[]),(104,[])):
        g.ref(a+at,g.append(struct.pack('>'+'I'*len(values),*values)) if values else None)
        g.put(a+at+4,len(values));g.put(a+at+8,0x80000000|len(values))
    g.ref(a+116,g.append(payload));g.put(a+120,len(payload));g.put(a+124,0x80000000|len(payload))
    g.ref(binding+28,None);g.put(binding+32,0);g.put(binding+36,0x80000000)
    result=g.encode();check=Graph(result)
    if check.u(a+40)!=len(poses) or check.u(a+120)!=len(payload) or check.u(a+128)!=1:raise ValueError('Native packfile roundtrip failed')
    return result

def compile_glb_clips(glb_path, template_blob, native_rig_blob):
    source=Glb(glb_path);maximum=source.rig(Graph(native_rig_blob));entries=[];report=[]
    animations={a.get('name'):a for a in source.doc['animations']}
    if set(animations)!=set(CLIPS):raise ValueError('Expected the five supplied EMR move animations')
    expected=[]
    for name,stem in CLIPS.items():
        animation=animations[name];duration=max(source.accessor(s['input'])[-1][0] for s in animation['samplers'])
        if not .01<=duration<=2:raise ValueError('Unexpected ability duration')
        if any(c['target']['node'] in source.joints[:2] for c in animation['channels']):raise ValueError('Locomotion root channels require explicit extraction review')
        # Four samples per original 30-Hz interval preserve every authored
        # key exactly, while reducing nlerp-vs-slerp rotation error in between.
        source_frames=max(len(source.accessor(s['input'])) for s in animation['samplers'])
        source_duration=duration;source_start=0.
        frames=(source_frames-1)*4+1
        if stem=='emr_pound_start':duration*=.5;frames=(source_frames-1)*2+1
        if stem=='emr_pound_drop':source_start=duration*.5;duration*=.5;frames=(source_frames-1)*2+1
        if frames>256:raise ValueError('Clip exceeds the single native spline block budget')
        poses=[source.poses(animation,source_start+duration*i/(frames-1)) for i in range(frames)]
        blob=encode_clip(template_blob,poses,duration);path='characters/mickey/animations/pc/'+stem+'.hkx'
        entries.append((path,blob,b'HKB\0'))
        report.append(dict(source=name,path=path,source_duration=source_duration,source_start=source_start,duration=duration,frames=frames,bytes=len(blob),sha256=hashlib.sha256(blob).hexdigest()))
        # Check endpoints and fractional frame positions in the real native sampler.
        expected.append((path,[(time,source.poses(animation,source_start+time)) for time in (0.,duration*.173,duration*.517,duration*.913,duration)]))
    # Both provided pound clips are the same motion, so the fall phase holds
    # the last authored drop pose rather than replaying the full startup.
    drop=animations['AM_mickey_GroundPound'];duration=max(source.accessor(s['input'])[-1][0] for s in drop['samplers'])
    pose=source.poses(drop,duration);path='characters/mickey/animations/pc/emr_pound_hold.hkx'
    blob=encode_clip(template_blob,[pose,pose],1/30);entries.append((path,blob,b'HKB\0'))
    expected.append((path,[(0.,pose),(1/60,pose),(1/30,pose)]))
    return entries,dict(source_sha256=hashlib.sha256(source.blob).hexdigest(),rig_bones=95,bind_position_scale_max_error=maximum,clips=report,root_motion='No Tracker/Reference channels; Root body pose preserved; no extracted physics motion'),expected

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--glb',type=Path,required=True);p.add_argument('--template',type=Path,required=True);p.add_argument('--rig',type=Path,required=True);p.add_argument('--out',type=Path,required=True);args=p.parse_args()
    entries,report,expected=compile_glb_clips(args.glb,args.template.read_bytes(),args.rig.read_bytes());args.out.mkdir(parents=True,exist_ok=True)
    for path,blob,_ in entries:(args.out/Path(path).name).write_bytes(blob)
    (args.out/'conversion-report.json').write_text(json.dumps(report,indent=2))
    # Little-endian host fixture; runtime relocates and samples the real HKX.
    fixture=bytearray(b'EMRQA001')+struct.pack('<I',len(expected))
    for path,samples in expected:
        name=Path(path).name.encode();fixture.extend(struct.pack('<II',len(name),len(samples))+name)
        for time,pose in samples:
            fixture.extend(struct.pack('<f',time))
            for t,q,s in pose:fixture.extend(struct.pack('<12f',*t,0.,*q,*s,0.))
    (args.out/'native-sampling-expected.bin').write_bytes(fixture)
    print(json.dumps(report,indent=2))
if __name__=='__main__':main()
