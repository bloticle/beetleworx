#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace em2::em1 {
// Xbox LVLC v9. Unknown metadata/flags are retained verbatim; this codec does
// not infer EM1 streaming semantics or register resources with the engine.
struct CompiledLevel {
  struct Resource {std::string path,alias;uint32_t reload=0;};
  struct Zone {std::string name,folder;std::array<uint32_t,5> metadata{};std::vector<Resource> files;std::vector<std::string> dependencies;};
  struct Link {uint32_t kind=0;std::string zone;uint32_t flag=0;};
  struct State {std::string name,level;std::vector<Link> links;};
  struct Destination {std::string state;uint32_t flag=0;};
  struct Destinations {std::string owner;std::vector<Destination> entries;};
  uint32_t trusted_measurements=0;
  std::vector<Zone> zones;
  std::vector<State> states;
  std::vector<Destinations> destinations;
};
namespace compiled_level_detail {
inline void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
inline std::string Key(std::string value){for(char& c:value)if(c>='A' && c<='Z')c+='a'-'A';return value;}
inline void Validate(const CompiledLevel& level){
  std::set<std::string> zones,states,owners;
  for(const auto& zone:level.zones)Check(!zone.name.empty() && zones.insert(Key(zone.name)).second,"Duplicate/empty compiled zone");
  for(const auto& state:level.states)Check(!state.name.empty() && states.insert(Key(state.name)).second,"Duplicate/empty compiled state");
  Check(level.destinations.size()==level.states.size(),"Compiled destination table count differs from states");
  for(const auto& row:level.destinations)Check(states.contains(Key(row.owner)) && owners.insert(Key(row.owner)).second,"Unknown/duplicate compiled destination owner");
  // Cross-registry zone/state references are valid in native game data.
}
}
inline bool ReadCompiledLevel(std::span<const uint8_t> bytes,CompiledLevel& output,std::string& error){
  using namespace compiled_level_detail;
  try {
    Check(bytes.size()>=16 && bytes.size()<=32ull*1024*1024,"Invalid compiled level size");
    auto raw=[&](size_t at){Check(at<=bytes.size()-4,"Truncated compiled word");return uint32_t(bytes[at])<<24|uint32_t(bytes[at+1])<<16|uint32_t(bytes[at+2])<<8|bytes[at+3];};
    const size_t pool=raw(8);Check(pool>=16 && pool<=bytes.size() && pool%4==0,"Invalid compiled string pool");
    size_t at=0;
    auto word=[&](){Check(at<=pool-4,"Compiled record crosses string pool");auto value=raw(at);at+=4;return value;};
    auto count=[&](size_t minimum){auto n=word();Check(n<=(pool-at)/minimum,"Invalid compiled record count");return n;};
    auto text=[&](){const auto offset=word();Check(offset<bytes.size()-pool,"Compiled string outside pool");const size_t start=pool+offset;
      auto end=std::find(bytes.begin()+start,bytes.end(),uint8_t(0));Check(end!=bytes.end() && size_t(end-(bytes.begin()+start))<=4096,"Invalid compiled string");
      return std::string(bytes.begin()+start,end);};
    Check(word()==9,"Unsupported compiled level version");CompiledLevel result;result.trusted_measurements=word();word();
    for(auto n=count(36);n;--n){CompiledLevel::Zone zone;zone.name=text();zone.folder=text();
      for(auto& value:zone.metadata)value=word();
      for(auto m=count(12);m;--m){CompiledLevel::Resource resource;resource.path=text();resource.alias=text();resource.reload=word();zone.files.push_back(std::move(resource));}
      for(auto m=count(4);m;--m)zone.dependencies.push_back(text());result.zones.push_back(std::move(zone));}
    for(auto n=count(12);n;--n){CompiledLevel::State state;state.name=text();state.level=text();
      for(auto m=count(12);m;--m){CompiledLevel::Link link;link.kind=word();link.zone=text();link.flag=word();state.links.push_back(std::move(link));}result.states.push_back(std::move(state));}
    for(size_t i=0;i<result.states.size();++i){CompiledLevel::Destinations row;row.owner=text();
      for(auto m=count(8);m;--m){CompiledLevel::Destination entry;entry.state=text();entry.flag=word();row.entries.push_back(std::move(entry));}result.destinations.push_back(std::move(row));}
    Check(at==pool,"Unparsed compiled level records");Validate(result);output=std::move(result);error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
inline bool WriteCompiledLevel(const CompiledLevel& level,std::vector<uint8_t>& output,std::string& error){
  using namespace compiled_level_detail;
  try {
    Validate(level);std::vector<uint8_t> bytes,pool;std::map<std::string,uint32_t> offsets;
    constexpr size_t limit=32ull*1024*1024;
    auto word=[&](uint32_t value){Check(bytes.size()+4<=limit,"Compiled records exceed budget");for(int i=24;i>=0;i-=8)bytes.push_back(uint8_t(value>>i));};
    auto count=[&](size_t n){Check(n<=limit/4,"Compiled count exceeds budget");word(uint32_t(n));};
    auto text=[&](const std::string& value){Check(value.size()<=4096 && value.find(char(0))==std::string::npos,"Invalid compiled string");
      auto found=offsets.find(value);if(found!=offsets.end()){word(found->second);return;}
      Check(value.size()+1<=limit-pool.size(),"Compiled strings exceed budget");const auto offset=uint32_t(pool.size());
      offsets.emplace(value,offset);pool.insert(pool.end(),value.begin(),value.end());pool.push_back(0);word(offset);};
    word(9);word(level.trusted_measurements);word(0);count(level.zones.size());
    for(const auto& zone:level.zones){text(zone.name);text(zone.folder);for(auto value:zone.metadata)word(value);
      count(zone.files.size());for(const auto& file:zone.files){text(file.path);text(file.alias);word(file.reload);}
      count(zone.dependencies.size());for(const auto& dependency:zone.dependencies)text(dependency);}
    count(level.states.size());for(const auto& state:level.states){text(state.name);text(state.level);count(state.links.size());
      for(const auto& link:state.links){word(link.kind);text(link.zone);word(link.flag);}}
    for(const auto& row:level.destinations){text(row.owner);count(row.entries.size());for(const auto& entry:row.entries){text(entry.state);word(entry.flag);}}
    Check(pool.size()<=limit-bytes.size(),"Compiled level exceeds budget");auto start=uint32_t(bytes.size());
    for(unsigned i=0;i<4;++i)bytes[8+i]=uint8_t(start>>(24-i*8));bytes.insert(bytes.end(),pool.begin(),pool.end());
    output=std::move(bytes);error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
struct CompiledZoneLink {uint32_t zone=0,kind=0;};
struct CompiledZoneTargets {std::vector<uint32_t> desired;int64_t interior=-1;};
// The target-update stage of Xbox82794EA8..82794FDF. The caller handles the
// same-state shortcut and resolves registry strings to mounted zone indices.
// This computes a plan only; ownership/publication remain with native loading.
inline bool EvaluateCompiledZoneLinks(std::span<const CompiledZoneLink> previous,
    std::span<const CompiledZoneLink> next,std::span<const uint32_t> current,
    CompiledZoneTargets& output,std::string& error){
  using namespace compiled_level_detail;
  try {
    Check(current.size()<=65536 && previous.size()<=65536 && next.size()<=65536,"Compiled transition exceeds bounds");
    CompiledZoneTargets result;result.desired.assign(current.begin(),current.end());
    std::vector<bool> touched(current.size(),false);
    for(const auto& link:next){
      Check(link.zone<current.size(),"Compiled transition references an unmounted zone");
      auto& desired=result.desired[link.zone];touched[link.zone]=true;
      switch(link.kind){
        case 0:case 1:case 2:desired=link.kind;break;
        case 3:if(!desired)desired=2;break;
        case 5:
          if(desired){if(result.interior<0)result.interior=link.zone;
            else {result.desired[size_t(result.interior)]=0;desired=0;}}
          break;
        default:break; // Native4 and unknown operations keep the target.
      }
    }
    if(result.interior>=0 && !result.desired[size_t(result.interior)])result.interior=-1;
    for(const auto& link:previous){
      Check(link.zone<current.size(),"Previous compiled state references an unmounted zone");
      if(!touched[link.zone])result.desired[link.zone]=0;
    }
    output=std::move(result);error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}

}
