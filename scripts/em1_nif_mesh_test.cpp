#include "em1_nif_meshes.h"
#include "em1_nif_container.h"
#include "em1_pack_source.h"
#include <fstream>
#include <iostream>
#include <set>
void Check(bool value,const std::string& text){if(!value)throw std::runtime_error(text);}
int wmain(int argc,wchar_t** argv){try{
 using namespace em2::em1;
 Check(argc==4,"Usage: em1_nif_mesh_test original.rvz texture-manifest.tsv new-output-directory");
 const std::filesystem::path directory(argv[3]);Check(!std::filesystem::exists(directory),"Refusing existing output directory");std::filesystem::create_directories(directory);
 std::string error;std::shared_ptr<RvzSource> source=RvzSource::Open(argv[1],error);Check(bool(source),error);
 auto pak=PakSource::Open(source,"packfiles/dbc_1st_zonec.pak",error);Check(bool(pak),error);
 std::ifstream manifest{std::filesystem::path(argv[2])};Check(bool(manifest),"Missing source manifest");
 std::ofstream result_manifest(directory/"manifest.tsv"),rejections(directory/"rejected.tsv");
 unsigned accepted=0,rejected=0,converted=0;std::string line;
 while(std::getline(manifest,line)){
  const auto path=line.substr(0,line.find('\t'));std::vector<uint8_t> bytes;Check(pak->ReadOriginal(path,bytes,error),error);
  NifMeshResult result;result.bytes={0x11};
  if(!AdaptNifMeshes(bytes,result,error)){Check(result.bytes==std::vector<uint8_t>{0x11},"Failure replaced output");rejections<<path<<'\t'<<error<<'\n';++rejected;continue;}
  nif::Nif original(bytes),adapted(result.bytes);Check(adapted.blocks.size()>=original.blocks.size(),"Lost block IDs");
  for(size_t i=0;i<original.string_values.size();++i)Check(original.String(uint32_t(i))==adapted.String(uint32_t(i)),"Original string changed");
  std::set<uint32_t> changed;for(const auto& mesh:result.meshes){changed.insert(mesh.mesh_block);++converted;}
  for(uint32_t i=0;i<original.blocks.size();++i){
   if(original.types[original.blocks[i].type].starts_with("NiDataStream\x01" "4\x01"))changed.insert(i);
   if(!changed.contains(i)){Check(original.types[original.blocks[i].type]==adapted.types[adapted.blocks[i].type],"Opaque type changed");const auto a=original.blocks[i].Data(),b=adapted.blocks[i].Data();Check(std::equal(a.begin(),a.end(),b.begin(),b.end()),"Opaque block changed");}
  }
  for(const auto& block:adapted.blocks)Check(!adapted.types[block.type].starts_with("NiDataStream\x01" "4\x01"),"Wii display-list block remains");
  if(result.meshes.empty())Check(bytes==result.bytes,"Unmodified NIF changed");
  const auto name="nif-"+std::to_string(accepted)+".bin";
  std::ofstream file(directory/name,std::ios::binary);file.write(reinterpret_cast<const char*>(result.bytes.data()),result.bytes.size());Check(bool(file),"Writing developer artifact failed");
  result_manifest<<path<<'\t'<<name<<'\t'<<result.meshes.size()<<'\n';++accepted;
  const auto prior=result.bytes.size();bytes.pop_back();Check(!AdaptNifMeshes(bytes,result,error),"Accepted truncated NIF");Check(result.bytes.size()==prior,"Malformed input replaced output");
 }
 std::cout<<"PASS native original-RVZ NIF mesh conversion: accepted="<<accepted<<" rejected="<<rejected<<" converted_meshes="<<converted<<"; opaque blocks, original IDs/strings and failure preservation checked\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
