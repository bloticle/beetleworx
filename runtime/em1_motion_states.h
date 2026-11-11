#pragma once
#include "em1_scene_files.h"
#include <array>
#include <map>
#include <set>

namespace em2::em1 {
// EM1 keeps movement tuning in its controller. EM2 splits that controller
// into state components. Retain every authored component/property and copy
// matching tuning into the added states; new engine fields use native defaults.
struct MotionStateContext {
  std::set<uint32_t> occupied;
  std::map<std::pair<std::array<uint32_t,3>,std::string>,uint32_t> assigned;
  uint32_t next=0xF1000000;
};
struct MotionStateReport {size_t entities=0,components=0,properties=0;};
namespace motion_state_detail {
struct Property {std::string name;size_t begin,end;};
struct Component {std::string name;uint32_t id;size_t begin,end;std::vector<Property> properties;};
struct Entity {size_t begin,end;std::array<uint32_t,3> id;std::vector<Component> components;};
inline uint32_t Word(std::span<const uint8_t> b,size_t at){
 if(at>b.size() || b.size()-at<4)throw std::runtime_error("Truncated motion-state scene");
 return uint32_t(b[at])<<24|uint32_t(b[at+1])<<16|uint32_t(b[at+2])<<8|b[at+3];
}
inline void Put(std::vector<uint8_t>& out,uint32_t value){for(int shift=24;shift>=0;shift-=8)out.push_back(uint8_t(value>>shift));}
inline std::vector<Entity> Parse(const SceneAdapterResult& scene){
 std::string error;std::vector<SceneFileReference> checked;
 if(!CollectSceneFileReferences(scene,checked,error))throw std::runtime_error(error);
 const auto& b=scene.bytes;const size_t boundary=size_t(Word(b,4))+4;
 auto text=[&](size_t offset){const auto at=size_t(Word(b,offset))+6;return std::string(reinterpret_cast<const char*>(b.data()+at));};
 std::vector<Entity> entities;size_t at=boundary+16;
 for(uint32_t i=0;i<scene.entity_count;++i){Entity entity{at,0,{Word(b,at+4),Word(b,at+8),Word(b,at+12)},{}};
  const auto count=Word(b,at+20);at+=24;
  for(uint32_t j=0;j<count;++j){Component component{text(at),Word(b,at+8),at,0,{}};const auto props=Word(b,at+16);at+=20;
   for(uint32_t k=0;k<props;++k){const auto begin=at;const auto name=text(at),type=text(at+4);const auto mode=Word(b,at+8),amount=Word(b,at+12);
    size_t width=4;if(type=="Point2")width=8;else if(type=="Point3" || type=="Color (RGB)")width=12;else if(type=="Color (RGBA)")width=16;else if(type=="Matrix3")width=36;
    at+=16+width*((mode&1)?amount:1);component.properties.push_back({name,begin,at});}
   component.end=at;entity.components.push_back(std::move(component));
  }
  entity.end=at;entities.push_back(std::move(entity));
 }
 return entities;
}
struct State {std::string_view name,version;std::initializer_list<std::string_view> fields;};
inline const std::array<State,5> states{{
 {"OnGroundStateComponent","3,0,0,0",{"Max Move Speed","GroundControlAccelStop","GroundControlAccel","Ground_FixedSpeeds","Ground_StickValues"}},
 {"InAirStateComponent","2,0,0,0",{"Max Air Speed","AirControlAccel","Character Gravity Down","MinNormalFallSpeed","TerminalSpeed","MinDeathFallSpeed"}},
 {"JumpingStateComponent","2,0,0,0",{"Jump Height","JumpType","JumpHeightMultipliers"}},
 {"KnockbackStateComponent","2,0,0,0",{"KnockbackMaxStim","KnockbackFriction"}},
 {"DamagedLeapStateComponent","2,0,0,0",{"DamagedLeapGravity","DamagedLeapStunTime","DamagedLeapMaxAirSpeed","DamagedLeapAirAccel","DamagedLeapLaunchSpeed"}}
}};
}
inline bool ReserveMotionStateIds(const SceneAdapterResult& scene,MotionStateContext& context,std::string& error){
 try{for(const auto& e:motion_state_detail::Parse(scene))for(const auto& c:e.components)context.occupied.insert(c.id);return true;}
 catch(const std::exception& e){error=e.what();return false;}
}
inline bool AdaptLegacyMotionStates(const SceneAdapterResult& source,MotionStateContext& context,
    SceneAdapterResult& result,MotionStateReport& report,std::string& error){
 using namespace motion_state_detail;
 try{
  const auto entities=Parse(source);const auto& b=source.bytes;const size_t boundary=size_t(Word(b,4))+4;
  std::vector<uint8_t> strings,body;std::map<std::string,uint32_t> offsets;MotionStateReport changed;
  auto intern=[&](std::string_view text){const std::string key(text);if(auto found=offsets.find(key);found!=offsets.end())return found->second;
   const auto start=strings.size();const auto length=text.size()+1;const auto size=(length+5)&~size_t(3);
   if(size>255 || length>255)throw std::runtime_error("Motion-state string exceeds scene encoding");
   const auto offset=uint32_t(boundary-4+start);strings.push_back(uint8_t(size));strings.push_back(uint8_t(length));strings.insert(strings.end(),text.begin(),text.end());strings.resize(start+size,0);offsets.emplace(key,offset);return offset;};
  body.insert(body.end(),b.begin()+boundary,b.begin()+boundary+16);
  for(const auto& entity:entities){const Component* controller=nullptr;std::set<std::string> existing;
   for(const auto& c:entity.components){existing.insert(c.name);if(c.name=="BasicMotionControllerComponent" || c.name=="MickeyPlayerMotionControllerComponent")controller=&c;}
   std::vector<uint8_t> added;uint32_t count=0;
   if(controller)for(const auto& state:states)if(!existing.contains(std::string(state.name))){
    const auto key=std::pair{entity.id,std::string(state.name)};auto found=context.assigned.find(key);
    if(found==context.assigned.end()){
     while(context.occupied.contains(context.next)){if(context.next==UINT32_MAX)throw std::runtime_error("Motion component IDs exhausted");++context.next;}
     const auto id=context.next;context.occupied.insert(id);found=context.assigned.emplace(key,id).first;
    }
    std::vector<const Property*> properties;for(const auto& p:controller->properties)
     if(std::find(state.fields.begin(),state.fields.end(),p.name)!=state.fields.end())properties.push_back(&p);
    Put(added,intern(state.name));Put(added,intern(state.version));Put(added,found->second);Put(added,0);Put(added,uint32_t(properties.size()));
    for(const auto* p:properties)added.insert(added.end(),b.begin()+p->begin,b.begin()+p->end);
    ++count;changed.properties+=properties.size();
   }
   body.insert(body.end(),b.begin()+entity.begin,b.begin()+entity.begin+20);Put(body,uint32_t(entity.components.size())+count);
   body.insert(body.end(),b.begin()+entity.begin+24,b.begin()+entity.end);body.insert(body.end(),added.begin(),added.end());
   if(count){++changed.entities;changed.components+=count;}
  }
  const auto trailer=entities.empty()?boundary+16:entities.back().end;body.insert(body.end(),b.begin()+trailer,b.end());
  if(!changed.components){result=source;report={};error.clear();return true;}
  SceneAdapterResult output=source;output.bytes.assign(b.begin(),b.begin()+boundary);output.bytes.insert(output.bytes.end(),strings.begin(),strings.end());output.bytes.insert(output.bytes.end(),body.begin(),body.end());
  const auto new_end=uint32_t(boundary-4+strings.size());for(unsigned i=0;i<4;++i)output.bytes[4+i]=uint8_t(new_end>>(24-i*8));
  output.component_count+=uint32_t(changed.components);Parse(output);
  result=std::move(output);report=changed;error.clear();return true;
 }catch(const std::exception& e){error=e.what();return false;}
}
}
