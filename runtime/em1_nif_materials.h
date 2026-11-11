#pragma once
#include "em1_nif_container.h"
#include "em1_material_effects.h"
#include "em1_nif_template_state.h"
#include <map>

namespace em2::em1 {
// Opt-in name mapping for original steam systems. Preserve every authored
// texture, sampler, alpha/depth property, controller and particle payload.
// Distortion is deliberately unmapped: its accumulation pass needs validation.
inline uint32_t AdaptPreviewParticleMaterials(nif::Nif& model) {
  uint32_t changed=0;
  for(auto& block:model.blocks){
    const auto& kind=model.types[block.type];
    if(kind!="NiPSParticleSystem" && kind!="NonUniformParticleSystem")continue;
    const auto bytes=block.Data();nif::Cursor c{bytes,0,model.big};
    c.Word();const auto extras=c.Word();
    nif::Check(extras<=100000,"Particle extra-data count exceeds limit");
    c.Take(size_t(extras)*4);c.Word();c.Take(54);
    const auto properties=c.Word();
    nif::Check(properties<=100000,"Particle property count exceeds limit");
    c.Take(size_t(properties)*4);c.Word();const auto count=c.Word();
    nif::Check(count<=1000,"Particle technique count exceeds limit");
    nif::Bytes replacement;
    for(uint32_t i=0;i<count;++i){
      const auto at=c.at;const auto name=c.Word();c.Word();
      if(name==UINT32_MAX)continue;
      const auto technique=model.String(name);const char* target=nullptr;
      if(technique=="FX_alphaBlend_singleChannel")target="Gen3_FX_alphaBlend_singleChannel";
      if(technique=="FX_DiffuseAlpha_VertexColorAlpha")target="Gen3_FX_DiffuseAlpha_VertexColorAlpha";
      if(!target)continue;
      nif::Check(count==1,"Particle candidate requires one authored technique");
      if(replacement.empty())replacement.assign(bytes.begin(),bytes.end());
      const auto mapped=model.AddString(target);
      for(unsigned b=0;b<4;++b)replacement[at+b]=uint8_t(mapped>>(8*(model.big?3-b:b)));
      ++changed;
    }
    if(!replacement.empty())block.replacement=std::move(replacement);
  }
  return changed;
}
// Only audited two-map, untransformed layouts can use these vertex interfaces.
// The dual-UV interface is separately gated and requires source UV1.
inline bool HasUntransformedBlendMaps(const nif::Nif& model, uint32_t link, bool dual_uv=false, bool cutout=false, bool masked=false) {
  if(link>=model.blocks.size())return false;
  const auto& block=model.blocks[link];
  if(model.types[block.type]!="NiTexturingProperty")return false;
  const auto bytes=block.Data();nif::Cursor c{bytes,0,model.big};
  c.Word();const auto extras=c.Word();if(extras>100000)return false;
  c.Take(size_t(extras)*4);c.Word();c.Short();const auto standard=c.Word();
  if(standard!=9)return false;
  for(unsigned i=0;i<standard;++i)if(c.Take(1)[0])return false;
  const unsigned maps=masked?3:cutout?1:2;
  if(c.Word()!=maps)return false;
  for(unsigned i=0;i<maps;++i){
    if(c.Take(1)[0]!=1)return false;
    const auto source=c.Word();if(source>=model.blocks.size())return false;
    if(c.Short()!=(0x3200+(dual_uv && i==1 ? 1 : 0)) || c.Short()!=1 || c.Take(1)[0]!=0)return false;
    if(c.Word()!=0)return false;
  }
  return c.at==bytes.size();
}
// Exploration-preview bridge, not a general Wii TEV compiler. The retail EM2
// technique samples shader-map 0 and vertex colour, and participates in native
// material/render-state setup. Its name is the technique, not an FXLO filename.
// The original Wii template uses Modulate2X; exact colour equivalence still
// needs a shader-level oracle. Original R3MTStateOverrideFlags=7 selects the
// template's PE state (Wii REL 0x179c00), which for this exact technique means
// Blend=None, alpha-test off and LEQUAL depth test/write. Legacy NiAlpha bytes
// are NOT the effective Wii state. Unmapped techniques remain untouched.
inline uint32_t AdaptPreviewDiffuseMaterials(nif::Nif& model, bool masked_candidate=false, bool dual_uv_candidate=false, bool cutout_candidate=false,
    uint32_t simple_effects=0,std::array<uint32_t,4>* effect_mappings=nullptr) {
  uint32_t changed = 0;
  struct AlphaPatch { uint32_t mesh, property; size_t offset; bool cutout; };
  std::vector<AlphaPatch> alpha_patches;
  struct CullPatch { uint32_t mesh, property, count; size_t count_at, link_at; };
  std::vector<CullPatch> cull_patches;
  std::vector<std::pair<uint32_t,uint32_t>> depth_patches;
  struct DepthMaterialPatch {uint32_t mesh,mapped;size_t offset;};
  std::vector<DepthMaterialPatch> depth_material_patches;
  const auto original_blocks = uint32_t(model.blocks.size());
  for (uint32_t mesh_index=0; mesh_index<original_blocks; ++mesh_index) {
    auto& block = model.blocks[mesh_index];
    if (model.types[block.type] != "NiMesh") continue;
    const auto bytes = block.Data();
    nif::Cursor c{bytes, 0, model.big};
    c.Word();
    const auto extra = c.Word();
    nif::Check(extra <= 100000, "Material adapter extra-data table exceeds limit");
    uint32_t override_flags=UINT32_MAX;
    unsigned depth_enabled_count=0,depth_material_count=0;
    bool depth_enabled=false;uint32_t depth_material=UINT32_MAX;size_t depth_material_slot=0;
    for(uint32_t i=0;i<extra;++i) {
      const auto link=c.Word();
      nif::Check(link<original_blocks,"Invalid material extra-data link");
      const auto& data=model.blocks[link];
      if(cutout_candidate && (model.types[data.type]=="NiBooleanExtraData" || model.types[data.type]=="NiStringExtraData")) {
        const auto payload=data.Data();nif::Check(payload.size()>=4,"Truncated prepass extra data");
        const auto name=nif::U32(payload,0,model.big);
        if(name!=UINT32_MAX && model.String(name)=="Option:ZPrepass") {
          nif::Check(model.types[data.type]=="NiBooleanExtraData" && payload.size()==5,"Invalid prepass option");
          ++depth_enabled_count;depth_enabled=payload[4]==1;
        }
        if(name!=UINT32_MAX && model.String(name)=="Option:ZPrepassMaterial") {
          nif::Check(model.types[data.type]=="NiStringExtraData" && payload.size()==8,"Invalid prepass technique");
          ++depth_material_count;depth_material=nif::U32(payload,4,model.big);
          depth_material_slot=c.at-4;
        }
      }
      if(model.types[data.type]=="NiIntegerExtraData") {
        const auto payload=data.Data();
        nif::Check(payload.size()==8,"Invalid material integer extra data");
        const auto name=nif::U32(payload,0,model.big);
        if(name!=UINT32_MAX && model.String(name)=="R3MTStateOverrideFlags")
          override_flags=nif::U32(payload,4,model.big);
      }
    }
    c.Word();
    c.Take(54); // NiAVObject flags and authored transform.
    const auto properties = c.Word();
    nif::Check(properties <= 100000, "Material adapter property table exceeds limit");
    const auto properties_at=c.at;
    c.Take(size_t(properties) * 4); c.Word(); // Property links and collision link.
    const auto count = c.Word();
    nif::Check(count <= 1000, "Material adapter technique table exceeds limit");
    nif::Bytes replacement;bool cutout_mesh=false,material_changed=false;
    const char* simple_alias=nullptr;const char* masked_alias=nullptr;
    if(simple_effects && (override_flags==0 || override_flags==4)){
      uint32_t alpha_count=0;uint16_t flags=0;uint8_t threshold=0;
      for(uint32_t p=0;p<properties;++p){
        const auto link=nif::U32(bytes,properties_at+size_t(p)*4,model.big);
        nif::Check(link<original_blocks,"Invalid simple-alpha property link");
        if(model.types[model.blocks[link].type]!="NiAlphaProperty")continue;
        const auto alpha=model.blocks[link].Data();nif::Cursor property{alpha,0,model.big};
        property.Word();const auto extras=property.Word();nif::Check(extras<=100000,"Simple alpha property extra-data limit");
        property.Take(size_t(extras)*4);property.Word();flags=property.Short();threshold=property.Take(1)[0];
        nif::Check(property.at==alpha.size(),"Unsupported simple-alpha property layout");++alpha_count;
      }
      simple_alias=SelectSimpleMaterialEffect(override_flags,alpha_count,flags,threshold,simple_effects);
      if(SelectMaskedMaterialEffect(override_flags,alpha_count,flags,threshold,simple_effects)){
        unsigned textures=0,matching=0;
        for(uint32_t p=0;p<properties;++p){
          const auto link=nif::U32(bytes,properties_at+size_t(p)*4,model.big);
          if(model.types[model.blocks[link].type]!="NiTexturingProperty")continue;
          ++textures;matching+=HasUntransformedBlendMaps(model,link,false,false,true);
        }
        if(textures==1 && matching==1)masked_alias="EM1H_StaticEnv_Blend";
      }
    }
    const auto count_effect=[&](std::string_view mapped){if(effect_mappings){
      if(mapped=="EM1S_StaticEnv_Simple")++(*effect_mappings)[0];
      if(mapped=="EM1G_StaticEnv_Blend")++(*effect_mappings)[1];
      if(mapped=="EM1Q_StaticEnv_Blend")++(*effect_mappings)[2];
      if(mapped=="EM1H_StaticEnv_Blend")++(*effect_mappings)[3];
    }};
    const auto ensure_depth=[&](uint32_t mapped) {
      if(!cutout_candidate)return;
      if(override_flags!=7){
        // PE4 overrides depth with the audited LEQUAL/write template but
        // preserves authored NiAlpha. Alpha TEST is cutout coverage, not
        // blending: those meshes still need the native depth pass. PE0 keeps
        // authored depth and is deliberately outside this template proof.
        if(override_flags!=4)return;
        unsigned alpha_count=0;bool blending=false;
        for(uint32_t p=0;p<properties;++p){
          const auto link=nif::U32(bytes,properties_at+size_t(p)*4,model.big);
          nif::Check(link<original_blocks,"Invalid property-alpha depth link");
          if(model.types[model.blocks[link].type]!="NiAlphaProperty")continue;
          ++alpha_count;const auto alpha=model.blocks[link].Data();
          nif::Cursor property{alpha,0,model.big};property.Word();const auto extras=property.Word();
          nif::Check(extras<=100000,"Depth alpha extra-data count exceeds limit");property.Take(size_t(extras)*4);property.Word();
          const auto flags=property.Short();property.Take(1);
          nif::Check(property.at==alpha.size(),"Unsupported depth alpha layout");
          blending|=(flags&1)!=0;
        }
        // Multiple same-type properties have unaudited native precedence.
        // Retain them unchanged instead of assuming opaque coverage.
        if(alpha_count>1 || blending)return;
      }
      if(depth_enabled_count || depth_material_count) {
        nif::Check(depth_enabled_count==1 && depth_material_count==1 && depth_enabled,
            "Conflicting native prepass metadata");
        if(depth_material!=mapped){
          nif::Check((simple_alias && model.String(depth_material)=="Gen3_StaticEnv_Simple" && model.String(mapped)==simple_alias) ||
              (masked_alias && model.String(depth_material)=="EM1M_StaticEnv_Blend" && model.String(mapped)==masked_alias),
              "Conflicting native prepass material");
          if(replacement.empty())replacement.assign(bytes.begin(),bytes.end());
          depth_material_patches.push_back({mesh_index,mapped,depth_material_slot});
        }
        return;
      }
      if(replacement.empty())replacement.assign(bytes.begin(),bytes.end());
      depth_patches.emplace_back(mesh_index,mapped);
    };
    for (uint32_t i = 0; i < count; ++i) {
      const size_t at = c.at;
      const auto name = c.Word(); c.Word(); // Keep the associated extra-data link.
      if (name == UINT32_MAX) continue;
      const auto technique=model.String(name);
      // ReadModelClosure converts basic materials before this candidate runs.
      // Retrofit depth metadata without remapping or altering their properties.
      if((cutout_candidate || simple_effects) && (technique=="Gen3_StaticEnv_Simple" || technique=="EM1S_StaticEnv_Simple" || technique=="EM1G_StaticEnv_Blend" || technique=="EM1Q_StaticEnv_Blend" || technique=="EM1H_StaticEnv_Blend" || technique=="EM1W_StaticEnv_Blend" ||
          technique=="EM1M_StaticEnv_Blend" || technique=="EM1D_StaticEnv_Blend" || technique=="EM1A_StaticEnv_Blend")) {
        nif::Check(count==1 && (override_flags==7 ||
            ((override_flags==0 || override_flags==4) && (technique=="Gen3_StaticEnv_Simple" || technique=="EM1S_StaticEnv_Simple" || technique=="EM1G_StaticEnv_Blend" || technique=="EM1Q_StaticEnv_Blend" ||
                technique=="EM1M_StaticEnv_Blend" || technique=="EM1H_StaticEnv_Blend"))),"Unaudited native preview material");
        uint32_t mapped=name;
        if(simple_alias && technique=="Gen3_StaticEnv_Simple"){
          // Only the caller's verified mounted effect availability enables
          // this ONE-map alias. Opaque template7 retains its native alpha1.
          mapped=model.AddString(simple_alias);
          if(replacement.empty())replacement.assign(bytes.begin(),bytes.end());
          for(unsigned b=0;b<4;++b)replacement[at+b]=uint8_t(mapped>>(8*(model.big?3-b:b)));
          ++changed;
        }
        if(masked_alias && technique=="EM1M_StaticEnv_Blend"){
          mapped=model.AddString(masked_alias);
          if(replacement.empty())replacement.assign(bytes.begin(),bytes.end());
          for(unsigned b=0;b<4;++b)replacement[at+b]=uint8_t(mapped>>(8*(model.big?3-b:b)));
          ++changed;
        }
        count_effect(model.String(mapped));
        ensure_depth(mapped);continue;
      }
      const bool mask=masked_candidate && technique=="GEN_Diffuse_VertexColorAlpha_Blend_Mask";
      bool blend=false,dual=false,cutout=false;
      if(cutout_candidate && technique=="GEN_DiffuseAlpha_VertexColor_AlphaTest")
        for(uint32_t p=0;p<properties;++p)
          cutout|=HasUntransformedBlendMaps(model,nif::U32(bytes,properties_at+size_t(p)*4,model.big),false,true);
      if(masked_candidate && technique=="GEN_DiffuseAlpha_VertexColorAlpha_Blend")
        for(uint32_t p=0;p<properties;++p) {
          const auto link=nif::U32(bytes,properties_at+size_t(p)*4,model.big);
          blend|=HasUntransformedBlendMaps(model,link);
          dual|=dual_uv_candidate && HasUntransformedBlendMaps(model,link,true);
        }
      if (technique != "GEN_Diffuse_VertexColor" && !mask && !blend && !dual && !cutout) continue;
      // Original REL179C00 selects alpha-test with bit2, depth with bit4 and
      // blending with bit1 (resolved GX calls in the source DOL). Simple
      // diffuse override4 uses template depth but authored NiAlpha state;
      // override0 retains all authored NiProperty state, including depth.
      const bool property_alpha=(override_flags==0 || override_flags==4) &&
          (technique=="GEN_Diffuse_VertexColor" || mask);
      nif::Check((override_flags==7 || property_alpha) && count==1,
          "Diffuse preview requires the audited Wii template PE override and one material");
      if(override_flags==4)for(uint32_t p=0;p<properties;++p){
        const auto link=nif::U32(bytes,properties_at+size_t(p)*4,model.big);
        nif::Check(link<original_blocks && model.types[model.blocks[link].type]!="NiZBufferProperty",
            "Partial PE override requires a native template-depth adapter for its explicit ZBuffer property");
      }
      if (replacement.empty()) replacement.assign(bytes.begin(), bytes.end());
      const auto mapped = model.AddString(cutout?"EM1A_StaticEnv_Blend":dual?"EM1D_StaticEnv_Blend":mask?(masked_alias?masked_alias:"EM1M_StaticEnv_Blend"):blend?"EM1W_StaticEnv_Blend":
          simple_alias && property_alpha?simple_alias:"Gen3_StaticEnv_Simple");
      count_effect(model.String(mapped));
      for (unsigned b = 0; b < 4; ++b)
        replacement[at+b] = uint8_t(mapped >> (8*(model.big ? 3-b : b)));
      ensure_depth(mapped);material_changed=true;
      cutout_mesh|=cutout;
      ++changed;
    }
    const auto active=c.Word();c.Take(1);
    if (!replacement.empty()) {
      nif::Check(active==0,"Mapped diffuse preview material is not active");
      uint32_t stencil=UINT32_MAX;size_t stencil_at=properties_at+size_t(properties)*4;
      for(uint32_t j=0;material_changed && j<properties;++j) {
        const auto at=properties_at+size_t(j)*4;
        const auto link=nif::U32(bytes,at,model.big);
        nif::Check(link<original_blocks,"Invalid material property link");
        if(override_flags==7 && model.types[model.blocks[link].type]=="NiAlphaProperty")
          alpha_patches.push_back({mesh_index,link,at,cutout_mesh});
        if(model.types[model.blocks[link].type]=="NiStencilProperty") {stencil=link;stencil_at=at;}
      }
      if(cutout_mesh)cull_patches.push_back({mesh_index,stencil,properties,properties_at-4,stencil_at});
      block.replacement = std::move(replacement);
    }
  }
  // Append clones after traversal: shared properties on other techniques must
  // remain unchanged, and growing the block table must not invalidate views.
  for(const auto& patch:depth_material_patches){
    nif::Bytes material;nif::Number(material,model.AddString("Option:ZPrepassMaterial"),model.big);nif::Number(material,patch.mapped,model.big);
    const auto link=model.AddBlock("NiStringExtraData",std::move(material));auto& mesh=model.blocks[patch.mesh].replacement;
    for(unsigned b=0;b<4;++b)mesh[patch.offset+b]=uint8_t(link>>(8*(model.big?3-b:b)));
  }
  std::map<std::pair<uint32_t,bool>,uint32_t> clones;
  for(const auto& patch:alpha_patches) {
    const auto original=model.blocks[patch.property].Data();
    nif::Cursor c{original,0,model.big};c.Word();const auto extra=c.Word();
    nif::Check(extra<=100000,"Alpha property extra-data table exceeds limit");
    c.Take(size_t(extra)*4);c.Word();const auto flags_at=c.at;const auto flags=c.Short();c.Take(1);
    nif::Check(c.at==original.size(),"Unsupported alpha property layout");
    const auto effective=patch.cutout?uint16_t((flags & ~uint16_t(0x1E01)) | 0x1A00):uint16_t(flags & ~uint16_t(0x0201));
    const auto threshold=patch.cutout?uint8_t(128):original[flags_at+2];
    if(effective==flags && threshold==original[flags_at+2])continue;
    auto found=clones.find({patch.property,patch.cutout});
    uint32_t link;
    if(found==clones.end()) {
      nif::Bytes clone(original.begin(),original.end());
      clone[flags_at]=uint8_t(effective>>(model.big?8:0));
      clone[flags_at+1]=uint8_t(effective>>(model.big?0:8));
      clone[flags_at+2]=threshold;
      link=model.AddBlock("NiAlphaProperty",std::move(clone));clones.emplace(std::pair{patch.property,patch.cutout},link);
    } else link=found->second;
    auto& mesh=model.blocks[patch.mesh].replacement;
    for(unsigned b=0;b<4;++b)mesh[patch.offset+b]=uint8_t(link>>(8*(model.big?3-b:b)));
  }
  // Preserve the Wii cutout's authored two-sided state in the native property.
  // The private EM1A effect also preserves this state in its native pass.
  // Draw-mode bits: https://github.com/niftools/nifxml/blob/master/nif.xml
  // Preserve other stencil settings, or add a disabled DRAW_BOTH property.
  for(const auto& patch:cull_patches) {
    nif::Bytes property;size_t flags_at=12;
    if(patch.property==UINT32_MAX) {
      for(auto word:{UINT32_MAX,0u,UINT32_MAX})nif::Number(property,word,model.big);
      nif::Number(property,0x4D80,model.big,2);
      nif::Number(property,0,model.big);nif::Number(property,UINT32_MAX,model.big);
    } else {
      const auto source=model.blocks[patch.property].Data();property.assign(source.begin(),source.end());
      nif::Cursor c{source,0,model.big};c.Word();const auto extras=c.Word();
      nif::Check(extras<=100000,"Stencil extra-data table exceeds limit");c.Take(size_t(extras)*4);c.Word();flags_at=c.at;
      const auto flags=c.Short();c.Take(8);nif::Check(c.at==source.size(),"Unknown stencil property layout");
      const auto both=uint16_t(flags|0x0C00);
      property[flags_at]=uint8_t(both>>(model.big?8:0));property[flags_at+1]=uint8_t(both>>(model.big?0:8));
    }
    const auto link=model.AddBlock("NiStencilProperty",std::move(property));
    auto& bytes=model.blocks[patch.mesh].replacement;
    if(patch.property==UINT32_MAX) {
      bytes.insert(bytes.begin()+patch.link_at,4,0);
      for(unsigned b=0;b<4;++b)bytes[patch.count_at+b]=uint8_t((patch.count+1)>>(8*(model.big?3-b:b)));
    }
    for(unsigned b=0;b<4;++b)bytes[patch.link_at+b]=uint8_t(link>>(8*(model.big?3-b:b)));
  }
  // Audited opaque templates and PE4 non-blended property-alpha materials
  // are depth writers, including texture blends.
  // With the complete private material candidate enabled, register their
  // matching techniques in the native prepass. Authored NiAlpha test/threshold
  // and the same mapped shader stay intact; GPU alpha coverage is separate.
  if(!depth_patches.empty()) {
    nif::Bytes enabled;
    nif::Number(enabled,model.AddString("Option:ZPrepass"),model.big);
    enabled.push_back(1);
    const auto enabled_link=model.AddBlock("NiBooleanExtraData",std::move(enabled));
    const auto material_name=model.AddString("Option:ZPrepassMaterial");
    std::map<uint32_t,uint32_t> materials;
    for(const auto& [mesh_index,technique]:depth_patches) {
      auto it=materials.find(technique);
      if(it==materials.end()) {
        nif::Bytes material;nif::Number(material,material_name,model.big);nif::Number(material,technique,model.big);
        it=materials.emplace(technique,model.AddBlock("NiStringExtraData",std::move(material))).first;
      }
      auto& mesh=model.blocks[mesh_index].replacement;
      const auto count=nif::U32(mesh,4,model.big);
      nif::Bytes links;for(auto link:{enabled_link,it->second})nif::Number(links,link,model.big);
      mesh.insert(mesh.begin()+8+size_t(count)*4,links.begin(),links.end());
      for(unsigned b=0;b<4;++b)mesh[4+b]=uint8_t((count+2)>>(8*(model.big?3-b:b)));
    }
  }
  return changed;
}
inline bool TestMaskedPropertyDepth(std::string& error){
  using namespace nif;
  try{
    for(bool big:{false,true})for(uint32_t pe:{0u,4u})for(uint32_t available:{0u,8u})
      for(uint16_t flags:{uint16_t(0x12EC),uint16_t(0x1AEC),uint16_t(0x12ED)})
      for(uint8_t threshold:{uint8_t(126),uint8_t(127)})for(bool valid_uv:{false,true})for(unsigned prior=0;prior<3;++prior){
      Bytes empty;constexpr std::string_view signature="Gamebryo File Format, Version 20.6.5.0\n";
      Append(empty,View(reinterpret_cast<const uint8_t*>(signature.data()),signature.size()));
      Number(empty,0x14060500,false);Number(empty,!big,false,1);Number(empty,15,false);Number(empty,0,false);
      Number(empty,0,big,2);for(unsigned i=0;i<4;++i)Number(empty,0,big);Nif model(empty);
      Bytes state;Number(state,model.AddString("R3MTStateOverrideFlags"),big);Number(state,pe,big);
      const auto state_link=model.AddBlock("NiIntegerExtraData",std::move(state));
      Bytes alpha;for(auto value:{UINT32_MAX,0u,UINT32_MAX})Number(alpha,value,big);
      Number(alpha,flags,big,2);Number(alpha,threshold,big,1);const auto alpha_link=model.AddBlock("NiAlphaProperty",alpha);
      const auto image=model.AddBlock("NiSourceTexture",{});
      Bytes texture;for(auto value:{UINT32_MAX,0u,UINT32_MAX})Number(texture,value,big);
      Number(texture,0,big,2);Number(texture,9,big);texture.insert(texture.end(),9,0);Number(texture,3,big);
      for(unsigned i=0;i<3;++i){Number(texture,1,big,1);Number(texture,image,big);
        Number(texture,!valid_uv && i==2?0x3201:0x3200,big,2);Number(texture,1,big,2);
        Number(texture,0,big,1);Number(texture,0,big);}
      const auto texture_link=model.AddBlock("NiTexturingProperty",texture);
      auto mesh=[&](std::string_view technique){Bytes bytes;Number(bytes,UINT32_MAX,big);Number(bytes,1,big);
        Number(bytes,state_link,big);Number(bytes,UINT32_MAX,big);bytes.resize(bytes.size()+54,0);
        Number(bytes,2,big);Number(bytes,alpha_link,big);Number(bytes,texture_link,big);Number(bytes,UINT32_MAX,big);
        Number(bytes,1,big);Number(bytes,model.AddString(technique),big);Number(bytes,UINT32_MAX,big);
        Number(bytes,0,big);Number(bytes,0,big,1);return bytes;};
      const auto mapped_mesh=model.AddBlock("NiMesh",mesh("GEN_Diffuse_VertexColorAlpha_Blend_Mask"));
      const auto sentinel=mesh("Unmapped_Sentinel");const auto untouched=model.AddBlock("NiMesh",sentinel);
      if(prior)AdaptPreviewDiffuseMaterials(model,true,true,prior==2);
      const bool selected=pe==4 && available==8 && flags==0x12EC && threshold==127 && valid_uv;
      std::array<uint32_t,4> counts{};
      Check(AdaptPreviewDiffuseMaterials(model,true,true,true,available,&counts)==unsigned(!prior || selected),"Masked cutout mapping count incorrect");
      Check(counts[0]==0 && counts[1]==0 && counts[2]==0 && counts[3]==unsigned(selected),"Masked cutout diagnostic count incorrect");
      const char* expected=selected?"EM1H_StaticEnv_Blend":"EM1M_StaticEnv_Blend";
      const auto bytes=model.blocks[mapped_mesh].Data();Cursor c{bytes,0,big};c.Word();const auto extras=c.Word();
      const bool depth=pe==4 && !(flags&1);Check(extras==(depth?3u:1u),"Masked cutout changed authored depth eligibility");
      unsigned passes=0;for(uint32_t i=0;i<extras;++i){const auto link=c.Word();const auto data=model.blocks[link].Data();
        if(model.String(U32(data,0,big))=="Option:ZPrepassMaterial"){
          Check(model.String(U32(data,4,big))==expected,"Masked depth/color alpha coverage differs");++passes;}}
      Check(passes==unsigned(depth),"Masked cutout depth material missing");c.Word();c.Take(54);
      Check(c.Word()==2 && c.Word()==alpha_link && c.Word()==texture_link,"Masked cutout rebound source properties");
      c.Word();Check(c.Word()==1 && model.String(c.Word())==expected,"Masked cutout selected unsupported alias");
      const auto alpha_after=model.blocks[alpha_link].Data(),texture_after=model.blocks[texture_link].Data(),other=model.blocks[untouched].Data();
      Check(std::equal(alpha.begin(),alpha.end(),alpha_after.begin()) && std::equal(texture.begin(),texture.end(),texture_after.begin()) &&
          std::equal(sentinel.begin(),sentinel.end(),other.begin()),"Masked cutout mutated source/shared properties");
      const auto serialized=model.Write();Check(AdaptPreviewDiffuseMaterials(model,true,true,true,available)==0 && model.Write()==serialized,"Masked cutout mapping is not idempotent");
    }
    error.clear();return true;
  }catch(const std::exception& failure){error=failure.what();return false;}
}
inline bool TestPreviewDiffusePeState(std::string& error){
  try {
    using namespace nif;
    if(!TestWiiTemplateRenderStates(error))return false;
    Check(TestSimpleMaterialEffectDirectory(error),error.c_str());
    Check(TestMaskedPropertyDepth(error),error.c_str());
    for(bool big:{false,true})for(uint32_t override_flags:{0u,4u,7u})for(bool mask:{false,true}){
      Bytes empty;constexpr std::string_view signature="Gamebryo File Format, Version 20.6.5.0\n";
      Append(empty,View(reinterpret_cast<const uint8_t*>(signature.data()),signature.size()));
      Number(empty,0x14060500,false);Number(empty,!big,false,1);Number(empty,15,false);Number(empty,0,false);
      Number(empty,0,big,2);for(unsigned i=0;i<4;++i)Number(empty,0,big);
      Nif model(empty);
      Bytes state;Number(state,model.AddString("R3MTStateOverrideFlags"),big);Number(state,override_flags,big);
      const auto state_link=model.AddBlock("NiIntegerExtraData",std::move(state));
      Bytes alpha;for(auto v:{UINT32_MAX,0u,UINT32_MAX})Number(alpha,v,big);Number(alpha,0x0ced,big,2);Number(alpha,0,big,1);
      const auto original_alpha=alpha;const auto alpha_link=model.AddBlock("NiAlphaProperty",std::move(alpha));
      Bytes mesh;Number(mesh,UINT32_MAX,big);Number(mesh,1,big);Number(mesh,state_link,big);Number(mesh,UINT32_MAX,big);
      mesh.resize(mesh.size()+54,0);Number(mesh,1,big);const auto properties_at=mesh.size();Number(mesh,alpha_link,big);Number(mesh,UINT32_MAX,big);
      Number(mesh,1,big);const auto material_at=mesh.size();Number(mesh,model.AddString(mask?"GEN_Diffuse_VertexColorAlpha_Blend_Mask":"GEN_Diffuse_VertexColor"),big);Number(mesh,UINT32_MAX,big);Number(mesh,0,big);Number(mesh,0,big,1);
      const auto mesh_link=model.AddBlock("NiMesh",std::move(mesh));
      Check(AdaptPreviewDiffuseMaterials(model,mask,false,false,override_flags==7?7u:0u)==1,"Diffuse state mapping failed");
      auto mapped=model.blocks[mesh_link].Data();
      Check(model.String(U32(mapped,material_at,big))==(mask?"EM1M_StaticEnv_Blend":"Gen3_StaticEnv_Simple"),"Diffuse technique was not mapped");
      const auto new_alpha=U32(mapped,properties_at,big);
      Check(model.blocks[alpha_link].Data().size()==original_alpha.size() &&
            std::equal(original_alpha.begin(),original_alpha.end(),model.blocks[alpha_link].Data().begin()),"Shared original alpha property was modified");
      if(override_flags!=7){
        Check(new_alpha==alpha_link,"Property-alpha material lost its authored alpha binding");
        const auto blocks=model.blocks.size();
        AdaptPreviewDiffuseMaterials(model,true,true,true);
        Check(model.blocks.size()==blocks && U32(model.blocks[mesh_link].Data(),4,big)==1,"Property-alpha material entered opaque depth prepass");
      }else{
        Check(new_alpha!=alpha_link && (U16(model.blocks[new_alpha].Data(),12,big)&0x0201)==0,"Template-alpha material retained legacy blend/test state");
        AdaptPreviewDiffuseMaterials(model,true,true,true);
        Check(U32(model.blocks[mesh_link].Data(),4,big)==3,"Opaque material lost native depth metadata");
      }
    }
    // Actual MeanStreet property-alpha flags0x12EC/0x1AEC are alpha-tested
    // NON-blended. Their template-depth override4 must not be conflated with
    // real alpha blending0x00ED/0x0C0D. PE0 remains outside the depth proof.
    for(bool big:{false,true})for(uint32_t override_flags:{0u,4u})
      for(uint32_t alpha_flags:{UINT32_MAX,0x12ECu,0x1AECu,0x00EDu,0x0C0Du})for(uint32_t available:{0u,1u,2u,4u,7u})for(unsigned prior=0;prior<3;++prior){
      Bytes empty;constexpr std::string_view signature="Gamebryo File Format, Version 20.6.5.0\n";
      Append(empty,View(reinterpret_cast<const uint8_t*>(signature.data()),signature.size()));
      Number(empty,0x14060500,false);Number(empty,!big,false,1);Number(empty,15,false);Number(empty,0,false);
      Number(empty,0,big,2);for(unsigned i=0;i<4;++i)Number(empty,0,big);Nif model(empty);
      Bytes state;Number(state,model.AddString("R3MTStateOverrideFlags"),big);Number(state,override_flags,big);
      const auto state_link=model.AddBlock("NiIntegerExtraData",std::move(state));
      uint32_t alpha_link=UINT32_MAX;Bytes original_alpha;
      if(alpha_flags!=UINT32_MAX){
        for(auto value:{UINT32_MAX,0u,UINT32_MAX})Number(original_alpha,value,big);
        Number(original_alpha,alpha_flags,big,2);Number(original_alpha,127,big,1);
        alpha_link=model.AddBlock("NiAlphaProperty",original_alpha);
      }
      auto mesh=[&](std::string_view technique){
        Bytes bytes;Number(bytes,UINT32_MAX,big);Number(bytes,1,big);Number(bytes,state_link,big);Number(bytes,UINT32_MAX,big);
        bytes.resize(bytes.size()+54,0);Number(bytes,alpha_link==UINT32_MAX?0:1,big);
        if(alpha_link!=UINT32_MAX)Number(bytes,alpha_link,big);Number(bytes,UINT32_MAX,big);
        Number(bytes,1,big);Number(bytes,model.AddString(technique),big);Number(bytes,UINT32_MAX,big);Number(bytes,0,big);Number(bytes,0,big,1);
        return bytes;
      };
      const auto mapped_mesh=model.AddBlock("NiMesh",mesh("GEN_Diffuse_VertexColor"));
      const auto untouched=mesh("Unmapped_Sentinel");const auto untouched_mesh=model.AddBlock("NiMesh",untouched);
      const char* alias=override_flags==4 && alpha_flags==0x12EC && (available&2)?"EM1G_StaticEnv_Blend":
          override_flags==4 && alpha_flags==0x1AEC && (available&4)?"EM1Q_StaticEnv_Blend":
          alpha_flags!=UINT32_MAX && (alpha_flags&1) && !(alpha_flags&0x200) && (available&1)?"EM1S_StaticEnv_Simple":"Gen3_StaticEnv_Simple";
      if(prior)AdaptPreviewDiffuseMaterials(model,prior==2,prior==2,prior==2);
      std::array<uint32_t,4> counts{};
      Check(AdaptPreviewDiffuseMaterials(model,true,true,true,available,&counts)==unsigned(!prior || std::string_view(alias)!="Gen3_StaticEnv_Simple"),"Property-alpha depth fixture mapping failed");
      Check(counts[0]==unsigned(std::string_view(alias)=="EM1S_StaticEnv_Simple") && counts[1]==unsigned(std::string_view(alias)=="EM1G_StaticEnv_Blend") && counts[2]==unsigned(std::string_view(alias)=="EM1Q_StaticEnv_Blend"),"One-map alias mapping counts incorrect");
      const bool expected=override_flags==4 && (alpha_flags==UINT32_MAX || !(alpha_flags&1));
      const auto mapped=model.blocks[mapped_mesh].Data();Cursor cursor{mapped,0,big};cursor.Word();const auto extras=cursor.Word();
      Check(extras==(expected?3u:1u),"Property-alpha prepass eligibility changed transparency or omitted a cutout");
      unsigned enabled=0,material=0;
      for(uint32_t i=0;i<extras;++i){const auto link=cursor.Word();const auto data=model.blocks[link].Data();const auto name=model.String(U32(data,0,big));
        if(name=="Option:ZPrepass"){Check(model.types[model.blocks[link].type]=="NiBooleanExtraData" && data.size()==5 && data[4]==1,"Property-alpha depth enable metadata invalid");++enabled;}
        if(name=="Option:ZPrepassMaterial"){Check(model.types[model.blocks[link].type]=="NiStringExtraData" && model.String(U32(data,4,big))==alias,"Property-alpha depth pass used a different shader");++material;}
      }
      Check(enabled==unsigned(expected) && material==unsigned(expected),"Property-alpha depth metadata is incomplete");
      cursor.Word();cursor.Take(54);const auto properties=cursor.Word();Check(properties==(alpha_link==UINT32_MAX?0u:1u),"Property-alpha depth changed property count");
      if(alpha_link!=UINT32_MAX){
        Check(cursor.Word()==alpha_link,"Property-alpha depth cloned or rebound authored alpha");const auto data=model.blocks[alpha_link].Data();
        Check(data.size()==original_alpha.size() && std::equal(data.begin(),data.end(),original_alpha.begin()),"Property-alpha depth changed alpha test/threshold/blend bytes");
      }
      cursor.Word();Check(cursor.Word()==1,"Property-alpha material technique count changed");
      Check(model.String(cursor.Word())==alias,"Dedicated one-map alpha alias gate ignored");
      const auto original=model.blocks[untouched_mesh].Data();Check(original.size()==untouched.size() && std::equal(original.begin(),original.end(),untouched.begin()),"Property-alpha depth mutated an unmapped shared-property mesh");
      const auto blocks=model.blocks.size();AdaptPreviewDiffuseMaterials(model,true,true,true,available);
      Check(model.blocks.size()==blocks,"Property-alpha depth metadata is not idempotent");
    }
    error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
}
