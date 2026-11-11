#pragma once
#include "em1_audio_parameters.h"
namespace em2::em1 {
struct WiiBusDefinition {
  struct Duck {uint32_t bus=0;float volume=0;int32_t fade_out=0,fade_in=0;uint8_t curve=0;};
  struct Point {float x=0,y=0;uint32_t interpolation=0;};
  struct Rtpc {uint32_t plugin=0,control=0,parameter=0,curve=0;uint8_t rendered=0,scaling=0;std::vector<Point> points;};
  struct State {uint32_t id=0,instance=0;uint8_t custom=0;};
  std::shared_ptr<const std::vector<uint8_t>> owner;
  std::string source_origin;
  uint32_t id=0,parent=0,state_group=0,state_sync=0;
  std::array<float,4> parameters{};
  uint8_t kill_newest=0,override_maximum=0,unused1=0,unused2=0;
  uint16_t maximum=0;
  int32_t recovery=0;float maximum_duck=0;
  std::vector<Duck> ducks;
  std::vector<Rtpc> rtpc;
  std::vector<State> states;
};
// Wii46 CAkBus serialized schema. Separate from ordinary parameter nodes:
// no66-byte NodePrefix, and states use a32-bit count. Preserve all route,
// ducking and control definitions; parsing does not activate/ignore them.
inline bool ReadWiiBusDefinition(const WiiAudioBank& bank,uint32_t id,WiiBusDefinition& output,std::string& error){
  try{
    if(bank.flags&1)throw std::runtime_error("Wii audio bus feedback requires its own reader");
    const auto bytes=audio_source_detail::Object(bank,8,id);size_t at=0;
    auto take=[&](size_t count){if(count>bytes.size()-at)throw std::runtime_error("Truncated original audio bus");auto value=bytes.subspan(at,count);at+=count;return value;};
    auto byte=[&](){return take(1)[0];};
    auto half=[&](){auto b=take(2);return uint16_t(uint16_t(b[0])<<8|b[1]);};
    auto word=[&](){return audio_source_detail::Word(take(4),0);};
    auto number=[&](){auto value=std::bit_cast<float>(word());if(!std::isfinite(value))throw std::runtime_error("Nonfinite original bus parameter");return value;};
    auto count=[&](uint32_t value,size_t stride){if(value>65536 || value>(bytes.size()-at)/stride)throw std::runtime_error("Invalid original audio bus count");return value;};
    WiiBusDefinition result;result.owner=bank.bytes;result.source_origin=bank.source_origin;result.id=id;result.parent=word();
    for(auto& value:result.parameters)value=number();
    result.kill_newest=byte();result.maximum=half();result.override_maximum=byte();result.unused1=byte();result.unused2=byte();
    result.state_group=word();result.recovery=int32_t(word());result.maximum_duck=number();result.state_sync=word();
    auto n=count(word(),17);for(uint32_t i=0;i<n;++i)result.ducks.push_back({word(),number(),int32_t(word()),int32_t(word()),byte()});
    if(byte()!=0)throw std::runtime_error("Original audio bus effects require their own reader/runtime");
    n=count(half(),20);
    for(uint32_t i=0;i<n;++i){WiiBusDefinition::Rtpc r;r.plugin=word();r.rendered=byte();r.control=word();r.parameter=word();r.curve=word();r.scaling=byte();
      auto points=count(half(),12);for(uint32_t j=0;j<points;++j)r.points.push_back({number(),number(),word()});result.rtpc.push_back(std::move(r));}
    n=count(word(),9);for(uint32_t i=0;i<n;++i){WiiBusDefinition::State state;state.id=word();state.custom=byte();state.instance=word();result.states.push_back(state);}
    if(at!=bytes.size())throw std::runtime_error("Unconsumed original audio bus fields");
    output=std::move(result);error.clear();return true;
  }catch(const std::exception& failure){error=failure.what();return false;}
}
// Resolve through the source bank set, with source/mount identity checked on
// every ancestor. A same-ID retail bus or different-ROM bank cannot fill a gap.
inline bool ReadWiiBusRoute(std::span<const std::shared_ptr<const WiiAudioBank>> banks,uint32_t id,
    std::vector<WiiBusDefinition>& output,std::string& error){
  try{
    if(!id || banks.empty())throw std::runtime_error("Missing original audio bus route");
    std::string source;std::set<uint32_t> visited;std::vector<WiiBusDefinition> route;
    while(id){
      if(route.size()>=64 || !visited.insert(id).second)throw std::runtime_error("Cyclic/oversized original bus route");
      const WiiAudioBank* owner=nullptr;
      for(const auto& bank:banks)if(bank){
        for(const auto& object:bank->objects)if(object.id==id && object.kind==8){
          if(owner)throw std::runtime_error("Ambiguous original audio bus identity");owner=bank.get();
        }
      }
      if(!owner)throw std::runtime_error("Missing original audio bus ancestor");
      const auto separator=owner->source_origin.find(":audio-wii46:");
      if(separator==std::string::npos || !separator)throw std::runtime_error("Bus route lacks original source identity");
      const auto identity=owner->source_origin.substr(0,separator);
      if(source.empty())source=identity;else if(source!=identity)throw std::runtime_error("Audio bus route crosses original mounts");
      WiiBusDefinition bus;if(!ReadWiiBusDefinition(*owner,id,bus,error))return false;
      id=bus.parent;route.push_back(std::move(bus));
    }
    output=std::move(route);error.clear();return true;
  }catch(const std::exception& failure){error=failure.what();return false;}
}
struct WiiAudioDefaultControls {
  std::shared_ptr<const std::vector<uint8_t>> owner;
  std::string source_origin;
  std::map<uint32_t,float> values;
};
// Initial, quiescent source-bus parameters. Dynamic states, duck transitions,
// effects and nonzero scaled curves require the live control evaluator; they
// never silently become zero/unity. This is not an active bus scheduler.
inline bool EvaluateWiiInitialBusParameters(std::span<const WiiBusDefinition> route,
    const WiiAudioDefaultControls& controls,std::array<float,4>& output,std::string& error){
  try{
    const auto marker=controls.source_origin.find(":audio-wii46:");
    if(!controls.owner || marker==std::string::npos || !marker || route.empty())throw std::runtime_error("Missing source-owned initial bus context");
    const auto identity=controls.source_origin.substr(0,marker);std::array<float,4> result{};
    for(const auto& bus:route){
      if(!bus.owner || !bus.source_origin.starts_with(identity+":audio-wii46:") || bus.state_group || !bus.states.empty() || !bus.ducks.empty())
        throw std::runtime_error("Original initial bus state/ducking context is unsupported or crosses mounts");
      for(size_t i=0;i<4;++i){if(!std::isfinite(bus.parameters[i]))throw std::runtime_error("Nonfinite initial bus parameter");result[i]+=bus.parameters[i];}
      std::set<std::pair<uint32_t,uint32_t>> keys;
      for(const auto& r:bus.rtpc){
        if(r.plugin || r.parameter>3 || r.points.empty() || !keys.emplace(r.parameter,r.curve).second || !controls.values.contains(r.control))
          throw std::runtime_error("Unsupported/missing original initial bus control");
        const auto input=controls.values.at(r.control);float value=r.points.back().y;
        for(size_t i=0;i<r.points.size();++i){const auto& a=r.points[i];
          if(!std::isfinite(a.x) || !std::isfinite(a.y) || (i && a.x<=r.points[i-1].x))throw std::runtime_error("Invalid original initial bus curve");
        }
        if(input<=r.points.front().x)value=r.points.front().y;
        else for(size_t i=0;i+1<r.points.size();++i){const auto& a=r.points[i];const auto& b=r.points[i+1];
          if(input<b.x){
            if(a.interpolation==9)value=a.y;
            else if(a.interpolation==4){const float phase=(input-a.x)/(b.x-a.x);const float difference=b.y-a.y;value=a.y+phase*difference;}
            else throw std::runtime_error("Original bus curve interpolation needs its source lookup tables");
            break;
          }
        }
        // Native803763f0 explicitly returns exact0 for input0. Nonzero
        // scaling2 uses the original generated conversion table, not dB
        // passed through unchanged or a guessed linear volume percentage.
        if(r.scaling!=0 && !(r.scaling==2 && value==0))throw std::runtime_error("Original bus curve scaling needs its source lookup table");
        result[r.parameter]+=value;
      }
    }
    for(auto value:result)if(!std::isfinite(value))throw std::runtime_error("Initial bus parameter overflow");
    output=result;error.clear();return true;
  }catch(const std::exception& failure){error=failure.what();return false;}
}
// Init46 STMG includes the original game-parameter defaults. These cannot be
// replaced with all-zero controls: the opening SFX-volume RTPC defaults to100.
inline bool ReadWiiAudioDefaultControls(const WiiAudioBank& bank,WiiAudioDefaultControls& output,std::string& error){
  try{
    const WiiAudioBank::Chunk* chunk=nullptr;
    for(const auto& c:bank.chunks)if(c.tag=="STMG"){if(chunk)throw std::runtime_error("Ambiguous original global audio settings");chunk=&c;}
    if(!chunk || !bank.bytes || uint64_t(chunk->offset)+chunk->size>bank.bytes->size())throw std::runtime_error("Missing original global audio settings");
    const auto bytes=std::span(*bank.bytes).subspan(chunk->offset,chunk->size);size_t at=0;
    auto take=[&](size_t n){if(n>bytes.size()-at)throw std::runtime_error("Truncated original global audio settings");auto b=bytes.subspan(at,n);at+=n;return b;};
    auto word=[&](){return audio_source_detail::Word(take(4),0);};
    auto count=[&](uint32_t n,size_t stride){if(n>65536 || n>(bytes.size()-at)/stride)throw std::runtime_error("Invalid original global audio settings count");return n;};
    take(4);auto groups=count(word(),16);
    for(uint32_t i=0;i<groups;++i){take(8);auto states=count(word(),12);
      for(uint32_t j=0;j<states;++j){take(8);auto size=word();take(size);}
      auto transitions=count(word(),12);take(size_t(transitions)*12);
    }
    auto switches=count(word(),12);
    for(uint32_t i=0;i<switches;++i){take(8);auto points=count(word(),12);take(size_t(points)*12);}
    WiiAudioDefaultControls result;result.owner=bank.bytes;result.source_origin=bank.source_origin;
    auto controls=count(word(),8);for(uint32_t i=0;i<controls;++i){const auto id=word();const auto value=std::bit_cast<float>(word());
      if(!std::isfinite(value) || !result.values.emplace(id,value).second)throw std::runtime_error("Invalid/duplicate original default audio control");}
    if(at!=bytes.size())throw std::runtime_error("Unconsumed original global audio settings");
    output=std::move(result);error.clear();return true;
  }catch(const std::exception& failure){error=failure.what();return false;}
}
inline bool TestWiiAudioBusOwnership(){
  auto bytes=std::make_shared<std::vector<uint8_t>>();
  auto word=[&](uint32_t v){for(int shift=24;shift>=0;shift-=8)bytes->push_back(uint8_t(v>>shift));};
  WiiAudioBank bank;bank.bytes=bytes;bank.source_origin="source-a:audio-wii46:audio/wii/init.bnk";
  auto bus=[&](uint32_t id,uint32_t parent,float volume){auto first=uint32_t(bytes->size());
    word(parent);word(std::bit_cast<uint32_t>(volume));for(size_t i=0;i<3;++i)word(0);
    bytes->insert(bytes->end(),6,0);word(0);word(1000);word(0xc2c0999a);word(0);word(0);
    bytes->push_back(0);bytes->insert(bytes->end(),2,0);word(0);
    bank.objects.push_back({8,id,first,uint32_t(bytes->size())-first});};
  bus(1,0,-2);bus(2,1,-4);
  auto start=uint32_t(bytes->size());word(0);word(0);word(0);word(1);word(0x7e0741ab);word(std::bit_cast<uint32_t>(100.f));
  bank.chunks.push_back({"STMG",start,uint32_t(bytes->size())-start});
  std::string error;WiiAudioDefaultControls controls;
  if(!ReadWiiAudioDefaultControls(bank,controls,error) || controls.values.at(0x7e0741ab)!=100)return false;
  std::array<std::shared_ptr<const WiiAudioBank>,1> owners{std::make_shared<WiiAudioBank>(bank)};
  std::vector<WiiBusDefinition> route;
  if(!ReadWiiBusRoute(owners,2,route,error) || route.size()!=2 || route[0].parameters[0]!=-4 || route[1].parameters[0]!=-2)return false;
  std::array<float,4> parameters;
  if(!EvaluateWiiInitialBusParameters(route,controls,parameters,error) || parameters!=std::array<float,4>{-6,0,0,0})return false;
  auto stateful=route;stateful[0].state_group=9;const auto unchanged=parameters;
  if(EvaluateWiiInitialBusParameters(stateful,controls,parameters,error) || parameters!=unchanged)return false;
  const auto saved=route;
  if(ReadWiiBusRoute(owners,3,route,error) || route.size()!=2 || route[0].owner!=saved[0].owner)return false;
  auto cycle=std::make_shared<WiiAudioBank>(bank);auto altered=std::make_shared<std::vector<uint8_t>>(*bytes);cycle->bytes=altered;
  (*altered)[3]=2;owners[0]=cycle;if(ReadWiiBusRoute(owners,2,route,error))return false;
  auto foreign=std::make_shared<WiiAudioBank>(bank);foreign->source_origin="source-b:audio-wii46:audio/wii/init.bnk";
  foreign->objects.erase(foreign->objects.begin()+1);auto child=std::make_shared<WiiAudioBank>(bank);child->objects.erase(child->objects.begin());
  std::array<std::shared_ptr<const WiiAudioBank>,2> mixed{child,foreign};
  if(ReadWiiBusRoute(mixed,2,route,error))return false;
  owners[0]=std::make_shared<WiiAudioBank>(bank);bytes.reset();
  if(!ReadWiiBusRoute(owners,2,route,error) || !route[0].owner)return false;
  return true;
}
}
