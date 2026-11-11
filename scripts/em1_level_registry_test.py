"""Entry-state compilation using original EM1 descriptors and metadata-only zones.
This does not test converted asset bindings, registry installation or gameplay.
"""
import argparse,ctypes as c,json,runpy,struct
from pathlib import Path
parser=argparse.ArgumentParser();parser.add_argument('--library',type=Path,required=True);parser.add_argument('--image',type=Path,required=True);parser.add_argument('--archive',type=Path,required=True);args=parser.parse_args()
raw=next(d for n,d,k in runpy.run_path(str(Path(__file__).with_name('game-pak.py')))['read_pak'](args.archive) if n=='levels/master.lvlc')
def decode(data):
    pool=struct.unpack_from('>I',data,8)[0];at=12
    def word():
        nonlocal at
        assert at+4<=pool
        value=struct.unpack_from('>I',data,at)[0];at+=4;return value
    def text():
        start=pool+word();return data[start:data.index(0,start)].decode()
    zones=[]
    for _ in range(word()):
        name,folder=text(),text();metadata=[word() for _ in range(5)]
        files=[(text(),text(),word()) for _ in range(word())];deps=[text() for _ in range(word())]
        zones.append((name,folder,metadata,files,deps))
    states=[]
    for _ in range(word()):
        name,level=text(),text();links=[(word(),text(),word()) for _ in range(word())];states.append((name,level,links))
    destinations=[(text(),[(text(),word()) for _ in range(word())]) for _ in states]
    assert at==pool
    return zones,states,destinations
original=decode(raw);dll=c.CDLL(str(args.library.resolve()));U=c.POINTER(c.c_ubyte)
dll.prepare.argtypes=[c.c_wchar_p,c.c_char_p,c.POINTER(U),c.POINTER(c.c_size_t)];dll.prepare.restype=c.c_char_p
dll.entry_registry.argtypes=[U,c.c_size_t,c.POINTER(U),c.POINTER(c.c_size_t),c.c_int];dll.entry_registry.restype=c.c_char_p
dll.bind_groups.argtypes=[c.c_int,c.POINTER(c.c_size_t)];dll.bind_groups.restype=c.c_char_p
dll.extension.argtypes=[U,c.c_size_t,U,c.c_size_t];dll.extension.restype=c.c_char_p
source=(c.c_ubyte*len(raw)).from_buffer_copy(raw);catalog=json.loads(Path('analysis/em1-compatibility/level-catalog-audit.json').read_text());results=[]
for level in ['dbc_courtyard_1st.level','dbc_start_1st.level','toontown.level','dbc_start_zonec.level','dbc_start_zoneg.level']:
    pointer=U();length=c.c_size_t();error=dll.prepare(str(args.image),level.encode(),c.byref(pointer),c.byref(length));assert not error,error
    row=next(x for x in catalog['levels'] if x['id']==level)
    error=dll.entry_registry(source,len(raw),c.byref(pointer),c.byref(length),0);assert not error,error
    zones,states,destinations=decode(c.string_at(pointer,length.value))
    assert zones[:len(original[0])]==original[0] and states[:-1]==original[1] and destinations[:-1]==original[2]
    links=[]
    if row['fields'].get('shared'):links.append((1,'em1_probe.shared',0))
    for key in ['initial','interior']:
        if row['fields'].get(key):
            zone='em1_probe.'+row['fields'][key].lower().removesuffix('.part');links.append((1,zone,0))
            if key=='interior':links.append((5,zone,0))
    assert states[-1]==('em1_probe.entry','em1_probe',links)
    assert destinations[-1]==('em1_probe.entry',[])
    bound_count=c.c_size_t()
    assert not dll.bind_groups(0,c.byref(bound_count)), 'Valid converted resource fixture rejected'
    assert bound_count.value>0
    assert dll.bind_groups(2,c.byref(bound_count)), 'Mismatched original scene identity accepted'
    # Fixture soundbanks are names only; no audio decoding/playback is claimed.
    missing_bank=dll.bind_groups(1,c.byref(bound_count))
    if level in ['dbc_courtyard_1st.level','dbc_start_1st.level']:
        assert missing_bank, 'Missing audio binding accepted'
    candidate=c.string_at(pointer,length.value)
    def validate(data):
        buffer=(c.c_ubyte*len(data)).from_buffer_copy(data)
        return dll.extension(source,len(raw),buffer,len(data))
    assert not validate(candidate), 'Valid extension refused'
    for offset in [4,24]:
        changed=bytearray(candidate);changed[offset+3]^=1
        assert validate(changed), 'Changed retail measurement/metadata accepted'
    assert validate(candidate.replace(b'em1_probe.entry',b'bad_probe.entry')), 'Unowned state accepted'
    error=dll.entry_registry(source,len(raw),c.byref(pointer),c.byref(length),1);assert error,'Missing zone binding accepted'
    results.append(dict(level=level,links=links,missing_binding_rejected=True))
Path('analysis/em1-compatibility/level-entry-registry-result.json').write_text(json.dumps(dict(cases=results,scope='Original descriptor roles mapped into state links against metadata-only test zones; retail records unchanged. No asset publication, native world activation, or transition streaming tested.'),indent=2)+'\n')
print('PASS: five entry states; retail registry records unchanged; missing bindings rejected')
