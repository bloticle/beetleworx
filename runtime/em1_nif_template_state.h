#pragma once
#include "em1_nif_container.h"
#include "em1_material_effects.h"

namespace em2::em1 {
// These original R3MT templates select their PE state in the Wii renderer
// (R3MTStateOverrideFlags bits 0=blend, 1=alpha comparison, 2=depth).
// Native fallback techniques have explicit 2D pass states. Translate the
// properties and select a verified private pass with identical programs;
// shader inputs, geometry and shared properties remain unchanged.
inline uint32_t AdaptWiiTemplateRenderStates(nif::Nif& model,uint32_t available=0) {
  using namespace nif;
  uint32_t changed=0;
  const auto blocks=uint32_t(model.blocks.size());
  for(uint32_t index=0;index<blocks;++index){
    if(model.types[model.blocks[index].type]!="NiMesh")continue;
    const auto source=model.blocks[index].Data();
    Cursor c{source,0,model.big};c.Word();const auto extras=c.Word();
    Check(extras<=100000,"Template-state extra-data limit");
    uint32_t override_flags=0;unsigned overrides=0;
    for(uint32_t i=0;i<extras;++i){
      const auto link=c.Word();Check(link<blocks,"Invalid template-state extra-data link");
      const auto& block=model.blocks[link];
      if(model.types[block.type]!="NiIntegerExtraData")continue;
      const auto data=block.Data();Check(data.size()==8,"Invalid template-state integer");
      const auto name=U32(data,0,model.big);
      if(name!=UINT32_MAX && model.String(name)=="R3MTStateOverrideFlags"){
        override_flags=U32(data,4,model.big);++overrides;
      }
    }
    c.Word();c.Take(54);const auto count_at=c.at;const auto properties=c.Word();
    Check(properties<=100000,"Template-state property limit");
    const auto properties_at=c.at;c.Take(size_t(properties)*4);const auto end_at=c.at;
    c.Word();const auto materials=c.Word();Check(materials<=1000,"Template-state material limit");
    bool known=false,blend=false,alpha_greater_zero=false;size_t technique_at=0;
    for(uint32_t i=0;i<materials;++i){
      const auto at=c.at;const auto name=c.Word();c.Word();if(name==UINT32_MAX)continue;
      const auto technique=model.String(name);
      if(technique=="SH_Glint" || technique=="CHAR_Generic")known=true;
      if(technique=="SFX_skybox_clouds" || technique=="SFX_fogBowl"){known=true;blend=true;}
      if(technique=="FX_alphaBlend_singleChannel" || technique=="FX_DiffuseAlpha_VertexColorAlpha"){
        known=true;blend=true;alpha_greater_zero=true;
      }
      if(known)technique_at=at;
    }
    if(!known || !overrides)continue;
    Check(overrides==1 && materials==1 && override_flags<=7,"Ambiguous template-state override");
    // Finish all reads before appending blocks (which invalidates views).
    Bytes mesh(source.begin(),source.end());
    std::vector<uint32_t> links;
    for(uint32_t i=0;i<properties;++i)links.push_back(U32(source,properties_at+size_t(i)*4,model.big));
    bool touched=false;
    const auto property=[&](std::string_view type,bool depth){
      size_t slot=links.size();Bytes bytes;size_t flags_at=12;unsigned found=0;
      for(size_t i=0;i<links.size();++i){
        Check(links[i]<model.blocks.size(),"Invalid template-state property link");
        const auto& block=model.blocks[links[i]];if(model.types[block.type]!=type)continue;
        ++found;slot=i;const auto data=block.Data();bytes.assign(data.begin(),data.end());
        Cursor p{data,0,model.big};p.Word();const auto n=p.Word();
        Check(n<=100000,"Template-state property extra-data limit");p.Take(size_t(n)*4);p.Word();flags_at=p.at;
        p.Short();if(!depth)p.Take(1);
        Check(p.at==data.size(),"Unsupported template-state property layout");
      }
      Check(found<=1,"Ambiguous template-state property precedence");
      if(!found){for(auto word:{UINT32_MAX,0u,UINT32_MAX})Number(bytes,word,model.big);Number(bytes,0,model.big,2);if(!depth)bytes.push_back(0);}
      const auto original=U16(bytes,flags_at,model.big);
      uint16_t flags=original;
      if(depth)flags=uint16_t((flags&~uint16_t(0x001f)) | (blend?13:15)); // LEQUAL, test on, opaque write only.
      else {
        if(override_flags&1)flags=uint16_t(blend?((flags&~uint16_t(0x01ff))|0x00ed):(flags&~uint16_t(1)));
        if(override_flags&2){
          flags=uint16_t((flags&~uint16_t(0x1e00)) | (alpha_greater_zero?0x1200:0));
        }
      }
      const bool threshold_change=!depth && (override_flags&2) && alpha_greater_zero && bytes[flags_at+2]!=0;
      if(found && flags==original && !threshold_change)return;
      bytes[flags_at]=uint8_t(flags>>(model.big?8:0));bytes[flags_at+1]=uint8_t(flags>>(model.big?0:8));
      if(!depth && (override_flags&2) && alpha_greater_zero)bytes[flags_at+2]=0;
      const auto link=model.AddBlock(std::string(type),std::move(bytes));
      if(found)links[slot]=link;else links.push_back(link);
      touched=true;
    };
    if(override_flags&4)property("NiZBufferProperty",true);
    if(override_flags&3)property("NiAlphaProperty",false);
    if(available){
      unsigned z_count=0,alpha_count=0,stencil_count=0;uint16_t z_flags=0,alpha_flags=0,stencil_flags=0;uint8_t threshold=0;
      for(const auto link:links){const auto& block=model.blocks[link];const auto type=model.types[block.type];
        if(type!="NiZBufferProperty" && type!="NiAlphaProperty" && type!="NiStencilProperty")continue;
        const auto data=block.Data();Cursor p{data,0,model.big};p.Word();const auto n=p.Word();
        Check(n<=100000,"Template effect property extra-data limit");p.Take(size_t(n)*4);p.Word();const auto flags=p.Short();
        if(type=="NiZBufferProperty"){++z_count;z_flags=flags;}
        if(type=="NiAlphaProperty"){++alpha_count;alpha_flags=flags;threshold=p.Take(1)[0];}
        if(type=="NiStencilProperty"){++stencil_count;stencil_flags=flags;p.Take(8);}
        Check(p.at==data.size(),"Unsupported template effect property");
      }
      const bool test=(alpha_flags&0x200)!=0,blending=(alpha_flags&1)!=0;
      // Greater-than-zero rejection is output-equivalent with alpha blending
      // and depth writes disabled. Other comparisons need a discard program.
      const bool safe_alpha=!test || (blending && (z_flags&0x1f)==13 &&
          (alpha_flags&0x1e00)==0x1200 && threshold==0);
      const bool source_front=!stencil_count || (stencil_count==1 && ((stencil_flags>>10)&3)==1);
      const char* target=nullptr;
      if(z_count==1 && alpha_count<=1 && source_front && safe_alpha){
        if((z_flags&0x1f)==15 && !blending && (available&kTemplateOpaqueEffect))target="EM1O_DefaultShaderTextured";
        if((z_flags&0x1f)==13 && !blending && (available&kTemplateReadOpaqueEffect))target="EM1R_DefaultShaderTextured";
        if((z_flags&0x1f)==13 && blending && (alpha_flags&0x1fe)==0xec &&
            (available&kTemplateAlphaEffect))target="EM1T_DefaultShaderTextured";
      }
      if(target){const auto name=model.AddString(target);
        for(unsigned b=0;b<4;++b)mesh[technique_at+b]=uint8_t(name>>(8*(model.big?3-b:b)));
        touched=true;
      }
    }
    if(!touched)continue;
    Bytes encoded;for(const auto link:links)Number(encoded,link,model.big);
    mesh.erase(mesh.begin()+properties_at,mesh.begin()+end_at);
    mesh.insert(mesh.begin()+properties_at,encoded.begin(),encoded.end());
    for(unsigned b=0;b<4;++b)mesh[count_at+b]=uint8_t(uint32_t(links.size())>>(8*(model.big?3-b:b)));
    model.blocks[index].replacement=std::move(mesh);++changed;
  }
  return changed;
}
inline bool TestWiiTemplateRenderStates(std::string& error){
  using namespace nif;
  try {
    for(bool big:{false,true})for(uint32_t pe=0;pe<8;++pe)for(bool present:{false,true})
      for(std::string_view technique:{"SH_Glint","CHAR_Generic","SFX_skybox_clouds","SFX_fogBowl","FX_alphaBlend_singleChannel","FX_DiffuseAlpha_VertexColorAlpha","Unknown"}){
      Bytes empty;constexpr std::string_view signature="Gamebryo File Format, Version 20.6.5.0\n";
      Append(empty,View(reinterpret_cast<const uint8_t*>(signature.data()),signature.size()));
      Number(empty,0x14060500,false);Number(empty,!big,false,1);Number(empty,15,false);Number(empty,0,false);
      Number(empty,0,big,2);for(unsigned i=0;i<4;++i)Number(empty,0,big);Nif model(empty);
      Bytes extra;Number(extra,model.AddString("R3MTStateOverrideFlags"),big);Number(extra,pe,big);
      const auto override_link=model.AddBlock("NiIntegerExtraData",std::move(extra));
      Bytes alpha,z;for(auto word:{UINT32_MAX,0u,UINT32_MAX}){Number(alpha,word,big);Number(z,word,big);}
      Number(alpha,0x92ec,big,2);Number(alpha,127,big,1);Number(z,0xa000,big,2);
      const auto alpha_link=model.AddBlock("NiAlphaProperty",alpha),z_link=model.AddBlock("NiZBufferProperty",z);
      Bytes mesh;Number(mesh,UINT32_MAX,big);Number(mesh,1,big);Number(mesh,override_link,big);Number(mesh,UINT32_MAX,big);
      mesh.resize(mesh.size()+54);Number(mesh,present?2:0,big);
      if(present){Number(mesh,alpha_link,big);Number(mesh,z_link,big);}
      Number(mesh,UINT32_MAX,big);Number(mesh,1,big);Number(mesh,model.AddString(technique),big);Number(mesh,UINT32_MAX,big);
      const auto mesh_link=model.AddBlock("NiMesh",mesh);
      // Shared authored properties must survive even when the known mesh changes.
      Bytes unknown=mesh;const auto unknown_name=model.AddString("UnmappedSharedPropertyUser");
      const auto material_at=16+54+4+(present?8:0)+8;
      for(unsigned b=0;b<4;++b)unknown[material_at+b]=uint8_t(unknown_name>>(8*(big?3-b:b)));
      const auto unknown_link=model.AddBlock("NiMesh",unknown);
      const bool known=technique!="Unknown",blend=technique!="SH_Glint" && technique!="CHAR_Generic";
      const auto count=AdaptWiiTemplateRenderStates(model);
      Check(count==uint32_t(known && pe!=0 && !(present && pe==1 && !blend)),"Template PE override selection failed");
      Check(std::equal(alpha.begin(),alpha.end(),model.blocks[alpha_link].Data().begin()) &&
          std::equal(z.begin(),z.end(),model.blocks[z_link].Data().begin()) &&
          std::equal(unknown.begin(),unknown.end(),model.blocks[unknown_link].Data().begin()),"Template adapter changed shared properties or another material");
      const auto output=model.blocks[mesh_link].Data();
      if(!count)Check(std::equal(mesh.begin(),mesh.end(),output.begin(),output.end()),"Unaudited template changed");
      else {
        Cursor c{output,16+54,big};const auto n=c.Word();bool saw_z=false,saw_alpha=false;
        for(uint32_t i=0;i<n;++i){
          const auto& block=model.blocks[c.Word()];const auto data=block.Data();const auto flags=U16(data,12,big);
          if(model.types[block.type]=="NiZBufferProperty"){
            saw_z=true;Check(flags==((pe&4)?uint16_t((present?0xa000:0)|(blend?13:15)):0xa000),"Template depth state mismatch");
          }else if(model.types[block.type]=="NiAlphaProperty"){
            saw_alpha=true;uint16_t expected=present?0x92ec:0;
            if(pe&1)expected=uint16_t(blend?((expected&~0x1ff)|0xed):(expected&~1));
            if(pe&2)expected=uint16_t((expected&~0x1e00)|(technique.starts_with("FX_")?0x1200:0));
            Check(flags==expected,"Template alpha/blend state mismatch");
            Check(data[14]==((pe&2)&&technique.starts_with("FX_")?0:present?127:0),"Template threshold mismatch");
          }
        }
        Check(saw_z==(present || bool(pe&4)) && saw_alpha==(present || bool(pe&3)),"Template property completeness mismatch");
      }
      const auto serialized=model.Write();Check(!AdaptWiiTemplateRenderStates(model) && serialized==model.Write(),"Template PE adapter is not idempotent");
      if(known && pe==7){
        // The pass override is a separate admission: absent/wrong mounted
        // effects cannot authorize a material name that will fall back again.
        const auto bit=blend?kTemplateAlphaEffect:kTemplateOpaqueEffect;
        for(uint32_t available:{0u,uint32_t(kTemplateReadOpaqueEffect),uint32_t(bit),112u}){
          Nif copy(serialized);const auto before=copy.blocks[mesh_link].Data();Bytes original_mesh(before.begin(),before.end());
          const auto expected=bool(available&bit);Check(AdaptWiiTemplateRenderStates(copy,available)==uint32_t(expected),"Template pass availability gate failed");
          const auto after=copy.blocks[mesh_link].Data();Cursor material{after,16+54,copy.big};
          const auto count=material.Word();material.Take(size_t(count)*4);material.Word();Check(material.Word()==1,"Template material count changed");
          const auto name_at=material.at;const auto name=copy.String(material.Word());
          Check(name==(expected?(blend?"EM1T_DefaultShaderTextured":"EM1O_DefaultShaderTextured"):technique),"Wrong native template pass");
          for(size_t i=0;i<after.size();++i)Check((expected && i>=name_at && i<name_at+4)||original_mesh[i]==after[i],"Template pass changed geometry or property links");
          const auto once=copy.Write();Check(!AdaptWiiTemplateRenderStates(copy,available) && once==copy.Write(),"Template pass mapping is not idempotent");
        }
      }
    }
    error.clear();return true;
  }catch(const std::exception& e){error=e.what();return false;}
}
}
