#pragma once
#include "em1_nif_container.h"
namespace em2::em1 {
// Standard NiMesh modifier layout. Native Gamebryo executes the original
// relative-position morph; the adapter must retain all targets and weights.
inline uint16_t ReadPositionMorphTargets(const nif::Nif& model,uint32_t block){
  using namespace nif;Check(block<model.blocks.size() && model.types[model.blocks[block].type]=="NiMorphMeshModifier","Unaudited mesh modifier");
  const auto bytes=model.blocks[block].Data();Cursor c{bytes,0,model.big};
  for(unsigned list=0;list<2;++list){const auto count=c.Word();Check(count<=16,"Morph sync-point count exceeds bounds");
    for(uint32_t i=0;i<count;++i){const auto point=c.Short();Check(point>=0x8000 && point<=0x8070 && !(point&15),"Unknown morph synchronization point");}}
  const auto flags=c.Take(1)[0];Check(flags==1,"Morph requires audited relative-position flags");
  const auto targets=c.Short();Check(targets && targets<=32,"Morph target count exceeds bounds");
  Check(c.Word()==1,"Morph requires one position element");const auto name=c.Word();
  Check(model.String(name)=="POSITION" && c.Word()==0 && c.Word()==0,"Unsupported morph semantic/normalization");
  Check(c.at==bytes.size(),"Unparsed morph modifier bytes");return targets;
}
}
