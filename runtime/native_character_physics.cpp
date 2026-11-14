#include "native_character_physics.h"
#include "native_entity_probe.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <cstring>
#include <bit>
#include <limits>
#include <utility>

namespace {
using Character=em2::net::GameplayObservation::CharacterPhysics;
struct HostState {unsigned fp=_mm_getcsr();DWORD error=GetLastError();~HostState(){_mm_setcsr(fp);SetLastError(error);}};
bool Readable(uint8_t* base,uint32_t p,size_t size){return base && p>=4096 && size && uint64_t(p)+size<=0x100000000ull && em2::GuestReadBatch::Readable(base+p,size);}
uint32_t Word(uint8_t* base,uint32_t p){return PPC_LOAD_U32(p);}
uint32_t Typed(uint8_t* base,uint32_t entity,unsigned kind,size_t size){
    const auto types=Word(base,entity+88),components=Word(base,entity+36);
    if(!Readable(base,types,kind+1))return 0;
    const auto slot=PPC_LOAD_U8(types+kind);
    if(slot==255 || !Readable(base,components,unsigned(slot)*4+4))return 0;
    const auto face=Word(base,components+unsigned(slot)*4);
    return face>=8 && Readable(base,face-8,size)?face-8:0;
}
struct Binding {uint32_t entity=0,motion=0,physics=0,controller=0,body=0,position=0;Character value;};
bool Sample(uint8_t* base,const Binding& b,Character& out){
    if(!Readable(base,b.entity,96) || !Readable(base,b.motion,388) || !Readable(base,b.physics,180) ||
       !Readable(base,b.controller,20) || !Readable(base,b.body,448) ||
       Word(base,b.motion+48)!=b.entity-8 || Word(base,b.motion+80)!=b.physics ||
       Word(base,b.physics+176)!=b.controller ||
       Word(base,b.controller+16)!=b.body || Word(base,b.body)!=0x82030384 || Word(base,b.body+224)!=0x8202E5CC)return false;
    Character value;for(unsigned i=0;i<4;++i)value.id[i]=Word(base,b.entity+4+i*4);
    value.active=PPC_LOAD_U8(b.physics+145)!=0;
    if(value.active){
        if(!b.position || !Readable(base,b.position,184))return false;
        value.state=Word(base,b.motion+384);
        for(unsigned i=0;i<3;++i){
            value.position[i]=Word(base,b.position+172+i*4);value.velocity[i]=Word(base,b.body+432+i*4);
            if((value.position[i]&0x7f800000)==0x7f800000 || (value.velocity[i]&0x7f800000)==0x7f800000)return false;
        }
    }
    out=value;return true;
}
}
bool em2::ReadNativeCharacterPhysics(uint8_t* base,std::vector<Character>& out){
    HostState host;GuestReadBatch reads;
    if(!Readable(base,0x833DAF54,1) || !Readable(base,0x833DBC3C,1))return false;
    NativeEntitySnapshot entities;const char* error=nullptr;uint32_t address=0;
    // Do not interpret positions of cameras, effects or unrelated entity types.
    if(!ReadNativeEntitySnapshot(base,entities,error,address,false))return false;
    const auto motion_kind=PPC_LOAD_U8(0x833DAF54),position_kind=PPC_LOAD_U8(0x833DBC3C);
    if(motion_kind>=entities.component_count || position_kind>=entities.component_count)return false;
    std::vector<Binding> bindings;std::vector<Character> values;
    for(const auto& entity:entities.records){
        if(!(entity.component_mask[motion_kind/8]&(1u<<(motion_kind%8))))continue;
        Binding b;b.entity=entity.debug_address;b.motion=Typed(base,b.entity,motion_kind,388);
        if(!b.motion || Word(base,b.motion+48)!=b.entity-8)return false;
        b.physics=Word(base,b.motion+80);
        if(!b.physics)continue;
        if(!Readable(base,b.physics,180))return false;
        b.controller=Word(base,b.physics+176);if(!b.controller)continue;
        if(!Readable(base,b.controller,20))return false;
        b.body=Word(base,b.controller+16);if(!b.body)continue;
        if(!Readable(base,b.body,448))return false;
        if(Word(base,b.body)!=0x82030384 || Word(base,b.body+224)!=0x8202E5CC)continue;
        if(bindings.size()==4096)return false;
        b.position=Typed(base,b.entity,position_kind,184);
        if(!Sample(base,b,b.value) || b.value.id!=entity.id)return false;
        bindings.push_back(b);values.push_back(b.value);
    }
    // Check the sampled routes and values again before publishing a whole set.
    // This detects mutations; it does not pin objects against other threads.
    for(const auto& b:bindings){Character current;
        if(Typed(base,b.entity,motion_kind,388)!=b.motion || Typed(base,b.entity,position_kind,184)!=b.position ||
           !Sample(base,b,current) || current!=b.value)return false;
    }
    if(Word(base,0x833DA7E4)!=entities.registry || Word(base,entities.registry+12)!=entities.header_word_12 ||
       PPC_LOAD_U8(0x833DAF54)!=motion_kind || PPC_LOAD_U8(0x833DBC3C)!=position_kind)return false;
    out=std::move(values);return true;
}

bool em2::ReadNativeActorPhysics(uint8_t* base,uint32_t entity,Character& out){
    HostState host;GuestReadBatch reads;
    if(!Readable(base,entity,96) || Word(base,entity)!=0x8208D908 ||
       !Readable(base,0x833DAF54,1) || !Readable(base,0x833DBC3C,1))return false;
    const auto motion_kind=PPC_LOAD_U8(0x833DAF54),position_kind=PPC_LOAD_U8(0x833DBC3C);
    Binding b;b.entity=entity;b.motion=Typed(base,entity,motion_kind,388);
    if(!b.motion)return false;
    b.physics=Word(base,b.motion+80);if(!Readable(base,b.physics,180))return false;
    b.controller=Word(base,b.physics+176);if(!Readable(base,b.controller,20))return false;
    b.body=Word(base,b.controller+16);b.position=Typed(base,entity,position_kind,184);
    Character first,second;
    if(!Sample(base,b,first) || !Sample(base,b,second) || first!=second ||
       Typed(base,entity,motion_kind,388)!=b.motion || Typed(base,entity,position_kind,184)!=b.position ||
       PPC_LOAD_U8(0x833DAF54)!=motion_kind || PPC_LOAD_U8(0x833DBC3C)!=position_kind)return false;
    out=first;return true;
}
bool em2::TestNativeCharacterPhysics(){
    HostState host;
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
    struct Release {uint8_t* p;~Release(){if(p)VirtualFree(p,0,MEM_RELEASE);}}release{base};
    if(!base || !VirtualAlloc(base,0x30000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0x833D0000,0x10000,MEM_COMMIT,PAGE_READWRITE))return false;
    PPC_STORE_U32(0x833DA7E4,0x2000);PPC_STORE_U8(0x833DBAF0,10);PPC_STORE_U8(0x833DBC3C,2);PPC_STORE_U8(0x833DAF54,7);
    PPC_STORE_U32(0x2004,0x827B58D0);PPC_STORE_U32(0x2008,0x82A69948);PPC_STORE_U32(0x200c,4);
    PPC_STORE_U32(0x2010,0x2800);PPC_STORE_U32(0x2014,1);PPC_STORE_U32(0x2800,0x3000);
    for(unsigned i=0;i<4;++i){
        const auto entity=0x4000+i*0x1000,types=0x8000+i*0x100,components=0x9000+i*0x100;
        const auto motion=0x10000+i*0x1000,physics=0x14000+i*0x1000,controller=0x18000+i*0x1000,body=0x1c000+i*0x1000,position=0xc000+i*0x100;
        PPC_STORE_U32(0x3000+i*16,100+i);PPC_STORE_U32(0x3004+i*16,entity);PPC_STORE_U32(0x3008+i*16,i==3?0:0x3010+i*16);
        PPC_STORE_U32(entity,0x8208D908);PPC_STORE_U32(entity+4,i+1);
        PPC_STORE_U32(entity+88,types);PPC_STORE_U32(entity+36,components);std::memset(base+types,255,10);
        PPC_STORE_U8(types+2,0);PPC_STORE_U32(components,position+8);PPC_STORE_U32(position+172,0x3f800000+i);
        if(i==3)continue; // A camera/effect with a transform, not a character.
        PPC_STORE_U8(types+7,1);PPC_STORE_U32(components+4,motion+8);
        PPC_STORE_U32(motion+48,entity-8);PPC_STORE_U32(motion+80,physics);PPC_STORE_U32(motion+384,i);
        // This is Motion's bridge, not a NiComponent with an owner at+48.
        PPC_STORE_U32(physics+176,controller);PPC_STORE_U8(physics+145,1);
        PPC_STORE_U32(controller+16,body);PPC_STORE_U32(body,0x82030384);PPC_STORE_U32(body+224,0x8202E5CC);
    }
    unsigned checks=0;auto check=[&](bool okay,const char* why){++checks;if(!okay)REXLOG_ERROR("Native character physics: {}",why);return okay;};
    const auto before=std::vector<uint8_t>(base,base+0x30000);std::vector<Character> first;
    const auto fp=_mm_getcsr();SetLastError(0x1234);
    if(!check(ReadNativeCharacterPhysics(base,first) && first.size()==3 && first[2].id[0]==3 && first[2].state==2,
        "native registry includes the third NPC beyond the retail player pair") ||
       !check(!std::memcmp(base,before.data(),before.size()) && _mm_getcsr()==fp && GetLastError()==0x1234,"snapshot leaves guest data and host FP/error state unchanged"))return false;
    net::GameplayObservation observed;observed.includes_character_physics=true;observed.characters=first;
    Character single,unrelated=first[0];
    if(!check(ReadNativeActorPhysics(base,0x5000,single) && single==first[1] &&
        !ReadNativeActorPhysics(base,0x7000,unrelated) && unrelated==first[0],"targeted character observation matches world proof and rejects unrelated visual entities without changing output"))return false;
    PPC_STORE_U32(0x11000+48,0x4000-8);
    const bool foreign_rejected=!ReadNativeActorPhysics(base,0x5000,single);
    PPC_STORE_U32(0x11000+48,0x5000-8);
    if(!check(foreign_rejected && single==first[1],"targeted character observation rejects foreign motion ownership without partial output"))return false;
    for(unsigned slot=0;slot<2;++slot){observed.player_ids[slot]=first[slot].id;
        for(unsigned axis=0;axis<3;++axis)observed.players[slot].position[axis]=std::bit_cast<float>(first[slot].position[axis]);}
    const auto digest=net::HashGameplayObservation(99,observed);
    if(!check(bool(digest),"complete character proof includes both identified native players"))return false;
    {net::GameplayObservation legacy;const auto result=net::HashGameplayObservation(99,legacy);
        std::string hex;if(result){constexpr char digits[]="0123456789abcdef";for(auto byte:*result){hex+=digits[byte>>4];hex+=digits[byte&15];}}
        if(!check(hex=="7bdde2747fe071d4322b3649cf26d4321bb7fa81862321a90e4e916741664add",
            "legacy bootstrap/disabled-character observation retains independently calculated v4 SHA256 bytes"))return false;}
    for(unsigned field=0;field<8;++field){auto changed=observed;auto& npc=changed.characters[2];
        if(field<3)npc.position[field]^=1;else if(field<6)npc.velocity[field-3]^=1;else if(field==6)++npc.state;else ++npc.id[3];
        const auto different=net::HashGameplayObservation(99,changed);
        if(!check(different && different!=digest,"NPC identity, every position/velocity axis and motion state affect proof with both player records unchanged"))return false;
    }
    {auto changed=observed;changed.characters.pop_back();if(!check(net::HashGameplayObservation(99,changed)!=digest,"NPC removal changes proof"))return false;}
    {auto changed=observed;changed.characters[2].active=false;changed.characters[2].state=0;changed.characters[2].position={};changed.characters[2].velocity={};
        if(!check(net::HashGameplayObservation(99,changed)!=digest,"proxy activation participates independently of its position"))return false;}
    for(unsigned invalid=0;invalid<9;++invalid){auto changed=observed;
        switch(invalid){
        case 0:changed.includes_character_physics=false;break;
        case 1:changed.characters.resize(1);break;
        case 2:changed.characters[2].id=changed.characters[1].id;break;
        case 3:std::swap(changed.characters[0],changed.characters[1]);break;
        case 4:changed.characters[2].velocity[1]=0x7f800001;break;
        case 5:changed.player_ids[1]=changed.player_ids[0];break;
        case 6:changed.player_ids[0][0]=500;break;
        case 7:changed.characters[2].active=false;break;
        case 8:changed.characters[0].position[0]^=1;break;
        }
        if(!check(!net::HashGameplayObservation(99,changed),"partial, duplicate, unordered, nonfinite, inactive garbage and missing/inconsistent player coverage cannot claim a proof"))return false;
    }
    {auto changed=observed;changed.characters.resize(4097);
        if(!check(!net::HashGameplayObservation(99,changed),"character proof size is bounded before serialization"))return false;}
    auto sample=first;PPC_STORE_U32(0x1e000+436,0x42b40000);
    if(!check(ReadNativeCharacterPhysics(base,sample) && sample[0]==first[0] && sample[1]==first[1] && sample[2].velocity[1]==0x42b40000,"NPC launch velocity is observed with both players unchanged"))return false;
    {auto launched=observed;launched.characters=sample;const auto launch_digest=net::HashGameplayObservation(99,launched);
        if(!check(launch_digest && launch_digest!=digest,"the observed native +90 NPC launch changes the actual proof with both player records unchanged"))return false;}
    const auto stable=sample;PPC_STORE_U32(0x1e000+436,0x7f800001);
    if(!check(!ReadNativeCharacterPhysics(base,sample) && sample==stable && _mm_getcsr()==fp,"nonfinite NPC velocity rejects without partial output or FP exceptions"))return false;
    PPC_STORE_U8(0x16000+145,0);
    if(!check(ReadNativeCharacterPhysics(base,sample) && !sample[2].active && sample[2].velocity==std::array<uint32_t,3>{} && sample[2].state==0,"inactive proxies retain identity without reading their unused motion vectors"))return false;
    PPC_STORE_U32(0xc300+172,0x7f800001);
    if(!check(ReadNativeCharacterPhysics(base,sample),"unrelated visual entity positions cannot invalidate character coverage"))return false;
    const auto retained=sample;DWORD old=0;if(!VirtualProtect(base+0x8000,4096,PAGE_NOACCESS,&old))return false;
    const bool rejected=!ReadNativeCharacterPhysics(base,sample);DWORD ignored=0;VirtualProtect(base+0x8000,4096,old,&ignored);
    if(!check(rejected && sample==retained,"protected component metadata rejects safely and transactionally"))return false;
    REXLOG_INFO("Native character physics passed {} bounded registry/NPC/activation/finite-state and preservation checks; no complete world or live gameplay assertion",checks);return true;
}
