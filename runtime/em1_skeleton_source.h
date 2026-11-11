#pragma once
#include "em1_havok_pack.h"
#include "em1_skeleton_pose.h"
namespace em2::em1 {
struct WiiSkeletonReference {
  std::string name;
  std::vector<std::string> names;
  std::vector<WiiRigBone> bones;
  std::vector<uint8_t> locked_translations;
  std::vector<std::string> float_slots;
};
// Strict original HK7 BE32 skeleton/reference-pose reader. Other objects and
// animation state remain in the original pack; not instantiated here.
inline bool ReadWiiSkeletonReference(std::span<const uint8_t> bytes,WiiSkeletonReference& output,std::string& error){
  try {
    hkx::Need(bytes.size()<=256ull*1024*1024,"Skeleton pack exceeds budget");hkx::Pack pack(bytes,true);
    hkx::Location skeleton{UINT32_MAX,0};
    for(const auto& [key,name]:pack.classes)if(name=="hkaSkeleton"){
      hkx::Need(skeleton.section==UINT32_MAX,"Multiple skeletons require explicit selection");skeleton={uint32_t(key>>32),uint32_t(key)};
    }
    hkx::Need(skeleton.section!=UINT32_MAX,"No original hkaSkeleton");const auto data=pack.Data(skeleton.section);const size_t base=skeleton.offset;
    hkx::Need(base<=data.size() && data.size()-base>=48,"Truncated hkaSkeleton");
    WiiSkeletonReference result;const auto name=pack.Pointer({skeleton.section,uint32_t(base+8)},true);if(name.section!=UINT32_MAX)result.name=pack.String(name);
    const auto count=hkx::U32(data,base+16);
    hkx::Need(count>0 && count<=4096 && hkx::U32(data,base+28)==count && hkx::U32(data,base+40)==count,"Skeleton reference array counts differ");
    const auto parents=pack.Pointer({skeleton.section,uint32_t(base+12)}),bones=pack.Pointer({skeleton.section,uint32_t(base+24)}),poses=pack.Pointer({skeleton.section,uint32_t(base+36)});
    auto range=[&](hkx::Location value,size_t stride){const auto d=pack.Data(value.section);hkx::Need(value.offset<=d.size() && count<=(d.size()-value.offset)/stride,"Truncated skeleton array");return d.subspan(value.offset,size_t(count)*stride);};
    const auto parent_data=range(parents,2),bone_data=range(bones,8),pose_data=range(poses,48);
    for(uint32_t i=0;i<count;++i){
      WiiRigBone bone;bone.parent=std::bit_cast<int16_t>(uint16_t(uint16_t(parent_data[i*2])<<8|parent_data[i*2+1]));
      hkx::Need(bone.parent==-1 || (bone.parent>=0 && uint32_t(bone.parent)<i),"Invalid/unordered skeleton parent");
      const auto bone_name=pack.Pointer({bones.section,bones.offset+i*8});result.names.push_back(pack.String(bone_name));
      result.locked_translations.push_back(bone_data[i*8+4]);
      for(size_t lane=0;lane<12;++lane){bone.local[lane]=std::bit_cast<float>(hkx::U32(pose_data,size_t(i)*48+lane*4));hkx::Need(std::isfinite(bone.local[lane]),"Nonfinite original reference pose");}
      result.bones.push_back(bone);
    }
    hkx::Need(data.size()-base>=60,"Truncated skeleton float-slot array");
    const auto slot_count=hkx::U32(data,base+52);hkx::Need(slot_count<=4096,"Too many skeleton float slots");
    if(slot_count){
      const auto slots=pack.Pointer({skeleton.section,uint32_t(base+48)});const auto slot_data=pack.Data(slots.section);
      hkx::Need(slots.offset<=slot_data.size() && slot_count<=(slot_data.size()-slots.offset)/4,"Truncated skeleton float slots");
      for(uint32_t i=0;i<slot_count;++i)result.float_slots.push_back(pack.String(pack.Pointer({slots.section,slots.offset+i*4})));
    }
    output=std::move(result);error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
}
