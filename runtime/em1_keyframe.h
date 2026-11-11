#pragma once
#include "em1_pack_source.h"
#include "em1_nif_controllers.h"
#include <set>

namespace em2::em1 {
struct KeyframeSequence {uint32_t root=0,evaluators=0;float duration=0,frequency=0;std::string name;};
struct KeyframeDocument {std::vector<KeyframeSequence> sequences;};
// Shared Gamebryo20.6.5 evaluator streams are already native binary data.
// Preserve every original key/channel/target/flag. NiStream owns decoding and
// animation execution; this inspection does not replace curves or bind actors.
inline bool InspectKeyframe(nif::View bytes,KeyframeDocument& output,std::string& error){
  try {
    using namespace nif;Check(bytes.size()<=16*1024*1024,"Keyframe exceeds bounds");Nif n(bytes);KeyframeDocument doc;
    const auto count=U32(n.footer,0,n.big);Check(count>0 && count<=256,"Keyframe sequence roots exceed bounds");std::set<uint32_t> roots;
    for(uint32_t i=0;i<count;++i){const auto id=U32(n.footer,4+i*4,n.big);Check(id<n.blocks.size() && n.types[n.blocks[id].type]=="NiSequenceData" && roots.insert(id).second,"Invalid/duplicate original keyframe root");}
    auto string=[&](uint32_t id){if(id!=UINT32_MAX)n.String(id);};
    auto link=[&](uint32_t id,std::string_view type){Check(id==UINT32_MAX || (id<n.blocks.size() && n.types[n.blocks[id].type]==type),"Keyframe dependency has absent/wrong block type");};
    auto number=[](uint32_t value){const auto f=std::bit_cast<float>(value);Check(std::isfinite(f),"Nonfinite keyframe value");return f;};
    ValidateNifControllerBlocks(n);
    for(size_t block=0;block<n.blocks.size();++block){
      const auto& type=n.types[n.blocks[block].type];const auto data=n.blocks[block].Data();Cursor c{data,0,n.big};
      if(type=="NiSequenceData"){
        Check(roots.contains(uint32_t(block)),"Unrooted keyframe sequence");KeyframeSequence sequence;sequence.root=uint32_t(block);sequence.name=n.String(c.Word());sequence.evaluators=c.Word();
        Check(sequence.evaluators>0 && sequence.evaluators<=4096 && sequence.evaluators<=(data.size()-c.at)/4,"Invalid keyframe evaluator count");
        std::set<uint32_t> unique;
        for(uint32_t i=0;i<sequence.evaluators;++i){const auto id=c.Word();Check(id<n.blocks.size() && n.types[n.blocks[id].type].ends_with("Evaluator") && unique.insert(id).second,"Invalid/duplicate sequence evaluator");}
        link(c.Word(),"NiTextKeyExtraData");sequence.duration=number(c.Word());Check(sequence.duration>=0,"Negative keyframe duration");
        Check(c.Word()<=2,"Unknown keyframe cycle policy");sequence.frequency=number(c.Word());Check(sequence.frequency>0,"Invalid keyframe frequency");string(c.Word());c.Word();doc.sequences.push_back(std::move(sequence));
      }else if(type.ends_with("Evaluator")){
        for(unsigned i=0;i<5;++i)string(c.Word());c.Take(4); // Original channel selectors/flags, retained verbatim.
        if(type=="NiTransformEvaluator"){for(unsigned i=0;i<8;++i)number(c.Word());link(c.Word(),"NiTransformData");}
        else if(type=="NiFloatEvaluator")link(c.Word(),"NiFloatData");
        else if(type=="NiColorEvaluator")link(c.Word(),"NiColorData");
        else if(type=="NiBoolEvaluator")link(c.Word(),"NiBoolData");
        else if(type=="NiConstFloatEvaluator")number(c.Word());
        else if(type=="NiConstColorEvaluator"){for(unsigned i=0;i<4;++i)number(c.Word());}
        else if(type=="NiConstBoolEvaluator"){const auto value=number(c.Word());Check(value==0 || value==1,"Invalid constant bool evaluator");} // Original pose is a float channel.
        else throw std::runtime_error("Unaudited keyframe evaluator: "+type);
      }else if(type=="NiTextKeyExtraData" || type=="NiTransformData" || type=="NiFloatData" || type=="NiBoolData" || type=="NiColorData")continue;
      else throw std::runtime_error("Unaudited keyframe block: "+type);
      Check(c.at==data.size(),"Unparsed keyframe sequence/evaluator fields");
    }
    output=std::move(doc);error.clear();return true;
  }catch(const std::exception& e){error=e.what();return false;}
}
struct KeyframeResource {std::string origin_key;std::vector<uint8_t> bytes;KeyframeDocument document;};
bool ReadKeyframe(std::shared_ptr<RvzSource>,std::string_view archive,std::string_view resource,KeyframeResource&,std::string& error);
bool TestKeyframeColorEvaluator(std::string& error);
}
