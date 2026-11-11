#pragma once
#include "em1_audio_source.h"
#include <bit>
#include <cmath>
#include <functional>
#include <set>
namespace em2::em1 {
struct WiiNodePrefix {
  uint8_t override_fx=0;
  uint32_t bus=0,parent=0,state_group=0;
  std::array<uint8_t,4> priority{};
  std::array<std::array<float,3>,4> ranges{};
  std::span<const uint8_t> remaining;
};
inline bool ReadWiiNodePrefix(std::span<const uint8_t> bytes,WiiNodePrefix& output,std::string& error){
  try {
    if(bytes.size()<66 || bytes[1]!=0)throw std::runtime_error("Truncated node or unsupported attached audio FX");
    WiiNodePrefix result;result.override_fx=bytes[0];result.bus=audio_source_detail::Word(bytes,2);result.parent=audio_source_detail::Word(bytes,6);
    std::copy_n(bytes.begin()+10,4,result.priority.begin());
    for(size_t i=0;i<4;++i)for(size_t j=0;j<3;++j){
      const auto value=std::bit_cast<float>(audio_source_detail::Word(bytes,14+(i*3+j)*4));
      if(!std::isfinite(value))throw std::runtime_error("Nonfinite audio node range");result.ranges[i][j]=value;
    }
    result.state_group=audio_source_detail::Word(bytes,62);result.remaining=bytes.subspan(66);output=result;error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
struct WiiOwnedNode {
  uint32_t id=0;
  std::shared_ptr<const std::vector<uint8_t>> owner;
  WiiNodePrefix prefix;
};
inline bool ReadWiiParameterChain(const WiiAudioBank& bank,uint32_t id,std::vector<WiiOwnedNode>& output,std::string& error){
  try {
    std::vector<WiiOwnedNode> result;std::set<uint32_t> visited;
    while(id){
      if(result.size()>=64 || !visited.insert(id).second)throw std::runtime_error("Cyclic/oversized audio parameter ancestry");
      const WiiAudioBank::Object* found=nullptr;
      for(const auto& object:bank.objects)if(object.id==id && (object.kind==2 || object.kind==5 || object.kind==6 || object.kind==7 || object.kind==9)){
        if(found)throw std::runtime_error("Ambiguous audio parameter ancestor");found=&object;
      }
      if(!found)throw std::runtime_error("Missing/unsupported audio parameter ancestor");
      std::span<const uint8_t> bytes;
      if(found->kind==2){WiiEmbeddedDspSound sound;if(!ResolveWiiEmbeddedDspSound(bank,id,sound,error))return false;bytes=sound.node_parameters;}
      else bytes=audio_source_detail::Object(bank,found->kind,id);
      WiiOwnedNode node;node.id=id;node.owner=bank.bytes;if(!ReadWiiNodePrefix(bytes,node.prefix,error))return false;
      id=node.prefix.parent;result.push_back(std::move(node));
    }
    if(result.empty())throw std::runtime_error("Empty audio parameter ancestry");output=std::move(result);error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
struct WiiFixedParameters {int16_t volume=0,lfe=0,pitch=0;uint8_t filter=0;};
inline bool LoadWiiFixedParameters(const std::array<float,4>& values,WiiFixedParameters& output,std::string& error){
  try {
    auto integer=[](float value){if(!std::isfinite(value) || double(value)<-2147483648.0 || double(value)>=2147483648.0)throw std::runtime_error("Unsafe audio integer conversion");return int32_t(value);};
    auto signed16=[](int32_t value){return std::bit_cast<int16_t>(uint16_t(value));};
    auto tenths=[&](float value){value*=10.f;return signed16(integer(value<0?std::ceil(value-.5f):std::floor(value+.5f)));};
    WiiFixedParameters result;result.volume=tenths(values[0]);result.lfe=tenths(values[1]);result.pitch=signed16(integer(values[2]));result.filter=uint8_t(integer(values[3]));
    output=result;error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
inline bool WiiPitchRatio(float pitch,float& output,std::string& error){
  if(!std::isfinite(pitch)){error="Nonfinite audio pitch";return false;}
  const float exponent=std::clamp(pitch,-2400.f,2400.f)/1200.f;
  output=float(std::pow(2.0,double(exponent)));error.clear();return true;
}
using WiiRandomRanges=std::array<std::array<float,2>,4>;
inline bool WiiRandomOffsets(std::span<const WiiRandomRanges> chain,const std::function<uint16_t()>& random,
    std::array<float,4>& output,std::string& error){
  try {
    std::array<float,4> result{};
    for(const auto& ranges:chain)for(const auto channel:{1,2,3,0}){
      const auto minimum=ranges[channel][0],maximum=ranges[channel][1];const float width=maximum-minimum;
      if(!std::isfinite(minimum) || !std::isfinite(maximum) || !std::isfinite(width))throw std::runtime_error("Nonfinite audio random range");
      float value=0;if(width!=0){const auto draw=random();if(draw>32767)throw std::runtime_error("Audio RNG exceeded fifteen bits");value=float(double(draw)/32767.0*double(width));}
      const float offset=value+minimum;result[channel]+=offset;
    }
    output=result;error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
}
