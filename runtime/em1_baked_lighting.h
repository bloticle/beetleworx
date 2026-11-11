#pragma once
#include "em1_nif_container.h"
#include <map>
#include <set>
#include <functional>

namespace em2::em1 {
// BKLT0001 references original NIF child paths and original color indices.
// Apply before GX tuple expansion; never interpret these as expanded vertices.
inline uint32_t ApplyBakedLighting(nif::Nif& model,nif::View bytes,uint32_t entity) {
  using namespace nif;
  Check(bytes.size()>=16 && bytes.size()<=128*1024*1024 &&
    std::equal(bytes.begin(),bytes.begin()+8,"BKLT0001"),"Unsupported baked lighting");
  const auto lookup=U32(bytes,8,true),count=U32(bytes,12,true);
  Check(lookup>=16 && lookup<=bytes.size() && lookup%4==0 && count<=(bytes.size()-lookup)/8,"Invalid lighting lookup");
  std::map<uint32_t,uint32_t> records;std::set<uint32_t> ids;
  for(uint32_t i=0;i<count;++i) {
    const auto id=U32(bytes,lookup+i*8,true),offset=U32(bytes,lookup+i*8+4,true);
    Check(offset>=16 && offset<lookup && offset%4==0 && ids.insert(id).second && records.emplace(offset,id).second,"Invalid lighting record");
  }
  std::map<std::vector<uint32_t>,View> colors;
  for(auto it=records.begin();it!=records.end();++it) {
    const auto next=std::next(it);const size_t end=next==records.end()?lookup:next->first;
    Cursor c{bytes.first(end),it->first,true};const auto meshes=c.Word();
    Check(meshes<=(end-c.at)/8,"Invalid baked mesh count");
    for(uint32_t j=0;j<meshes;++j) {
      const auto vertices=c.Word();Check(vertices<=(end-c.at)/4,"Truncated baked colors");
      const auto data=c.Take(size_t(vertices)*4);const auto groups=c.Word();
      Check(groups<=(end-c.at)/4,"Invalid baked group count");
      for(uint32_t k=0;k<groups;++k) {
        const auto n=c.Word();Check(n<=256 && n<=(end-c.at)/4,"Invalid baked hierarchy path");
        std::vector<uint32_t> path;for(uint32_t p=0;p<n;++p)path.push_back(c.Word());
        if(it->second==entity)Check(colors.emplace(std::move(path),data).second,"Duplicate baked hierarchy path");
      }
    }
    Check(c.at==end,"Unexpected baked record tail");
  }
  Check(!colors.empty(),"No authored lighting for entity");
  Check(U32(model.footer,0,model.big)==1,"Baked lighting needs a single model root");
  struct Edit {uint32_t mesh,stream;size_t link;Bytes data;};std::vector<Edit> edits;
  std::set<uint32_t> ancestors,meshes_seen;std::set<std::vector<uint32_t>> used;
  std::function<void(uint32_t,std::vector<uint32_t>)> visit;
  visit=[&](uint32_t block,std::vector<uint32_t> path) {
    if(block==UINT32_MAX)return;
    Check(block<model.blocks.size() && ancestors.size()<256 && ancestors.insert(block).second,"Invalid baked model hierarchy");
    const auto& kind=model.types[model.blocks[block].type];Cursor c{model.blocks[block].Data(),0,model.big};
    c.Word();c.Take(size_t(c.Word())*4);c.Word();c.Take(54);c.Take(size_t(c.Word())*4);c.Word();
    // JPSJigsawNode serializes the same NiNode child prefix, followed by its
    // separately converted piece/physics payload. Preserve original child paths.
    if(kind=="NiNode" || kind=="JPSJigsawNode") {
      const auto n=c.Word();Check(n<=(c.data.size()-c.at)/4,"Invalid node child count");
      for(uint32_t i=0;i<n;++i){auto child_path=path;child_path.push_back(i);visit(c.Word(),std::move(child_path));}
    } else {
      Check(kind=="NiMesh","Unaudited baked hierarchy object");
      Check(meshes_seen.insert(block).second,"Shared mesh instances require separate baked model ownership");
      const auto found=colors.find(path);
      // Cooked lighting is sparse on paintable/Jigsaw models. Unlisted meshes
      // keep their authored colors; every listed path must still match below.
      if(found==colors.end()){ancestors.erase(block);return;}
      Check(used.insert(path).second,"Duplicate baked mesh path");
      c.Take(size_t(c.Word())*8);c.Take(5);
      if(model.user_version>=4 && c.Word())c.Take(8);
      if(model.user_version>=15)c.Word();
      if(model.user_version>=3 && c.Word())c.Word();
      c.Word();c.Short();c.Take(17);const auto refs=c.Word();uint32_t matches=0;
      Check(refs<=1000,"Invalid mesh stream count");
      for(uint32_t i=0;i<refs;++i) {
        const auto link=c.at;const auto stream=c.Word();c.Take(1);const auto regions=c.Short();
        std::vector<uint16_t> selected;for(uint32_t r=0;r<regions;++r)selected.push_back(c.Short());
        const auto semantics=c.Word();Check(semantics<=1000,"Invalid semantic count");bool color=false;
        for(uint32_t s=0;s<semantics;++s){auto name=c.Word(),index=c.Word();if(model.String(name)=="COLOR" && index==0)color=true;}
        if(!color)continue;
        Check(++matches==1 && semantics==1 && stream<model.blocks.size() && (selected.empty() || (selected.size()==1 && selected[0]==0)),"Unaudited color binding");
        Check(model.types[model.blocks[stream].type].starts_with("NiDataStream\1"),"Invalid color stream");
        Cursor d{model.blocks[stream].Data(),0,model.big};const auto size=d.Word();d.Word();
        Check(d.Word()==1,"Unaudited color regions");const auto start=d.Word(),vertices=d.Word();
        Check(start==0 && uint64_t(vertices)*4==size && size==found->second.size() && d.Word()==1 && d.Word()==0x00040110,"Baked/source color layout mismatch");
        const auto at=d.at;d.Take(size);d.Take(1);Check(d.at==d.data.size(),"Unexpected color stream tail");
        Bytes replacement(d.data.begin(),d.data.end());std::copy(found->second.begin(),found->second.end(),replacement.begin()+at);
        edits.push_back({block,stream,link,std::move(replacement)});
      }
      Check(matches==1,"Baked mesh has no color stream");
    }
    ancestors.erase(block);
  };
  visit(U32(model.footer,4,model.big),{});
  Check(used.size()==colors.size(),"Unmatched baked mesh paths");
  // Isolate each binding, including source streams shared with other meshes.
  // No mutation occurs until every record/path/binding has been validated.
  for(auto& edit:edits) {
    const auto type=model.types[model.blocks[edit.stream].type];
    const auto index=model.AddBlock(type,std::move(edit.data));
    const auto before=model.blocks[edit.mesh].Data();Bytes mesh(before.begin(),before.end());
    for(unsigned i=0;i<4;++i)mesh[edit.link+i]=uint8_t(index>>(8*(model.big?3-i:i)));
    model.blocks[edit.mesh].replacement=std::move(mesh);
  }
  return uint32_t(edits.size());
}
}
