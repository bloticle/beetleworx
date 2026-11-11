#include "em1_resource_catalog.h"
#include "em1_scene_files.h"
#include <stdexcept>
namespace em2::em1 {
namespace {
std::string Canonical(std::string_view input){
  if(input.empty() || input.size()>2048 || input.front()=='/' || input.front()=='\\')throw std::runtime_error("Invalid original resource path");
  std::string value;for(auto c:input){if(c==':' || c==0)throw std::runtime_error("Invalid original resource path");if(c=='\\')c='/';if(c>='A' && c<='Z')c+=32;value+=c;}
  size_t at=0;while(at<=value.size()){auto end=value.find('/',at);if(end==value.npos)end=value.size();const auto part=std::string_view(value).substr(at,end-at);
    if(part.empty() || part=="." || part=="..")throw std::runtime_error("Original resource path contains traversal/empty segment");if(end==value.size())break;at=end+1;}
  return value;
}
}
bool BuildOriginalResourceCatalog(std::shared_ptr<RvzSource> source,OriginalResourceCatalog& output,std::string& error){
  try{
    if(!source)throw std::runtime_error("Original catalog requires a source mount");
    OriginalResourceCatalog catalog;catalog.source=source;catalog.source_identity=source->SourceIdentity();
    for(const auto& file:source->Files()){
      const auto path=Canonical(file.path);catalog.resources[path].push_back({{},path,{}});
      if(!path.starts_with("packfiles/") || !path.ends_with(".pak"))continue;
      if(++catalog.packfiles>4096)throw std::runtime_error("Original packfile catalog exceeds archive budget");
      auto pak=PakSource::Open(source,path,error);if(!pak)return false;
      for(const auto& entry:pak->Entries()){
        const auto key=Canonical(entry.path);auto& locations=catalog.resources[key];
        if(locations.size()>4096 || catalog.resources.size()>1000000)throw std::runtime_error("Original resource catalog exceeds index budget");
        locations.push_back({path,key,entry.type});
      }
    }
    if(!catalog.packfiles)throw std::runtime_error("Source has no original packfile catalog");
    output=std::move(catalog);error.clear();return true;
  }catch(const std::exception& e){error=e.what();return false;}
}
bool QueryOriginalResource(const OriginalResourceCatalog& catalog,std::string_view logical_path,
    std::span<const std::string> authored_archives,OriginalResourceQuery& output,std::string& error){
  try{
    if(!catalog.source || catalog.source_identity.empty() || catalog.source_identity!=catalog.source->SourceIdentity() || !catalog.packfiles)
      throw std::runtime_error("Original resource catalog source identity is invalid");
    const auto path=Canonical(logical_path);std::vector<std::string> candidates{path};
    const auto ext=SceneFileExtension(path);
    if(!path.ends_with("_wii") && (ext==".nif" || ext==".kfm" || ext==".kf" || ext==".hkx" || ext==".hkw" || ext==".hkb" || ext==".hkp"))candidates.push_back(path+"_wii");
    std::vector<OriginalResourceLocation> found;
    for(const auto& candidate:candidates){auto at=catalog.resources.find(candidate);if(at!=catalog.resources.end())found.insert(found.end(),at->second.begin(),at->second.end());}
    OriginalResourceQuery query;
    for(const auto& authored:authored_archives){const auto archive=authored.empty()?std::string():Canonical(authored);
      for(const auto& location:found)if(location.archive==archive)query.locations.push_back(location);
      if(!query.locations.empty()){query.presence=OriginalResourcePresence::InScope;output=std::move(query);error.clear();return true;}}
    query.presence=found.empty()?OriginalResourcePresence::NotFound:OriginalResourcePresence::OutsideScope;query.locations=std::move(found);
    output=std::move(query);error.clear();return true;
  }catch(const std::exception& e){error=e.what();return false;}
}
bool ReadOriginalBootstrapArchives(const OriginalResourceCatalog& catalog,std::vector<std::string>& output,std::string& error){
  try{
    if(!catalog.source || catalog.source_identity!=catalog.source->SourceIdentity())throw std::runtime_error("Bootstrap catalog source is invalid");
    std::vector<uint8_t> bytes;if(!catalog.source->ReadFile("always_loaded.txt",bytes,error))return false;
    if(bytes.size()>65536 || std::find(bytes.begin(),bytes.end(),uint8_t(0))!=bytes.end())throw std::runtime_error("Original bootstrap manifest exceeds supported bounds");
    const std::string text(bytes.begin(),bytes.end());std::vector<std::string> names;size_t at=0;
    while(at<text.size()){
      auto end=text.find('\n',at);if(end==text.npos)end=text.size();auto line=std::string_view(text).substr(at,end-at);at=end+1;
      const auto first=line.find_first_not_of(" \t\r");if(first==line.npos || line[first]==';')continue;
      line=line.substr(first,line.find_last_not_of(" \t\r")-first+1);const auto name=Canonical(line);
      if(!name.ends_with(".pak") || name.find('/')!=name.npos)throw std::runtime_error("Unsupported original bootstrap manifest entry");
      const auto path="packfiles/"+name;
      if(!catalog.resources.contains(path))throw std::runtime_error("Original bootstrap archive absent from source: "+path);
      if(names.size()>=256)throw std::runtime_error("Original bootstrap archive count exceeds bounds");names.push_back(path);
    }
    if(names.empty())throw std::runtime_error("Original bootstrap archive manifest is empty");output=std::move(names);error.clear();return true;
  }catch(const std::exception& e){error=e.what();return false;}
}
bool PlanLevelResourceScope(const OriginalResourceCatalog& catalog,const LevelContents& contents,
    std::vector<SceneResourceLookup>& output,std::string& error){
  try{
    auto origin=[&](const std::string& archive){return catalog.source_identity+":pak-v1:"+std::to_string(archive.size())+":"+archive;};
    if(!contents.plan.origin_key.starts_with(origin("packfiles/levels.pak")+":level-v1:"))throw std::runtime_error("Level resource plan belongs to a different source");
    std::vector<std::string> bootstrap;if(!ReadOriginalBootstrapArchives(catalog,bootstrap,error))return false;
    std::vector<SceneResourceLookup> plan;
    for(size_t i=0;i<contents.entry_scenes.size();++i){const auto& scene=contents.entry_scenes[i];
      if(!scene.resource.origin_key.starts_with(origin(Canonical(scene.request.archive))+":scene-v"+std::to_string(kSceneAdapterVersion)+":"))throw std::runtime_error("Scene resource plan belongs to a different original owner");
      std::vector<std::string> scope{Canonical(scene.request.archive)};
      auto add=[&](const std::string& archive){if(std::find(scope.begin(),scope.end(),archive)==scope.end())scope.push_back(archive);};
      for(const auto& archive:bootstrap)add(archive);
      for(const auto& sibling:contents.entry_scenes)add(Canonical(sibling.request.archive));
      add(""); // Explicit original loose-source scope, after authored archives.
      std::vector<SceneFileReference> refs;if(!CollectDeclaredSceneAssetReferences(scene.resource.scene,refs,error))return false;
      for(auto& ref:refs){SceneResourceLookup lookup;lookup.scene_index=i;lookup.reference=std::move(ref);
        if(!QueryOriginalResource(catalog,lookup.reference.path,scope,lookup.query,error))return false;
        if(plan.size()>=1000000)throw std::runtime_error("Level declared resource plan exceeds bounds");plan.push_back(std::move(lookup));}
    }
    output=std::move(plan);error.clear();return true;
  }catch(const std::exception& e){error=e.what();return false;}
}
}
