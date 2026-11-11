#pragma once
#include "em1_nif_skin.h"
#include <map>
namespace em2::em1 {
using SkinMatrix=std::array<float,16>;
using SkinVector=std::array<float,3>;
// Input poses are already resolved in MODEL space. Parent Qs composition and
// rig bone-order binding must happen before this stage; no world-root transform.
inline bool BuildWiiSkinPalette(const WiiCookedSkin& skin,std::span<const SkinMatrix> poses,
    std::vector<SkinMatrix>& output,std::string& error){
  try {
    nif::Check(poses.size()<=100000 && skin.transforms.size()==poses.size()*16,"Skin/model-pose bone counts differ");
    std::vector<SkinMatrix> result(poses.size());
    for(size_t bone=0;bone<poses.size();++bone){
      const auto& model=poses[bone];const auto* inverse=skin.transforms.data()+bone*16;
      for(size_t i=0;i<16;++i)nif::Check(std::isfinite(model[i]) && std::isfinite(inverse[i]),"Nonfinite skin palette input");
      for(size_t column=0;column<4;++column)for(size_t row=0;row<4;++row){
        float value=0;for(size_t k=0;k<4;++k)value+=model[k*4+row]*inverse[column*4+k];
        nif::Check(std::isfinite(value),"Skin palette overflow");result[bone][column*4+row]=value;
      }
    }
    output=std::move(result);error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
// Compile source-vertex mappings once. Weights are never normalized, and inactive
// negative bone sentinels are preserved semantically by skipping zero weights.
class WiiSkinDeformer {
  std::vector<WiiCookedSkin::Weight> weights_;
  size_t bones_=0;
 public:
  bool Bind(const WiiCookedSkin& skin,std::span<const uint32_t> sources,size_t bones,std::string& error){
    try {
      nif::Check(skin.has_weights && bones && bones<=100000 && sources.size()<=16*1024*1024,"Invalid skin binding dimensions");
      std::vector<WiiCookedSkin::Weight> result;result.reserve(sources.size());
      for(const auto source:sources){
        nif::Check(source<skin.vertex_weights.size() && skin.vertex_weights[source]<skin.weights.size(),"Missing source weight mapping");
        const auto& weight=skin.weights[skin.vertex_weights[source]];bool active=false;
        for(size_t i=0;i<3;++i){const auto value=weight.values[i];nif::Check(std::isfinite(value) && value>=0,"Invalid skin weight");
          if(value!=0){nif::Check(weight.bones[i]>=0 && size_t(weight.bones[i])<bones,"Active weight references absent bone");active=true;}}
        nif::Check(active,"Skin vertex has no active weights");result.push_back(weight);
      }
      weights_=std::move(result);bones_=bones;error.clear();return true;
    }catch(const std::exception& exception){error=exception.what();return false;}
  }
  bool Deform(std::span<const SkinMatrix> palette,std::span<const SkinVector> input,bool translate,
      std::vector<SkinVector>& output,std::string& error)const{
    try {
      nif::Check(palette.size()==bones_ && input.size()==weights_.size() && bones_,"Deformation binding dimensions differ");
      for(const auto& matrix:palette)for(const auto value:matrix)nif::Check(std::isfinite(value),"Nonfinite deformation palette");
      std::vector<SkinVector> result(input.size());
      for(size_t vertex=0;vertex<input.size();++vertex){
        const auto& p=input[vertex];for(const auto value:p)nif::Check(std::isfinite(value),"Nonfinite source vector");
        const auto& weight=weights_[vertex];
        for(size_t i=0;i<3;++i)if(weight.values[i]!=0){const auto& m=palette[size_t(weight.bones[i])];
          for(size_t axis=0;axis<3;++axis)result[vertex][axis]+=weight.values[i]*(m[axis]*p[0]+m[4+axis]*p[1]+m[8+axis]*p[2]+(translate?m[12+axis]:0.f));}
        for(const auto value:result[vertex])nif::Check(std::isfinite(value),"Deformation overflow");
      }
      output=std::move(result);error.clear();return true;
    }catch(const std::exception& exception){error=exception.what();return false;}
  }
};
class WiiMorphDeformer {
  uint32_t width_=0;
  std::vector<float> deltas_;
  std::vector<uint32_t> rows_;
 public:
  bool Bind(const WiiCookedSkin& skin,std::span<const uint32_t> sources,std::string& error){
    try {
      nif::Check(sources.size()<=16*1024*1024 && uint64_t(skin.morph_indices.size())*skin.morph_width*3==skin.morph_deltas.size(),"Invalid morph binding dimensions");
      for(const auto value:skin.morph_deltas)nif::Check(std::isfinite(value),"Nonfinite morph delta");
      std::map<uint32_t,uint32_t> last;
      for(size_t i=0;i<skin.morph_indices.size();++i)last[skin.morph_indices[i]]=uint32_t(i);
      std::vector<uint32_t> rows;rows.reserve(sources.size());
      for(const auto source:sources){const auto found=last.find(source);rows.push_back(found==last.end()?UINT32_MAX:found->second);}
      auto deltas=skin.morph_deltas;width_=skin.morph_width;rows_=std::move(rows);deltas_=std::move(deltas);error.clear();return true;
    }catch(const std::exception& exception){error=exception.what();return false;}
  }
  bool Deform(std::span<const SkinVector> base,std::span<const float> slots,std::vector<SkinVector>& output,std::string& error)const{
    try {
      nif::Check(base.size()==rows_.size() && slots.size()==width_,"Morph input dimensions differ");
      std::array<std::pair<size_t,float>,4> selected;size_t count=0;
      for(size_t i=0;i<slots.size();++i){nif::Check(std::isfinite(slots[i]),"Nonfinite morph slot");if(slots[i]>0 && count<4)selected[count++]={i,slots[i]};}
      std::vector<SkinVector> result(base.begin(),base.end());
      for(size_t vertex=0;vertex<base.size();++vertex){
        for(const auto value:base[vertex])nif::Check(std::isfinite(value),"Nonfinite morph base");
        if(rows_[vertex]!=UINT32_MAX)for(size_t i=0;i<count;++i){const auto [slot,weight]=selected[i];const size_t at=(size_t(rows_[vertex])*width_+slot)*3;
          for(size_t axis=0;axis<3;++axis)result[vertex][axis]=std::fma(deltas_[at+axis],weight,result[vertex][axis]);}
        for(const auto value:result[vertex])nif::Check(std::isfinite(value),"Morph deformation overflow");
      }
      output=std::move(result);error.clear();return true;
    }catch(const std::exception& exception){error=exception.what();return false;}
  }
};

}
