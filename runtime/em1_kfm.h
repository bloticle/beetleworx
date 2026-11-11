#pragma once
#include "em1_nif_container.h"
#include "em1_pack_source.h"
#include <bit>
#include <cmath>
#include <map>

namespace em2::em1 {
struct KfmPathField { size_t offset=0,bytes=0;std::string authored;bool model=false; };
struct KfmDocument {
  bool big=true;uint32_t sequences=0,transitions=0,groups=0;
  std::vector<KfmPathField> paths;
  std::vector<uint32_t> sequence_ids;
};
// Original binary20.6.5.0b schema. Only filename spans are rewritten; root and
// sequence names, IDs, transition policies/blend keys/chains and all group data
// remain original bytes. No animation is removed or replaced with a pose.
inline bool InspectKfm(nif::View input,KfmDocument& output,std::string& error){
  try {
    using namespace nif;constexpr std::string_view header=";Gamebryo KFM File Version 20.6.5.0b\n";
    Check(input.size()>header.size() && input.size()<=4*1024*1024 &&
      std::equal(header.begin(),header.end(),input.begin()),"Unsupported KFM header/bounds");
    Check(input[header.size()]<=1,"Invalid KFM byte order");KfmDocument result;result.big=input[header.size()]==0;
    Cursor c{input,header.size()+1,result.big};
    auto count=[&](size_t minimum){const auto n=c.Word();Check(n<=65536 && n<=(input.size()-c.at)/minimum,"KFM count exceeds remaining data");return n;};
    auto number=[&](){Check(std::isfinite(std::bit_cast<float>(c.Word())),"Nonfinite KFM time/weight");};
    auto text=[&](bool fixed,bool path,bool model=false){const auto start=c.at;const auto size=c.Word();
      if(fixed && std::bit_cast<int32_t>(size)<=0)return;
      Check(size<=(path?2048u:65536u),"KFM string exceeds bounds");const auto bytes=c.Take(size);
      if(path){Check(std::find(bytes.begin(),bytes.end(),0)==bytes.end(),"Embedded null in KFM filename");
        result.paths.push_back({start,c.at-start,std::string(bytes.begin(),bytes.end()),model});}};
    text(false,true,true);text(false,false);c.Word();c.Word();number();number();
    result.sequences=count(16);
    uint64_t transition_budget=0;
    for(uint32_t sequence=0;sequence<result.sequences;++sequence){
      result.sequence_ids.push_back(c.Word());text(false,true);text(false,false);const auto transitions=count(8);
      Check(transition_budget+transitions<=65536,"KFM transition budget exceeded");transition_budget+=transitions;
      for(uint32_t transition=0;transition<transitions;++transition){c.Word();const auto type=c.Word();
        if(type==4 || type==5)continue;
        number();const auto pairs=count(8);for(uint32_t i=0;i<pairs;++i){text(true,false);text(true,false);}
        const auto chain=count(8);for(uint32_t i=0;i<chain;++i){c.Word();number();}
      }
    }
    result.transitions=uint32_t(transition_budget);result.groups=count(12);
    for(uint32_t group=0;group<result.groups;++group){c.Word();text(true,false);const auto members=count(24);
      for(uint32_t member=0;member<members;++member){c.Word();c.Word();number();number();number();c.Word();}}
    Check(c.at==input.size(),"Unparsed KFM payload");output=std::move(result);error.clear();return true;
  }catch(const std::exception& e){error=e.what();return false;}
}

namespace kfm_detail {
inline std::vector<std::string> Parts(std::string_view path){
  using nif::Check;Check(!path.empty() && path.size()<=2048 && path.find(':')==path.npos && path.front()!='/' && path.front()!='\\',"Invalid KFM resource path");
  std::vector<std::string> result;std::string part;
  auto finish=[&](){if(part.empty() || part=="."){part.clear();return;}
    if(part==".."){Check(!result.empty(),"KFM path escapes source root");result.pop_back();}else result.push_back(part);part.clear();};
  for(char ch:path){Check(ch!=0,"Null in KFM resource path");if(ch=='/' || ch=='\\')finish();else part+=ch>='A'&&ch<='Z'?char(ch+'a'-'A'):ch;}
  finish();Check(!result.empty(),"Empty KFM resource path");return result;
}
inline std::string Join(std::span<const std::string> parts){std::string result;for(const auto& part:parts){if(!result.empty())result+='/';result+=part;}return result;}
inline std::string SourcePath(std::string_view manager,std::string_view filename){
  nif::Check(!filename.empty() && filename.front()!='/' && filename.front()!='\\',"Absolute authored KFM filename is unsupported");
  const auto slash=manager.find_last_of("/\\");return Join(Parts(std::string(slash==manager.npos?"":manager.substr(0,slash+1))+std::string(filename)));
}
inline std::string NativeRelative(std::string_view manager,std::string_view target,bool model){
  using nif::Check;auto a=Parts(manager),b=Parts(target);
  Check(Join(a)==manager && Join(b)==target && manager.starts_with("em1/runtime/") && target.starts_with("em1/runtime/") &&
    manager.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789/.-")==manager.npos &&
    target.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789/.-")==target.npos &&
    manager.ends_with(".kfm") && target.ends_with(model?".nif":".kf"),"KFM dependency lacks native origin/type");
  a.pop_back();size_t common=0;while(common<a.size() && common<b.size() && a[common]==b[common])++common;
  Check(common>=2,"KFM dependency escapes native namespace");std::string relative;
  for(size_t i=common;i<a.size();++i)relative+="../";
  relative+=Join(std::span(b).subspan(common));return relative;
}
}
inline bool BindKfmPaths(nif::View input,std::string_view source_manager,std::string_view native_manager,
    const std::map<std::string,std::string>& bindings,nif::Bytes& output,std::string& error){
  try {
    KfmDocument parsed;if(!InspectKfm(input,parsed,error))return false;
    const auto native_parts=kfm_detail::Parts(native_manager);
    nif::Check(kfm_detail::Join(native_parts)==native_manager && native_manager.starts_with("em1/runtime/") && native_manager.ends_with(".kfm") &&
      native_manager.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789/.-")==native_manager.npos,"Invalid native KFM manager name");
    (void)kfm_detail::Parts(source_manager);
    std::map<std::string,std::string> normalized;
    for(const auto& [source,target]:bindings){const auto key=kfm_detail::Join(kfm_detail::Parts(source));
      const auto [at,inserted]=normalized.emplace(key,target);nif::Check(inserted || at->second==target,"Conflicting KFM dependency bindings");}
    nif::Bytes result;size_t previous=0;
    for(const auto& field:parsed.paths){if(field.authored.empty())continue;
      const auto source=kfm_detail::SourcePath(source_manager,field.authored);const auto found=normalized.find(source);
      if(found==normalized.end())throw std::runtime_error("Unbound KFM dependency: "+source);
      const auto relative=kfm_detail::NativeRelative(native_manager,found->second,field.model);
      nif::Check(relative.size()<=2048,"Rebound KFM path exceeds bounds");
      nif::Append(result,input.subspan(previous,field.offset-previous));nif::Number(result,uint32_t(relative.size()),parsed.big);
      nif::Append(result,nif::View(reinterpret_cast<const uint8_t*>(relative.data()),relative.size()));previous=field.offset+field.bytes;
    }
    nif::Append(result,input.subspan(previous));KfmDocument check;
    if(!InspectKfm(result,check,error))return false;
    output=std::move(result);error.clear();return true;
  }catch(const std::exception& e){error=e.what();return false;}
}
struct BoundKfmResource { std::string origin_key;std::vector<uint8_t> bytes;KfmDocument source_document; };
bool ReadBoundKfm(std::shared_ptr<RvzSource>,std::string_view archive,std::string_view resource,
    std::string_view native_manager,const std::map<std::string,std::string>& bindings,BoundKfmResource&,std::string& error);
}
