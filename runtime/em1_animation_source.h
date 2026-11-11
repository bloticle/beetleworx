#pragma once
#include "em1_skeleton_source.h"

namespace em2::em1 {
// Original HK7 BE32 clip and authored binding. Compressed samples, annotations,
// extracted motion and other objects stay owned in source_pack. This reader does
// not sample a clip, evaluate behavior graphs or declare it an EM2 HKX resource.
struct WiiBoundSplineClip {
  std::string skeleton_name;
  float duration=0,block_duration=0,inverse_block_duration=0,frame_duration=0;
  uint32_t transform_tracks=0,float_tracks=0,frames=0,blocks=0,max_frames_per_block=0,mask_bytes=0;
  uint8_t blend_hint=0;
  bool has_extracted_motion=false;
  std::vector<int16_t> bone_indices,float_indices;
  std::vector<uint32_t> block_offsets,float_block_offsets,transform_offsets,float_offsets;
  uint32_t data_section=0,data_offset=0,data_size=0;
  std::vector<uint8_t> source_pack;
};
inline bool ReadWiiBoundSplineClip(std::span<const uint8_t> bytes,WiiBoundSplineClip& output,std::string& error){
  try {
    hkx::Need(bytes.size()<=256ull*1024*1024,"Animation pack exceeds budget");hkx::Pack pack(bytes,true);
    hkx::Location binding{UINT32_MAX,0};
    for(const auto& [key,name]:pack.classes)if(name=="hkaAnimationBinding"){
      hkx::Need(binding.section==UINT32_MAX,"Multiple animation bindings require explicit selection");binding={uint32_t(key>>32),uint32_t(key)};
    }
    hkx::Need(binding.section!=UINT32_MAX,"No authored animation binding");
    auto range=[&](hkx::Location at,size_t size){const auto data=pack.Data(at.section);hkx::Need(at.offset<=data.size() && size<=data.size()-at.offset,"Truncated animation object/array");return data.subspan(at.offset,size);};
    auto field=[](hkx::Location base,uint32_t offset){hkx::Need(offset<=UINT32_MAX-base.offset,"Animation field overflow");return hkx::Location{base.section,base.offset+offset};};
    const auto bd=range(binding,41);const auto animation=pack.Pointer(field(binding,12));
    hkx::Need(pack.Class(animation)=="hkaSplineCompressedAnimation","Unsupported authored animation compression");const auto ad=range(animation,128);
    WiiBoundSplineClip result;const auto name=pack.Pointer(field(binding,8),true);if(name.section!=UINT32_MAX)result.skeleton_name=pack.String(name);
    auto scalar=[&](size_t at){const auto value=std::bit_cast<float>(hkx::U32(ad,at));hkx::Need(std::isfinite(value)&&value>=0,"Invalid animation time value");return value;};
    hkx::Need(hkx::U32(ad,8)==5,"Spline class/type mismatch");
    result.duration=scalar(12);result.transform_tracks=hkx::U32(ad,16);result.float_tracks=hkx::U32(ad,20);
    hkx::Need(result.transform_tracks<=4096 && result.float_tracks<=4096,"Too many animation tracks");
    result.has_extracted_motion=pack.Pointer(field(animation,24),true).section!=UINT32_MAX;
    result.frames=hkx::U32(ad,40);result.blocks=hkx::U32(ad,44);result.max_frames_per_block=hkx::U32(ad,48);result.mask_bytes=hkx::U32(ad,52);
    hkx::Need(result.frames>0 && result.frames<=10000000 && result.blocks>0 && result.blocks<=1000000 && result.max_frames_per_block>0,"Invalid animation frame/block counts");
    result.block_duration=scalar(56);result.inverse_block_duration=scalar(60);result.frame_duration=scalar(64);result.blend_hint=bd[40];
    auto indices=[&](uint32_t offset,uint32_t tracks,std::vector<int16_t>& target){
      const auto count=hkx::U32(bd,offset+4);hkx::Need(count==tracks,"Animation binding/track count mismatch");
      if(!count)return;const auto data=range(pack.Pointer(field(binding,offset)),size_t(count)*2);
      for(uint32_t i=0;i<count;++i){const auto value=std::bit_cast<int16_t>(uint16_t(uint16_t(data[i*2])<<8|data[i*2+1]));hkx::Need(value>=-1,"Invalid negative animation binding");target.push_back(value);}
    };
    indices(16,result.transform_tracks,result.bone_indices);indices(28,result.float_tracks,result.float_indices);
    auto offsets=[&](uint32_t offset,std::vector<uint32_t>& target){
      const auto count=hkx::U32(ad,offset+4);hkx::Need(count<=1000000,"Animation offset array exceeds budget");if(!count)return;
      const auto data=range(pack.Pointer(field(animation,offset)),size_t(count)*4);for(uint32_t i=0;i<count;++i)target.push_back(hkx::U32(data,i*4));
    };
    offsets(68,result.block_offsets);offsets(80,result.float_block_offsets);offsets(92,result.transform_offsets);offsets(104,result.float_offsets);
    result.data_size=hkx::U32(ad,120);hkx::Need(result.data_size>0 && result.data_size<=128ull*1024*1024,"Invalid compressed animation data size");
    const auto payload=pack.Pointer(field(animation,116));range(payload,result.data_size);result.data_section=payload.section;result.data_offset=payload.offset;
    hkx::Need(result.block_offsets.size()==result.blocks,"Animation block offset/count mismatch");
    for(size_t i=0;i<result.block_offsets.size();++i)hkx::Need(result.block_offsets[i]<result.data_size && (!i || result.block_offsets[i]>=result.block_offsets[i-1]),"Invalid compressed animation block offset");
    result.source_pack.assign(bytes.begin(),bytes.end());output=std::move(result);error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
// Validate indices against an EXPLICITLY selected destination rig. Exporter
// namespaces differ in actual source files (Mickey_Anim:Tracker versus Tracker).
// Names stay opaque; this does not select a rig or prove bone-order semantics.
inline bool ValidateWiiClipRigIndices(const WiiBoundSplineClip& clip,const WiiSkeletonReference& skeleton,std::string& error){
  try {
    hkx::Need(clip.bone_indices.size()==clip.transform_tracks && clip.float_indices.size()==clip.float_tracks,"Animation binding/track count mismatch");
    for(auto index:clip.bone_indices)hkx::Need(index==-1 || (index>=0 && size_t(index)<skeleton.bones.size()),"Animation bone index exceeds rig");
    for(auto index:clip.float_indices)hkx::Need(index==-1 || (index>=0 && size_t(index)<skeleton.float_slots.size()),"Animation float index exceeds rig");
    error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
}
