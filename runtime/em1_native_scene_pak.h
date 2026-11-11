#pragma once
#include <algorithm>
#include <cstdint>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace em2::em1 {
struct NativeScenePakEntry {
  std::string_view name;
  std::span<const uint8_t> bytes;
  uint32_t resource_type=0; // Native RAW, NIF, KF and KFM factory tags.
  std::span<const uint8_t> packed_bytes={}; // Optional zlib stream; original bytes own decoded size.
};
// Native big-endian v2 PAK, with every adapted entry scene retained. Source
// files remain untouched. This only packages bytes; it does not admit entities.
inline std::shared_ptr<const std::vector<uint8_t>> BuildNativeScenePak(
    std::span<const NativeScenePakEntry> scenes) {
  auto check=[](bool condition,const char* why){if(!condition)throw std::runtime_error(why);};
  constexpr size_t budget=256ull*1024*1024;
  check(!scenes.empty() && scenes.size()<=1024,"Invalid native level scene count");
  struct Record {uint32_t directory,name;size_t aligned;};
  std::vector<Record> records;
  std::vector<uint8_t> strings;
  std::set<std::string> names;
  size_t payload_bytes=0;
  for(const auto& scene:scenes) {
    const bool raw=scene.resource_type==0 && scene.name.ends_with(".bin");
    const bool model=scene.resource_type==0x4E494600 && scene.name.ends_with(".nif");
    const bool keyframe=scene.resource_type==0x4B460000 && scene.name.ends_with(".kf");
    const bool animation=scene.resource_type==0x4B464D00 && scene.name.ends_with(".kfm");
    const bool physics=scene.resource_type==0x484B5800 && scene.name.ends_with(".hkx");
    check(scene.name.starts_with("em1/runtime/") && (raw || model || keyframe || animation || physics) &&
      scene.name.size()<=240 && !scene.bytes.empty() && scene.bytes.size()<=64ull*1024*1024,
      "Invalid native level scene resource");
    std::string canonical(scene.name);
    for(char& c:canonical){
      check(static_cast<unsigned char>(c)>=32 && static_cast<unsigned char>(c)<127 && c!=':' && c!=char(92),
        "Invalid native scene path character");
      if(c>='A' && c<='Z')c+=('a'-'A');
    }
    for(size_t at=0;at<canonical.size();) {
      auto end=canonical.find('/',at);
      auto part=std::string_view(canonical).substr(at,end==std::string::npos?canonical.size()-at:end-at);
      check(!part.empty() && part!="." && part!="..","Invalid native scene path component");
      at=end==std::string::npos?canonical.size():end+1;
    }
    check(names.insert(canonical).second,"Duplicate native scene path");
    auto slash=scene.name.rfind('/');
    check(scene.packed_bytes.empty() || scene.packed_bytes.size()<scene.bytes.size(),"Invalid compressed native resource size");
    const auto stored=scene.packed_bytes.empty()?scene.bytes:scene.packed_bytes;
    Record record{uint32_t(strings.size()),0,(stored.size()+31)&~size_t(31)};
    strings.insert(strings.end(),scene.name.begin(),scene.name.begin()+slash);strings.push_back(0);
    record.name=uint32_t(strings.size());
    strings.insert(strings.end(),scene.name.begin()+slash+1,scene.name.end());strings.push_back(0);
    check(record.aligned<=budget-payload_bytes,"Native level scene archive exceeds memory budget");
    payload_bytes+=record.aligned;records.push_back(record);
  }
  const size_t pool=36+24*scenes.size();
  const size_t payload=(pool+strings.size()+31)&~size_t(31);
  check(payload<=budget-payload_bytes,"Native level scene archive exceeds memory budget");
  auto bytes=std::make_shared<std::vector<uint8_t>>(payload+payload_bytes,0);
  auto word=[&](size_t at,uint32_t value){for(unsigned i=0;i<4;++i)(*bytes)[at+i]=uint8_t(value>>(24-i*8));};
  (*bytes)[0]=' ';(*bytes)[1]='K';(*bytes)[2]='A';(*bytes)[3]='P';
  word(4,2);word(12,32);word(16,uint32_t(payload-32));word(32,uint32_t(scenes.size()));
  std::copy(strings.begin(),strings.end(),bytes->begin()+pool);
  size_t position=payload;
  for(size_t i=0;i<scenes.size();++i) {
    const auto& scene=scenes[i];const auto& record=records[i];const size_t at=36+i*24;
    const auto stored=scene.packed_bytes.empty()?scene.bytes:scene.packed_bytes;
    word(at,uint32_t(scene.bytes.size()));word(at+4,uint32_t(stored.size()));
    word(at+8,uint32_t(record.aligned));word(at+12,record.directory);word(at+16,scene.resource_type);word(at+20,record.name);
    std::copy(stored.begin(),stored.end(),bytes->begin()+position);position+=record.aligned;
  }
  return bytes;
}
}
