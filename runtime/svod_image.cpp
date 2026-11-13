// SVOD layout references: Xenia's stfs_xbox.h and stfs_container_device.cc.
// Xenia copyright 2020-2021 Ben Vanik; BSD notice: docs/licenses/xenia-bsd.txt.
#include "svod_image.h"
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <stdexcept>
#include <vector>
namespace em2 {
namespace {
constexpr uint64_t sector=2048,sectors_per_table=0x198,sectors_per_file=0x14388,max_file=0xa290000;
void Require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
uint32_t BE32(const uint8_t* p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
uint32_t LE24(const uint8_t* p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16;}
struct File {
 HANDLE handle=INVALID_HANDLE_VALUE;uint64_t size=0;
 explicit File(const std::filesystem::path& path){handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  LARGE_INTEGER length{};if(handle==INVALID_HANDLE_VALUE || !GetFileSizeEx(handle,&length)){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);throw std::runtime_error("Cannot open a required GOD package/data fragment; keep the package and its .data folder together");}size=uint64_t(length.QuadPart);}
 ~File(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
 void Read(uint64_t offset,std::span<uint8_t> bytes){Require(offset<=size && bytes.size()<=size-offset && bytes.size()<=MAXDWORD,"GOD data fragment is truncated");
  LARGE_INTEGER at{};at.QuadPart=LONGLONG(offset);DWORD got=0;Require(SetFilePointerEx(handle,at,nullptr,FILE_BEGIN) && ReadFile(handle,bytes.data(),DWORD(bytes.size()),&got,nullptr) && got==bytes.size(),"Cannot read GOD data fragment");}
};
struct Sha1 {
 BCRYPT_ALG_HANDLE provider=nullptr;
 Sha1(){Require(BCryptOpenAlgorithmProvider(&provider,BCRYPT_SHA1_ALGORITHM,nullptr,0)>=0,"Cannot initialize GOD SHA-1 verification");}
 ~Sha1(){if(provider)BCryptCloseAlgorithmProvider(provider,0);}
 std::array<uint8_t,20> Hash(std::span<const uint8_t> data){std::array<uint8_t,20> hash{};Require(data.size()<=ULONG_MAX && BCryptHash(provider,nullptr,0,const_cast<PUCHAR>(data.data()),ULONG(data.size()),hash.data(),ULONG(hash.size()))>=0,"GOD SHA-1 verification failed");return hash;}
 void Verify(std::span<const uint8_t> data,std::span<const uint8_t> expected,const char* error){const auto actual=Hash(data);Require(expected.size()==actual.size() && std::equal(actual.begin(),actual.end(),expected.begin()),error);}
};
}
struct SvodImage::Impl {
 std::unique_ptr<File> package;std::vector<std::unique_ptr<File>> fragments;
 std::array<uint8_t,2048> descriptor{};std::vector<uint8_t> identity;
 uint64_t prefix=0,size=0,base=0;bool enhanced=false,internal=false;
 Sha1 sha;std::vector<std::array<uint8_t,4096>> masters;std::vector<uint8_t> group_cache;
 size_t cached_part=SIZE_MAX,cached_group=SIZE_MAX;
 File& Part(size_t i){Require(i<(internal?1:fragments.size()),"GOD logical data references a missing fragment");return internal?*package:*fragments.at(i);}
 void LoadGroup(size_t part,size_t group){
  if(cached_part==part && cached_group==group)return;
  cached_part=cached_group=SIZE_MAX;Require(group<203,"Invalid GOD data hash-group index");auto& file=Part(part);
  const auto table_at=base+4096+uint64_t(group)*(4096+sectors_per_table*sector);
  std::array<uint8_t,4096> table{};file.Read(table_at,table);
  sha.Verify(table,std::span(masters.at(part)).subspan(group*20,20),"GOD data hash table is corrupt");
  const auto data_at=table_at+4096;Require(data_at<file.size,"GOD data hash group has no data");
  const auto length=size_t(std::min<uint64_t>(sectors_per_table*sector,file.size-data_at));Require(length%2048==0,"GOD data hash group ends in a partial sector");
  group_cache.resize(length);file.Read(data_at,group_cache);
  for(size_t at=0;at<length;at+=4096){const auto bytes=std::span(group_cache).subspan(at,std::min<size_t>(4096,length-at));
   sha.Verify(bytes,std::span(table).subspan(at/4096*20,20),"GOD game data is corrupt (SHA-1 block mismatch)");}
  cached_part=part;cached_group=group;
 }
 explicit Impl(const std::filesystem::path& path){
  namespace fs=std::filesystem;package=std::make_unique<File>(path);Require(package->size>=0x1000,"GOD package header is truncated");
  std::array<uint8_t,4096> header{};package->Read(0,header);
  const auto magic=BE32(header.data());Require(magic==0x4c495645 || magic==0x50495253 || magic==0x434f4e20,"Invalid Xbox content package signature");
  const auto header_size=BE32(header.data()+0x340),version=BE32(header.data()+0x348);
  Require(header_size>=0x971a && header_size<=1024*1024 && package->size>=((uint64_t(header_size)+4095)&~uint64_t(4095)) && (version==1 || version==2),"GOD metadata header is truncated or unsupported");
  Require(BE32(header.data()+0x3a9)==1 && header[0x379]==0x24,"This Xbox content package is not an SVOD game");
  Require(BE32(header.data()+0x360)==0x42560816,"This GOD package is not Epic Mickey 2 for Xbox 360");
  const auto type=BE32(header.data()+0x344);Require(type==0x7000 || type==0x4000,"Select a full installed/Games on Demand game package");
  const auto count=BE32(header.data()+0x39d),start=LE24(header.data()+0x395);
  Require(count>0 && count<=128,"GOD fragment count exceeds disc limits");
  enhanced=(header[0x391]&0x40)!=0;prefix=uint64_t(start)*4096;
  Require(prefix<=16ull*1024*1024*1024,"Invalid GOD logical prefix");
  const auto folder=fs::path(path.wstring()+L".data");
  if(fs::is_directory(folder)){
   for(uint32_t i=0;i<count;++i){auto digits=std::to_wstring(i);digits.insert(0,4-digits.size(),L'0');auto fragment=std::make_unique<File>(folder/(L"Data"+digits));
    Require(fragment->size>=0x2000 && fragment->size<=max_file,"Invalid GOD data fragment size");if(i+1<count)Require(fragment->size==max_file,"A non-final GOD data fragment is incomplete");fragments.push_back(std::move(fragment));}
   for(const auto& entry:fs::directory_iterator(folder)){const auto name=entry.path().filename().wstring();if(name.size()==8 && CompareStringOrdinal(name.c_str(),4,L"Data",4,TRUE)==CSTR_EQUAL && std::all_of(name.begin()+4,name.end(),[](wchar_t c){return c>=L'0' && c<=L'9';}))Require(std::stoul(name.substr(4))<count,"GOD folder has more fragments than its package declares");}
  }else{Require(count==1 && !enhanced,"GOD package is missing its required .data folder");internal=true;base=0xb000;Require(package->size<=max_file+base,"Single-file GOD package exceeds its limit");}
  // Converter allocation counts may be byte-swapped or underreport trimmed
  // data. Derive the readable extent from the actual fragment/hash geometry.
  uint64_t data_bytes=0;
  for(uint32_t i=0;i<count;++i){const auto length=Part(i).size;Require(length>=base+0x2000,"GOD fragment is missing its hash tables");const auto physical=length-base;
   Require(physical>=0x2000,"GOD fragment has no data hash group");const auto rest=physical-4096,group=4096+sectors_per_table*sector,full=rest/group,tail=rest%group;
   Require(full<=203 && (!tail || (full<203 && tail>4096 && (tail-4096)%2048==0)),"GOD final hash group is truncated or invalid");
   data_bytes+=full*sectors_per_table*sector+(tail?tail-4096:0);
  }
  Require(data_bytes>=(enhanced?4096:0),"GOD enhanced descriptor is missing");
  size=prefix+data_bytes-(enhanced?4096:0);Require(size>=0x10800 && size<=16ull*1024*1024*1024,"GOD logical disc exceeds its limit");
  masters.resize(count);std::array<uint8_t,20> expected{};std::copy_n(header.begin()+0x37d,20,expected.begin());
  for(size_t i=0;i<count;++i){Part(i).Read(base,masters[i]);sha.Verify(masters[i],expected,"GOD master hash table/fragment chain is corrupt");
   std::copy_n(masters[i].begin()+203*20,20,expected.begin());}
  LoadGroup(0,0);const auto descriptor_at=enhanced || internal?0:0x10000;
  Require(group_cache.size()>=size_t(descriptor_at)+descriptor.size(),"GOD descriptor exceeds its verified hash group");std::copy_n(group_cache.begin()+descriptor_at,descriptor.size(),descriptor.begin());
  Require(std::equal(descriptor.begin(),descriptor.begin()+20,"MICROSOFT*XBOX*MEDIA"),"GOD game filesystem descriptor is missing");
  identity.assign(header.begin(),header.end());identity.insert(identity.end(),descriptor.begin(),descriptor.end());
 }
 void Read(uint64_t offset,std::span<uint8_t> output){
  Require(offset<=size && output.size()<=size-offset,"Read outside GOD logical disc");size_t done=0;
  while(done<output.size()){
   if(offset<prefix){const auto count=size_t(std::min<uint64_t>(output.size()-done,prefix-offset));std::fill_n(output.begin()+done,count,0);offset+=count;done+=count;continue;}
   const auto logical=(offset-prefix)/sector+(enhanced?2:0),within=offset%sector;
   const auto part=size_t(logical/sectors_per_file),block=logical%sectors_per_file;LoadGroup(part,size_t(block/sectors_per_table));
   const auto at=size_t(block%sectors_per_table*sector+within);Require(at<group_cache.size(),"GOD sector exceeds its verified data group");
   const auto count=std::min(output.size()-done,group_cache.size()-at);std::copy_n(group_cache.begin()+at,count,output.begin()+done);done+=count;offset+=count;
  }
 }
};
SvodImage::SvodImage(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
SvodImage::~SvodImage()=default;
std::unique_ptr<SvodImage> SvodImage::Open(const std::filesystem::path& path,std::string& error){try{auto result=std::unique_ptr<SvodImage>(new SvodImage(std::make_unique<Impl>(path)));error.clear();return result;}catch(const std::exception& e){error=e.what();return {};}}
std::filesystem::path SvodImage::FindPackage(const std::filesystem::path& folder,std::string& error){
 namespace fs=std::filesystem;error.clear();try{
  auto name=folder.wstring();if(name.size()>=5 && CompareStringOrdinal(name.c_str()+name.size()-5,5,L".data",5,TRUE)==CSTR_EQUAL){fs::path package=name.substr(0,name.size()-5);if(fs::is_regular_file(package))return package;}
  fs::path result;size_t count=0;
  for(const auto& entry:fs::directory_iterator(folder)){
   Require(++count<=1024,"Too many entries in the selected folder; select the GOD package header directly");
   if(!entry.is_regular_file() || entry.file_size()<0x971a)continue;
   std::array<uint8_t,4096> header{};try{File file(entry.path());file.Read(0,header);}catch(const std::exception&){continue;}
   const auto signature=BE32(header.data());if((signature!=0x4c495645 && signature!=0x50495253 && signature!=0x434f4e20) || BE32(header.data()+0x3a9)!=1 || BE32(header.data()+0x360)!=0x42560816)continue;
   Require(result.empty(),"This folder contains multiple EM2 GOD packages; select the package header you want");result=entry.path();
  }return result;
 }catch(const std::exception& e){error=e.what();return {};}
}
uint64_t SvodImage::Size()const{return impl_->size;}
const std::array<uint8_t,2048>& SvodImage::Descriptor()const{return impl_->descriptor;}
std::span<const uint8_t> SvodImage::IdentityPrefix()const{return impl_->identity;}
bool SvodImage::Read(uint64_t offset,std::span<uint8_t> bytes,std::string& error){try{impl_->Read(offset,bytes);error.clear();return true;}catch(const std::exception& e){error=e.what();return false;}}
}
