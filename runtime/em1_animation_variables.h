#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <functional>
namespace em2::em1 {
// Actual Xbox enum constructor82841078 registers122 VAR_* names in order
// via82840ED8. Read the native program, rather than assume Wii ordinals.
inline bool ReadXboxAnimationVariables(const std::function<bool(uint32_t,uint32_t&)>& word,
    const std::function<bool(uint32_t,std::string&)>& string,
    std::map<std::string,uint32_t>& output,std::string& error){
  uint32_t value=0;
  if(!word(0x82841098,value) || value!=0x2f0a007a ||
      !word(0x828410b8,value) || value!=0x3d60820b ||
      !word(0x828410c0,value) || value!=0x3bcbddb8){error="Unexpected native animation variable constructor";return false;}
  std::map<std::string,uint32_t> result;
  for(uint32_t id=0;id<122;++id){
    const auto call=0x828410c8+id*12;uint32_t a=0,b=0,branch=0;
    if(!word(call-8,a) || !word(call-4,b) || !word(call,branch) || (branch&0xfc000003)!=0x48000001){error="Unreadable native animation variable registration";return false;}
    const auto displacement=int32_t((branch&0x03fffffc)<<6)>>6;
    if(int64_t(call)+displacement!=0x82840ed8){error="Native animation variable registration target changed";return false;}
    const auto symbol=(a&0xffff0000)==0x389e0000 || a==0x7fc4f378?a:b;
    int32_t offset=0;
    if((symbol&0xffff0000)==0x389e0000)offset=int16_t(symbol);
    else if(symbol!=0x7fc4f378){error="Native animation variable symbol instruction changed";return false;}
    std::string name;
    if(!string(uint32_t(0x820addb8ll+offset),name) || !name.starts_with("VAR_") || name.size()>127 ||
        name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_0123456789")!=name.npos || !result.emplace(name,id).second){
      error="Missing/invalid/duplicate native animation variable symbol";return false;
    }
  }
  output=std::move(result);error.clear();return true;
}
inline bool TestXboxAnimationVariables(){
  std::map<uint32_t,uint32_t> words{{0x82841098,0x2f0a007a},{0x828410b8,0x3d60820b},{0x828410c0,0x3bcbddb8}};
  std::map<uint32_t,std::string> strings;
  for(uint32_t i=0;i<122;++i){const auto call=0x828410c8+i*12;
    const auto instruction=0x389e0000u|uint16_t(-int32_t(i*32));
    words[call-4]=instruction;if(i)words[call-8]=0x7fe3fb78;
    words[call]=0x48000001u|(uint32_t(int64_t(0x82840ed8)-call)&0x03fffffc);
    strings[0x820addb8-i*32]=i==99?"VAR_In_Conversation":"VAR_Test_"+std::to_string(i);
  }
  auto word=[&](uint32_t at,uint32_t& out){const auto it=words.find(at);if(it==words.end())return false;out=it->second;return true;};
  auto string=[&](uint32_t at,std::string& out){const auto it=strings.find(at);if(it==strings.end())return false;out=it->second;return true;};
  std::map<std::string,uint32_t> catalog;std::string error;
  if(!ReadXboxAnimationVariables(word,string,catalog,error) || catalog.size()!=122 || catalog.at("VAR_In_Conversation")!=99)return false;
  const auto unchanged=catalog;words[0x828410c8]^=4;
  if(ReadXboxAnimationVariables(word,string,catalog,error) || catalog!=unchanged)return false;
  words[0x828410c8]^=4;strings[0x820addb8-32]="VAR_Test_0";
  if(ReadXboxAnimationVariables(word,string,catalog,error) || catalog!=unchanged)return false;
  return true;
}
}
