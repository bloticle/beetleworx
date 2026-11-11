#pragma once
#include "em1_nif_container.h"
#include <array>
#include <bit>
#include <cmath>
namespace em2::em1 {
struct WiiCookedSkin {
  struct Weight {std::array<int32_t,3> bones{};std::array<float,3> values{};};
  struct Partition {int32_t start=0,end=0;std::array<uint16_t,10> weights{};};
  uint32_t has_weights=0,morph_width=0,max_partition_index=0;
  std::vector<float> transforms,morph_deltas;
  std::vector<Weight> weights;
  std::vector<uint32_t> vertex_weights;
  std::vector<uint16_t> morph_indices;
  std::vector<uint8_t> mapped_primitives;
  std::vector<Partition> partitions;
};
// Original embedded skin/morph arrays; no flattening or native skin conversion.
inline bool ReadWiiCookedSkin(nif::View bytes,bool big,uint32_t user,WiiCookedSkin& output,std::string& error){
  try {
    using namespace nif;Check(bytes.size()<=kMaximum,"Cooked skin exceeds budget");Cursor c{bytes,0,big};
    auto count=[&](size_t stride){const auto n=c.Word();Check(n<=(bytes.size()-c.at)/stride,"Cooked skin count exceeds block");return n;};
    auto number=[&](){const auto value=std::bit_cast<float>(c.Word());Check(std::isfinite(value),"Nonfinite cooked skin value");return value;};
    WiiCookedSkin result;result.has_weights=c.Word();
    const auto transforms=count(4);result.transforms.reserve(transforms);for(uint32_t i=0;i<transforms;++i)result.transforms.push_back(number());
    const auto weights=count(24);result.weights.reserve(weights);
    for(uint32_t i=0;i<weights;++i){WiiCookedSkin::Weight weight;for(auto& bone:weight.bones)bone=std::bit_cast<int32_t>(c.Word());for(auto& value:weight.values)value=number();result.weights.push_back(weight);}
    const auto vertices=count(4);result.vertex_weights.reserve(vertices);for(uint32_t i=0;i<vertices;++i)result.vertex_weights.push_back(c.Word());
    const auto morphs=c.Word();result.morph_width=c.Word();Check(morphs<=(bytes.size()-c.at)/2,"Cooked morph count exceeds block");const uint64_t values=uint64_t(morphs)*result.morph_width*3;
    Check(values<=(bytes.size()-c.at)/4,"Truncated cooked morph values");result.morph_deltas.reserve(size_t(values));for(uint64_t i=0;i<values;++i)result.morph_deltas.push_back(number());
    Check(morphs<=(bytes.size()-c.at)/2,"Truncated morph indices");result.morph_indices.reserve(morphs);for(uint32_t i=0;i<morphs;++i)result.morph_indices.push_back(c.Short());
    const auto primitives=user<17?uint32_t(c.Short()):c.Word();const auto data=c.Take(primitives);result.mapped_primitives.assign(data.begin(),data.end());
    const auto partitions=count(28);result.partitions.reserve(partitions);
    for(uint32_t i=0;i<partitions;++i){WiiCookedSkin::Partition partition;partition.start=std::bit_cast<int32_t>(c.Word());partition.end=std::bit_cast<int32_t>(c.Word());for(auto& weight:partition.weights)weight=c.Short();result.partitions.push_back(partition);}
    result.max_partition_index=c.Word();Check(c.at==bytes.size(),"Unparsed cooked skin bytes");
    if(result.has_weights)for(const auto index:result.vertex_weights)Check(index<result.weights.size(),"Vertex references absent cooked weight");
    output=std::move(result);error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
// Round-trip source/cache encoding only. This is NOT an EM2 skin adapter.
inline bool WriteWiiCookedSkin(const WiiCookedSkin& skin,bool big,uint32_t user,nif::Bytes& output,std::string& error){
  try {
    using namespace nif;Bytes bytes;
    auto count=[&](size_t size){Check(size<=UINT32_MAX,"Cooked skin count overflow");Number(bytes,uint32_t(size),big);};
    auto number=[&](float value){Check(std::isfinite(value),"Nonfinite cooked skin value");Number(bytes,std::bit_cast<uint32_t>(value),big);};
    Number(bytes,skin.has_weights,big);count(skin.transforms.size());for(const auto value:skin.transforms)number(value);
    count(skin.weights.size());for(const auto& weight:skin.weights){for(const auto bone:weight.bones)Number(bytes,uint32_t(bone),big);for(const auto value:weight.values)number(value);}
    count(skin.vertex_weights.size());for(const auto index:skin.vertex_weights)Number(bytes,index,big);
    Check(uint64_t(skin.morph_indices.size())*skin.morph_width*3==skin.morph_deltas.size(),"Invalid morph dimensions");
    count(skin.morph_indices.size());Number(bytes,skin.morph_width,big);for(const auto value:skin.morph_deltas)number(value);for(const auto index:skin.morph_indices)Number(bytes,index,big,2);
    Check(skin.mapped_primitives.size()<=(user<17?UINT16_MAX:UINT32_MAX),"Mapped primitive count overflow");Number(bytes,uint32_t(skin.mapped_primitives.size()),big,user<17?2:4);Append(bytes,skin.mapped_primitives);
    count(skin.partitions.size());for(const auto& partition:skin.partitions){Number(bytes,uint32_t(partition.start),big);Number(bytes,uint32_t(partition.end),big);for(const auto weight:partition.weights)Number(bytes,weight,big,2);}
    Number(bytes,skin.max_partition_index,big);
    WiiCookedSkin checked;if(!ReadWiiCookedSkin(bytes,big,user,checked,error))return false;
    output=std::move(bytes);error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}

}
