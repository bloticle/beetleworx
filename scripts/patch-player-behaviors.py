"""Author native ability states in the original, big-endian Havok graphs.

Preserves all existing objects, animation bindings, events and transitions.
New objects have native class finish records and independently cloned effects.
The original graph's descending pose supplies Mickey's pound animation; Oswald
uses his existing airborne spin. No external game animation assets are used.
"""
import json
import struct
import re
from pathlib import Path


class Graph:
    def __init__(self, blob):
        self.blob = blob
        if blob[:8] != bytes.fromhex('57e0e05710c0c010') or blob[16:18] != b'\x04\x00':
            raise ValueError('Expected original Xbox 360 Havok packfile')
        self.sections = [struct.unpack_from('>7I',blob,64+48*i+20) for i in range(self.word(blob,20))]
        if len(self.sections)!=3 or self.sections[2][0]+self.sections[2][6]!=len(blob):
            raise ValueError('Unexpected Havok section layout')
        self.start,self.local,self.global_,self.virtual,self.exports,self.imports,self.end = self.sections[2]
        if self.exports!=self.imports or self.imports!=self.end:
            raise ValueError('External Havok imports require a separate linker')
        self.data=bytearray(blob[self.start:self.start+self.local]);self.refs={};self.types={}
        for o in range(self.start+self.local,self.start+self.global_,8):
            a,b=struct.unpack_from('>2I',blob,o)
            if a!=0xffffffff:self.refs[a]=(2,b,False)
        for o in range(self.start+self.global_,self.start+self.virtual-11,12):
            a,s,b=struct.unpack_from('>3I',blob,o)
            if a!=0xffffffff:self.refs[a]=(s,b,True)
        for o in range(self.start+self.virtual,self.start+self.exports-11,12):
            a,s,b=struct.unpack_from('>3I',blob,o)
            if a!=0xffffffff:self.types[a]=(s,b)
        self.original_objects=sorted(self.types)

    @staticmethod
    def word(data, offset): return struct.unpack_from('>I',data,offset)[0]
    def u(self, offset): return self.word(self.data,offset)
    def put(self, offset, value): struct.pack_into('>I',self.data,offset,value)
    def ptr(self, offset):
        section,target,_ = self.refs[offset]
        if section!=2: raise ValueError('Expected a data-section reference')
        return target
    def ref(self, offset, target, object=False):
        self.put(offset,0)
        if target is None:self.refs.pop(offset,None)
        else:self.refs[offset]=(2,target,object)
    def string(self, offset): return self.data[offset:self.data.index(0,offset)].decode()
    def text(self, text): return self.append(text.encode()+b'\0')
    def typename(self, offset):
        s,p=self.types[offset];start=self.sections[s][0]+p
        return self.blob[start:self.blob.index(0,start)].decode()
    def objects(self, typename): return [o for o in self.types if self.typename(o)==typename]
    def append(self, data):
        self.data.extend(bytes((-len(self.data))%16));p=len(self.data);self.data.extend(data);return p
    def clone(self, old, size=None):
        if size is None:size=min(o for o in self.original_objects if o>old)-old
        new=self.append(self.data[old:old+size])
        for src,(section,target,global_) in list(self.refs.items()):
            if old<=src<old+size:self.refs[new+src-old]=(section,new+target-old if section==2 and old<=target<old+size else target,global_)
        if old in self.types:self.types[new]=self.types[old]
        return new
    def array(self, owner, offset, pointers, object=True):
        new=self.append(bytes(len(pointers)*4))
        for i,p in enumerate(pointers):self.ref(new+i*4,p,object)
        self.ref(owner+offset,new);self.put(owner+offset+4,len(pointers));self.put(owner+offset+8,0x80000000|len(pointers))
    def elements(self, owner, offset):
        return [self.ptr(self.ptr(owner+offset)+i*4) for i in range(self.u(owner+offset+4))]
    def transitions(self, owner):
        array=self.ptr(owner+48);return [self.ptr(array+8)+i*60 for i in range(self.u(array+12))]
    def transition_array(self, template, entries):
        array=self.clone(template,32);data=self.append(bytes(60*len(entries)))
        for i,entry in enumerate(entries):
            source,event,target=entry[:3];new_entry=len(entry)>3 and entry[3]
            dest=data+i*60;self.data[dest:dest+60]=self.data[source:source+60]
            for field in [32,36]:
                if source+field in self.refs:
                    pointer=self.ptr(source+field)
                    self.ref(dest+field,(self.clone(pointer) if new_entry else pointer) if field==32 else (None if new_entry else pointer),True)
                else:self.ref(dest+field,None)
            self.put(dest+40,event);self.put(dest+44,target)
        self.ref(array+8,data);self.put(array+12,len(entries));self.put(array+16,0x80000000|len(entries));return array
    def encode(self):
        self.data.extend(bytes((-len(self.data))%16));local=len(self.data)
        for src,(section,target,global_) in sorted(self.refs.items()):
            if not global_:
                if section!=2:raise ValueError('Unexpected local cross-section reference')
                self.data.extend(struct.pack('>2I',src,target))
        while len(self.data)%16:self.data.extend(b'\xff'*8)
        globals_offset=len(self.data)
        for src,(section,target,is_global) in sorted(self.refs.items()):
            if is_global:self.data.extend(struct.pack('>3I',src,section,target))
        while len(self.data)%16:self.data.extend(b'\xff'*12)
        virtual=len(self.data)
        for src,(section,name) in sorted(self.types.items()):self.data.extend(struct.pack('>3I',src,section,name))
        while len(self.data)%16:self.data.extend(b'\xff'*12)
        end=len(self.data);header=bytearray(self.blob[:self.start])
        struct.pack_into('>7I',header,64+48*2+20,self.start,local,globals_offset,virtual,end,end,end)
        return bytes(header+self.data)


def patch(blob, actor, ability_clips=None):
    if ability_clips is not None and actor=='mickey':
        return patch_emr(blob,ability_clips)
    g=Graph(blob)
    root_name='mickeybehaviorgraph' if actor=='mickey' else 'globalstatemachine'
    roots=[o for o in g.objects('hkbStateMachine') if g.string(g.ptr(o+32)).lower()==root_name]
    if len(roots)!=1:raise ValueError('Expected one native player root state machine')
    root=roots[0];states=g.elements(root,100);original_states=list(states);original_data=bytes(g.data);by_name={g.string(g.ptr(o+56)):o for o in states}
    get=lambda name:g.u(by_name[name]+60)
    air,land,run=get('InAir'),get('Land'),get('RunWalkState')
    stringdata=g.objects('hkbBehaviorGraphStringData')[0]
    names=g.elements(stringdata,8);events=[g.string(p) for p in names]
    header=(Path(__file__).resolve().parents[1]/'runtime/native_ability_events.h').read_text()
    aliases=dict(re.findall(r'\{"([^"]+)","([^"]+)"\}',header))
    custom=list(aliases.values())
    if len(custom)!=6:raise ValueError('Expected six native ability event aliases')
    if any(name in events for name in custom):raise ValueError('Player graph is already patched')
    indexes={name:len(events)+i for i,name in enumerate(custom)}
    g.array(stringdata,8,names+[g.text(name) for name in custom],False)
    graphdata=g.objects('hkbBehaviorGraphData')[0]
    if g.u(graphdata+48)!=len(events):raise ValueError('Event metadata and names disagree')
    event_info=g.ptr(graphdata+44)
    expanded=g.append(g.data[event_info:event_info+len(events)*4]+bytes(len(custom)*4))
    g.ref(graphdata+44,expanded);g.put(graphdata+48,len(events)+len(custom));g.put(graphdata+52,0x80000000|len(events)+len(custom))
    clips=g.objects('hkbClipGenerator')
    def clip(suffix):
        found=[o for o in clips if g.string(g.ptr(o+48)).lower().endswith(suffix.lower())]
        if not found:raise ValueError('Missing original native clip: '+suffix)
        return found[0]
    pound_clip=clip('mickey_head_stomp_jump_down.hkx' if actor=='mickey' else 'oswald_jump_spin.hkx')
    ground_clip=clip('mickey_attack_spin.hkx' if actor=='mickey' else 'oswald_attack_spin.hkx')
    air_clip=clip('mickey_attack_jump_brush.hkx' if actor=='mickey' else 'oswald_jump_spin.hkx')
    wildcard=g.ptr(root+112);wildentries=[g.ptr(wildcard+8)+i*60 for i in range(g.u(wildcard+12))]
    start_template=next((o for o in wildentries if g.u(o+40)==events.index('EVENT_Start_Stomping')),wildentries[0])
    land_template=next(o for o in g.transitions(by_name['InAir']) if g.u(o+40)==events.index('EVENT_Start_Land'))
    info_template=states[0];new_ids=[];report=[]
    highest=max(g.u(o+60) for o in states)
    for i,(name,source,event) in enumerate([('PCGroundPound',pound_clip,custom[0]),('PCDashGround',ground_clip,custom[2]),('PCDashAir',air_clip,custom[3])]):
        state_id=highest+i+1;new_ids.append(state_id)
        info=g.clone(info_template,80)
        for field in [8,12,24,28,40,44]:g.ref(info+field,None)
        for field in [16,32]:g.put(info+field,0)
        for field in [20,36]:g.put(info+field,0x80000000)
        animation=g.clone(source);g.ref(animation+32,g.text(name+'Clip'))
        # Spin's original playback binding follows the combat controller.
        # The movement state has an independent, constant playback rate.
        g.ref(animation+8,None,True);g.put(animation+68,0x3f800000)
        if i>0:g.data[animation+82]=1  # native LOOPING clip mode
        # These are movement animations, not a spin-attack state: no attack
        # activation/deactivation/particle callbacks may escape the new clip.
        g.ref(animation+52,None,True)
        g.ref(info+52,animation,True);g.ref(info+56,g.text(name));g.put(info+60,state_id)
        exits=[(land_template,events.index('EVENT_Start_Land'),land,True)]
        if i==0:exits.append((land_template,indexes[custom[1]],air,True))
        else:exits.extend([(land_template,indexes[custom[4]],run,True),(land_template,indexes[custom[5]],air,True)])
        transitions=g.transition_array(g.ptr(by_name['InAir']+48),exits);g.ref(info+48,transitions,True)
        states.append(info);report.append(dict(state=name,id=state_id,clip=g.string(g.ptr(source+48))))
    existing=[(o,g.u(o+40),g.u(o+44)) for o in wildentries]
    additional=[(start_template,indexes[name],new_ids[i],True) for i,name in enumerate([custom[0],custom[2],custom[3]])]
    g.ref(root+112,g.transition_array(wildcard,existing+additional),True);g.array(root,100,states)
    result=g.encode();check=Graph(result)
    if len(check.elements(root,100))!=len(states) or check.u(stringdata+12)!=len(events)+len(custom) or check.u(graphdata+48)!=len(events)+len(custom):raise ValueError('Graph round-trip failed')
    for state in states[-3:]:
        animation=check.ptr(state+52)
        if animation+52 in check.refs or check.u(animation+52)!=0:raise ValueError('Dash/pound clip retained combat triggers')
    for state in original_states:
        if check.data[state:state+80]!=original_data[state:state+80]:raise ValueError('Original player state was modified')
    for source,(section,target,_) in check.refs.items():
        if section==2 and target>=len(check.data):raise ValueError('Dangling Havok data reference')
    return result,dict(actor=actor,states=report,events=indexes,command_aliases=aliases,original_bytes=len(blob),patched_bytes=len(result))


def patch_emr(blob, ability_clips):
    """Install provided clips while leaving every retail state/Oswald intact.

    ability_clips is the native-path->bytes map returned by emr_ability_clips.
    Existing registered ability IDs own dash/pound and local end triggers.
    Sprint uses two already registered IDs absent from the original graph.
    """
    paths={Path(name.replace('\\','/')).stem.lower():name for name in ability_clips}
    required={'emr_dash','emr_sprint','emr_pound_start','emr_pound_drop','emr_pound_hold','emr_land_hard'}
    if set(paths)!=required:raise ValueError('All six converted EMR clips are required')
    patched,report=patch(blob,'mickey');g=Graph(patched)
    root=next(o for o in g.objects('hkbStateMachine') if g.string(g.ptr(o+32)).lower()=='mickeybehaviorgraph')
    states=g.elements(root,100);by_name={g.string(g.ptr(o+56)):o for o in states}
    stringdata=g.objects('hkbBehaviorGraphStringData')[0];names=g.elements(stringdata,8);events=[g.string(p) for p in names]
    sprint_names=['EVENT_Start_AttackChargeLoop','EVENT_Start_AttackChargeEnd']
    if any(name in events for name in sprint_names):raise ValueError('Sprint IDs already used by the original graph')
    g.array(stringdata,8,names+[g.text(name) for name in sprint_names],False)
    graphdata=g.objects('hkbBehaviorGraphData')[0];infos=g.ptr(graphdata+44)
    expanded=g.append(g.data[infos:infos+len(events)*4]+bytes(8));g.ref(graphdata+44,expanded)
    g.put(graphdata+48,len(events)+2);g.put(graphdata+52,0x80000000|len(events)+2)
    events+=sprint_names;event=lambda name:events.index(name)
    start=by_name['PCGroundPound'];dash_ground=by_name['PCDashGround'];dash_air=by_name['PCDashAir']
    run=g.u(by_name['RunWalkState']+60);air=g.u(by_name['InAir']+60);idle=g.u(by_name['Idle']+60)
    transitions_template=g.ptr(start+48);transition_template=g.transitions(start)[0]
    clip_template=g.ptr(start+52);trigger_template=g.objects('hkbClipTriggerArray')[0]
    highest=max(g.u(s+60) for s in states)
    extra={}
    for i,name in enumerate(['PCGroundPoundDrop','PCGroundPoundHold','PCGroundPoundLand','PCSprint']):
        info=g.clone(start,80);g.ref(info+56,g.text(name));g.put(info+60,highest+i+1)
        animation=g.clone(clip_template,192);g.ref(info+52,animation,True);states.append(info);extra[name]=info
    g.ref(start+56,g.text('PCGroundPoundStart'))
    def state_id(info):return g.u(info+60)
    def animation(info,stem,loop=False,crop_start=0.,crop_end=0.,finish=None):
        clip=g.ptr(info+52);g.ref(clip+32,g.text(stem+'Clip'))
        # The HKW lists paths relative to Mickey/Animations; the graph uses
        # the same case-insensitive native resource-relative spelling.
        g.ref(clip+48,g.text('Animations\\pc\\'+stem+'.hkx'))
        for offset,value in [(56,crop_start),(60,crop_end),(64,0.),(68,1.),(72,0.),(76,0.)]:
            struct.pack_into('>f',g.data,clip+offset,value)
        struct.pack_into('>h',g.data,clip+80,-1);g.data[clip+82]=int(loop);g.data[clip+83]=0
        g.ref(clip+8,None,True);g.ref(clip+52,None,True)
        if finish:
            trigger=g.clone(trigger_template,20)
            # Native reflected hkbClipTrigger is16bytes: localTime,eventId,
            # payload,relativeToEnd,acyclic,isAnnotation,pad.
            record=g.append(struct.pack('>fII4B',-.001,event(finish),0,1,1,0,0))
            g.ref(trigger+8,record);g.put(trigger+12,1);g.put(trigger+16,0x80000001)
            g.ref(clip+52,trigger,True)
        return clip
    def exits(info,pairs):
        array=g.transition_array(transitions_template,[(transition_template,event(name),target,True) for name,target in pairs])
        g.ref(info+48,array,True)
        for entry in [g.ptr(array+8)+i*60 for i in range(g.u(array+12))]:
            # Prefer the owning pound state's landing over the root's normal
            # LAND wildcard, while all native damage/death interrupts remain.
            if g.u(entry+40)==event('EVENT_Start_Land'):struct.pack_into('>h',g.data,entry+56,1)
    done_ground='EVENT_Finish_AttackCharge';done_air='EVENT_Finish_AttackLeap'
    drop=extra['PCGroundPoundDrop'];hold=extra['PCGroundPoundHold'];land=extra['PCGroundPoundLand'];sprint=extra['PCSprint']
    # The converter split the duplicate Start/Drop's common .3s authored
    # motion into two .15s resources. No crop-relative trigger ambiguity.
    animation(start,'emr_pound_start',finish=done_ground)
    animation(drop,'emr_pound_drop',finish=done_air)
    animation(hold,'emr_pound_hold',loop=True)
    animation(land,'emr_land_hard',finish=done_ground)
    for info,next_event,next_id in [(start,done_ground,state_id(drop)),(drop,done_air,state_id(hold))]:
        exits(info,[(next_event,next_id),('EVENT_Start_Land',state_id(land)),('EVENT_AbortAttack',air)])
    exits(hold,[('EVENT_Start_Land',state_id(land)),('EVENT_AbortAttack',air)])
    exits(land,[(done_ground,idle),('EVENT_Start_Running_On_Ground',run),('EVENT_Start_Jump',g.u(by_name['StartJump']+60)),('EVENT_Start_Fall',air)])
    for info in (dash_ground,dash_air):
        animation(info,'emr_dash')
        exits(info,[(done_ground,run),(done_air,air),('EVENT_Start_Land',run)])
    animation(sprint,'emr_sprint',loop=True)
    exits(sprint,[(sprint_names[1],run),('EVENT_Stop_Running_On_Ground',idle),('EVENT_Start_Fall',air),('EVENT_Start_Jump',g.u(by_name['StartJump']+60))])
    wildcard=g.ptr(root+112);existing=[g.ptr(wildcard+8)+i*60 for i in range(g.u(wildcard+12))]
    # Use an original effect with a following serialized object; the newest
    # legacy ability effect can be the final finish-record in the pack.
    template=existing[0]
    g.ref(root+112,g.transition_array(wildcard,[(t,g.u(t+40),g.u(t+44)) for t in existing]+[(template,event(sprint_names[0]),state_id(sprint),True)]),True)
    g.array(root,100,states);result=g.encode();check=Graph(result)
    authored=[]
    for info in [start,drop,hold,land,dash_ground,dash_air,sprint]:
        clip=check.ptr(info+52);path=check.string(check.ptr(clip+48))
        if not path.startswith('Animations\\pc\\') or check.u(clip+68)!=0x3f800000 or clip+8 in check.refs:
            raise ValueError('Authored ability clip lost constant playback/resource binding')
        triggers=check.ptr(clip+52) if clip+52 in check.refs else None
        if triggers is not None:
            record=check.ptr(triggers+8)
            if check.u(triggers+12)!=1 or check.u(record+4) not in [event(done_ground),event(done_air)] or record+8 in check.refs:
                raise ValueError('Unexpected combat/event trigger in authored movement clip')
        authored.append(dict(state=check.string(check.ptr(info+56)),id=state_id(info),clip=path,loop=bool(check.data[clip+82])))
    report.update(states=authored,sprint_events=dict(start=sprint_names[0],stop=sprint_names[1]),authored_emr=True,patched_bytes=len(result),clip_resources=sorted(paths.values()))
    return result,report


def append_emr_hkw(blob, ability_clips):
    """Preserve all native HKW project/dependency lines and append six clips."""
    text=blob.decode('utf-8');newline='\r\n' if '\r\n' in text else '\n'
    lines=text.splitlines();existing={line.lower().replace('\\','/') for line in lines}
    names=[]
    for path in ability_clips:
        clean=path.replace('\\','/').lower();prefix='characters/mickey/animations/'
        if not clean.startswith(prefix) or not clean.endswith('.hkx'):raise ValueError('Invalid Mickey animation resource path')
        relative=clean[len(prefix):]
        if relative in existing:raise ValueError('EMR clip is already in HKW')
        names.append(relative.replace('/','\\'))
    if len(names)!=6 or len(set(names))!=6:raise ValueError('Expected six distinct EMR HKW entries')
    return blob+(b'' if blob.endswith(b'\n') else newline.encode())+(newline.join(sorted(names))+newline).encode()


def append_emr_character(blob, ability_clips):
    """Append matching animation name/file slots to the native character."""
    g=Graph(blob);objects=g.objects('hkbCharacterStringData')
    if len(objects)!=1:raise ValueError('Expected one native character string table')
    owner=objects[0];count=g.u(owner+36)
    if g.u(owner+48)!=count:raise ValueError('Character animation name/file slots disagree')
    pointers=[]
    for offset in (32,44):
        data=g.ptr(owner+offset);pointers.append([g.ptr(data+i*4) if data+i*4 in g.refs else None for i in range(count)])
    paths=['Animations\\pc\\'+Path(name.replace('\\','/')).name for name in sorted(ability_clips)]
    if len(paths)!=6 or len(set(paths))!=6:raise ValueError('Expected six distinct native clips')
    existing={g.string(p).lower() for p in pointers[0] if p is not None}
    if any(path.lower() in existing for path in paths):raise ValueError('Character already contains EMR animation bindings')
    for offset,original in zip((32,44),pointers):g.array(owner,offset,original+[g.text(path) for path in paths],False)
    result=g.encode();check=Graph(result)
    if check.u(owner+36)!=count+6 or check.u(owner+48)!=count+6:raise ValueError('Native character binding roundtrip failed')
    return result


def append_emr_project(blob, ability_clips):
    """Add actual clip dependencies to the original native project resource."""
    g=Graph(blob);objects=g.objects('hkbProjectStringData')
    if len(objects)!=1:raise ValueError('Expected one native project string table')
    owner=objects[0];existing=g.elements(owner,8);names={g.string(p).lower() for p in existing}
    paths=['Animations\\pc\\'+Path(name.replace('\\','/')).name for name in sorted(ability_clips)]
    if len(paths)!=6 or len(set(paths))!=6 or any(p.lower() in names for p in paths):raise ValueError('Invalid/duplicate project clip dependencies')
    g.array(owner,8,existing+[g.text(path) for path in paths],False)
    result=g.encode();check=Graph(result)
    if check.u(owner+12)!=len(existing)+6:raise ValueError('Native project dependency roundtrip failed')
    return result
