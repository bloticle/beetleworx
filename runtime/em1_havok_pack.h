#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>
namespace em2::em1::hkx {
inline void Need(bool value,const char* why){if(!value)throw std::runtime_error(why);}
inline uint32_t U32(std::span<const uint8_t> bytes,size_t at){Need(at<=bytes.size() && bytes.size()-at>=4,"HKX word range");return uint32_t(bytes[at])<<24|uint32_t(bytes[at+1])<<16|uint32_t(bytes[at+2])<<8|bytes[at+3];}
struct Location{uint32_t section,offset;};
struct Section{uint32_t start,local,global,virtuals,exports,imports,end;};
class Pack {
public:
 std::span<const uint8_t> bytes;std::vector<Section> sections;
 std::map<uint64_t,Location> pointers;std::map<uint64_t,std::string> classes;
 static uint64_t Key(Location p){return uint64_t(p.section)<<32|p.offset;}
 std::span<const uint8_t> Data(uint32_t section)const{Need(section<sections.size(),"HKX section index");const auto& s=sections[section];return bytes.subspan(s.start,s.local);}
 std::string String(Location p)const{auto d=Data(p.section);Need(p.offset<d.size(),"HKX string range");auto end=std::find(d.begin()+p.offset,d.end(),0);Need(end!=d.end(),"Unterminated HKX string");return std::string(d.begin()+p.offset,end);}
 Location Pointer(Location p,bool optional=false)const{auto i=pointers.find(Key(p));if(i!=pointers.end())return i->second;Need(optional&&U32(Data(p.section),p.offset)==0,"Missing authored HKX fixup");return {UINT32_MAX,0};}
 std::string Class(Location p)const{auto i=classes.find(Key(p));Need(i!=classes.end(),"Missing authored object class");return i->second;}
 explicit Pack(std::span<const uint8_t> source,bool original):bytes(source){
  Need(bytes.size()>=64&&U32(bytes,0)==0x57E0E057&&U32(bytes,4)==0x10C0C010,"Not a Havok packfile");
  Need(U32(bytes,12)==(original?7u:9u)&&bytes[16]==4&&bytes[17]==0&&bytes[18]==(original?1:0)&&bytes[19]==1,"Unsupported Havok packfile ABI");
  const std::string_view version(reinterpret_cast<const char*>(bytes.data()+40),16);
  Need(version.starts_with(original?"Havok-7.0.0-r1":"hk_2011.2.0-r1"),"Unexpected Havok schema version");
  const auto count=U32(bytes,20);Need(count==3&&bytes.size()>=64+48*count,"Unsupported HKX section table");
  for(uint32_t i=0;i<count;++i){const auto p=84+i*48;Section s{U32(bytes,p),U32(bytes,p+4),U32(bytes,p+8),U32(bytes,p+12),U32(bytes,p+16),U32(bytes,p+20),U32(bytes,p+24)};
   Need(s.local<=s.global&&s.global<=s.virtuals&&s.virtuals<=s.exports&&s.exports<=s.imports&&s.imports<=s.end&&uint64_t(s.start)+s.end<=bytes.size(),"Invalid HKX section spans");sections.push_back(s);}
  auto insert=[&](Location from,Location to){Need(uint64_t(from.offset)+4<=Data(from.section).size()&&to.section<count&&to.offset<Data(to.section).size(),"Invalid HKX pointer fixup");Need(pointers.emplace(Key(from),to).second,"Duplicate HKX pointer fixup");};
  for(uint32_t n=0;n<count;++n){const auto& s=sections[n];
   for(uint32_t p=s.local;p+8<=s.global;p+=8){const auto a=U32(bytes,s.start+p);if(a==UINT32_MAX)break;insert({n,a},{n,U32(bytes,s.start+p+4)});}
   for(uint32_t p=s.global;p+12<=s.virtuals;p+=12){const auto a=U32(bytes,s.start+p);if(a==UINT32_MAX)break;insert({n,a},{U32(bytes,s.start+p+4),U32(bytes,s.start+p+8)});}
   for(uint32_t p=s.virtuals;p+12<=s.exports;p+=12){const auto a=U32(bytes,s.start+p);if(a==UINT32_MAX)break;Need(a<Data(n).size(),"HKX virtual fixup range");Need(classes.emplace(Key({n,a}),String({U32(bytes,s.start+p+4),U32(bytes,s.start+p+8)})).second,"Duplicate HKX virtual fixup");}
  }
 }
};

}
