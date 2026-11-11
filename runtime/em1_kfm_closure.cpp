#include "em1_kfm_closure.h"
#include <set>
namespace em2::em1 {
bool ReadKfmClosure(std::shared_ptr<RvzSource> source,std::string_view archive,std::string_view resource,
    KfmClosure& output,std::string& error,std::span<const std::string> level_archives){
  try{
    auto local=PakSource::Open(source,archive,error);if(!local)return false;
    const auto path=kfm_detail::Join(kfm_detail::Parts(resource));
    const auto manager=std::find_if(local->Entries().begin(),local->Entries().end(),[&](const auto& e){return e.path==path;});
    nif::Check(manager!=local->Entries().end() && manager->type==std::array<uint8_t,4>{'K','F','M',0},"Original KFM entry unavailable");
    KfmClosure result;result.source=source;result.resource=path;
    if(!local->ReadOriginal(path,result.original,error) || !InspectKfm(result.original,result.document,error))return false;
    std::unique_ptr<PakSource> global;
    std::set<std::pair<bool,std::string>> seen;
    size_t prepared_bytes=result.original.size();
    result.origin_key=local->OriginKey()+":kfm-closure-v1:"+path;
    for(const auto& field:result.document.paths){
      if(field.authored.empty())continue;
      const auto resolved=kfm_detail::SourcePath(path,field.authored);if(!seen.emplace(field.model,resolved).second)continue;
      auto lookup=[&](const PakSource* pak)->std::string{
        for(const auto& name:{resolved,resolved+"_wii"}){
          if(name!=resolved && !resolved.ends_with(field.model?".nif":".kf"))continue;
          const auto found=std::find_if(pak->Entries().begin(),pak->Entries().end(),[&](const auto& e){return e.path==name;});
          if(found!=pak->Entries().end()){
            nif::Check(found->type==(field.model?std::array<uint8_t,4>{'N','I','F',0}:std::array<uint8_t,4>{'K','F',0,0}),"Original KFM dependency has wrong native type");return name;
          }
        }return {};
      };
      auto* owner=local.get();auto dependency=lookup(owner);
      if(dependency.empty() && archive!="packfiles/_dynamic.pak"){
        if(!global){global=PakSource::Open(source,"packfiles/_dynamic.pak",error);if(!global)return false;}
        dependency=lookup(global.get());if(!dependency.empty())owner=global.get();
      }
      if(dependency.empty())throw std::runtime_error("Original KFM dependency unavailable: "+resolved);
      const auto owner_archive=owner==local.get()?std::string(archive):std::string("packfiles/_dynamic.pak");
      if(field.model){KfmModelDependency model{resolved,owner_archive,dependency,{}};
        nif::Check(result.models.size()<16,"KFM model dependency budget exceeded");
        if(!ReadModelClosure(source,owner_archive,dependency,model.closure,error,nullptr,level_archives))return false;
        nif::Check(model.closure.model.bytes.size()<=256*1024*1024-prepared_bytes,"KFM prepared byte budget exceeded");prepared_bytes+=model.closure.model.bytes.size();
        result.origin_key+=":"+std::to_string(model.closure.model.origin_key.size())+":"+model.closure.model.origin_key;
        result.models.push_back(std::move(model));
      }else{KfmKeyframeDependency clip{resolved,owner_archive,dependency,{}};
        nif::Check(result.keyframes.size()<256,"KFM keyframe dependency budget exceeded");
        if(!ReadKeyframe(source,owner_archive,dependency,clip.keyframe,error))return false;
        nif::Check(clip.keyframe.bytes.size()<=256*1024*1024-prepared_bytes,"KFM prepared byte budget exceeded");prepared_bytes+=clip.keyframe.bytes.size();
        result.origin_key+=":"+std::to_string(clip.keyframe.origin_key.size())+":"+clip.keyframe.origin_key;
        result.keyframes.push_back(std::move(clip));}
    }
    output=std::move(result);error.clear();return true;
  }catch(const std::exception& e){error=e.what();return false;}
}
}
