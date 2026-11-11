#include "em1_nif_materials.h"
#include "em1_baked_lighting.h"
#include <fstream>
#include <iostream>
#include <set>

int main(int argc, char** argv) { try {
  using namespace em2::em1;
  nif::Check(argc == 2 || argc==4, "Usage: em1_nif_material_test cached-original-courtyard.nif [lighting.lit_cooked output.nif]");
  std::ifstream file(argv[1], std::ios::binary);
  nif::Check(bool(file), "Cannot read courtyard fixture");
  nif::Bytes bytes((std::istreambuf_iterator<char>(file)), {});
  nif::Nif original(bytes), candidate(bytes);
  const auto count = AdaptPreviewDiffuseMaterials(candidate);
  nif::Check(count == 70, "Expected exactly 70 courtyard diffuse material names");
  const auto output = candidate.Write(); nif::Nif restored(output);
  nif::Check(restored.blocks.size() == original.blocks.size()+1 && restored.types == original.types,
             "Expected one private alpha-property clone and unchanged type inventory");
  nif::Check(restored.string_values.size() == original.string_values.size()+1,
             "Expected precisely one added technique string");
  for (size_t i=0;i<original.string_values.size();++i)
    nif::Check(original.String(uint32_t(i)) == restored.String(uint32_t(i)), "Source string changed");
  uint32_t names=0,alpha_links=0;
  for (size_t i=0;i<original.blocks.size();++i) {
    const auto a=original.blocks[i].Data(), b=restored.blocks[i].Data();
    nif::Check(a.size()==b.size(), "Block size changed");
    std::set<size_t> permitted;
    if(original.types[original.blocks[i].type]=="NiMesh") {
      nif::Cursor c{a,0,original.big};c.Word();const auto extras=c.Word();c.Take(size_t(extras)*4);c.Word();
      c.Take(54);const auto props=c.Word();const auto properties_at=c.at;c.Take(size_t(props)*4);c.Word();const auto n=c.Word();
      for(uint32_t j=0;j<n;++j) {
        const size_t at=c.at;const auto index=c.Word();c.Word();
        if(index!=UINT32_MAX && original.String(index)=="GEN_Diffuse_VertexColor") {
          nif::Check(restored.String(nif::U32(b,at,restored.big))=="Gen3_StaticEnv_Simple", "Wrong target technique");
          for(size_t k=0;k<4;++k)permitted.insert(at+k);
          ++names;
          for(uint32_t q=0;q<props;++q) {
            const auto link_at=properties_at+size_t(q)*4;
            const auto old_link=nif::U32(a,link_at,original.big);
            if(original.types[original.blocks[old_link].type]!="NiAlphaProperty")continue;
            const auto old_alpha=original.blocks[old_link].Data();
            const auto flags=nif::U16(old_alpha,old_alpha.size()-3,original.big);
            if(!(flags&0x0201))continue;
            const auto new_link=nif::U32(b,link_at,restored.big);
            nif::Check(new_link==original.blocks.size() && restored.types[restored.blocks[new_link].type]=="NiAlphaProperty",
                       "Alpha state did not use the isolated shared clone");
            auto expected=nif::Bytes(old_alpha.begin(),old_alpha.end());const auto effective=uint16_t(flags&~0x0201);
            expected[expected.size()-3]=uint8_t(effective>>8);expected[expected.size()-2]=uint8_t(effective);
            const auto new_alpha=restored.blocks[new_link].Data();
            nif::Check(std::equal(expected.begin(),expected.end(),new_alpha.begin(),new_alpha.end()),
                       "Clone changed more than effective blend/test enable");
            for(size_t k=0;k<4;++k)permitted.insert(link_at+k);
            ++alpha_links;
          }
        }
      }
    }
    for(size_t j=0;j<a.size();++j)
      nif::Check(permitted.contains(j)||a[j]==b[j], "Changed property, texture, stream, transform or associated material link");
  }
  nif::Check(names==70 && alpha_links==50 && std::equal(original.footer.begin(),original.footer.end(),restored.footer.begin(),restored.footer.end()),
             "Material count or root footer changed");
  nif::Check(AdaptPreviewDiffuseMaterials(restored)==0 && restored.Write()==output, "Material adapter is not idempotent");
  nif::Check(AdaptPreviewDiffuseMaterials(restored,true)==33,"Expected 10 masked and 23 single-UV blends");
  const auto blended=restored.Write();nif::Nif checked(blended);
  unsigned masks=0,blends=0,dual=0;
  for(size_t i=0;i<original.blocks.size();++i){
    if(original.types[original.blocks[i].type]!="NiMesh")continue;
    const auto data=checked.blocks[i].Data();nif::Cursor c{data,0,checked.big};
    c.Word();const auto extras=c.Word();c.Take(size_t(extras)*4);c.Word();c.Take(54);
    const auto props=c.Word();c.Take(size_t(props)*4);c.Word();const auto count=c.Word();
    for(unsigned j=0;j<count;++j){const auto name=c.Word();c.Word();if(name==UINT32_MAX)continue;
      const auto text=checked.String(name);masks+=text=="EM1M_StaticEnv_Blend";blends+=text=="EM1W_StaticEnv_Blend";
      if(text=="GEN_DiffuseAlpha_VertexColorAlpha_Blend"){
        ++dual;const auto before=original.blocks[i].Data();
        nif::Check(std::equal(before.begin(),before.end(),data.begin(),data.end()),"Unsupported dual-UV mesh changed");
      }
    }
  }
  nif::Check(masks==10 && blends==23 && dual==11,"Blend mapping coverage mismatch");
  nif::Check(AdaptPreviewDiffuseMaterials(checked,true)==0 && checked.Write()==blended,"Blend mapping is not idempotent");
  nif::Check(AdaptPreviewDiffuseMaterials(checked,true,true)==11,"Expected eleven additional dual-UV mappings");
  const auto dual_bytes=checked.Write();nif::Nif dual_checked(dual_bytes);
  nif::Check(AdaptPreviewDiffuseMaterials(dual_checked,true,true)==0 && dual_checked.Write()==dual_bytes,"Dual UV mapping is not idempotent");
  nif::Check(AdaptPreviewDiffuseMaterials(dual_checked,true,true,true)==12,"Expected twelve additional cutout mappings");
  const auto cutout_bytes=dual_checked.Write();nif::Nif cutout_checked(cutout_bytes);
  unsigned cutouts=0;
  for(size_t i=0;i<original.blocks.size();++i){
    if(cutout_checked.types[cutout_checked.blocks[i].type]!="NiMesh")continue;
    nif::Cursor c{cutout_checked.blocks[i].Data(),0,cutout_checked.big};
    c.Word();const auto extras=c.Word();std::vector<uint32_t> extra_links;
    for(unsigned j=0;j<extras;++j)extra_links.push_back(c.Word());
    c.Word();c.Take(54);
    const auto props=c.Word();std::vector<uint32_t> links;
    for(unsigned j=0;j<props;++j)links.push_back(c.Word());c.Word();
    bool cutout=false;const auto count=c.Word();
    for(unsigned j=0;j<count;++j){const auto name=c.Word();c.Word();cutout|=name!=UINT32_MAX && cutout_checked.String(name)=="EM1A_StaticEnv_Blend";}
    if(!cutout)continue;++cutouts;unsigned prepass=0,prepass_material=0;
    for(auto link:extra_links) {
      const auto data=cutout_checked.blocks[link].Data();
      const auto name=cutout_checked.String(nif::U32(data,0,cutout_checked.big));
      if(name=="Option:ZPrepassMaterial") {
        nif::Check(cutout_checked.types[cutout_checked.blocks[link].type]=="NiStringExtraData" && data.size()==8 && cutout_checked.String(nif::U32(data,4,cutout_checked.big))=="EM1A_StaticEnv_Blend","Cutout prepass must use the alpha-discard material");++prepass_material;
      }
      if(name!="Option:ZPrepass")continue;
      nif::Check(cutout_checked.types[cutout_checked.blocks[link].type]=="NiBooleanExtraData" && data.size()==5 && data[4]==1,"Cutout depth prepass metadata mismatch");++prepass;
    }
    nif::Check(prepass==1 && prepass_material==1,"Cutout must have exactly one native depth-prepass option");
    unsigned alpha=0;
    for(auto link:links)if(cutout_checked.types[cutout_checked.blocks[link].type]=="NiAlphaProperty"){
      const auto data=cutout_checked.blocks[link].Data();
      nif::Check(data.size()==15 && (nif::U16(data,12,cutout_checked.big)&0x1E01)==0x1A00 && data[14]==128,"Cutout PE threshold/function/blend mismatch");++alpha;
    }
    nif::Check(alpha==1,"Cutout must own one alpha state");
    unsigned stencil=0;for(auto link:links)if(cutout_checked.types[cutout_checked.blocks[link].type]=="NiStencilProperty"){
      const auto data=cutout_checked.blocks[link].Data();nif::Check(data.size()==22 && (nif::U16(data,12,cutout_checked.big)&0x0C00)==0x0C00,"Cutout must draw both faces");++stencil;
    }
    nif::Check(stencil==1,"Cutout must own one face-culling state");
  }
  nif::Check(cutouts==12 && AdaptPreviewDiffuseMaterials(cutout_checked,true,true,true)==0 && cutout_checked.Write()==cutout_bytes,"Cutout count/idempotence failed");
  nif::Check(std::equal(original.blocks[32].Data().begin(),original.blocks[32].Data().end(),cutout_checked.blocks[32].Data().begin(),cutout_checked.blocks[32].Data().end()),"Shared original alpha property changed");
  nif::Nif full(bytes);
  nif::Check(AdaptPreviewDiffuseMaterials(full,true,true,true)==126,"Full material coverage changed");
  const auto check_prepasses=[](const nif::Nif& full) {
  unsigned prepass_meshes=0;
  for(const auto& block:full.blocks) {
    if(full.types[block.type]!="NiMesh")continue;
    nif::Cursor c{block.Data(),0,full.big};c.Word();const auto count=c.Word();
    unsigned enabled=0,material=0;uint32_t prepass_technique=UINT32_MAX;
    for(unsigned j=0;j<count;++j) {
      const auto link=c.Word();const auto data=full.blocks[link].Data();
      const auto name=full.String(nif::U32(data,0,full.big));
      if(name=="Option:ZPrepass") {nif::Check(data.size()==5 && data[4]==1,"Prepass must be enabled");++enabled;}
      if(name=="Option:ZPrepassMaterial") {nif::Check(data.size()==8,"Invalid prepass material");prepass_technique=nif::U32(data,4,full.big);++material;}
    }
    c.Word();c.Take(54);c.Take(size_t(c.Word())*4);c.Word();const auto techniques=c.Word();
    if(!enabled && !material)continue;
    nif::Check(enabled==1 && material==1 && techniques==1 && c.Word()==prepass_technique,"Prepass does not match the mesh color technique");++prepass_meshes;
  }
  nif::Check(prepass_meshes==126,"Not all audited opaque meshes have prepass metadata");
  };
  check_prepasses(full);
  nif::Nif baseline(bytes);
  nif::Check(AdaptPreviewDiffuseMaterials(baseline)==70,"Live first stage changed");
  const auto baseline_bytes=baseline.Write();nif::Nif live(baseline_bytes);
  nif::Check(AdaptPreviewDiffuseMaterials(live,true,true,true)==56,"Live second stage changed");
  check_prepasses(live);
  check_prepasses(cutout_checked);
  const auto live_bytes=live.Write();nif::Nif live_again(live_bytes);
  nif::Check(AdaptPreviewDiffuseMaterials(live_again,true,true,true)==0 && live_again.Write()==live_bytes,"Live staged prepass not idempotent");
  for(size_t i=0;i<original.blocks.size();++i) {
    if(original.types[original.blocks[i].type]=="NiMesh")continue;
    const auto before=original.blocks[i].Data(),after=full.blocks[i].Data();
    nif::Check(std::equal(before.begin(),before.end(),after.begin(),after.end()),"Full prepass adaptation changed an original non-mesh block");
  }
  nif::Check(std::equal(full.footer.begin(),full.footer.end(),original.footer.begin(),original.footer.end()),"Full prepass adaptation changed scene roots");
  const auto full_bytes=full.Write();nif::Nif full_roundtrip(full_bytes);
  nif::Check(AdaptPreviewDiffuseMaterials(full_roundtrip,true,true,true)==0 && full_roundtrip.Write()==full_bytes,"Full prepass metadata is not idempotent");
  if(argc==4) {
    std::ifstream cooked_file(argv[2],std::ios::binary);nif::Check(bool(cooked_file),"Missing cooked lighting fixture");
    nif::Bytes cooked((std::istreambuf_iterator<char>(cooked_file)),{});
    nif::Nif lit(bytes);nif::Check(ApplyBakedLighting(lit,cooked,0x17b19)==127,"Expected 127 authored lighting bindings");
    nif::Check(lit.blocks.size()==original.blocks.size()+127,"Expected isolated baked color streams");
    for(size_t i=0;i<original.blocks.size();++i) {
      const auto a=original.blocks[i].Data(),b=lit.blocks[i].Data();nif::Check(a.size()==b.size(),"Lighting resized an original block");
      size_t changed=0;for(size_t j=0;j<a.size();++j)changed+=a[j]!=b[j];
      if(original.types[original.blocks[i].type]=="NiMesh")nif::Check(changed>0 && changed<=4,"Lighting modified more than a stream link");
      else nif::Check(changed==0,"Lighting changed original shared data");
    }
    size_t last_path_word=0;
    const auto lookup=nif::U32(cooked,8,true),records=nif::U32(cooked,12,true);
    for(uint32_t i=0;i<records;++i)if(nif::U32(cooked,lookup+i*8,true)==0x17b19) {
      nif::Cursor c{cooked,nif::U32(cooked,lookup+i*8+4,true),true};const auto meshes=c.Word();
      for(uint32_t m=0;m<meshes;++m) {
        c.Take(size_t(c.Word())*4);const auto groups=c.Word();
        for(uint32_t g=0;g<groups;++g){const auto n=c.Word();c.Take(size_t(n)*4);if(n)last_path_word=c.at-4;}
      }
    }
    nif::Check(last_path_word!=0,"No late baked hierarchy path in fixture");
    for(unsigned bad=0;bad<4;++bad) {
      auto invalid=cooked;if(bad==1)invalid.resize(20);if(bad==2)invalid[0]=0;
      if(bad==3)std::fill(invalid.begin()+last_path_word,invalid.begin()+last_path_word+4,255);
      nif::Nif rejected(bytes);bool failed=false;
      try{ApplyBakedLighting(rejected,invalid,bad==0?0xffffffff:0x17b19);}catch(const std::exception&){failed=true;}
      nif::Check(failed && rejected.Write()==bytes,"Bad lighting partially modified a model");
    }
    const auto out=lit.Write();std::ofstream target(argv[3],std::ios::binary);target.write(reinterpret_cast<const char*>(out.data()),out.size());
    nif::Check(bool(target),"Cannot save baked lighting test result");
    std::cout << "PASS: 127 isolated baked bindings; source data preserved; invalid/missing lighting rejected without mutation\n";
  }
  std::cout << "PASS: 70 material names; 50 isolated alpha links; one effective-state clone; all other original bytes/links retained; plus 10 masked and 23 single-UV blends, 11 dual-UV meshes unchanged until separately enabled; then all 11 mapped; 12 cutouts have isolated GEQUAL128 properties; all 126 audited meshes have matching prepass metadata; original non-mesh blocks and roots preserved; idempotent.\n";
  return 0;
} catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
