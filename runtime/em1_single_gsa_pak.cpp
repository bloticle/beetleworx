#include "em1_native_scene_pak.h"
#include "em1_deflate.h"
#include "em1_single_gsa_pak.h"
#include <rex/filesystem/vfs.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/cvar.h>
#include <fstream>
#include <filesystem>
#include <array>
#include <algorithm>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <set>
#include <rex/logging.h>
#include <windows.h>
REXCVAR_DEFINE_BOOL(em1_probe_disk_archives,false,"Debug","Private normal-entry experiment: identical converted PAKs through standard disk-backed files");
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(std::string,em2_user_data_directory);
namespace em2::em1 {
bool CopyRvzBytes(void*,const void*,size_t);
bool RegisteredLevelEntryProbeRequested();
namespace {
using namespace rex::filesystem;
using rex::X_STATUS;
using PakBytes=std::shared_ptr<const std::vector<uint8_t>>;
class PakFile final:public File {
 public:PakFile(uint32_t access,Entry* e,PakBytes bytes):File(access,e),bytes_(std::move(bytes)){}
  void Destroy() override{delete this;}
  X_STATUS ReadSync(void* output,size_t length,size_t offset,size_t* read) override{
    const bool observe=RegisteredLevelEntryProbeRequested() && observations_.fetch_add(1,std::memory_order_relaxed)<12;
    if(observe){const auto error=GetLastError();
      REXLOG_INFO("[em1] full-entry PAK read: '{}' offset={} requested={} file_bytes={}",entry_->name(),offset,length,bytes_?bytes_->size():0);SetLastError(error);}
    if(!read)return X_STATUS_INVALID_PARAMETER;*read=0;
    if(entry_->attributes()&kFileAttributeDirectory)return X_STATUS_FILE_IS_A_DIRECTORY;
    if(!(file_access_&(FileAccess::kGenericRead|FileAccess::kFileReadData)))return X_STATUS_ACCESS_DENIED;
    if(!length)return X_STATUS_SUCCESS;if(!output)return X_STATUS_INVALID_PARAMETER;
    if(!bytes_)return X_STATUS_UNSUCCESSFUL;
    if(offset>=bytes_->size())return X_STATUS_END_OF_FILE;
    const auto count=std::min(length,bytes_->size()-offset);
    if(!CopyRvzBytes(output,bytes_->data()+offset,count)){
      if(observe)REXLOG_ERROR("[em1] full-entry PAK copy failed: '{}' offset={} bytes={}",entry_->name(),offset,count);
      return X_STATUS_ACCESS_VIOLATION;
    }
    if(observe){const auto error=GetLastError();REXLOG_INFO("[em1] full-entry PAK read complete: '{}' offset={} bytes={}",entry_->name(),offset,count);SetLastError(error);}
    *read=count;return X_STATUS_SUCCESS;
  }
  X_STATUS WriteSync(const void*,size_t,size_t,size_t* count)override{if(count)*count=0;return X_STATUS_ACCESS_DENIED;}
  X_STATUS SetLength(size_t)override{return X_STATUS_ACCESS_DENIED;}
 private:PakBytes bytes_;std::atomic<unsigned> observations_{0};
};
class PakEntry final:public Entry {
 public:PakEntry(Device* d,Entry* parent,std::string_view path,bool directory=false,PakBytes bytes={}):Entry(d,parent,path),bytes_(std::move(bytes)){
    attributes_=kFileAttributeReadOnly|(directory?kFileAttributeDirectory:kFileAttributeNormal);size_=directory?0:bytes_->size();allocation_size_=(size_+511)&~size_t(511);
    create_timestamp_=access_timestamp_=write_timestamp_=0;
  }
  Entry* Add(std::unique_ptr<Entry> child){auto* result=child.get();children_.push_back(std::move(child));return result;}
  X_STATUS Open(uint32_t access,File** out)override{
    if(!out)return X_STATUS_INVALID_PARAMETER;*out=nullptr;
    constexpr uint32_t writes=FileAccess::kGenericWrite|FileAccess::kGenericAll|FileAccess::kFileWriteData|
        FileAccess::kFileAppendData|0x10|0x100|0x10000|0x40000|0x80000;
    if(access&writes)return X_STATUS_ACCESS_DENIED;
    *out=new PakFile(access,this,bytes_);return X_STATUS_SUCCESS;
  }
 private:PakBytes bytes_;
};
class ForwardEntry final:public Entry {
 public:
  static Entry* Original(Entry* source){
    while(auto* forwarded=dynamic_cast<ForwardEntry*>(source))source=forwarded->source_;
    return source;
  }
ForwardEntry(Device* device,Entry* parent,Entry* source,std::string path,unsigned depth,unsigned& budget)
      :Entry(device,parent,path),source_(Original(source)){
    if(depth>16 || !budget--)throw std::runtime_error("Original pack directory metadata exceeds bounds");
    source_->update();attributes_=source_->attributes()|kFileAttributeReadOnly;
    size_=source_->size();allocation_size_=source_->allocation_size();
    create_timestamp_=source_->create_timestamp();access_timestamp_=source_->access_timestamp();write_timestamp_=source_->write_timestamp();
    for(const auto& child:source_->children())
      children_.push_back(std::make_unique<ForwardEntry>(device,this,child.get(),path+char(92)+child->name(),depth+1,budget));
  }
  X_STATUS Open(uint32_t access,File** out)override{
    constexpr uint32_t writes=FileAccess::kGenericWrite|FileAccess::kGenericAll|FileAccess::kFileWriteData|
        FileAccess::kFileAppendData|0x10|0x100|0x10000|0x40000|0x80000;
    if(access&writes){if(out)*out=nullptr;return X_STATUS_ACCESS_DENIED;}
    return source_->Open(access,out);
  }
  bool can_map()const override{return source_->can_map();}
  std::unique_ptr<rex::memory::MappedMemory> OpenMapped(rex::memory::MappedMemory::Mode mode,size_t offset=0,size_t length=0)override{
    return mode==rex::memory::MappedMemory::Mode::kRead?source_->OpenMapped(mode,offset,length):nullptr;
  }
 private:Entry* source_;
};
class PakDevice final:public Device {
 public:PakDevice(std::string_view mount,Entry* original,std::vector<std::pair<std::string,PakBytes>> archives,Entry* disk=nullptr):Device(mount),original_(original),archives_(std::move(archives)),disk_(disk){}
  bool Initialize()override{entry_=std::make_unique<PakEntry>(this,nullptr,"",true);
    unsigned budget=20000;original_->update();
    for(const auto& child:original_->children()) {
      const bool replaced=std::any_of(archives_.begin(),archives_.end(),[&](const auto& item){
        if(item.first.size()!=child->name().size())return false;
        for(size_t i=0;i<item.first.size();++i){auto a=item.first[i],b=child->name()[i];
          if(a>='A'&&a<='Z')a+='a'-'A';if(b>='A'&&b<='Z')b+='a'-'A';if(a!=b)return false;}
        return true;});
      if(!replaced)entry_->Add(std::make_unique<ForwardEntry>(this,entry_.get(),child.get(),child->name(),0,budget));
    }
    for(const auto& [name,bytes]:archives_){
      if(disk_){auto* file=disk_->ResolvePath(name);if(!file)throw std::runtime_error("Private disk PAK is unavailable");
        entry_->Add(std::make_unique<ForwardEntry>(this,entry_.get(),file,name,0,budget));}
      else entry_->Add(std::make_unique<PakEntry>(this,entry_.get(),name,false,bytes));
    }return true;}
  Entry* ResolvePath(std::string_view p)override{
    while(!p.empty() && (p.front()==char(92)||p.front()==char(47)))p.remove_prefix(1);
    return p.empty()?entry_.get():entry_->ResolvePath(p);
  }
  void Dump(rex::string::StringBuffer* b)override{entry_->Dump(b,0);}
  const std::string& name()const override{return name_;}
  uint32_t attributes()const override{return kFileAttributeReadOnly;}
  uint32_t component_name_max_length()const override{return 255;}
  uint32_t total_allocation_units()const override{return 1;}
  uint32_t available_allocation_units()const override{return 0;}
  uint32_t sectors_per_allocation_unit()const override{return 1;}
  uint32_t bytes_per_sector()const override{return 512;}
 private:std::unique_ptr<PakEntry> entry_;Entry* original_;std::vector<std::pair<std::string,PakBytes>> archives_;Entry* disk_;std::string name_="EM1 native scene prerequisite";
};
std::atomic<unsigned> mounts{0};
}
namespace {
bool MountOwnedNativePaks(rex::filesystem::VirtualFileSystem& fs,
    std::vector<std::pair<std::string,PakBytes>> archives,const std::string& basename,
    bool allow_disk,std::string& error){
  const auto reservation=mounts.fetch_add(1);
  bool accepted=false;
  struct Slot {bool& accepted;~Slot(){if(!accepted)--mounts;}} slot{accepted};
  if(reservation>=64){error="Native scene PAK overlay limit reached";return false;}
  const auto mount="\\Device\\Em1SingleGsa_"+basename;
  auto* original=fs.ResolvePath("d:\\packfiles");
  auto* original_probe=ForwardEntry::Original(fs.ResolvePath("d:\\packfiles\\globalscripts.pak"));
  if(!original || !(original->attributes()&kFileAttributeDirectory) || !original_probe){error="Original pack directory/probe is unavailable";return false;}
  Entry* disk=nullptr;const auto disk_mount=mount+"_Disk";
  struct DiskRollback {VirtualFileSystem& fs;const std::string& mount;bool registered=false;bool& accepted;
    ~DiskRollback(){if(registered && !accepted)fs.UnregisterDevice(mount);}} disk_rollback{fs,disk_mount,false,accepted};
  if(allow_disk && REXCVAR_GET(em1_probe_disk_archives)){
    try{
      const auto user=REXCVAR_GET(em2_user_data_directory);
      if(!REXCVAR_GET(em2_private_profile) || user.empty())throw std::runtime_error("Disk archive experiment requires an isolated user-data directory");
      const auto folder=std::filesystem::path(user)/("em1-native-paks-"+basename);
      if(std::filesystem::exists(folder))throw std::runtime_error("Private disk PAK directory already exists");
      std::filesystem::create_directories(folder);
      for(const auto& [name,bytes]:archives){std::ofstream file(folder/name,std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes->data()),std::streamsize(bytes->size()));file.close();
        if(!file)throw std::runtime_error("Could not write private native PAK");}
      auto host=std::make_unique<HostPathDevice>(disk_mount,folder,true);
      if(!host->Initialize() || !fs.RegisterDevice(std::move(host)))throw std::runtime_error("Could not mount private disk PAKs");
      disk_rollback.registered=true;disk=fs.ResolvePath(disk_mount);
      if(!disk)throw std::runtime_error("Private disk PAK root is unavailable");
      REXLOG_INFO("[em1] private disk archive comparison: same {} converted PAK payloads via standard host files",archives.size());
    }catch(const std::exception& exception){error=exception.what();return false;}
  }
  auto device=std::make_unique<PakDevice>(mount,original,archives,disk);
  try{device->Initialize();}catch(const std::exception& exception){error=exception.what();return false;}
  std::vector<Entry*> expected;for(const auto& archive:archives)expected.push_back(device->ResolvePath(archive.first));
  if(!fs.RegisterDevice(std::move(device))){error="Could not register native scene PAK device";return false;}
  struct DeviceRollback {
    VirtualFileSystem& fs;const std::string& mount;bool& accepted;
    ~DeviceRollback(){if(!accepted)fs.UnregisterDevice(mount);}
  } device_rollback{fs,mount,accepted};
  const std::string logical="d:\\packfiles",physical="\\Device\\Harddisk0\\Partition1\\packfiles";
  std::string old_logical,old_physical;
  const bool had_logical=fs.FindSymbolicLink(logical,old_logical),had_physical=fs.FindSymbolicLink(physical,old_physical);
  struct Rollback {
    VirtualFileSystem& fs;const std::string& logical;const std::string& physical;
    const std::string& old_logical;const std::string& old_physical;bool had_logical,had_physical;bool& accepted;
    ~Rollback(){if(!accepted){fs.UnregisterSymbolicLink(logical);fs.UnregisterSymbolicLink(physical);
      if(had_logical)fs.RegisterSymbolicLink(logical,old_logical);if(had_physical)fs.RegisterSymbolicLink(physical,old_physical);}}
  } rollback{fs,logical,physical,old_logical,old_physical,had_logical,had_physical,accepted};
  if(had_logical)fs.UnregisterSymbolicLink(logical);
  if(had_physical)fs.UnregisterSymbolicLink(physical);
  if(!fs.RegisterSymbolicLink(physical,mount) || !fs.RegisterSymbolicLink(logical,mount)){error="Could not register level archive aliases";return false;}
  struct Close {void operator()(File* file)const{if(file)file->Destroy();}};
  std::array<uint8_t,65536> buffer;
  for(size_t i=0;i<archives.size();++i){
    const auto& [name,bytes]=archives[i];const auto path="d:\\packfiles\\"+name;
    if(fs.ResolvePath(path)!=expected[i]){error="VFS did not resolve a level archive";return false;}
    File* opened=nullptr;FileAction action{};
    const auto status=fs.OpenFile(nullptr,path,FileDisposition::kOpen,FileAccess::kGenericRead,false,true,&opened,&action);
    std::unique_ptr<File,Close> owner(opened);
    if(status!=X_STATUS_SUCCESS || !opened){error="VFS could not open level archive: "+name;return false;}
    for(size_t at=0;at<bytes->size();){
      const auto count=std::min(buffer.size(),bytes->size()-at);size_t read=0;
      if(opened->ReadSync(buffer.data(),count,at,&read)!=X_STATUS_SUCCESS || read!=count ||
        !std::equal(buffer.begin(),buffer.begin()+count,bytes->begin()+at)){error="VFS level archive read verification failed: "+name;return false;}
      at+=count;
    }
  }
  File* opened=nullptr;FileAction action{};
  const auto status=fs.OpenFile(nullptr,"d:\\packfiles\\globalscripts.pak",FileDisposition::kOpen,FileAccess::kGenericRead,false,true,&opened,&action);
  std::unique_ptr<File,Close> owner(opened);
  if(status!=X_STATUS_SUCCESS || !opened || opened->entry()!=original_probe){error="Original PAK forwarding changed source ownership";return false;}
  accepted=true;error.clear();return true;
}
}
bool MountNativeScenePaks(rex::filesystem::VirtualFileSystem& fs,std::span<const NativeScenePakRequest> requests,std::string& error,
    std::vector<PakBytes>* mounted_bytes){
  if(requests.empty() || requests.size()>1024){error="Invalid level archive count";return false;}
  std::vector<std::pair<std::string,PakBytes>> archives;
  std::vector<PakBytes> owners;
  std::set<std::string> names;size_t total=0;
  try {
    for(const auto& request:requests){
      const auto& name=request.basename;
      if((!name.starts_with("em1_courtyard_") && !name.starts_with("em1_level_")) || name.size()>80 ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789")!=std::string::npos || !names.insert(name).second)
        throw std::runtime_error("Invalid/duplicate native scene PAK basename");
      if(fs.ResolvePath("d:\\packfiles\\"+name+".pak"))throw std::runtime_error("Refusing to shadow an existing game PAK");
      std::vector<NativeScenePakEntry> encoded(request.scenes.begin(),request.scenes.end());
      std::vector<std::vector<uint8_t>> compressed(encoded.size());
      for(size_t i=0;i<encoded.size();++i){
        if(request.compress){
          compressed[i]=DeflateNativePakResource(encoded[i].bytes);
          if(compressed[i].size()<encoded[i].bytes.size())encoded[i].packed_bytes=compressed[i];
        }
        const auto stored=encoded[i].packed_bytes.empty()?encoded[i].bytes.size():encoded[i].packed_bytes.size();
        if(stored>8ull*1024*1024)throw std::runtime_error("Converted resource exceeds native streaming window even after compression");
      }
      auto bytes=BuildNativeScenePak(encoded);
      if(bytes->size()>256ull*1024*1024-total)throw std::runtime_error("Level archives exceed memory budget");
      total+=bytes->size();if(mounted_bytes)owners.push_back(bytes);archives.emplace_back(name+".pak",std::move(bytes));
    }
  }catch(const std::exception& exception){error=exception.what();return false;}
  if(!MountOwnedNativePaks(fs,std::move(archives),requests.front().basename,true,error))return false;
  if(mounted_bytes)mounted_bytes->swap(owners);
  return true;
}

bool MountNativeScenePak(rex::filesystem::VirtualFileSystem& fs,std::string_view basename,std::span<const NativeScenePakEntry> scenes,std::string& error){
  const NativeScenePakRequest request{std::string(basename),{scenes.begin(),scenes.end()}};
  return MountNativeScenePaks(fs,std::span(&request,1),error);
}
bool MountSingleGsaPak(rex::filesystem::VirtualFileSystem& fs,std::string_view basename,std::string_view scene_name,std::span<const uint8_t> scene,std::string& error){
  const NativeScenePakEntry entry{scene_name,scene};
  // The single-scene owner retains the archive's raw buffer and compares its
  // identity during scene/model publication. Compressed records return a
  // temporary decompression buffer instead; use native uncompressed storage
  // here so both the owner and original loader refer to the same scene.
  const NativeScenePakRequest request{std::string(basename),{entry},false};
  return MountNativeScenePaks(fs,std::span(&request,1),error);
}

}

namespace em2::em1 {
bool MountNativePlayerPak(rex::filesystem::VirtualFileSystem& fs,
    std::shared_ptr<const std::vector<uint8_t>> bytes,std::string& error){
  if(!bytes || bytes->empty() || bytes->size()>256ull*1024*1024){error="Invalid restored player archive size";return false;}
  if(!fs.ResolvePath("d:\\packfiles\\_Dynamic.pak")){error="Original player archive is unavailable";return false;}
  return MountOwnedNativePaks(fs,{{"_Dynamic.pak",std::move(bytes)}},"restored_player",false,error);
}
}
