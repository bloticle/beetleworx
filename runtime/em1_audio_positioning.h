#pragma once
#include "em1_audio_parameters.h"
namespace em2::em1 {
struct WiiVoiceSettings {
  uint8_t virtual_queue=0,below_threshold=0;
  uint16_t maximum_instances=0;
  bool kill_newest=false,override_maximum=false,override_virtual=false;
};
struct WiiPositioning {
  struct Vertex {std::array<float,3> position{};int32_t duration=0;};
  struct Path {uint32_t first=0,count=0;std::array<float,2> range{};};
  bool overridden=false,three_d=false,panner=false,spatialized=false,dynamic=false,looping=false,follow=false;
  int16_t center=0;std::array<float,2> pan{};
  uint32_t kind=0,attenuation=0,mode=0;int32_t transition=0;
  std::vector<Vertex> vertices;std::vector<Path> paths;
};
inline bool ReadWiiPositioning(std::span<const uint8_t> bytes,WiiPositioning& position,WiiVoiceSettings& voices,size_t& consumed,std::string& error){
  try {
    size_t at=0;
    auto take=[&](size_t n){if(at>bytes.size() || n>bytes.size()-at)throw std::runtime_error("Truncated audio positioning/advanced settings");auto value=bytes.subspan(at,n);at+=n;return value;};
    auto byte=[&](){return take(1)[0];};auto flag=[&](){return byte()!=0;};
    auto word=[&](){return audio_source_detail::Word(take(4),0);};
    auto number=[&](){auto value=std::bit_cast<float>(word());if(!std::isfinite(value))throw std::runtime_error("Nonfinite positioning parameter");return value;};
    auto count=[&](size_t stride){const auto n=word();if(n>1000000 || n>(bytes.size()-at)/stride)throw std::runtime_error("Invalid audio path count");return n;};
    WiiPositioning result;WiiVoiceSettings settings;
    result.overridden=flag();
    if(result.overridden){
      result.center=std::bit_cast<int16_t>(uint16_t(word()));result.pan={number(),number()};result.three_d=flag();
      if(!result.three_d)result.panner=flag();
      else {
        result.kind=word();result.attenuation=word();result.spatialized=flag();
        if(result.kind==3)result.dynamic=flag();
        else if(result.kind==2){
          result.mode=word();result.looping=flag();result.transition=std::bit_cast<int32_t>(word());result.follow=flag();
          const auto vertices=count(16);result.vertices.reserve(vertices);
          for(uint32_t i=0;i<vertices;++i)result.vertices.push_back({{number(),number(),number()},std::bit_cast<int32_t>(word())});
          const auto paths=count(8);result.paths.reserve(paths);
          for(uint32_t i=0;i<paths;++i)result.paths.push_back({word(),word(),{}});
          for(auto& path:result.paths)path.range={number(),number()};
        }
        // Other native numeric kinds have no extra payload. Retain their
        // identity; the eventual mixer must not reinterpret them as 2D.
      }
    }
    settings.virtual_queue=byte();settings.kill_newest=flag();const auto half=take(2);settings.maximum_instances=uint16_t(uint16_t(half[0])<<8|half[1]);
    settings.below_threshold=byte();settings.override_maximum=flag();settings.override_virtual=flag();
    position=std::move(result);voices=settings;consumed=at;error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
inline bool WiiVirtualPolicy(std::span<const WiiVoiceSettings> nodes,WiiVoiceSettings& output,std::string& error){
  if(nodes.empty()){error="Missing virtual-policy ancestry";return false;}
  for(const auto& node:nodes)if(node.override_virtual){output=node;error.clear();return true;}
  output=nodes.back();error.clear();return true;
}
struct WiiVoiceLimitDefinition {
  uint32_t node=0;uint16_t maximum=0;bool kill_newest=false;
  int8_t base_priority=0,distance_offset=0;
  uint8_t virtual_queue=0,below_threshold=0;
};
inline bool ReadWiiVoiceLimitDefinition(const WiiAudioBank& bank,uint32_t sound,WiiVoiceLimitDefinition& output,std::string& error){
  std::vector<WiiOwnedNode> chain;if(!ReadWiiParameterChain(bank,sound,chain,error))return false;
  WiiVoiceLimitDefinition result;bool maximum=false,priority=false;std::vector<WiiVoiceSettings> policies;
  for(const auto& node:chain){
    WiiPositioning position;WiiVoiceSettings settings;size_t consumed=0;
    if(!ReadWiiPositioning(node.prefix.remaining,position,settings,consumed,error))return false;
    if(!maximum && (settings.override_maximum || !node.prefix.parent)){result.node=node.id;result.maximum=settings.maximum_instances;result.kill_newest=settings.kill_newest;maximum=true;}
    if(!priority && (node.prefix.priority[1] || !node.prefix.parent)){
      result.base_priority=std::bit_cast<int8_t>(node.prefix.priority[0]);result.distance_offset=node.prefix.priority[2]?std::bit_cast<int8_t>(node.prefix.priority[3]):0;priority=true;
    }
    policies.push_back(settings);
  }
  WiiVoiceSettings policy;if(!maximum || !priority || !WiiVirtualPolicy(policies,policy,error))return false;
  result.virtual_queue=policy.virtual_queue;result.below_threshold=policy.below_threshold;output=result;error.clear();return true;
}
}
