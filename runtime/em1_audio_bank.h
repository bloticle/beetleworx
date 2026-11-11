#pragma once
#include "em1_rvz.h"
#include <memory>
#include <algorithm>
#include <span>
#include <string>
#include <vector>
#include <map>
#include <stdexcept>
#include <cstdint>
namespace em2::em1 {
struct WiiAudioBank {
  struct Chunk {std::string tag;uint32_t offset=0,size=0;};
  struct Object {uint32_t kind=0,id=0,offset=0,size=0;};
  struct Media {uint32_t id=0,offset=0,size=0;};
  std::shared_ptr<const std::vector<uint8_t>> bytes;
  std::string source_origin;
  uint32_t id=0,language=0,flags=0;
  std::vector<Chunk> chunks;
  std::vector<Object> objects;
  std::vector<Media> media;
};
// Owns original bytes; unknown chunks and type-specific identities are retained.
// A bank container is not an interpreted/playable event graph.
inline bool ReadWiiAudioBank(std::shared_ptr<const std::vector<uint8_t>> bytes,WiiAudioBank& output,std::string& error){
  try {
    auto check=[](bool value,const char* why){if(!value)throw std::runtime_error(why);};
    check(bytes && !bytes->empty() && bytes->size()<=256ull*1024*1024,"Invalid audio bank size");
    auto word=[&](size_t at){check(at<=bytes->size() && bytes->size()-at>=4,"Truncated bank integer");return uint32_t((*bytes)[at])<<24|uint32_t((*bytes)[at+1])<<16|uint32_t((*bytes)[at+2])<<8|(*bytes)[at+3];};
    WiiAudioBank result;result.bytes=bytes;std::map<std::string,WiiAudioBank::Chunk> known;
    for(size_t at=0;at<bytes->size();){
      check(result.chunks.size()<65536,"Audio bank chunk limit exceeded");
      check(bytes->size()-at>=8,"Truncated bank chunk header");
      const std::string tag(reinterpret_cast<const char*>(bytes->data()+at),4);const auto size=word(at+4);
      check(size<=bytes->size()-at-8,"Truncated bank chunk");
      WiiAudioBank::Chunk chunk{tag,uint32_t(at+8),size};result.chunks.push_back(chunk);
      if(tag=="BKHD" || tag=="HIRC" || tag=="DIDX" || tag=="DATA")check(known.emplace(tag,chunk).second,"Duplicate bank control chunk");
      at+=8+size;
    }
    check(known.contains("BKHD"),"Missing BKHD");const auto header=known.at("BKHD");
    check(header.size>=16 && word(header.offset)==46,"Unsupported bank version/metadata");
    result.id=word(header.offset+4);result.language=word(header.offset+8);result.flags=word(header.offset+12);
    if(known.contains("HIRC")){
      const auto h=known.at("HIRC");check(h.size>=4,"Truncated HIRC count");const auto count=word(h.offset);
      check(count<=1000000 && count<=(h.size-4)/12,"Invalid HIRC count");
      size_t at=h.offset+4,end=size_t(h.offset)+h.size;
      for(uint32_t i=0;i<count;++i){
        check(end-at>=12,"Truncated HIRC object");const auto kind=word(at),size=word(at+4);
        check(size>=4 && size<=end-at-8,"Invalid HIRC object size");
        result.objects.push_back({kind,word(at+8),uint32_t(at+12),size-4});at+=8+size;
      }
      check(at==end,"Unaccounted HIRC bytes");
    }
    if(known.contains("DIDX")){
      const auto index=known.at("DIDX");check(index.size%12==0 && index.size/12<=1000000 && known.contains("DATA"),"Invalid DIDX/DATA");
      const auto data=known.at("DATA");
      for(size_t at=index.offset;at<size_t(index.offset)+index.size;at+=12){
        const auto offset=word(at+4),size=word(at+8);
        check(offset<=data.size && size<=data.size-offset,"Media exceeds DATA");
        result.media.push_back({word(at),data.offset+offset,size});
      }
    }
    output=std::move(result);error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
inline bool WiiEventActions(const WiiAudioBank& bank,uint32_t id,std::vector<uint32_t>& output,std::string& error){
  try {
    const WiiAudioBank::Object* event=nullptr;
    for(const auto& object:bank.objects)if(object.id==id && object.kind==4){if(event)throw std::runtime_error("Ambiguous audio event");event=&object;}
    if(!event || !bank.bytes || event->size<4 || uint64_t(event->offset)+event->size>bank.bytes->size())throw std::runtime_error("Missing/invalid audio event");
    const auto* b=bank.bytes->data()+event->offset;
    auto word=[&](size_t at){return uint32_t(b[at])<<24|uint32_t(b[at+1])<<16|uint32_t(b[at+2])<<8|b[at+3];};
    const auto count=word(0);if(uint64_t(count)*4+4!=event->size)throw std::runtime_error("Invalid event action list");
    std::vector<uint32_t> result;result.reserve(count);for(size_t i=0;i<count;++i)result.push_back(word(4+i*4));
    output=std::move(result);error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
inline bool ReadOriginalWiiAudioBank(RvzSource& source,std::string name,WiiAudioBank& output,std::string& error){
  for(auto& c:name)if(c>='A' && c<='Z')c+=32;
  if(name.empty() || name.size()>240 || name.find("..")!=std::string::npos ||
     name.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789/.-")!=std::string::npos || !name.ends_with(".bnk")){
    error="Invalid original soundbank name";return false;
  }
  if(!name.starts_with("audio/wii/")){
    if(name.find('/')!=std::string::npos){error="Soundbank must belong to original Audio/Wii";return false;}
    name="audio/wii/"+name;
  }
  const auto file=std::find_if(source.Files().begin(),source.Files().end(),[&](const auto& value){
    auto path=value.path;for(auto& c:path)if(c>='A' && c<='Z')c+=32;return path==name;
  });
  if(file==source.Files().end() || !file->size || file->size>256ull*1024*1024){error="Original soundbank missing/oversized: "+name;return false;}
  auto bytes=std::make_shared<std::vector<uint8_t>>();if(!source.ReadFile(file->path,*bytes,error))return false;
  WiiAudioBank result;if(!ReadWiiAudioBank(bytes,result,error))return false;
  result.source_origin=source.SourceIdentity()+":audio-wii46:"+name;
  output=std::move(result);return true;
}

}
