#include "em1_nif_textures.h"
#include "em1_pack_source.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void Check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}
std::string Hash(std::span<const uint8_t> bytes) {
  BCRYPT_ALG_HANDLE handle{};
  Check(BCryptOpenAlgorithmProvider(&handle,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"Hash provider");
  std::array<uint8_t,32> digest;
  const auto result=BCryptHash(handle,nullptr,0,const_cast<PUCHAR>(bytes.data()),ULONG(bytes.size()),digest.data(),ULONG(digest.size()));
  BCryptCloseAlgorithmProvider(handle,0);Check(result>=0,"Hash failed");
  const char* hex="0123456789abcdef";std::string text;
  for(auto value:digest){text+=hex[value>>4];text+=hex[value&15];}return text;
}
}
int wmain(int argc,wchar_t** argv) {
  try {
    using namespace em2::em1;
    Check(argc==3,"Usage: em1_nif_texture_test original.rvz independent-texture-goldens.tsv");
    std::string error;
    std::shared_ptr<RvzSource> source=RvzSource::Open(argv[1],error);Check(bool(source),error);
    auto pak=PakSource::Open(source,"packfiles/dbc_1st_zonec.pak",error);Check(bool(pak),error);
    std::ifstream golden{std::filesystem::path(argv[2])};Check(golden.good(),"Missing oracle manifest");
    std::string line;unsigned files=0,textures=0,display_lists=0;
    while(std::getline(golden,line)) {
      std::istringstream row(line);std::string path,expected,count;
      std::getline(row,path,'\t');std::getline(row,expected,'\t');std::getline(row,count,'\t');
      std::vector<uint8_t> original;Check(pak->ReadOriginal(path,original,error),error);
      NifTextureResult adapted;Check(AdaptNifTextures(original,adapted,error),path+": "+error);
      Check(Hash(adapted.bytes)==expected && adapted.textures==std::stoul(count),"Texture/container oracle mismatch: "+path);
      ++files;textures+=adapted.textures;display_lists+=adapted.remaining_wii_display_lists;
      const auto preserved_size=adapted.bytes.size();const auto preserved_count=adapted.textures;
      original.pop_back();Check(!AdaptNifTextures(original,adapted,error),"Accepted truncated NIF footer");
      Check(adapted.bytes.size()==preserved_size && adapted.textures==preserved_count,"Failure replaced NIF output");
    }
    Check(files>0 && textures>0,"Empty native texture validation corpus");
    std::cout<<"PASS: "<<files<<" NIF/KF containers, "<<textures<<" Wii textures match independent oracle; "
             <<display_lists<<" display-list blocks explicitly remain unconverted\n";
    return 0;
  }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
