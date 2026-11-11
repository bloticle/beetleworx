#pragma once
#include "em1_nif_container.h"
#include <cmath>
#include <bit>
namespace em2::em1 {
struct JigsawBuffer {
  nif::View bytes;
  uint32_t format=0,count=0;
};
// Views borrow the original NIF. Their owner must outlive conversion.
struct JigsawMeshBindings {
  std::vector<JigsawBuffer> vertices,indices;
  std::vector<uint32_t> triangle_vertices,triangle_indices,triangle_pieces,convex_pieces;
};
// Version4 suffix supplies PHYSICS streams, independent of GX render tuples.
// Reserved arrays are bounded but deliberately unsupported when nonempty.
inline JigsawMeshBindings ReadJigsawMeshBindings(const nif::Nif& model,nif::View suffix,uint32_t pieces){
  using namespace nif;Cursor c{suffix,0,model.big};
  auto array=[&](){const auto n=c.Word();Check(n<=1000000 && n<=(c.data.size()-c.at)/4,"Invalid Jigsaw binding array");std::vector<uint32_t> out;for(uint32_t i=0;i<n;++i)out.push_back(c.Word());return out;};
  auto vertex_links=array(),index_links=array();const auto region_count=c.Word();
  Check(region_count==vertex_links.size() && region_count<=(c.data.size()-c.at)/2,"Jigsaw vertex-region binding count mismatch");
  std::vector<uint16_t> regions;for(uint32_t i=0;i<region_count;++i)regions.push_back(c.Short());
  Check(c.Word()==0 && c.Word()==0,"Nonempty reserved Jigsaw binding arrays need an adapter");
  JigsawMeshBindings result;result.triangle_vertices=array();result.triangle_indices=array();result.triangle_pieces=array();result.convex_pieces=array();
  Check(c.at==suffix.size(),"Unexpected Jigsaw binding suffix tail");
  Check(result.triangle_vertices.size()==result.triangle_indices.size() && result.triangle_vertices.size()==result.triangle_pieces.size(),"Jigsaw triangle binding count mismatch");
  auto stream=[&](uint32_t link,uint32_t region,bool vertex){
    Check(link<model.blocks.size() && model.types[model.blocks[link].type].starts_with("NiDataStream\1"),"Jigsaw binding is not a NIF data stream");
    Cursor d{model.blocks[link].Data(),0,model.big};const auto size=d.Word();d.Word();const auto count=d.Word();
    Check(count<=1000000 && count<=(d.data.size()-d.at)/8 && region<count,"Invalid Jigsaw stream regions");
    uint32_t start=0,rows=0;for(uint32_t i=0;i<count;++i){const auto a=d.Word(),b=d.Word();if(i==region){start=a;rows=b;}}
    Check(d.Word()==1,"Interleaved Jigsaw physics streams need an adapter");const auto format=d.Word();
    uint32_t width=0;
    if(vertex){Check(format==0x00030437,"Unsupported Jigsaw position format");width=12;}
    else {Check(count==1 && start==0,"Unsupported Jigsaw index regions");width=format==0x00010105?1:format==0x00010215?2:format==0x00010425?4:0;Check(width!=0,"Unsupported Jigsaw index format");}
    const auto raw=d.Take(size);d.Take(1);Check(d.at==d.data.size(),"Unexpected Jigsaw stream tail");
    Check(uint64_t(start)*width<=raw.size() && uint64_t(rows)*width<=raw.size()-uint64_t(start)*width,"Truncated Jigsaw stream region");
    const auto selected=raw.subspan(size_t(start)*width,size_t(rows)*width);
    if(vertex)for(size_t at=0;at<selected.size();at+=4)Check(std::isfinite(std::bit_cast<float>(U32(selected,at,model.big))),"Nonfinite Jigsaw physics position");
    return JigsawBuffer{selected,format,rows};
  };
  for(size_t i=0;i<vertex_links.size();++i)result.vertices.push_back(stream(vertex_links[i],regions[i],true));
  for(auto link:index_links)result.indices.push_back(stream(link,0,false));
  for(size_t i=0;i<result.triangle_vertices.size();++i){
    Check(result.triangle_vertices[i]<result.vertices.size() && result.triangle_indices[i]<result.indices.size(),"Jigsaw triangle references absent physics stream");
    Check(result.triangle_pieces[i]<pieces,"Jigsaw triangle references absent owner piece");
  }
  for(auto owner:result.convex_pieces)Check(owner<pieces,"Jigsaw convex part references absent owner piece");
  return result;
}
}
