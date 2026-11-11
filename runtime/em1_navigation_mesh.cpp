#include "em1_navigation_mesh.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
namespace em2::em1 {
namespace {
void Check(bool condition,const char* reason){if(!condition)throw std::runtime_error(reason);}
struct Reader {
  std::span<const uint8_t> b;
  void Range(size_t at,size_t size,size_t begin,size_t end)const{Check(begin<=end && end<=b.size() && at>=begin && at<=end && size<=end-at,"Navigation mesh reference outside its owner");}
  uint32_t Word(size_t at)const{Range(at,4,0,b.size());return uint32_t(b[at])|(uint32_t(b[at+1])<<8)|(uint32_t(b[at+2])<<16)|(uint32_t(b[at+3])<<24);}
  float Number(size_t at)const{const auto f=std::bit_cast<float>(Word(at));Check(std::isfinite(f),"Nonfinite runtime navigation coordinate");return f;}
  NavigationPoint Point(size_t at)const{return {Number(at),Number(at+4),Number(at+8)};}
  NavigationSegment Segment(size_t at)const{return {Point(at),Point(at+12)};}
};
}
bool ReadNavigationRuntimeMesh(std::span<const uint8_t> bytes,NavigationRuntimeMesh& output,std::string& error){
  try {
    Reader r{bytes};Check(bytes.size()>=88 && bytes.size()<=128*1024*1024 && !std::memcmp(bytes.data(),"KynogonRuntimeMesh",18),"Invalid runtime navigation mesh header");
    Check(r.Word(20)==2 && r.Word(bytes.size()-8)==bytes.size()-8,"Runtime navigation mesh version/length mismatch");
    const size_t payload_end=bytes.size()-8;
    NavigationRuntimeMesh result;result.sector=r.Word(28);result.width=r.Word(56);result.height=r.Word(60);
    result.inverse_cell_size=r.Number(32);result.border_tolerance=r.Number(36);
    for(unsigned i=0;i<4;++i)result.bounds[i]=r.Number(40+i*4);
    Check(result.inverse_cell_size>0 && result.border_tolerance>=0 && result.bounds[0]<=result.bounds[1] && result.bounds[2]<=result.bounds[3],"Invalid navigation mesh grid transform");
    Check(result.width && result.height && uint64_t(result.width)*result.height<=1000000,"Invalid runtime navigation cell grid");
    const size_t cells=size_t(result.width)*result.height,table=r.Word(68);
    result.cell_floors.resize(cells);
    r.Range(table,(cells+1)*4,80,payload_end);
    std::map<uint32_t,uint32_t> floor_indices;
    for(size_t cell=0;cell<cells;++cell){
      const size_t begin=r.Word(table+cell*4),end=r.Word(table+(cell+1)*4);
      r.Range(begin,4,table+(cells+1)*4,payload_end);Check(begin<end && end<=payload_end,"Invalid navigation cell range");
      const auto count=r.Word(begin);
      if(!count){Check(end-begin==4,"Unexpected empty navigation cell payload");continue;}
      r.Range(begin+4,size_t(count)*12+4,begin,end);
      for(uint32_t layer=0;layer<count;++layer){
        const size_t row=begin+4+size_t(layer)*12,at=r.Word(row),last=r.Word(row+12);
        r.Range(at,80,begin+8+size_t(count)*12,end);Check(at<last && last<=end,"Invalid navigation floor range");
        NavigationFloor floor;floor.source_offset=uint32_t(at);floor.source_size=uint32_t(last-at);
        floor.x=r.Word(at+4);floor.z=r.Word(at+8);floor.layer=r.Word(at+12);
        Check(r.Word(at)==result.sector && floor.x==cell/result.height && floor.z==cell%result.height && floor.layer==layer,"Navigation floor cell/sector identity mismatch");
        floor.minimum_height=r.Number(row+4);floor.maximum_height=r.Number(row+8);
        Check(floor.minimum_height<=floor.maximum_height && floor.minimum_height==r.Number(at+28) && floor.maximum_height==r.Number(at+24),"Navigation floor height bounds mismatch");
        Check(r.Number(at+16)>0 && r.Number(at+20)>0,"Invalid navigation floor resolution");
        floor.height_tolerance=r.Number(at+20);
        Check(r.Word(at+32)==0 && r.Word(at+36)==UINT32_MAX,"Unsupported navigation floor metadata array");
        std::copy_n(bytes.begin()+at,80,floor.header.begin());
        Check(floor_indices.emplace(floor.source_offset,uint32_t(result.floors.size())).second,"Duplicate navigation floor offset");
        result.cell_floors[cell].push_back(uint32_t(result.floors.size()));
        result.floors.push_back(std::move(floor));
      }
    }
    std::map<uint32_t,std::pair<uint32_t,uint32_t>> local_links;
    for(uint32_t index=0;index<result.floors.size();++index){auto& floor=result.floors[index];const size_t at=floor.source_offset,end=at+floor.source_size;
      auto fixed=[&](unsigned field,size_t stride,auto read){const auto count=r.Word(at+field),offset=r.Word(at+field+4);
        if(!count){Check(offset==UINT32_MAX,"Unexpected empty navigation array offset");return;}
        r.Range(at+offset,size_t(count)*stride,at+80,end);for(uint32_t i=0;i<count;++i)read(at+offset+size_t(i)*stride);
      };
      fixed(40,32,[&](size_t pos){const auto target=floor_indices.find(r.Word(pos+24));Check(target!=floor_indices.end(),"Navigation portal target floor absent");
        Check(result.floors[target->second].source_size==r.Word(pos+28),"Navigation portal target size mismatch");
        floor.portals.push_back({r.Segment(pos),target->second});
      });
      fixed(48,44,[&](size_t pos){NavigationUnboundLink link;link.source_offset=uint32_t(pos);link.segment=r.Segment(pos);
        link.link_words={r.Word(pos+24),r.Word(pos+28)};link.direction=r.Point(pos+32);
        Check(local_links.emplace(link.source_offset,std::pair{index,uint32_t(floor.unbound_links.size())}).second,"Duplicate navigation local link offset");floor.unbound_links.push_back(link);
      });
      auto variable=[&](unsigned field,size_t stride,auto read){const auto count=r.Word(at+field),offset=r.Word(at+field+4);
        if(!count){Check(offset==UINT32_MAX,"Unexpected empty navigation record table offset");return;}
        r.Range(at+offset,(size_t(count)+1)*stride,at+80,end);
        for(uint32_t i=0;i<count;++i){const size_t row=at+offset+size_t(i)*stride,begin=at+r.Word(row),last=at+r.Word(row+stride);
          Check(begin<=last,"Reversed navigation record range");r.Range(begin,last-begin,at+offset+(size_t(count)+1)*stride,end);read(row,begin,last);
        }
      };
      variable(56,4,[&](size_t,size_t begin,size_t last){Check((last-begin)%12==0 && last-begin>=24,"Invalid navigation polyline size");std::vector<NavigationPoint> line;
        for(auto pos=begin;pos<last;pos+=12)line.push_back(r.Point(pos));floor.polylines.push_back(std::move(line));
      });
      variable(64,4,[&](size_t,size_t begin,size_t last){Check(last-begin==24,"Invalid navigation boundary segment size");floor.segments.push_back(r.Segment(begin));});
      variable(72,16,[&](size_t row,size_t begin,size_t last){Check((last-begin)%12==0 && last-begin>=48,"Invalid navigation patch size");NavigationPatch patch;patch.direction=r.Point(row+4);
        for(auto pos=begin;pos<last;pos+=12)patch.points.push_back(r.Point(pos));Check(patch.points.front()==patch.points.back(),"Navigation patch is not closed");floor.patches.push_back(std::move(patch));
      });
    }
    const auto neighbors=r.Word(72),neighbor_table=r.Word(76);std::set<uint32_t> assigned;
    if(neighbors){r.Range(neighbor_table,size_t(neighbors)*12,80,payload_end);
      for(uint32_t i=0;i<neighbors;++i){const size_t row=neighbor_table+size_t(i)*12;const auto sector=r.Word(row),groups=r.Word(row+4),group_table=r.Word(row+8);
        r.Range(group_table,size_t(groups)*4,80,payload_end);
        for(uint32_t j=0;j<groups;++j){const size_t pos=r.Word(group_table+size_t(j)*4);r.Range(pos,16,80,payload_end);NavigationFloorNeighbor neighbor;
          neighbor.sector=sector;neighbor.x=r.Word(pos);neighbor.z=r.Word(pos+4);neighbor.layer=r.Word(pos+8);const auto count=r.Word(pos+12);
          r.Range(pos+16,size_t(count)*4,80,payload_end);
          for(uint32_t k=0;k<count;++k){const auto link=r.Word(pos+16+size_t(k)*4);Check(local_links.contains(link),"Navigation neighbor references a nonlocal link");Check(assigned.insert(link).second,"Navigation local link assigned more than once");neighbor.local_links.push_back(link);}
          result.neighbors.push_back(std::move(neighbor));
        }
      }
    }
    output=std::move(result);error.clear();return true;
  }catch(const std::exception& e){error=e.what();return false;}
}
}
