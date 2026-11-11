#pragma once
#include "em1_audio_positioning.h"
#include <functional>
namespace em2::em1 {
// User-defined3D uses zero translation (8037ab8c), unlike game-position
// emitters. Follow controls the listener basis, not an emitter position:
//8037ac64..acc0 chooses current listener axes or original806c1140 axes.
inline bool ProjectWiiUserAudioPath(const std::array<float,3>& position,bool follow,
    const std::array<std::array<float,3>,3>& listener,std::array<float,2>& output,std::string& error){
  const auto half=std::bit_cast<float>(0x3f3504f3u);
  const std::array<std::array<float,3>,3> fixed{{{half,0,half},{0,1,0},{-half,0,half}}};
  const auto& basis=follow?listener:fixed;
  for(auto value:position)if(!std::isfinite(value)){error="Nonfinite user audio path position";return false;}
  for(const auto& row:basis)for(auto value:row)if(!std::isfinite(value)){error="Nonfinite user audio path listener basis";return false;}
  std::array<float,2> result;
  for(size_t i=0;i<2;++i){const auto& row=basis[i*2];
    const float x=row[0]*position[0],y=row[1]*position[1],z=row[2]*position[2];
    const float sum=x+y;result[i]=sum+z;
  }
  if(!std::isfinite(result[0]) || !std::isfinite(result[1])){error="User audio path projection overflow";return false;}
  output=result;error.clear();return true;
}
// Original CAkPath kernels80354fb0/803553b0/803556e0. The caller supplies the
// audio manager clock and shared fifteen-bit C RNG, never render/simulation dt.
// This class returns authored local path positions; listener/follow projection
// and routing remain the voice owner's responsibility.
class WiiUserPath {
  WiiPositioning definition_;
  size_t path_=0,vertex_=0;
  uint32_t start_=0,end_=0,duration_=1;
  float inverse_=1,offset_=0;
  std::array<float,3> base_{},delta_{},position_{};
  bool active_=false;
  static uint32_t Duration(int32_t value){
    if(value<0 || value>INT32_MAX-2)throw std::runtime_error("Unsupported negative/overflowing authored audio path duration");
    return std::max(1u,(uint32_t(value)+2)/3);
  }
  float Random(size_t axis,const std::function<uint16_t()>& random){
    const auto value=random();if(value>32767)throw std::runtime_error("Audio path RNG exceeds fifteen bits");
    // Native806fe6b0 is16383 (not32767), followed by subtracting1.
    const float normalized=float(value)/16383.f;
    return definition_.paths[path_].range[axis]*(normalized-1.f);
  }
  auto Vertex(size_t index)const{return definition_.vertices.at(definition_.paths[path_].first+index);}
  void Clock(uint32_t start){start_=start;end_=start+duration_;inverse_=1.f/float(duration_);offset_=-(float(start)*inverse_);}
  void BeginNext(const std::function<uint16_t()>& random){
    const auto& old_path=definition_.paths[path_];
    if(vertex_<old_path.count){
      auto source=Vertex(vertex_++);base_=source.position;
      base_[0]+=Random(0,random);base_[2]+=Random(1,random);
      duration_=Duration(source.duration);
    }
    if(vertex_>=old_path.count){
      if(!(definition_.mode&2)){active_=false;return;}
      if(++path_>=definition_.paths.size()){
        path_=0;if(!definition_.looping){active_=false;return;}
      }
      vertex_=0;
    }
    auto target=Vertex(vertex_).position;
    target[0]+=Random(0,random);target[2]+=Random(1,random);
    for(size_t i=0;i<3;++i)delta_[i]=target[i]-base_[i];
    Clock(end_);
  }
 public:
  bool Begin(const WiiPositioning& source,uint32_t audio_tick,
      const std::function<uint16_t()>& random,std::string& error){
    try{
      if(!source.overridden || !source.three_d || source.kind!=2 ||
          (source.mode!=0 && source.mode!=2) || source.paths.empty() || source.paths.size()>65535 || source.vertices.empty())
        throw std::runtime_error("Unsupported authored audio path definition/mode");
      WiiUserPath next;next.definition_=source;
      for(const auto& path:source.paths){
        if(!path.count || path.count>65535 || path.first>=source.vertices.size() || path.count>source.vertices.size()-path.first ||
            !std::isfinite(path.range[0]) || !std::isfinite(path.range[1]))throw std::runtime_error("Invalid authored audio path bounds/range");
        //80354670 overwrites every path's final vertex duration, preserving
        // authored order for overlapping paths.
        next.definition_.vertices[path.first+path.count-1].duration=source.transition;
      }
      for(const auto& v:next.definition_.vertices){
        if(!std::all_of(v.position.begin(),v.position.end(),[](float n){return std::isfinite(n);}))throw std::runtime_error("Nonfinite audio path vertex");
        Duration(v.duration);
      }
      auto first=next.Vertex(0);next.base_=first.position;next.duration_=Duration(first.duration);next.vertex_=1;
      if(next.vertex_<source.paths[0].count){
        auto target=next.Vertex(next.vertex_).position;
        if(target==next.base_ && source.paths[0].count<=2){
          next.base_[0]+=next.Random(0,random);next.base_[2]+=next.Random(1,random);target=next.base_;
        }else{
          next.base_[0]+=next.Random(0,random);next.base_[2]+=next.Random(1,random);
          target[0]+=next.Random(0,random);target[2]+=next.Random(1,random);
        }
        for(size_t i=0;i<3;++i)next.delta_[i]=target[i]-next.base_[i];
      }
      next.position_=next.base_;next.active_=true;next.Clock(audio_tick);*this=std::move(next);error.clear();return true;
    }catch(const std::exception& failure){error=failure.what();return false;}
  }
  bool Update(uint32_t audio_tick,const std::function<uint16_t()>& random,std::string& error){
    try{
      if(!active_){error.clear();return true;}
      const float product=inverse_*float(audio_tick);
      const float fraction=std::clamp(offset_+product,0.f,1.f);
      for(size_t i=0;i<3;++i){const float product=fraction*delta_[i];position_[i]=base_[i]+product;}
      if(audio_tick>=end_)BeginNext(random);
      error.clear();return true;
    }catch(const std::exception& failure){active_=false;error=failure.what();return false;}
  }
  const std::array<float,3>& Position()const{return position_;}
  bool Active()const{return active_;}
  size_t PathIndex()const{return path_;}
};
inline bool TestWiiUserAudioPath(){
  WiiPositioning source;source.overridden=source.three_d=true;source.kind=2;
  source.mode=2;source.looping=true;source.transition=9;
  source.vertices={{{0,0,0},6},{{6,3,-3},0},{{12,0,0},12},{{18,0,6},0}};
  source.paths={{0,2,{0,0}},{2,2,{0,0}}};
  // Instruction-executed original main.dol994779fa...80354fb0/803556e0:
  // first interpolation, inter-path transition, wrap, and float rounding.
  constexpr std::array<std::array<uint32_t,3>,17> original{{
    {0,0,0},{1077936128,1069547520,3217031168},{1086324736,1077936128,3225419776},
    {1090519040,1073741824,3221225472},{1092616192,1065353216,3212836864},{1094713344,0,0},
    {1096286208,0,1069547520},{1097859072,0,1077936128},{1099169792,0,1083179008},
    {1099956224,0,1086324736},{1094713341,0,1082130428},{1086324732,0,1073741820},
    {0,0,0},{1077936128,1069547520,3217031168},{1086324736,1077936128,3225419776},
    {1090519036,1073741828,3221225476},{1092616191,1065353220,3212836868}}};
  WiiUserPath path;std::string error;size_t draws=0;
  auto random=[&](){++draws;return uint16_t(16383);};
  if(!path.Begin(source,0,random,error) || draws!=4)return false;
  for(uint32_t tick=0;tick<original.size();++tick){
    if(!path.Update(tick,random,error))return false;
    for(size_t axis=0;axis<3;++axis)if(std::bit_cast<uint32_t>(path.Position()[axis])!=original[tick][axis])return false;
  }
  if(draws!=24 || !path.Active())return false;
  source.looping=false;draws=0;
  if(!path.Begin(source,0,random,error))return false;
  for(uint32_t tick=0;tick<=9;++tick)if(!path.Update(tick,random,error))return false;
  if(path.Active() || path.Position()!=std::array<float,3>{18,0,6})return false;
  const auto retained=path.Position();source.paths[1].count=UINT32_MAX;
  if(path.Begin(source,0,random,error) || path.Position()!=retained)return false;
  source.paths[1].count=2;source.mode=1;
  if(path.Begin(source,0,random,error))return false;
  // Equal endpoints take the original two-draw branch (even zero range).
  source.mode=2;source.looping=true;source.vertices[1].position=source.vertices[0].position;draws=0;
  if(!path.Begin(source,0,random,error) || draws!=2)return false;
  const std::array<std::array<float,3>,3> identity{{{1,0,0},{0,1,0},{0,0,1}}};
  std::array<float,2> projected;
  if(!ProjectWiiUserAudioPath({3,4,5},false,identity,projected,error) ||
      std::bit_cast<uint32_t>(projected[0])!=1085605107 || std::bit_cast<uint32_t>(projected[1])!=1068827892)return false;
  if(!ProjectWiiUserAudioPath({3,4,5},true,identity,projected,error) || projected!=std::array<float,2>{3,5})return false;
  return true;
}
}
