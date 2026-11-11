#include "em1_kfm.h"
namespace em2::em1 {
bool ReadBoundKfm(std::shared_ptr<RvzSource> source,std::string_view archive,std::string_view resource,
    std::string_view native_manager,const std::map<std::string,std::string>& bindings,BoundKfmResource& output,std::string& error){
  try {
    auto pak=PakSource::Open(std::move(source),archive,error);if(!pak)return false;
    const auto path=kfm_detail::Join(kfm_detail::Parts(resource));
    const auto found=std::find_if(pak->Entries().begin(),pak->Entries().end(),[&](const auto& entry){return entry.path==path;});
    nif::Check(found!=pak->Entries().end() && found->type==std::array<uint8_t,4>{'K','F','M',0},"Source resource is not an original KFM entry");
    std::vector<uint8_t> original;if(!pak->ReadOriginal(path,original,error))return false;
    BoundKfmResource prepared;
    if(!InspectKfm(original,prepared.source_document,error) || !BindKfmPaths(original,path,native_manager,bindings,prepared.bytes,error))return false;
    prepared.origin_key=pak->OriginKey()+":kfm-paths-v1:"+std::to_string(path.size())+":"+path+":"+std::string(native_manager);
    for(const auto& [name,target]:bindings)prepared.origin_key+=":"+std::to_string(name.size())+":"+name+":"+std::to_string(target.size())+":"+target;
    output=std::move(prepared);error.clear();return true;
  }catch(const std::exception& e){error=e.what();return false;}
}
}
