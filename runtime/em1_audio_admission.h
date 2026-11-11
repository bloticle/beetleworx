#pragma once
#include "em1_audio_positioning.h"
namespace em2::em1 {
struct WiiVoiceAdmission {
  uint64_t owner=0;
  uint32_t node=0;
  uint16_t maximum=0;
  bool kill_newest=false;
  float priority=50;
  std::array<uint32_t,64> lineage{};
  uint8_t lineage_size=0;
};
struct WiiVoiceCandidate {
  uint64_t id=0,owner=0;
  float priority=0;
  bool matches=true,stopping=false;
};
// Entries are oldest first. Caller excludes finished voices and supplies actual
// object/ancestry membership. Zero owner/group selects the unscoped list.
inline uint64_t WiiSelectVoiceVictim(float incoming,bool kill_newest,
    std::span<const WiiVoiceCandidate> voices,uint64_t owner,uint32_t group){
  if(!std::isfinite(incoming))return 0;
  float lowest=101;uint64_t selected=0;
  for(const auto& voice:voices){
    if(voice.stopping || (owner && voice.owner!=owner) || (group && !voice.matches))continue;
    if(voice.priority<lowest || (kill_newest && voice.priority==lowest)){lowest=voice.priority;selected=voice.id;}
  }
  return incoming<lowest || (kill_newest && incoming==lowest)?0:selected;
}
inline bool BuildWiiVoiceAdmission(const WiiAudioBank& bank,uint32_t sound,uint64_t owner,float priority,
    WiiVoiceAdmission& output,std::string& error){
  WiiVoiceLimitDefinition definition;if(!ReadWiiVoiceLimitDefinition(bank,sound,definition,error))return false;
  std::vector<WiiOwnedNode> chain;if(!ReadWiiParameterChain(bank,sound,chain,error))return false;
  if(!std::isfinite(priority) || chain.size()>64){error="Invalid audio admission priority/ancestry";return false;}
  WiiVoiceAdmission result;result.owner=owner;result.node=definition.node;result.maximum=definition.maximum;
  result.kill_newest=definition.kill_newest;result.priority=priority;result.lineage_size=uint8_t(chain.size());
  for(size_t i=0;i<chain.size();++i)result.lineage[i]=chain[i].id;
  output=result;error.clear();return true;
}
}
