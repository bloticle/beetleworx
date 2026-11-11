#pragma once
#include "em1_nif_container.h"
#include <map>
namespace em2::em1 {
struct NifRootPlan {
  std::vector<uint32_t> roots;
  // Extra footer entries must be aliases inside the first original graph.
  // Independent roots require a different native ownership route; no wrapper,
  // dropped root, or rewritten footer is introduced here.
  std::vector<std::vector<uint32_t>> child_paths;
};
inline NifRootPlan PlanContainedNifRoots(const nif::Nif& model){
  using namespace nif;NifRootPlan result;Cursor f{model.footer,0,model.big};const auto count=f.Word();
  Check(count && count<=16,"NIF root count exceeds native fixture bounds");
  for(uint32_t i=0;i<count;++i){const auto root=f.Word();Check(root<model.blocks.size(),"NIF footer references absent root");result.roots.push_back(root);}
  Check(f.at==model.footer.size(),"Unparsed NIF root footer");
  result.child_paths.resize(count);if(count==1)return result;
  std::map<uint32_t,std::vector<uint32_t>> paths;paths.emplace(result.roots[0],std::vector<uint32_t>{});
  std::vector<uint32_t> pending{result.roots[0]};
  for(size_t next=0;next<pending.size();++next){
    Check(pending.size()<=4096,"Contained-root graph exceeds node budget");const auto node=pending[next];
    if(model.types[model.blocks[node].type]!="NiNode")continue;
    Cursor c{model.blocks[node].Data(),0,model.big};c.Word();const auto extras=c.Word();Check(extras<=model.blocks.size(),"Node extra-data count exceeds bounds");
    c.Take(size_t(extras)*4);c.Word();c.Take(54);const auto properties=c.Word();Check(properties<=model.blocks.size(),"Node property count exceeds bounds");
    c.Take(size_t(properties)*4);c.Word();const auto children=c.Word();Check(children<=4096,"Contained-root child count exceeds bounds");
    for(uint32_t index=0;index<children;++index){const auto child=c.Word();if(child==UINT32_MAX)continue;
      Check(child<model.blocks.size(),"Contained-root graph references absent child");
      auto path=paths.at(node);Check(path.size()<128,"Contained-root graph exceeds depth budget");path.push_back(index);
      Check(paths.emplace(child,std::move(path)).second,"Contained-root graph has repeated/cyclic child ownership");pending.push_back(child);
    }
  }
  for(size_t i=1;i<result.roots.size();++i){const auto path=paths.find(result.roots[i]);
    Check(path!=paths.end(),"NIF has independent roots requiring a separate ownership adapter");result.child_paths[i]=path->second;}
  return result;
}
}
