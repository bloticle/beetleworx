// ZArchive v1 layout reference: Exzap/ZArchive at
// 965b66c8d67b6b7e30fd63b3b75aa91a99ff303b (MIT-0).
#include "zar_image.h"
#include "archive_io.h"
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include "em1_compat/zstd/lib/zstd.h"
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <stdexcept>
namespace em2 {
namespace {
constexpr uint64_t block_size=65536,limit=16ull*1024*1024*1024;
void Require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
struct Canceled{};
uint32_t U32(const uint8_t* p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
uint64_t U64(const uint8_t* p){return uint64_t(U32(p))<<32|U32(p+4);}
using archive_detail::File;
using archive_detail::Hash;
std::string Lower(std::string s){for(auto& c:s)if(c>='A' && c<='Z')c+=32;return s;}
}
struct ZarImage::Impl {
 File file;uint64_t size=0,data_start=0,data_size=0,cached=UINT64_MAX;
 std::array<uint8_t,32> identity{};std::vector<uint8_t> records,cache;
 std::vector<ZarFile> files;
 explicit Impl(const std::filesystem::path& path,const std::function<bool(uint64_t,uint64_t)>& progress):file(path){
  Require(file.size>=144 && file.size<=limit,"Invalid ZArchive size");std::array<uint8_t,144> footer{};file.Read(file.size-144,footer);
  Require(U32(footer.data()+140)==0x169f52d6 && U32(footer.data()+136)==0x61bf3a01 && U64(footer.data()+128)==file.size,"Invalid ZArchive footer or version");
  std::array<std::pair<uint64_t,uint64_t>,6> sections{};std::map<uint64_t,uint64_t> occupied;
  for(size_t i=0;i<sections.size();++i){const auto at=U64(footer.data()+i*16),length=U64(footer.data()+i*16+8);Require(at<=file.size-144 && length<=file.size-144-at,"ZArchive section exceeds its container");sections[i]={at,length};
   if(length){const auto next=occupied.lower_bound(at);Require((next==occupied.end() || at+length<=next->first) && (next==occupied.begin() || std::prev(next)->second<=at),"Overlapping ZArchive sections");occupied.emplace(at,at+length);}}
  Require(sections[1].second>0 && sections[1].second%40==0 && sections[1].second<=64*1024*1024 && sections[2].second<=32*1024*1024 && sections[3].second>=16 && sections[3].second%16==0 && sections[3].second/16<=100000,"ZArchive metadata exceeds its budget");
  std::copy_n(footer.begin()+96,32,identity.begin());Hash hash;std::vector<uint8_t> buffer(1024*1024);
  for(uint64_t at=0;at<file.size-144;){if(progress && !progress(at,file.size))throw Canceled{};const auto n=size_t(std::min<uint64_t>(buffer.size(),file.size-144-at));file.Read(at,std::span(buffer).first(n));hash.Add(std::span(buffer).first(n));at+=n;}
  auto zeroed=footer;std::fill_n(zeroed.begin()+96,32,0);hash.Add(zeroed);Require(hash.Finish()==identity,"ZArchive integrity checksum mismatch");if(progress && !progress(file.size,file.size))throw Canceled{};
  const auto read_section=[&](size_t i){std::vector<uint8_t> bytes(size_t(sections[i].second));file.Read(sections[i].first,bytes);return bytes;};
  records=read_section(1);const auto names=read_section(2),tree=read_section(3);const auto count=tree.size()/16;
  struct Node {size_t index;std::string path;unsigned depth;};std::vector<Node> pending{{0,"",0}};std::vector<bool> visited(count,false);visited[0]=true;std::set<std::string> paths;uint64_t max_end=0,path_bytes=0;
  while(!pending.empty()){auto node=std::move(pending.back());pending.pop_back();Require(node.index<count && node.depth<=64,"ZArchive directory tree is too deep");
   const auto* entry=tree.data()+node.index*16;const auto type=U32(entry),name_at=type&0x7fffffff;const bool directory=!(type&0x80000000);std::string name;
   if(name_at!=0x7fffffff || node.index!=0){Require(name_at<names.size(),"Invalid ZArchive name offset");size_t at=name_at;size_t length=names[at]&127;const bool extended=(names[at++]&128)!=0;
    if(extended){Require(at<names.size(),"Truncated ZArchive name length");length|=size_t(names[at++])<<7;}Require(length<=names.size()-at,"Truncated ZArchive name");name.assign(reinterpret_cast<const char*>(names.data()+at),length);}
   if(node.index==0)Require(directory && name.empty(),"Invalid ZArchive root directory");
   else{Require(!name.empty() && name!="." && name!=".." && name.find_first_of("/\\:\0",0,4)==std::string::npos && name.size()<=255,"Unsafe ZArchive filename");node.path+=name;path_bytes+=node.path.size();Require(node.path.size()<=2048 && path_bytes<=64*1024*1024 && paths.insert(Lower(node.path)).second,"Duplicate or oversized ZArchive path");}
   if(directory){const auto start=U32(entry+4),children=U32(entry+8);Require(start<=count && children<=count-start,"ZArchive directory range exceeds the node table");
    if(node.index)files.push_back({node.path,0,0,true});for(size_t i=0;i<children;++i){Require(!visited[start+i],"Cyclic or multiply-parented ZArchive directory tree");visited[start+i]=true;pending.push_back({start+i,node.path.empty()?"":node.path+"/",node.depth+1});}}
   else{const uint64_t offset=U32(entry+4)|(uint64_t(U32(entry+12)&65535)<<32),length=U32(entry+8)|(uint64_t(U32(entry+12)>>16)<<32);
    Require(offset<=limit && length<=limit-offset,"ZArchive file exceeds its logical limit");files.push_back({node.path,offset,length,false});max_end=std::max(max_end,offset+length);}
  }
  Require(std::all_of(visited.begin(),visited.end(),[](bool b){return b;}),"ZArchive has unreachable directory nodes");
  size=(max_end+block_size-1)/block_size*block_size;Require(size>0 && size<=limit && size/block_size<=(records.size()/40)*16,"ZArchive file extent exceeds its block map");
  data_start=sections[0].first;data_size=sections[0].second;
  for(uint64_t i=0;i<size/block_size;++i){const auto [at,length]=Block(i);Require(at<=data_size && length<=data_size-at,"ZArchive compressed block exceeds its data section");}
  cache.resize(block_size);
 }
 std::pair<uint64_t,uint32_t> Block(uint64_t index)const{const auto* record=records.data()+(index/16)*40;uint64_t offset=U64(record);for(size_t i=0;i<index%16;++i){const auto length=(uint32_t(record[8+i*2])<<8|record[9+i*2])+1;Require(offset<=UINT64_MAX-length,"ZArchive block offset overflow");offset+=length;}
  const auto i=size_t(index%16);return {offset,(uint32_t(record[8+i*2])<<8|record[9+i*2])+1};}
 void Read(uint64_t offset,std::span<uint8_t> out){Require(offset<=size && out.size()<=size-offset,"Read outside ZArchive logical stream");size_t done=0;
  while(done<out.size()){const auto block=offset/block_size;const auto within=size_t(offset%block_size);if(cached!=block){cached=UINT64_MAX;const auto [at,length]=Block(block);
    if(length==block_size)file.Read(data_start+at,cache);else{std::vector<uint8_t> packed(length);file.Read(data_start+at,packed);const auto written=ZSTD_decompress(cache.data(),cache.size(),packed.data(),packed.size());Require(written==block_size,"Invalid ZArchive Zstandard block");}cached=block;}
   const auto n=std::min(out.size()-done,cache.size()-within);std::copy_n(cache.begin()+within,n,out.begin()+done);done+=n;offset+=n;
  }
 }
};
ZarImage::ZarImage(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
ZarImage::~ZarImage()=default;
std::unique_ptr<ZarImage> ZarImage::Open(const std::filesystem::path& path,std::string& error,const std::function<bool(uint64_t,uint64_t)>& progress){try{auto out=std::unique_ptr<ZarImage>(new ZarImage(std::make_unique<Impl>(path,progress)));error.clear();return out;}catch(const Canceled&){error.clear();return {};}catch(const std::exception& e){error=e.what();return {};}}
uint64_t ZarImage::Size()const{return impl_->size;}
std::span<const uint8_t> ZarImage::Identity()const{return impl_->identity;}
const std::vector<ZarFile>& ZarImage::Files()const{return impl_->files;}
bool ZarImage::Read(uint64_t at,std::span<uint8_t> out,std::string& error){try{impl_->Read(at,out);error.clear();return true;}catch(const std::exception& e){error=e.what();return false;}}
}
