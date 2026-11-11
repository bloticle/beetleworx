#pragma once
#include "em1_nif_container.h"
#include <bit>
#include <cmath>

namespace em2::em1 {
// Shared Gamebryo serialized controller records. Keep their original channels,
// flags, sentinel poses, key data and links; native NiStream/samplers own their
// execution. This validation does not replace animation or attach a live actor.
inline void ValidateNifControllerBlocks(const nif::Nif& model) {
  using namespace nif;
  auto link=[&](uint32_t value){Check(value==UINT32_MAX || value<model.blocks.size(),"Controller references absent NIF block");};
  auto string=[&](uint32_t value){if(value!=UINT32_MAX)model.String(value);};
  for(const auto& block:model.blocks){
    const auto& type=model.types[block.type];const auto bytes=block.Data();Cursor c{bytes,0,model.big};
    auto number=[&](){Check(std::isfinite(std::bit_cast<float>(c.Word())),"Nonfinite serialized controller value");};
    if(type=="NiMorphWeightsController"){
      link(c.Word());c.Short();for(unsigned i=0;i<4;++i)number();link(c.Word());
      const auto count=c.Word(),interpolators=c.Word();Check(count && count<=32 && interpolators==count,"Morph controller target/interpolator count differs");
      for(uint32_t i=0;i<interpolators;++i)link(c.Word());
      const auto targets=c.Word();Check(targets==count,"Morph controller names/count differs");for(uint32_t i=0;i<targets;++i)string(c.Word());
    }else if(type=="NiTransformController" || type=="NiTextureTransformController" || type=="NiVisController" ||
        type=="NiFloatExtraDataController" || type=="NiColorExtraDataController" || type=="NiMaterialColorController"){
      const bool extra=type=="NiFloatExtraDataController" || type=="NiColorExtraDataController";
      Check(bytes.size()==(type=="NiTextureTransformController"?39:extra?34:type=="NiMaterialColorController"?32:30),"Unsupported time-controller record");
      link(c.Word());c.Short();for(unsigned i=0;i<4;++i)number();link(c.Word());link(c.Word());
      if(type=="NiTextureTransformController"){
        Check(c.Take(1)[0]<=1,"Invalid texture-controller shader-map flag");c.Word();
        Check(c.Word()<=4,"Unknown texture-controller operation");
      }
      if(extra)string(c.Word());
      if(type=="NiMaterialColorController")Check(c.Short()<=3,"Unknown material color channel");
    }else if(type=="NiTransformInterpolator" || type=="NiFloatInterpolator" || type=="NiColorInterpolator" || type=="NiPoint3Interpolator"){
      const unsigned values=type=="NiTransformInterpolator"?8u:type=="NiColorInterpolator"?4u:type=="NiPoint3Interpolator"?3u:1u;
      Check(bytes.size()==values*4+4,"Unsupported interpolator record");
      for(unsigned i=0;i<values;++i)number();link(c.Word());
    }else if(type=="NiBoolInterpolator"){
      Check(bytes.size()==5,"Unsupported bool interpolator record");
      Check(c.Take(1)[0]<=2,"Invalid bool interpolator pose"); //2 is the authored invalid-pose sentinel.
      link(c.Word());
    }else if(type=="NiFloatExtraData"){
      Check(bytes.size()==8,"Invalid float extra data");string(c.Word());number();
    }else if(type=="NiTextKeyExtraData"){
      string(c.Word());const auto count=c.Word();Check(count<=(bytes.size()-c.at)/8,"Text-key count exceeds block");
      for(uint32_t i=0;i<count;++i){number();string(c.Word());}
    }else if(type=="NiWireframeProperty" || type=="NiBillboardNode"){
      string(c.Word());const auto extras=c.Word();Check(extras<=(bytes.size()-c.at)/4,"Object extra-data count exceeds block");
      for(uint32_t i=0;i<extras;++i)link(c.Word());link(c.Word());
      if(type=="NiBillboardNode"){
        c.Take(54);const auto properties=c.Word();Check(properties<=(bytes.size()-c.at)/4,"Billboard property count exceeds block");
        for(uint32_t i=0;i<properties;++i)link(c.Word());link(c.Word());
        for(unsigned list=0;list<2;++list){const auto count=c.Word();Check(count<=(bytes.size()-c.at)/4,"Billboard node links exceed block");for(uint32_t i=0;i<count;++i)link(c.Word());}
        // NIF stores mode in the low3 bits; original pickups use flag9.
        // Preserve the complete field for the native consumer, including bit3.
        // Format reference: niftools/nifxml BillboardMode. No custom facing math.
        const auto flags=c.Short();Check(flags<=4 || flags==9,"Unaudited billboard flags");
      }else c.Short();
    }else continue; // Key-data payloads are read by the original native decoder.
    Check(c.at==bytes.size(),"Unparsed serialized controller fields");
  }
}
}
