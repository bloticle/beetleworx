#include "seven_image.h"
#include "archive_io.h"
#include <algorithm>
#include <map>
#include <functional>
extern "C" int bw_seven_unpack(uint64_t,void*,int(*)(void*,uint64_t,uint8_t*,size_t),int(*)(void*,uint64_t),int(*)(void*,const uint8_t*,size_t,uint64_t,int),int(*)(void*,const uint8_t*,size_t),uint8_t*,size_t);
namespace em2 {
namespace {using namespace archive_detail;struct Canceled{};
bool SplitPath(const std::filesystem::path& path){const auto number=path.extension().wstring(),extension=path.stem().extension().wstring();return number.size()==4 && number[0]==L'.' && std::all_of(number.begin()+1,number.end(),[](wchar_t c){return c>=L'0' && c<=L'9';}) && CompareStringOrdinal(extension.c_str(),-1,L".7z",-1,TRUE)==CSTR_EQUAL;}
struct SevenSource {
 std::vector<std::unique_ptr<File>> parts;std::vector<uint64_t> starts;uint64_t size=0;
 explicit SevenSource(const std::filesystem::path& path){
  auto add=[&](const std::filesystem::path& part){auto file=std::make_unique<File>(part);Require(file->size && file->size<=16ull*1024*1024*1024-size,"Empty or excessively large 7z volume");starts.push_back(size);size+=file->size;parts.push_back(std::move(file));};
  if(!SplitPath(path)){add(path);return;}
  const auto chosen=std::stoul(path.extension().wstring().substr(1));Require(chosen>=1 && chosen<=128,"7z volume number must be between 001 and 128");
  const auto prefix=path.stem().wstring();auto sibling=[&](unsigned n){auto digits=std::to_wstring(n);digits.insert(0,3-digits.size(),L'0');return path.parent_path()/(prefix+L'.'+digits);};
  Require(!std::filesystem::exists(sibling(129)),"7z archive exceeds the 128-volume limit");bool gap=false;
  for(unsigned n=1;n<=128;++n){const auto part=sibling(n);if(!std::filesystem::exists(part)){gap=true;continue;}Require(!gap,"7z archive has a missing volume; keep every numbered part in the same folder");add(part);}
  Require(parts.size()>=chosen,"7z archive is missing the selected volume");
 }
 void Read(uint64_t at,std::span<uint8_t> out){Require(at<=size && out.size()<=size-at,"Read outside 7z volume set");size_t copied=0;
  while(copied<out.size()){const auto i=size_t(std::upper_bound(starts.begin(),starts.end(),at)-starts.begin()-1);const auto local=at-starts[i];const auto n=size_t(std::min<uint64_t>(out.size()-copied,parts[i]->size-local));parts[i]->Read(local,out.subspan(copied,n));copied+=n;at+=n;}
 }
};
struct Temporary {
 HANDLE value=INVALID_HANDLE_VALUE;
 ~Temporary(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
 void Create(uint64_t size){std::wstring directory(32768,L'\0');const auto count=GetTempPathW(DWORD(directory.size()),directory.data());Require(count && count<directory.size(),"Cannot locate the temporary cache folder");directory.resize(count);
  ULARGE_INTEGER available{};Require(GetDiskFreeSpaceExW(directory.c_str(),&available,nullptr,nullptr) && available.QuadPart>=size,"Not enough space in the Windows temporary folder to unpack this 7z archive");
  for(unsigned attempt=0;attempt<8 && value==INVALID_HANDLE_VALUE;++attempt){std::array<uint8_t,16> random{};Require(BCryptGenRandom(nullptr,random.data(),ULONG(random.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0,"Cannot create a unique 7z cache name");std::wstring name=directory+L"beetleworx-7z-";for(auto b:random){name+=L"0123456789abcdef"[b>>4];name+=L"0123456789abcdef"[b&15];}name+=L".tmp";
   value=CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
  }Require(value!=INVALID_HANDLE_VALUE,"Cannot create the temporary 7z cache");
 }
};
std::string Lower(std::string s){for(auto& c:s)if(c>='A' && c<='Z')c+=32;return s;}
}
struct SevenImage::Impl {
 SevenSource source;Temporary temporary;std::array<uint8_t,32> identity{};std::vector<ArchiveFile> files;
 std::map<std::string,std::pair<ArchiveFile,bool>> tree;std::function<bool(uint64_t,uint64_t)> progress;
 uint64_t size=0,total=0,remaining=0,path_bytes=0,next_progress=0,progress_total=UINT64_MAX;bool canceled=false;std::string failure;
 bool Notify(uint64_t done,uint64_t all){if(progress && (all!=progress_total || done>=next_progress || done==all)){progress_total=all;next_progress=done+1024*1024;if(!progress(done,all)){canceled=true;return false;}}return true;}
 template<class F> static int Callback(void* value,F action){auto& self=*static_cast<Impl*>(value);try{return action(self)?1:0;}catch(const std::exception& e){self.failure=e.what();return 0;}catch(...){self.failure="7z source/output callback failed";return 0;}}
 static int SourceRead(void* value,uint64_t at,uint8_t* data,size_t length){return Callback(value,[&](Impl& s){s.source.Read(at,std::span(data,length));return true;});}
 static int Begin(void* value,uint64_t total){return Callback(value,[&](Impl& s){s.total=total;Require(total<=16ull*1024*1024*1024,"7z output exceeds the setup budget");if(!s.Notify(s.source.size,s.source.size+total))return false;s.temporary.Create(total);return true;});}
 static int Entry(void* value,const uint8_t* name,size_t length,uint64_t bytes,int directory){return Callback(value,[&](Impl& s){Require(!s.remaining && length && length<=32768,"Invalid 7z entry boundary");std::string path(reinterpret_cast<const char*>(name),length);size_t at=0;
  for(;;){const auto end=path.find('/',at);const bool leaf=end==std::string::npos;const auto part=leaf?path:path.substr(0,end);const bool dir=!leaf || directory;const auto key=Lower(part);auto found=s.tree.find(key);
   if(found==s.tree.end()){Require(s.tree.size()<100000 && part.size()<=64*1024*1024-s.path_bytes,"7z path metadata exceeds the setup budget");s.path_bytes+=part.size();s.tree.emplace(key,std::make_pair(ArchiveFile{part,leaf?s.size:0,leaf?bytes:0,dir},leaf));}
   else{Require(dir && found->second.first.directory && (!leaf || !found->second.second),"Duplicate or conflicting 7z file/directory path");if(leaf)found->second.second=true;}
   if(leaf)break;at=end+1;
  }s.remaining=bytes;return s.Notify(s.source.size+s.size,s.source.size+s.total);});}
 static int Data(void* value,const uint8_t* data,size_t length){return Callback(value,[&](Impl& s){Require(length<=65536 && length<=s.remaining && length<=s.total-s.size,"7z data exceeds its declared size");if(!s.Notify(s.source.size+s.size,s.source.size+s.total))return false;DWORD written=0;
  Require(WriteFile(s.temporary.value,data,DWORD(length),&written,nullptr) && written==length,"Could not write the temporary 7z cache; check free disk space");s.size+=length;s.remaining-=length;return true;});}
 Impl(const std::filesystem::path& path,const std::function<bool(uint64_t,uint64_t)>& callback):source(path),progress(callback){Require(source.size>=32 && source.size<=16ull*1024*1024*1024,"Invalid 7z container size");
  Hash hash;std::vector<uint8_t> buffer(1024*1024);for(uint64_t at=0;at<source.size;){if(!Notify(at,source.size))throw Canceled{};const auto n=size_t(std::min<uint64_t>(buffer.size(),source.size-at));source.Read(at,std::span(buffer).first(n));hash.Add(std::span(buffer).first(n));at+=n;}identity=hash.Finish();
  std::array<uint8_t,2048> error{};const auto result=bw_seven_unpack(source.size,this,SourceRead,Begin,Entry,Data,error.data(),error.size());
  if(canceled)throw Canceled{};Require(result==0,failure.empty()?reinterpret_cast<const char*>(error.data()):failure.c_str());Require(!remaining && size==total,"7z cache did not receive all declared data");for(auto& [key,value]:tree)files.push_back(std::move(value.first));tree.clear();if(!Notify(source.size+size,source.size+size))throw Canceled{};
 }
 void Read(uint64_t at,std::span<uint8_t> out){Require(at<=size && out.size()<=size-at && out.size()<=MAXDWORD,"Read outside the temporary 7z cache");if(out.empty())return;LARGE_INTEGER offset{};offset.QuadPart=LONGLONG(at);DWORD got=0;Require(SetFilePointerEx(temporary.value,offset,nullptr,FILE_BEGIN) && ReadFile(temporary.value,out.data(),DWORD(out.size()),&got,nullptr) && got==out.size(),"Could not read temporary 7z data");}
};
SevenImage::SevenImage(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
SevenImage::~SevenImage()=default;
bool SevenImage::IsSplitPath(const std::filesystem::path& path){return SplitPath(path);}
std::unique_ptr<SevenImage> SevenImage::Open(const std::filesystem::path& path,std::string& error,const std::function<bool(uint64_t,uint64_t)>& progress){try{auto out=std::unique_ptr<SevenImage>(new SevenImage(std::make_unique<Impl>(path,progress)));error.clear();return out;}catch(const Canceled&){error.clear();return {};}catch(const std::exception& e){error=e.what();return {};}}
bool TestSevenImageCancellation(const std::filesystem::path& path){
 try{const auto physical=SevenSource(path).size;bool canceled=false;std::string error;
  auto image=SevenImage::Open(path,error,[&](uint64_t done,uint64_t total){if(total>physical && done>physical && done<total){canceled=true;return false;}return true;});
  return canceled && !image && error.empty();
 }catch(const std::exception&){return false;}
}
uint64_t SevenImage::Size()const{return impl_->size;}
std::span<const uint8_t> SevenImage::Identity()const{return impl_->identity;}
const std::vector<ArchiveFile>& SevenImage::Files()const{return impl_->files;}
bool SevenImage::Read(uint64_t at,std::span<uint8_t> out,std::string& error){try{impl_->Read(at,out);error.clear();return true;}catch(const std::exception& e){error=e.what();return false;}}
}
