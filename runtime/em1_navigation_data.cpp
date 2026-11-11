#include "em1_navigation_data.h"
#include "em1_rvz.h"
#include "em1_pack_source.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
namespace em2::em1 {
namespace {
void Check(bool value,const char* error){if(!value)throw std::runtime_error(error);}
uint32_t Word(std::span<const uint8_t> b,size_t at){Check(at<=b.size() && b.size()-at>=4,"Truncated navigation word");return uint32_t(b[at])|uint32_t(b[at+1])<<8|uint32_t(b[at+2])<<16|uint32_t(b[at+3])<<24;}
uint16_t Half(std::span<const uint8_t> b,size_t at){Check(at<=b.size() && b.size()-at>=2,"Truncated navigation index");return uint16_t(b[at])|uint16_t(b[at+1])<<8;}
float Number(std::span<const uint8_t> b,size_t at){const auto v=std::bit_cast<float>(Word(b,at));Check(std::isfinite(v),"Nonfinite navigation coordinate");return v;}
std::string Key(std::string_view value){std::string result(value);for(auto& c:result){if(c=='\\')c='/';if(c>='A' && c<='Z')c+=32;}return result;}
}
bool ReadNavigationHpd(std::span<const uint8_t> b,std::vector<NavigationSectorDeclaration>& output,std::string& error){
  try {
    Check(b.size()>=156 && b.size()<=16*1024*1024 && !std::memcmp(b.data(),"Kynogon HPD\0",12),"Invalid original HPD header");
    Check(Word(b,12)==2 && Word(b,16)==2,"Unsupported original HPD version");const auto count=Word(b,20);
    Check(count<=(b.size()-156)/48,"Truncated HPD sector declarations");
    std::vector<NavigationSectorDeclaration> result;result.reserve(count);std::set<uint32_t> ids;
    for(uint32_t i=0;i<count;++i){const size_t at=156+size_t(i)*48;NavigationSectorDeclaration row;row.id=Word(b,at);
      Check(ids.insert(row.id).second,"Duplicate HPD sector identifier");
      for(unsigned axis=0;axis<6;++axis)row.bounds[axis]=Number(b,at+4+axis*4);
      for(unsigned axis=0;axis<3;++axis)Check(row.bounds[axis*2]<=row.bounds[axis*2+1],"Reversed HPD bounds");
      row.unknown=Word(b,at+28);row.payload_size=Word(b,at+32);row.payload_offset=Word(b,at+36);
      row.cook={Word(b,at+40),Word(b,at+44)};result.push_back(row);
    }
    output=std::move(result);error.clear();return true;
  }catch(const std::exception& e){error=e.what();return false;}
}
bool ReadNavigationSector(std::span<const uint8_t> b,NavigationSector& output,std::string& error){
  try {
    Check(b.size()>=24 && b.size()<=128*1024*1024 && Word(b,0)==2,"Invalid prelinked navigation sector wrapper");
    NavigationSector result;result.id=Word(b,4);result.cook={Word(b,8),Word(b,12)};
    const auto nodes=Word(b,16),edges=Word(b,20);
    Check(nodes<=65536 && uint64_t(nodes)*36+uint64_t(edges)*12<=b.size()-24,"Truncated prelinked navigation graph");
    result.nodes.reserve(nodes);std::set<uint32_t> ids;
    for(uint32_t i=0;i<nodes;++i){const size_t at=24+size_t(i)*36;NavigationNode node;node.id=Word(b,at);
      Check(ids.insert(node.id).second,"Duplicate prelinked graph node identifier");
      for(unsigned axis=0;axis<3;++axis)node.position[axis]=Number(b,at+4+axis*4);
      std::copy_n(b.begin()+at+16,20,node.metadata.begin());node.first_edge=Word(b,at+24);result.nodes.push_back(node);
    }
    const size_t edge_start=24+size_t(nodes)*36;result.edges.reserve(edges);
    std::vector<uint32_t> starts(nodes),counts(nodes);
    for(uint32_t i=0;i<edges;++i){const auto at=edge_start+size_t(i)*12;NavigationEdge edge{Word(b,at),Word(b,at+4),Word(b,at+8)};
      Check(edge.source<nodes && edge.target<nodes,"Prelinked navigation edge target outside graph");
      if(!counts[edge.source])starts[edge.source]=i+1;
      else Check(starts[edge.source]+counts[edge.source]==i+1,"Noncontiguous navigation adjacency");
      ++counts[edge.source];result.edges.push_back(edge);
    }
    for(uint32_t i=0;i<nodes;++i)Check(result.nodes[i].first_edge==starts[i],"Navigation adjacency start does not match source edges");
    for(unsigned axis=0;axis<3;++axis){std::vector<bool> seen(nodes);float previous=0;bool first=true;
      for(uint32_t rank=0;rank<nodes;++rank){const auto index=Half(b,24+size_t(rank)*36+28+axis*2);
        Check(index<nodes && !seen[index],"Invalid navigation spatial permutation");seen[index]=true;
        const auto coordinate=result.nodes[index].position[axis];Check(first || previous<=coordinate,"Unsorted navigation spatial index");previous=coordinate;first=false;
      }
    }
    result.mesh_offset=edge_start+size_t(edges)*12;
    Check(b.size()-result.mesh_offset>=88 && !std::memcmp(b.data()+result.mesh_offset,"KynogonRuntimeMesh",18),"Original navigation runtime mesh missing");
    Check(Word(b,result.mesh_offset+20)==2 && Word(b,result.mesh_offset+28)==result.id,"Runtime mesh version/sector mismatch");
    Check(Word(b,b.size()-8)==b.size()-result.mesh_offset-8,"Runtime navigation mesh length mismatch");
    if(!ReadNavigationRuntimeMesh(b.subspan(result.mesh_offset),result.runtime_mesh,error))return false;
    result.bytes.assign(b.begin(),b.end());output=std::move(result);error.clear();return true;
  }catch(const std::exception& e){error=e.what();return false;}
}
bool LoadNavigationDatabase(std::shared_ptr<RvzSource> source,std::string_view archive,std::string_view path,NavigationDatabase& output,std::string& error){
  try {
    Check(bool(source),"Original navigation source unavailable");
    auto packed=PakSource::Open(source,archive,error);if(!packed)return false;
    NavigationDatabase result;result.source_path=std::string(path);result.source_archive=std::string(archive);result.source_identity=packed->OriginKey();
    if(!packed->ReadOriginal(path,result.bytes,error) || !ReadNavigationHpd(result.bytes,result.declarations,error))return false;
    const auto normalized=Key(path);const auto slash=normalized.rfind('/');Check(slash!=normalized.npos,"HPD has no navigation folder");
    const auto before=normalized.rfind('/',slash-1);const auto name=normalized.substr(before==normalized.npos?0:before+1,slash-(before==normalized.npos?0:before+1));
    std::map<std::string,const RvzFile*> files;for(const auto& file:source->Files())Check(files.emplace(Key(file.path),&file).second,"Ambiguous source path casing");
    for(const auto& declaration:result.declarations){const auto companion="sectors/"+name+"/sector_"+std::to_string(declaration.id)+".prelnk.obj";
      const auto found=files.find(companion);Check(found!=files.end(),"Original HPD companion sector is missing");
      std::vector<uint8_t> bytes;if(!source->ReadFile(found->second->path,bytes,error))return false;
      NavigationSector sector;if(!ReadNavigationSector(bytes,sector,error)){error=found->second->path+": "+error;return false;}
      Check(sector.id==declaration.id && sector.cook==declaration.cook && bytes.size()>=16 && bytes.size()-16==declaration.payload_size,"HPD companion identity/cook/size mismatch");
      sector.source_path=found->second->path;result.sectors.push_back(std::move(sector));
    }
    std::map<uint32_t,uint32_t> meshes;
    for(uint32_t i=0;i<result.sectors.size();++i)Check(meshes.emplace(result.sectors[i].id,i).second,"Repeated navigation sector identity");
    for(auto& sector:result.sectors)for(const auto& neighbor:sector.runtime_mesh.neighbors){
      const auto target=meshes.find(neighbor.sector);Check(target!=meshes.end(),"Navigation neighbor target sector absent from HPD");
      const auto& floors=result.sectors[target->second].runtime_mesh.floors;
      const auto floor=std::find_if(floors.begin(),floors.end(),[&](const auto& floor){return floor.x==neighbor.x && floor.z==neighbor.z && floor.layer==neighbor.layer;});
      Check(floor!=floors.end(),"Navigation neighbor target floor absent");
      for(const auto offset:neighbor.local_links){bool found=false;
        for(auto& local:sector.runtime_mesh.floors)for(auto& link:local.unbound_links)if(link.source_offset==offset){
          Check(link.target_sector_index==UINT32_MAX,"Navigation link target already bound");
          link.target_sector_index=target->second;link.target_floor_index=uint32_t(floor-floors.begin());found=true;
        }
        Check(found,"Navigation neighbor source link absent");
      }
    }
    output=std::move(result);error.clear();return true;
  }catch(const std::exception& e){error=std::string(path)+": "+e.what();return false;}
}
}
