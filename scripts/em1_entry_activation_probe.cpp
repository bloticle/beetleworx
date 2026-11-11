// Developer DLL: compile entry links using the exact runtime implementation.
#include "em1_level_registry.h"
#include <cstdio>
static std::vector<uint8_t> output;
static std::string error;
extern "C" __declspec(dllexport) const char* entry_links(const uint8_t* bytes,size_t size,int shared,
    const char** names,const int* interior,size_t count,const uint8_t** data,size_t* length){
  using namespace em2::em1;
  try {
    CompiledLevel registry;if(!ReadCompiledLevel({bytes,size},registry,error))return error.c_str();
    LevelPlan plan;plan.origin_key="explicit-source-plan-fixture";
    LevelEntryBindings bindings;bindings.level="em1_activation_probe";bindings.state=bindings.level+".entry";
    uint32_t id=0;for(const auto& zone:registry.zones)id=std::max(id,zone.metadata[0]);
    auto zone=[&](std::string name){CompiledLevel::Zone value;value.name=std::move(name);char folder[40];
      std::snprintf(folder,sizeof(folder),"/zone@%08u/",++id);value.folder=folder;value.metadata={id,0,0,0,0};registry.zones.push_back(std::move(value));};
    if(shared){plan.shared_scenes.emplace_back();bindings.shared_zone=bindings.level+".shared";zone(bindings.shared_zone);}
    for(size_t i=0;i<count;++i){LevelPartition part;part.name=names[i];plan.partitions.push_back(part);
      plan.entry_partitions.push_back({part.name,interior[i]?LevelEntryPartition::Mode::Interior:LevelEntryPartition::Mode::Initial});
      auto target=bindings.level+".part_"+std::to_string(i);zone(target);bindings.partitions.emplace(part.name,target);}
    if(!AppendLevelEntryState(plan,bindings,registry,error) || !WriteCompiledLevel(registry,output,error))return error.c_str();
    *data=output.data();*length=output.size();return nullptr;
  }catch(const std::exception& failure){error=failure.what();return error.c_str();}
}
