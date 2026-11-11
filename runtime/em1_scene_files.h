#pragma once
#include "em1_scene_adapter.h"
#include <algorithm>
#include <string_view>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
namespace em2::em1 {
// Runtime dependencies include animation managers/lists and conversations,
// not just visible geometry. Platform suffixes are explicit, never guessed by
// stripping arbitrary underscores from authored names.
inline std::string_view SceneFileExtension(std::string_view lower){
  for(const std::string_view type:{".nif",".hkx",".kfm",".kf",".hkw",".hkb",".hkp"}){
    if(lower.ends_with(type))return type;
    if(lower.size()>=type.size()+4 && lower.ends_with("_wii") && lower.substr(0,lower.size()-4).ends_with(type))return type;
  }
  for(const std::string_view type:{".lua",".bnk",".gsa",".lit_cooked",".bsq",".hpd"})if(lower.ends_with(type))return type;
  return {};
}
struct SceneFileReference {
  uint32_t entity=0,component=0;
  std::string component_name,property,path;
  size_t value_offset=0;
  uint32_t storage_mode=0;
  bool DeclaredAsset() const {return storage_mode==2 || storage_mode==3;}
};
// File-valued String properties in COMPLETE adapted scenes, including prefab
// definitions and list values. References are not evidence of converted assets.
inline bool CollectSceneFileReferences(const SceneAdapterResult& scene,std::vector<SceneFileReference>& output,std::string& error,bool declared_assets_only=false,std::vector<std::string>* component_classes=nullptr){
  try {
    const auto& b=scene.bytes;
    auto check=[](bool value,const char* why){if(!value)throw std::runtime_error(why);};
    auto word=[&](size_t at){check(at<=b.size() && b.size()-at>=4,"Truncated adapted scene word");return uint32_t(b[at])<<24|uint32_t(b[at+1])<<16|uint32_t(b[at+2])<<8|b[at+3];};
    check(b.size()>=24 && b.size()<=80ull*1024*1024 && word(0)==0x01000001,"Invalid adapted scene header");
    const size_t table_end=uint64_t(word(4))+4;
    check(table_end>=8 && table_end<=b.size() && b.size()-table_end>=16 && word(table_end)==0x02000002,"Invalid adapted scene table boundary");
    auto text=[&](uint32_t offset){const uint64_t at=uint64_t(offset)+6;check(offset>=4 && at<table_end,"Invalid adapted scene string offset");const auto end=std::find(b.begin()+size_t(at),b.begin()+table_end,uint8_t(0));check(end!=b.begin()+table_end,"Unterminated adapted scene string");return std::string(b.begin()+size_t(at),end);};
    auto stride=[&](const std::string& type)->size_t{
      if(type=="Point2")return 8;if(type=="Point3" || type=="Color (RGB)")return 12;if(type=="Color (RGBA)")return 16;if(type=="Matrix3")return 36;
      check(type=="Boolean" || type=="Integer" || type=="Unsigned Integer" || type=="Short" || type=="Unsigned Short" || type=="Float" || type=="String" || type=="Entity Pointer","Unknown scene property type");return 4;
    };
    const auto entities=word(table_end+8),references=word(table_end+12);size_t at=table_end+16;
    check(entities==scene.entity_count && entities<=(b.size()-at)/24,"Invalid adapted entity count");
    std::vector<SceneFileReference> result;std::vector<std::string> classes;uint64_t components=0;
    for(uint32_t i=0;i<entities;++i){
      text(word(at));const auto entity=word(at+4),count=word(at+20);check(count<=b.size()/20,"Invalid component count");at+=24;components+=count;
      for(uint32_t j=0;j<count;++j){
        const auto name=text(word(at)),class_name=text(word(at+4));const auto component=word(at+8),properties=word(at+16);(void)class_name;if(component_classes)classes.push_back(name);at+=20;check(properties<=b.size()/16,"Invalid property count");
        for(uint32_t k=0;k<properties;++k){
          const auto property=text(word(at)),type=text(word(at+4));const auto mode=word(at+8),amount=word(at+12);check(mode<6,"Invalid property mode");at+=16;
          const size_t count=(mode&1)?amount:1,width=stride(type);check(at<=b.size() && count<=(b.size()-at)/width,"Truncated property values");
          if(type=="String")for(size_t n=0;n<count;++n){
            auto path=text(word(at+n*4)),lower=path;for(auto& c:lower)if(c>='A' && c<='Z')c+=32;
            if(!SceneFileExtension(lower).empty() && (!declared_assets_only || mode==2 || mode==3))
              result.push_back({entity,component,name,property,path,at+n*4,mode});
          }
          at+=count*width;
        }
      }
    }
    check(components==scene.component_count && at<=b.size() && uint64_t(references)*4==b.size()-at,"Adapted scene counts/trailer mismatch");
    output=std::move(result);if(component_classes)*component_classes=std::move(classes);error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
// Original storage2/3 marks scalar/list assets;0/1 are ordinary values and
//4/5 palette/template references. Retain the complete scene and forensic file
// inventory above. This classification is NOT proof of mandatory readiness,
// actual entity placement, converted data or native component load behavior.
inline bool CollectDeclaredSceneAssetReferences(const SceneAdapterResult& scene,std::vector<SceneFileReference>& output,std::string& error){
  return CollectSceneFileReferences(scene,output,error,true);
}
// Bindings belong to this scene's source archive. All recognized file-valued
// properties must resolve; no entity, component or non-file value is dropped.
inline bool BindSceneFilePaths(const SceneAdapterResult& scene,
    const std::map<std::string,std::string>& bindings,SceneAdapterResult& output,std::string& error,bool declared_assets_only=false,std::string_view only_extension={},std::span<const std::string> retain_source_paths={}){
  try {
    auto check=[](bool value,const char* why){if(!value)throw std::runtime_error(why);};
    auto key=[](std::string value){for(auto& c:value){if(c=='\\')c='/';if(c>='A' && c<='Z')c+=32;}return value;};
    auto extension=[&](const std::string& value){return std::string(SceneFileExtension(key(value)));};
    std::map<std::string,std::string> normalized;
    std::set<std::string> retained;
    for(const auto& path:retain_source_paths){
      check(!only_extension.empty() && extension(path)==only_extension,"Retained source path has wrong asset type");retained.insert(key(path));
    }
    for(const auto& [source,target]:bindings){
      check(target.starts_with("em1/runtime/") && target.size()<=240 && target.find("..") == std::string::npos && target.find("//")==std::string::npos && target.find("/./")==std::string::npos &&
        target.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789/.-")==std::string::npos,"Invalid native scene asset path");
      const auto source_type=extension(source);
      check(!source_type.empty() && source_type==extension(target),"Scene asset binding changes or omits resource type");
      check(only_extension.empty() || source_type==only_extension,"Scene binding does not match the selected resource type");
      auto [found,inserted]=normalized.emplace(key(source),target);
      check(inserted || found->second==target,"Conflicting case-normalized scene bindings");
    }
    std::vector<SceneFileReference> references;if(!CollectSceneFileReferences(scene,references,error,declared_assets_only))return false;
    if(!only_extension.empty())std::erase_if(references,[&](const auto& reference){return extension(reference.path)!=only_extension;});
    const auto& bytes=scene.bytes;
    auto word=[&](size_t at){return uint32_t(bytes[at])<<24|uint32_t(bytes[at+1])<<16|uint32_t(bytes[at+2])<<8|bytes[at+3];};
    const auto old_end=word(4);const size_t boundary=size_t(old_end)+4;
    std::vector<uint8_t> strings;std::map<std::string,uint32_t> offsets;
    std::vector<std::pair<size_t,uint32_t>> patches;
    for(const auto& reference:references){
      const auto binding=normalized.find(key(reference.path));
      if(binding==normalized.end()){
        if(retained.contains(key(reference.path)))continue; // Explicit unavailable source reference stays unchanged.
        throw std::runtime_error("Unbound scene asset: "+reference.path);
      }
      const auto& name=binding->second;
      auto found=offsets.find(name);
      if(found==offsets.end()){
        const size_t length=name.size()+1,size=(length+5)&~size_t(3);
        check(strings.size()<=16ull*1024*1024-size,"Scene asset string growth exceeds budget");
        const auto begin=strings.size();const auto offset=uint32_t(old_end+begin);
        strings.push_back(uint8_t(size));strings.push_back(uint8_t(length));strings.insert(strings.end(),name.begin(),name.end());strings.resize(begin+size,0);
        found=offsets.emplace(name,offset).first;
      }
      patches.emplace_back(reference.value_offset,found->second);
    }
    check(strings.size()<=80ull*1024*1024-bytes.size(),"Bound scene exceeds budget");
    SceneAdapterResult candidate=scene;candidate.bytes.clear();candidate.bytes.reserve(bytes.size()+strings.size());
    candidate.bytes.insert(candidate.bytes.end(),bytes.begin(),bytes.begin()+boundary);
    candidate.bytes.insert(candidate.bytes.end(),strings.begin(),strings.end());candidate.bytes.insert(candidate.bytes.end(),bytes.begin()+boundary,bytes.end());
    auto store=[&](size_t at,uint32_t value){for(unsigned i=0;i<4;++i)candidate.bytes[at+i]=uint8_t(value>>(24-i*8));};
    store(4,uint32_t(old_end+strings.size()));for(const auto& [at,value]:patches)store(at+strings.size(),value);
    std::vector<SceneFileReference> verified;if(!CollectSceneFileReferences(candidate,verified,error,declared_assets_only))return false;
    if(!only_extension.empty())std::erase_if(verified,[&](const auto& reference){return extension(reference.path)!=only_extension;});
    check(verified.size()==references.size(),"Scene asset rebinding changed reference count");
    output=std::move(candidate);error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
// Runtime binding follows original declared asset flags. Plain-string source
// paths/palette metadata remain byte-identical, even if they look like files.
// All original definitions/components/placements remain in the complete scene.
inline bool BindDeclaredSceneAssetPaths(const SceneAdapterResult& scene,
    const std::map<std::string,std::string>& bindings,SceneAdapterResult& output,std::string& error){
  return BindSceneFilePaths(scene,bindings,output,error,true);
}
// Model stage only: every declared NIF must bind; all other declared assets,
// plain source strings and palette/template metadata retain their values.
inline bool BindDeclaredSceneModelPaths(const SceneAdapterResult& scene,
    const std::map<std::string,std::string>& bindings,SceneAdapterResult& output,std::string& error){
  return BindSceneFilePaths(scene,bindings,output,error,true,".nif");
}

}
