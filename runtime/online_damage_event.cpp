#include "online_damage_event.h"
#include <rex/logging.h>
#include <algorithm>
#include <bit>
#include <cmath>
namespace em2::net {
namespace {
bool Id(const DamageEntityId& id){return std::any_of(id.begin(),id.end(),[](auto value){return value!=0;});}
bool Float(uint32_t bits){return std::isfinite(std::bit_cast<float>(bits));}
void Put(std::vector<uint8_t>& out,uint32_t value,unsigned bytes=4){for(unsigned i=bytes;i;--i)out.push_back(uint8_t(value>>((i-1)*8)));}
struct Reader {
    std::span<const uint8_t> bytes;size_t at=0;bool good=true;
    uint32_t Get(unsigned count=4){if(count>bytes.size()-at){good=false;return 0;}uint32_t value=0;while(count--)value=(value<<8)|bytes[at++];return value;}
};
}
bool ValidDamageEvent(const DamageEvent& value){
    return value.target_peer<=16 && value.instigator_peer<=16 && (bool(value.target_peer)!=Id(value.target)) &&
        (!value.instigator || Id(*value.instigator)) && (!value.instigator_peer || !value.instigator) && value.flags[0]<=1 && value.flags[1]<=1 &&
        std::all_of(value.vectors.begin(),value.vectors.end(),Float) && !value.stimuli.empty() && value.stimuli.size()<=kMaxDamageStimuli &&
        std::all_of(value.stimuli.begin(),value.stimuli.end(),[](const auto& s){return s.type<=31 && Float(s.raw_bits) && Float(s.effective_bits);}) &&
        value.shape_keys.size()<=kMaxDamageShapeKeys && (value.tag_present || value.tag.empty()) && value.tag.size()<=kMaxDamageTag && value.tag.find('\0')==std::string::npos;
}
std::vector<uint8_t> EncodeDamageEvent(const DamageEvent& value){
    if(!ValidDamageEvent(value))return {};
    std::vector<uint8_t> out{'B','W','D','E',5};out.reserve(kMaxDamageEventBytes);
    Put(out,value.instigator.has_value(),1);for(auto word:value.target)Put(out,word);
    if(value.instigator)for(auto word:*value.instigator)Put(out,word);
    for(auto flag:value.flags)Put(out,flag,1);for(auto word:value.vectors)Put(out,word);
    Put(out,uint32_t(value.stimuli.size()),1);
    for(const auto& s:value.stimuli){Put(out,s.type);Put(out,s.raw_bits);Put(out,s.effective_bits);}
    Put(out,value.target_peer,1);Put(out,value.instigator_peer,1);
    Put(out,uint32_t(value.shape_keys.size()),2);for(auto key:value.shape_keys)Put(out,key);
    Put(out,value.tag_present,1);Put(out,uint32_t(value.tag.size()),2);out.insert(out.end(),value.tag.begin(),value.tag.end());return out;
}
bool DecodeDamageEvent(std::span<const uint8_t> bytes,DamageEvent& out){
    if(bytes.size()>kMaxDamageEventBytes)return false;
    Reader r{bytes};DamageEvent value;
    if(r.Get()!=0x42574445 || r.Get(1)!=5)return false;
    const auto instigator=r.Get(1);if(instigator>1)return false;
    for(auto& word:value.target)word=r.Get();
    if(instigator){DamageEntityId id;for(auto& word:id)word=r.Get();value.instigator=id;}
    for(auto& flag:value.flags)flag=uint8_t(r.Get(1));for(auto& word:value.vectors)word=r.Get();
    const auto count=r.Get(1);if(!count || count>kMaxDamageStimuli)return false;
    for(unsigned i=0;i<count;++i)value.stimuli.push_back({r.Get(),r.Get(),r.Get()});
    value.target_peer=uint8_t(r.Get(1));value.instigator_peer=uint8_t(r.Get(1));
    const auto shapes=r.Get(2);if(!r.good || shapes>kMaxDamageShapeKeys)return false;
    for(unsigned i=0;i<shapes;++i)value.shape_keys.push_back(r.Get());
    const auto tag_present=r.Get(1);if(!r.good || tag_present>1)return false;value.tag_present=tag_present!=0;
    const auto tag=r.Get(2);if(!r.good || tag>kMaxDamageTag || tag!=bytes.size()-r.at)return false;
    value.tag.assign(reinterpret_cast<const char*>(bytes.data()+r.at),tag);r.at+=tag;
    if(!ValidDamageEvent(value))return false;out=std::move(value);return true;
}
bool TestDamageEventCodec(){
    unsigned checks=0;auto check=[&](bool good,const char* why){++checks;if(!good)REXLOG_ERROR("Damage event codec: {}",why);return good;};
    DamageEvent value;value.target={0x10203040,2,3,4};value.instigator=DamageEntityId{5,6,7,8};
    value.flags={1,0};value.vectors[0]=0x80000000;value.vectors[1]=0x3f800001;
    value.stimuli={{2,0x447A0000,0x43FA0000},{0,0x3EAAAAA6,0x3EAAAAA6}};value.tag="PaintDamage";
    value.shape_keys={0x3152,0x161E,UINT32_MAX,0,0x80000000};
    const auto bytes=EncodeDamageEvent(value);DamageEvent out;
    if(!check(!bytes.empty() && DecodeDamageEvent(bytes,out) && out==value,"target/instigator GUIDs, flags, float bits, raw and adjusted amounts and tag round-trip exactly"))return false;
    const auto sentinel=out;
    auto bad_shapes=bytes;
    const auto shape_count_offset=bytes.size()-value.tag.size()-3-value.shape_keys.size()*4-2;
    bad_shapes[shape_count_offset]=uint8_t((kMaxDamageShapeKeys+1)>>8);
    bad_shapes[shape_count_offset+1]=uint8_t(kMaxDamageShapeKeys+1);
    if(!check(!DecodeDamageEvent(bad_shapes,out) && out==sentinel,"oversized wire child-key count rejected before allocation"))return false;
    auto bad_tag=bytes;const auto tag_present_offset=bytes.size()-value.tag.size()-3;
    bad_tag[tag_present_offset]=2;
    if(!check(!DecodeDamageEvent(bad_tag,out) && out==sentinel,"invalid tag-presence boolean rejected transactionally"))return false;
    bad_tag[tag_present_offset]=0;
    if(!check(!DecodeDamageEvent(bad_tag,out) && out==sentinel,"absent wire tag cannot smuggle nonempty tag bytes"))return false;
    for(size_t n=0;n<bytes.size();++n)if(!check(!DecodeDamageEvent(std::span(bytes).first(n),out) && out==sentinel,"every truncated prefix leaves the destination unchanged"))return false;
    for(unsigned mutation=0;mutation<8;++mutation){auto bad=bytes;
        if(mutation==0)bad[0]^=1;if(mutation==1)bad[4]=1;if(mutation==2)bad[5]=2;
        if(mutation==3)bad[38]=2; // First native boolean flag.
        if(mutation==4){bad[40]=0x7f;bad[41]=0xc0;bad[42]=0;bad[43]=0;}
        if(mutation==5)bad.push_back(0);if(mutation==6)bad.resize(kMaxDamageEventBytes+1);
        if(mutation==7)std::fill(bad.begin()+6,bad.begin()+22,0);
        if(!check(!DecodeDamageEvent(bad,out) && out==sentinel,"malformed identity, boolean, nonfinite vector, format and trailing/oversized payload rejected transactionally"))return false;
    }
    value.instigator.reset();value.tag.assign(kMaxDamageTag,'x');value.stimuli.assign(kMaxDamageStimuli,{31,0x80000000,0});
    value.shape_keys.assign(kMaxDamageShapeKeys,UINT32_MAX);
    if(!check(DecodeDamageEvent(EncodeDamageEvent(value),out) && out==value,"maximum bounded payload and null instigator retain signed-zero bits"))return false;
    value.stimuli[0].type=32;if(!check(EncodeDamageEvent(value).empty(),"unknown stimulus kinds are not silently converted"))return false;
    value.stimuli[0].type=31;value.shape_keys.push_back(0);
    if(!check(EncodeDamageEvent(value).empty(),"oversized child shape arrays are rejected"))return false;
    value.shape_keys.clear();value.tag_present=false;value.tag.clear();
    if(!check(DecodeDamageEvent(EncodeDamageEvent(value),out) && out==value,"null native tag remains distinct from an interned empty tag"))return false;
    value.tag="invalid";if(!check(EncodeDamageEvent(value).empty(),"absent tag cannot carry nonempty bytes"))return false;
    value.tag.clear();value.target={};value.target_peer=1;value.instigator_peer=16;
    if(!check(DecodeDamageEvent(EncodeDamageEvent(value),out) && out==value,"stable lobby player references survive without process-local GUIDs"))return false;
    value.target_peer=17;if(!check(EncodeDamageEvent(value).empty(),"out-of-range lobby player reference rejected"))return false;
    value.target_peer=1;value.target[0]=1;if(!check(EncodeDamageEvent(value).empty(),"ambiguous player and GUID target rejected"))return false;
    value.target={};value.instigator=DamageEntityId{1,0,0,0};
    if(!check(EncodeDamageEvent(value).empty(),"ambiguous player and GUID instigator rejected"))return false;
    REXLOG_INFO("Damage event codec passed {} portable GUID/bit-preservation, boundary and malformed-payload checks; no native replay or network delivery assertion",checks);return true;
}
}
