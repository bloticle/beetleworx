#include "em1_keyframe.h"
namespace em2::em1 {
bool TestKeyframeColorEvaluator(std::string& error){
  try{
    using namespace nif;
    for(bool big:{false,true}){
      const std::string header="Gamebryo File Format, Version 20.6.5.0\n";
      Bytes empty(header.begin(),header.end());Number(empty,0x14060500,false);Number(empty,!big,false,1);
      Number(empty,0,false);Number(empty,0,false);Number(empty,0,big,2);for(unsigned i=0;i<4;++i)Number(empty,0,big);
      Nif n(empty);Bytes sequence;
      for(auto value:{n.AddString("PortalPowerPulse"),1u,1u,UINT32_MAX,0x3F800000u,0u,0x3F800000u,UINT32_MAX,0u})Number(sequence,value,big);
      n.AddBlock("NiSequenceData",std::move(sequence));
      Bytes evaluator;
      for(auto value:{n.AddString("PortalPowerMach"),UINT32_MAX,n.AddString("NiColorExtraDataController"),n.AddString("Constant_Color_0"),UINT32_MAX})Number(evaluator,value,big);
      evaluator.insert(evaluator.end(),{1,0,0,0});Number(evaluator,2,big);
      n.AddBlock("NiColorEvaluator",std::move(evaluator));
      Bytes curve;Number(curve,2,big);Number(curve,1,big);
      for(auto value:{0u,0u,0u,0u,0x3F800000u,0x3F800000u,0x3F800000u,0x3F800000u,0x3F800000u,0x3F800000u})Number(curve,value,big);
      n.AddBlock("NiColorData",curve);Bytes footer;Number(footer,1,big);Number(footer,0,big);n.footer=footer;
      const auto original=n.Write();KeyframeDocument doc;
      const bool inspected=InspectKeyframe(original,doc,error);Check(inspected,error.c_str());
      Check(doc.sequences.size()==1 && doc.sequences[0].evaluators==1 && doc.sequences[0].duration==1 &&
            Nif(original).Write()==original,"Keyframe color inspection changed original curves or sequence metadata");
      auto invalid=n.blocks[1].replacement;Number(invalid,0,big);n.blocks[1].replacement=invalid;
      KeyframeDocument untouched;untouched.sequences.push_back({99,7,3,2,"retained"});
      Check(!InspectKeyframe(n.Write(),untouched,error) && untouched.sequences[0].root==99,
            "Overlong color evaluator was accepted or partially published");
      invalid.resize(28);for(unsigned i=0;i<4;++i)invalid[24+i]=uint8_t(0xFFFFFFFFu>>(8*i));n.blocks[1].replacement=invalid;
      // Null is a valid native unkeyed link; a link to the sequence is not.
      KeyframeDocument null_link;const bool null_ok=InspectKeyframe(n.Write(),null_link,error);Check(null_ok,error.c_str());
      for(unsigned i=0;i<4;++i)n.blocks[1].replacement[24+i]=0;
      Check(!InspectKeyframe(n.Write(),untouched,error) && untouched.sequences[0].root==99,
            "Color evaluator accepted a non-color dependency");
    }
    error.clear();return true;
  }catch(const std::exception& failure){error=failure.what();return false;}
}
bool ReadKeyframe(std::shared_ptr<RvzSource> source,std::string_view archive,std::string_view resource,KeyframeResource& output,std::string& error){
  try{
    auto pak=PakSource::Open(std::move(source),archive,error);if(!pak)return false;
    auto path=std::string(resource);for(auto& ch:path){if(ch=='\\')ch='/';if(ch>='A' && ch<='Z')ch+=32;}
    const auto found=std::find_if(pak->Entries().begin(),pak->Entries().end(),[&](const auto& entry){return entry.path==path;});
    nif::Check(found!=pak->Entries().end() && found->type==std::array<uint8_t,4>{'K','F',0,0},"Source resource is not an original KF entry");
    KeyframeResource result;if(!pak->ReadOriginal(path,result.bytes,error) || !InspectKeyframe(result.bytes,result.document,error))return false;
    result.origin_key=pak->OriginKey()+":kf-original-v1:"+path;output=std::move(result);error.clear();return true;
  }catch(const std::exception& e){error=e.what();return false;}
}
}
