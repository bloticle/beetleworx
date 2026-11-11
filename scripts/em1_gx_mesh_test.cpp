#include "em1_gx_mesh.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
using em2::em1::DecodeGxMesh;
using em2::em1::GxMeshTopology;
void Check(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
struct Reader {
 std::vector<uint8_t> bytes;size_t at=0;
 std::span<const uint8_t> Take(size_t count){Check(at<=bytes.size()&&count<=bytes.size()-at,"Truncated oracle corpus");auto result=std::span(bytes).subspan(at,count);at+=count;return result;}
 uint32_t Word(){auto b=Take(4);return uint32_t(b[0])|uint32_t(b[1])<<8|uint32_t(b[2])<<16|uint32_t(b[3])<<24;}
 std::span<const uint8_t> Blob(){return Take(Word());}
};
void Synthetic(){
 std::string error;GxMeshTopology output;
 const std::array<uint32_t,2> counts{3,2};
 const std::array<uint8_t,9> seams{0x90,0,3,0,0,0,1,1,0};
 Check(DecodeGxMesh(seams,counts,output,error),error);
 Check(output.source_index_bytes==1&&output.attribute_indices==std::vector<uint32_t>({0,0,0,1,1,0})&&output.triangle_indices==std::vector<uint32_t>({0,1,2}),"Color seam collapsed");
 const std::array<uint32_t,1> four{4};
 for(uint8_t primitive:{uint8_t(0x80),uint8_t(0x98),uint8_t(0xa0)}){
  const std::array<uint8_t,7> data{primitive,0,4,0,1,2,3};
  Check(DecodeGxMesh(data,four,output,error),error);
  const std::vector<uint32_t> want=primitive==0x98?std::vector<uint32_t>{0,1,2,2,1,3}:std::vector<uint32_t>{0,1,2,0,2,3};
  Check(output.triangle_indices==want,"Primitive triangulation/winding mismatch");
 }
 const std::array<uint8_t,9> shorts{0x90,0,3,0,0,0,1,0,2};
 Check(DecodeGxMesh(shorts,four,output,error)&&output.source_index_bytes==2,"16-bit draw failed");
 const auto unchanged=output.triangle_indices;
 const std::vector<std::vector<uint8_t>> invalid={{},{0x90,0},{0x90,0,3,0,1},{0x90,0,3,0,1,4},{0x80,0,3,0,1,2},{0xb0,0,3,0,1,2},{0x98,0,3,0,0,0,0,0,0}};
 for(const auto& data:invalid){Check(!DecodeGxMesh(data,four,output,error),"Accepted malformed/ambiguous draw");Check(output.triangle_indices==unchanged,"Failure replaced prior output");}
}
int main(int argc,char** argv){try{
 Synthetic();Check(argc==2,"Usage: em1_gx_mesh_test independent-reference-goldens.bin");
 std::ifstream file(argv[1],std::ios::binary);Check(bool(file),"Missing oracle corpus");
 Reader reader{{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()}};
 const auto magic=reader.Take(4);Check(std::equal(magic.begin(),magic.end(),"E1GX"),"Bad oracle signature");Check(reader.Word()==1,"Bad oracle version");
 const auto cases=reader.Word();uint64_t vertices=0,triangles=0;unsigned secondary_uvs=0;
 for(unsigned c=0;c<cases;++c){
  const auto raw_name=reader.Blob();const std::string name(raw_name.begin(),raw_name.end());
  const auto commands=reader.Blob();const auto attributes=reader.Word();
  struct Attribute{uint32_t kind,index;std::vector<std::array<uint32_t,4>> values;};
  std::vector<Attribute> source;std::vector<uint32_t> counts;
  for(unsigned a=0;a<attributes;++a){Attribute attr{reader.Word(),reader.Word(),{}};const auto count=reader.Word();counts.push_back(count);attr.values.resize(count);for(auto& v:attr.values)for(auto& x:v)x=reader.Word();source.push_back(std::move(attr));}
  const auto vertex_count=reader.Word(),index_count=reader.Word();std::vector<uint32_t> indices(index_count);for(auto& i:indices)i=reader.Word();
  GxMeshTopology result;std::string error;Check(DecodeGxMesh(commands,counts,result,error),name+": "+error);
  Check(result.attributes==attributes&&result.attribute_indices.size()==size_t(vertex_count)*attributes,name+": vertex tuple count");Check(result.triangle_indices==indices,name+": topology mismatch");
  for(unsigned v=0;v<vertex_count;++v){
   const auto position=reader.Word(),color=reader.Word();Check(result.attribute_indices[v*attributes]==position,name+": original position index lost");
   for(unsigned a=0;a<attributes;++a){std::array<uint32_t,4> want;for(auto& x:want)x=reader.Word();const auto index=result.attribute_indices[v*attributes+a];Check(index<source[a].values.size()&&source[a].values[index]==want,name+": attribute mismatch");if(source[a].kind==2&&source[a].index==0)Check(index==color,name+": original color index lost");}
  }
  secondary_uvs+=std::any_of(source.begin(),source.end(),[](const auto& a){return a.kind==3&&a.index>0;});vertices+=vertex_count;triangles+=index_count/3;
 }
 Check(reader.at==reader.bytes.size(),"Trailing oracle corpus data");
 std::cout<<"PASS GX native/reference cases="<<cases<<" vertices="<<vertices<<" triangles="<<triangles<<" meshes_with_secondary_uvs="<<secondary_uvs<<"; synthetic seams/winding/index-width/malformed checks passed\n";return 0;
 }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}}
