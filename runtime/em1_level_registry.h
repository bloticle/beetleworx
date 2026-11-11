#pragma once
#include "em1_level_source.h"
#include "em1_compiled_level.h"

namespace em2::em1 {
struct LevelEntryBindings {
  std::string level,state,shared_zone;
  std::map<std::string,std::string> partitions;
};
// Preflight for the first native registry parse. Preserve every retail record
// and accept additions only in the EM1 namespace with resolved zone references.
inline bool ValidateLevelRegistryExtension(const CompiledLevel& original,
    const CompiledLevel& candidate,std::string& error){
  using namespace compiled_level_detail;
  try {
    Validate(original);Validate(candidate);
    Check(candidate.zones.size()>=original.zones.size() && candidate.states.size()>=original.states.size() &&
      candidate.destinations.size()>=original.destinations.size(),"Registry extension removes retail records");
    auto prefix=candidate;
    prefix.zones.resize(original.zones.size());prefix.states.resize(original.states.size());
    prefix.destinations.resize(original.destinations.size());
    std::vector<uint8_t> before,after;
    if(!WriteCompiledLevel(original,before,error) || !WriteCompiledLevel(prefix,after,error))return false;
    Check(before==after,"Registry extension changes retail records or measurements");
    auto owned=[](const std::string& name){return name.starts_with("em1_") && name.size()<=240 &&
      name.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789.")==std::string::npos;};
    std::set<std::string> zones,added_states;
    for(const auto& zone:candidate.zones)zones.insert(Key(zone.name));
    for(size_t i=original.zones.size();i<candidate.zones.size();++i){
      const auto& zone=candidate.zones[i];Check(owned(zone.name),"Imported zone lacks EM1 ownership");
      for(const auto& dependency:zone.dependencies)Check(zones.contains(Key(dependency)),"Imported zone dependency is unresolved");
      for(const auto& file:zone.files)Check(!file.path.empty() && !file.alias.empty(),"Imported resource needs both original and alias bindings");
    }
    for(size_t i=original.states.size();i<candidate.states.size();++i){
      const auto& state=candidate.states[i];
      Check(owned(state.name) && owned(state.level) && state.name.starts_with(state.level+"."),"Imported state lacks EM1 ownership");
      Check(!state.links.empty(),"Imported state has no bound resources");
      for(const auto& link:state.links)Check(zones.contains(Key(link.zone)),"Imported state zone is unresolved");
      added_states.insert(Key(state.name));
    }
    for(size_t i=original.destinations.size();i<candidate.destinations.size();++i)
      Check(added_states.contains(Key(candidate.destinations[i].owner)),"Imported destinations do not belong to an added state");
    std::vector<uint8_t> encoded;if(!WriteCompiledLevel(candidate,encoded,error))return false;
    error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
// Compile the entry-state links against explicitly supplied, existing native
// zones. No zones/assets are invented or published here. The caller owns asset
// conversion, group IDs, native residency and registry installation.
inline bool AppendLevelEntryState(const LevelPlan& plan,const LevelEntryBindings& bindings,
    CompiledLevel& registry,std::string& error){
  using namespace compiled_level_detail;
  try {
    Validate(registry);
    auto identifier=[](const std::string& value){return value.starts_with("em1_") &&
      value.size()<=240 && value.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789.")==std::string::npos;};
    Check(identifier(bindings.level) && identifier(bindings.state) && bindings.state.starts_with(bindings.level+"."),
      "EM1 entry state requires qualified native identifiers");
    Check(!plan.origin_key.empty(),"EM1 entry state lacks source identity");
    for(const auto& state:registry.states)Check(Key(state.name)!=Key(bindings.state),"EM1 entry state already registered");
    CompiledLevel::State state;state.name=bindings.state;state.level=bindings.level;
    std::set<std::string> referenced;
    auto activate=[&](const std::string& name){
      Check(identifier(name) && name.starts_with(bindings.level+"."),"EM1 zone is not owned by this level");
      Check(std::any_of(registry.zones.begin(),registry.zones.end(),[&](const auto& zone){return zone.name==name;}),"EM1 entry resource zone is not bound");
      // Retail active entry states use kind1. Kind2 appears on neighboring
      // zones in transition states and only preloads their resources; it is
      // not an active entry load (mainmenu.start and campaign states use1).
      if(referenced.insert(name).second)state.links.push_back({1,name,0});
    };
    if(!plan.shared_scenes.empty())activate(bindings.shared_zone);
    else Check(bindings.shared_zone.empty(),"Unexpected EM1 shared-zone binding");
    std::set<std::string> modes;
    for(const auto& entry:plan.entry_partitions){
      Check(modes.insert(entry.name).second,"Repeated initial/interior partition requires explicit force-load semantics");
      const auto binding=bindings.partitions.find(entry.name);
      Check(binding!=bindings.partitions.end(),"EM1 entry partition has no native resource binding");
      Check(std::any_of(plan.partitions.begin(),plan.partitions.end(),[&](const auto& part){return part.name==entry.name;}),"EM1 entry partition is absent");
      Check(!referenced.contains(binding->second),"Different EM1 resource groups alias one native zone");
      activate(binding->second);
      if(entry.mode==LevelEntryPartition::Mode::Interior)state.links.push_back({5,binding->second,0});
    }
    Check(!state.links.empty(),"EM1 entry state is empty");
    auto candidate=registry;
    candidate.states.push_back(std::move(state));
    candidate.destinations.push_back({bindings.state,{}});
    // Verify native-format bounds before committing any mutation.
    std::vector<uint8_t> bytes;
    if(!WriteCompiledLevel(candidate,bytes,error))return false;
    if(!ValidateLevelRegistryExtension(registry,candidate,error))return false;
    registry=std::move(candidate);error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
}
